/* The LLM agent workspace: chats with a local model that works on the open
 * project through the built-in harness (llmtools.h, after mcp-fbsd).
 *
 *   - left: New Chat, the project's chats (newest first; each kept as JSON
 *     in $XDG_STATE_HOME/vxide/llm/<project>/), and the harness: the model
 *     server, the model, Auto / Server / Mock, read-only, the system prompt
 *   - right, the whole width that is left: the transcript (questions,
 *     answers as Markdown, the model's reasoning folded, each tool call a
 *     row that opens to its arguments and answer) and the question box
 *   - a turn runs on a thread of its own (llmchat.c's llm_turn); the chat is
 *     shared under S.mu, and the UI only reads it while a turn runs
 *   - the system prompt is a file the user edits:
 *     $XDG_CONFIG_HOME/vxide/llm-system.md, read at every turn ({root} is
 *     the project); the default is three lines
 * Nothing here blocks the UI thread on the model. */
#define _DEFAULT_SOURCE
#include "ui.h"
#include "llmchat.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <SDL3/SDL.h>

#define DEFAULT_SERVER "http://127.0.0.1:8080"
#define TOOL_LINES_MAX 60 /* of a tool's answer, shown when it is open */

enum { MF_OPEN = 1, MF_THINK = 2 };

/* what the tab keeps for a message: its Markdown layouts, its title */
typedef struct {
	MdText *md, *think;
	char title[240];
	bool titled;
} UiMsg;

static struct {
	bool inited;
	pthread_mutex_t mu;
	char dir[4096]; /* where the chats are kept ("" = nowhere: headless) */
	LlmChat **chats;
	u32 nchats, cap;
	LlmChat *cur; /* the chat shown */
	/* the turn */
	bool running;
	pthread_t th;
	LlmRun run;
	LlmCfg cfg;
	char *system;
	LlmChat *run_chat;
	_Atomic int changed, finished;
	int last_used; /* LLM_SERVER / LLM_MOCK after a turn, -1 before */
	char why_mock[200];
	/* the view */
	i64 scroll, list_scroll;
	bool stick;
	Input in, srv_in, model_in;
	int field; /* 0 the question, 1 the server, 2 the model */
} S;

static void lockf_(void *c)
{
	(void)c;
	pthread_mutex_lock(&S.mu);
}
static void unlockf_(void *c)
{
	(void)c;
	pthread_mutex_unlock(&S.mu);
}
static void changedf_(void *c)
{
	(void)c;
	atomic_store(&S.changed, 1);
}

/* ---- settings, paths ------------------------------------------------------------------------- */

static const char *server_url(void)
{
	const char *e = getenv("VXIDE_LLM_SERVER");
	if (e && *e)
		return e;
	return app.llm_server[0] ? app.llm_server : DEFAULT_SERVER;
}

static int backend(void)
{
	const char *e = getenv("VXIDE_LLM_BACKEND");
	if (e && *e)
		return !strcmp(e, "server") ? LLM_SERVER : !strcmp(e, "mock") ? LLM_MOCK : LLM_AUTO;
	return VX_CLAMP(app.llm_backend, LLM_AUTO, LLM_MOCK);
}

static void system_path(char *out, size_t cap)
{
	const char *e = getenv("VXIDE_LLM_SYSTEM");
	if (e && *e) {
		snprintf(out, cap, "%s", e);
		return;
	}
	const char *x = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (x && *x)
		snprintf(out, cap, "%s/vxide/llm-system.md", x);
	else
		snprintf(out, cap, "%s/.config/vxide/llm-system.md", home ? home : ".");
}

/* the user's system prompt, or the default; {root} filled in */
static char *system_prompt(void)
{
	char p[4096];
	system_path(p, sizeof p);
	size_t n = 0;
	char *s = vx_read_file(p, &n);
	char *r = llm_expand_system(s ? s : llm_default_system, app.root);
	free(s);
	return r;
}

static void edit_system_prompt(void)
{
	char p[4096];
	system_path(p, sizeof p);
	struct stat st;
	if (stat(p, &st) != 0) {
		char d[4096];
		snprintf(d, sizeof d, "%s", p);
		for (char *s = d + 1; *s; s++)
			if (*s == '/') {
				*s = 0;
				mkdir(d, 0755);
				*s = '/';
			}
		if (vx_write_file(p, llm_default_system, strlen(llm_default_system)) != 0) {
			ui_status("Cannot write %s: %s", p, strerror(errno));
			return;
		}
	}
	app.side_view = SIDE_EXPLORER; /* the editors, to show it */
	open_file(p, false, UINT32_MAX, 0);
	ui_status("The system prompt: saved changes apply from the next message ({root} is the project folder)");
}

/* ---- chats ----------------------------------------------------------------------------------- */

static void ui_free_msgs(LlmChat *c)
{
	for (u32 i = 0; i < c->n; i++) {
		UiMsg *u = c->m[i].ui;
		if (!u)
			continue;
		md_text_free(u->md);
		md_text_free(u->think);
		free(u);
		c->m[i].ui = NULL;
	}
}

static void chat_free(LlmChat *c)
{
	ui_free_msgs(c);
	llm_chat_free(c);
	free(c);
}

static int by_updated(const void *a, const void *b)
{
	const LlmChat *x = *(LlmChat *const *)a, *y = *(LlmChat *const *)b;
	return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : 0;
}

static void sort_chats(void)
{
	if (S.nchats > 1)
		qsort(S.chats, S.nchats, sizeof *S.chats, by_updated);
}

static void chats_dir(void)
{
	S.dir[0] = 0;
	char st[4096];
	if (!app.root || !session_state_dir(st, sizeof st))
		return;
	char name[2048];
	size_t k = 0;
	for (const char *p = app.root; *p && k + 1 < sizeof name; p++)
		name[k++] = *p == '/' ? '%' : *p;
	name[k] = 0;
	snprintf(S.dir, sizeof S.dir, "%s/llm/%s", st, name);
}

static void load_chats(void)
{
	chats_dir();
	if (!S.dir[0])
		return;
	DIR *d = opendir(S.dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".json"))
			continue;
		char p[8192];
		snprintf(p, sizeof p, "%s/%s", S.dir, e->d_name);
		LlmChat *c = vx_alloc(sizeof *c);
		if (!llm_chat_load(c, p)) {
			free(c);
			continue;
		}
		VX_GROW(S.chats, S.cap, S.nchats + 1);
		S.chats[S.nchats++] = c;
	}
	closedir(d);
	sort_chats();
}

static void save_chat(const LlmChat *c)
{
	if (S.dir[0] && c->n)
		llm_chat_save(c, S.dir);
}

/* a new chat at the top (the empty one there is reused) */
static void new_chat(void)
{
	if (S.nchats && !S.chats[0]->n) {
		S.cur = S.chats[0];
	} else {
		LlmChat *c = vx_alloc(sizeof *c);
		llm_chat_init(c);
		VX_GROW(S.chats, S.cap, S.nchats + 1);
		memmove(S.chats + 1, S.chats, S.nchats * sizeof *S.chats);
		S.chats[0] = c;
		S.nchats++;
		S.cur = c;
	}
	S.scroll = 0;
	S.stick = true;
	S.list_scroll = 0;
	S.field = 0;
	app.focus = FOCUS_MAN_INPUT;
	app.dirty = true;
}

static void delete_chat(u32 i)
{
	if (i >= S.nchats)
		return;
	LlmChat *c = S.chats[i];
	if (S.running && c == S.run_chat) {
		ui_status("The chat is running: stop it first (Esc)");
		return;
	}
	if (S.dir[0] && c->n) {
		char p[8192];
		snprintf(p, sizeof p, "%s/%s.json", S.dir, c->id);
		unlink(p);
	}
	memmove(S.chats + i, S.chats + i + 1, (S.nchats - i - 1) * sizeof *S.chats);
	S.nchats--;
	if (S.cur == c)
		S.cur = S.nchats ? S.chats[VX_MIN(i, S.nchats - 1)] : NULL;
	chat_free(c);
	if (!S.cur)
		new_chat();
	app.dirty = true;
}

/* ---- the turn -------------------------------------------------------------------------------- */

static void *turn_main(void *a)
{
	(void)a;
	llm_turn(&S.run);
	atomic_store(&S.finished, 1);
	atomic_store(&S.changed, 1);
	return NULL;
}

static void ask(const char *s, size_t n)
{
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' '))
		n--;
	while (n && (*s == '\n' || *s == ' '))
		s++, n--;
	if (!n)
		return;
	if (S.running) {
		ui_status("The agent is still working: Esc stops it");
		return;
	}
	if (!app.root) {
		ui_status("Open a folder first: the agent works on the project");
		return;
	}
	if (!S.cur)
		new_chat();
	LlmChat *c = S.cur;
	pthread_mutex_lock(&S.mu);
	llm_msg_add(c, LLM_USER, s, n);
	if (!c->title.n)
		llm_chat_set_title(c);
	pthread_mutex_unlock(&S.mu);
	sort_chats();
	save_chat(c);
	/* the configuration of this turn */
	free(S.system);
	S.system = system_prompt();
	memset(&S.cfg, 0, sizeof S.cfg);
	snprintf(S.cfg.server, sizeof S.cfg.server, "%s", server_url());
	snprintf(S.cfg.model, sizeof S.cfg.model, "%s", app.llm_model);
	const char *key = getenv("VXIDE_LLM_API_KEY");
	snprintf(S.cfg.api_key, sizeof S.cfg.api_key, "%s", key ? key : "");
	S.cfg.backend = backend();
	S.cfg.system = S.system;
	S.cfg.tools.root = app.root;
	S.cfg.tools.read_only = app.llm_read_only;
	S.cfg.tools.cancel = &S.run.cancel;
	S.cfg.mock_delay_ms = app.headless ? 0 : 10;
	memset(&S.run, 0, sizeof S.run);
	S.run.cfg = &S.cfg;
	S.run.chat = c;
	S.run.lock = lockf_;
	S.run.unlock = unlockf_;
	S.run.changed = changedf_;
	S.run_chat = c;
	atomic_store(&S.finished, 0);
	if (pthread_create(&S.th, NULL, turn_main, NULL) != 0) {
		ui_status("Cannot start the agent: %s", strerror(errno));
		return;
	}
	S.running = true;
	S.stick = true;
	app.dirty = true;
}

static void stop(void)
{
	if (S.running) {
		S.run.cancel = 1;
		ui_status("Stopping the agent");
	}
}

void llmview_tick(void)
{
	if (!S.inited)
		return;
	if (atomic_exchange(&S.changed, 0))
		app.dirty = true;
	if (S.running && atomic_load(&S.finished)) {
		pthread_join(S.th, NULL);
		S.running = false;
		S.last_used = S.run.used;
		snprintf(S.why_mock, sizeof S.why_mock, "%s", S.run.why_mock);
		llm_chat_set_title(S.run_chat);
		save_chat(S.run_chat);
		S.run_chat = NULL;
		app.dirty = true;
	}
}

bool llmview_busy(void) { return S.running; }

void llmview_shutdown(void)
{
	if (!S.inited)
		return;
	if (S.running) {
		S.run.cancel = 1;
		pthread_join(S.th, NULL);
		S.running = false;
		save_chat(S.run_chat);
	}
	for (u32 i = 0; i < S.nchats; i++)
		chat_free(S.chats[i]);
	free(S.chats);
	free(S.system);
	S.chats = NULL;
	S.nchats = S.cap = 0;
	S.cur = NULL;
	S.inited = false;
}

/* ---- drawing helpers ---------------------------------------------------------------------- */

enum {
	H_NONE,
	H_NEW,
	H_CHAT,
	H_CHAT_DEL,
	H_STOP,
	H_SEND,
	H_SYSTEM,
	H_BACKEND,
	H_READONLY,
	H_SRV,
	H_MODEL,
	H_INPUT,
	H_TOOL,
	H_THINK,
	H_COPY,
	H_MD,
	H_EXAMPLE,
	H_LIST,
	H_TRANSCRIPT
};
typedef struct {
	int x, y, w, h, act;
	i64 arg;
	int ox, oy; /* H_MD: where its layout was drawn */
} Hit;
static Hit hits[1024];
static int nhit;

static bool in_r(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static Hit *hit(int x, int y, int w, int h, int act, i64 arg)
{
	if (nhit >= (int)VX_COUNT(hits))
		return NULL;
	hits[nhit] = (Hit){x, y, w, h, act, arg, 0, 0};
	return &hits[nhit++];
}

static int ty(int y, int h, int font) { return y + (h - font_m[font].line) / 2; }

static int button(int x, int y, const char *label, int act, i64 arg, bool primary, const char *tip)
{
	int h = PX(24), w = font_width(FONT_UI, label, strlen(label)) + PX(20);
	bool hov = in_r(app.hover_x, app.hover_y, x, y, w, h);
	u32 bg = primary ? (hov ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG)) : hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG);
	draw_rect(x, y, w, h, bg);
	if (!primary)
		draw_frame(x, y, w, h, COL(C_INPUT_BORDER));
	draw_text(FONT_UI, x + PX(10), ty(y, h, FONT_UI), label, strlen(label), primary ? COL(C_BUTTON_FG) : COL(C_FG));
	hit(x, y, w, h, act, arg);
	if (tip)
		ui_tip(x, y, w, h, tip);
	return w;
}

/* a small toggle chip; returns its width */
static int chip(int x, int y, const char *label, bool on, int act, i64 arg, const char *tip)
{
	int h = PX(22), w = font_width(FONT_SMALL, label, strlen(label)) + PX(16);
	bool hov = in_r(app.hover_x, app.hover_y, x, y, w, h);
	draw_rect(x, y, w, h, on ? COL(C_LIST_ACTIVE_BG) : hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
	draw_frame(x, y, w, h, on ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
	draw_text(FONT_SMALL, x + PX(8), ty(y, h, FONT_SMALL), label, strlen(label),
	          on ? COL(C_LIST_ACTIVE_FG) : COL(C_FG));
	hit(x, y, w, h, act, arg);
	if (tip)
		ui_tip(x, y, w, h, tip);
	return w;
}

static int section(int x, int y, const char *t)
{
	draw_text(FONT_SMALL_BOLD, x, y, t, strlen(t), COL(C_SECTION_HEADER_FG));
	return y + font_m[FONT_SMALL_BOLD].line + PX(6);
}

/* "14:05", or "Mon 14:05" this week, or "2026-11-02" */
static void when(i64 ms, char *out, size_t cap)
{
	time_t t = (time_t)(ms / 1000), now = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	if (now - t < 20 * 3600)
		strftime(out, cap, "%H:%M", &tm);
	else if (now - t < 6 * 86400)
		strftime(out, cap, "%a %H:%M", &tm);
	else
		strftime(out, cap, "%Y-%m-%d", &tm);
}

/* ---- the chats column ------------------------------------------------------------------------ */

static const char *const backend_names[3] = {"Auto", "Server", "Mock"};
static const char *const backend_tips[3] = {
    "The model server when it answers, else the mock model",
    "Only the model server: an error when it does not answer",
    "Only the mock model: it calls the tools and writes up their answers, no language model",
};

static void draw_chats(int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_SIDEBAR_BG));
	int pad = PX(14), cx = x + pad, cw = w - 2 * pad;
	int cy = y + PX(12);
	int bw = button(cx, cy, "New Chat", H_NEW, 0, true, "Start a new chat about this project");
	(void)bw;
	cy += PX(24) + PX(14);
	cy = section(cx, cy, "CHATS");
	/* the harness panel at the bottom; the list takes what is left */
	int ih = PX(26);
	int panel_h = font_m[FONT_SMALL_BOLD].line + PX(6) + 2 * (font_m[FONT_SMALL].line + PX(2) + ih + PX(8)) + PX(22) +
	              PX(10) + PX(24) + PX(10) + font_m[FONT_SMALL].line * 2 + PX(16);
	int ly = cy, lh = VX_MAX(y + h - panel_h - PX(12) - ly, PX(40));
	int rh = font_m[FONT_UI].line + font_m[FONT_SMALL].line + PX(12);
	i64 maxs = VX_MAX((i64)S.nchats * rh - lh, 0);
	S.list_scroll = VX_CLAMP(S.list_scroll, 0, maxs);
	hit(x, ly, w, lh, H_LIST, 0);
	draw_clip(x, ly, w, lh);
	for (u32 i = 0; i < S.nchats; i++) {
		int ry = ly + (int)((i64)i * rh - S.list_scroll);
		if (ry + rh < ly || ry > ly + lh)
			continue;
		LlmChat *c = S.chats[i];
		bool cur = c == S.cur, hov = in_r(app.hover_x, app.hover_y, x, ry, w, rh) && in_r(app.hover_x, app.hover_y, x, ly, w, lh);
		if (cur)
			draw_rect(x, ry, w, rh, COL(C_LIST_ACTIVE_BG));
		else if (hov)
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		hit(x, ry, w, rh, H_CHAT, i);
		pthread_mutex_lock(&S.mu);
		const char *t = c->title.n ? c->title.b : "New chat";
		int tw = cw - (hov ? PX(22) : 0);
		draw_text_fit(c->title.n ? FONT_UI : FONT_UI_ITALIC, cx, ry + PX(5), t, strlen(t), tw,
		              cur ? COL(C_LIST_ACTIVE_FG) : COL(C_FG));
		char sub[200], wn[40];
		when(c->updated, wn, sizeof wn);
		u32 nq = 0;
		for (u32 k = 0; k < c->n; k++)
			nq += c->m[k].role == LLM_USER;
		bool live = S.running && c == S.run_chat;
		snprintf(sub, sizeof sub, "%s%s  \xC2\xB7  %u question%s%s%s", live ? "working  \xC2\xB7  " : "", wn, nq,
		         nq == 1 ? "" : "s", c->model[0] ? "  \xC2\xB7  " : "", c->model);
		pthread_mutex_unlock(&S.mu);
		draw_text_fit(FONT_SMALL, cx, ry + PX(5) + font_m[FONT_UI].line + PX(1), sub, strlen(sub), cw,
		              live ? COL(C_WARNING) : COL(C_DESCRIPTION_FG));
		if (hov) {
			int dx = x + w - pad - PX(16), dy = ry + (rh - PX(16)) / 2;
			bool dh = in_r(app.hover_x, app.hover_y, dx, dy, PX(16), PX(16));
			if (dh)
				draw_rect(dx, dy, PX(16), PX(16), COL(C_HOVER_BG));
			draw_icon(ICON_CLOSE, dx, dy, COL(C_FG));
			hit(dx, dy, PX(16), PX(16), H_CHAT_DEL, i);
			ui_tip(dx, dy, PX(16), PX(16), "Delete this chat");
		}
	}
	draw_noclip();
	if (maxs > 0) {
		int th = VX_MAX((int)((i64)lh * lh / ((i64)S.nchats * rh)), PX(20));
		int tyy = ly + (int)((i64)(lh - th) * S.list_scroll / maxs);
		draw_rect(x + w - PX(5), tyy, PX(4), th, COL(C_SCROLL_SLIDER));
	}
	/* the harness */
	cy = y + h - panel_h;
	draw_rect(x, cy - PX(10), w, PX(1), COL(C_SIDEBAR_BORDER));
	cy = section(cx, cy, "HARNESS: MCP-FBSD (BUILT IN)");
	draw_text(FONT_SMALL, cx, cy, "Model server", 12, COL(C_DESCRIPTION_FG));
	cy += font_m[FONT_SMALL].line + PX(2);
	if (S.field != 1)
		input_set(&S.srv_in, server_url(), strlen(server_url()));
	input_draw_ex(&S.srv_in, cx, cy, cw, 0, DEFAULT_SERVER, app.focus == FOCUS_MAN_INPUT && S.field == 1);
	hit(cx, cy, cw, ih, H_SRV, 0);
	ui_tip(cx, cy, cw, ih, "An OpenAI-compatible server, e.g. llama.cpp's llama-server (tool calls need --jinja). Enter applies.");
	cy += ih + PX(8);
	draw_text(FONT_SMALL, cx, cy, "Model", 5, COL(C_DESCRIPTION_FG));
	cy += font_m[FONT_SMALL].line + PX(2);
	if (S.field != 2)
		input_set(&S.model_in, app.llm_model, strlen(app.llm_model));
	input_draw_ex(&S.model_in, cx, cy, cw, 0, "the server's own", app.focus == FOCUS_MAN_INPUT && S.field == 2);
	hit(cx, cy, cw, ih, H_MODEL, 0);
	ui_tip(cx, cy, cw, ih, "Sent as \"model\"; empty: the model the server was started with. Enter applies.");
	cy += ih + PX(8);
	int bx = cx, be = backend();
	for (int i = 0; i < 3; i++)
		bx += chip(bx, cy, backend_names[i], be == i, H_BACKEND, i, backend_tips[i]) + PX(4);
	bx += PX(6);
	chip(bx, cy, "Read-only", app.llm_read_only, H_READONLY, 0,
	     "Only read, ls, stat and search: the agent cannot change files or run commands");
	cy += PX(22) + PX(10);
	button(cx, cy, "System Prompt...", H_SYSTEM, 0, false, "Edit the system prompt (a Markdown file; {root} is the project)");
	cy += PX(24) + PX(10);
	const char *tl = app.llm_read_only ? "Tools: read ls stat search" : "Tools: read write edit ls stat mv mkdir search exec";
	draw_text_fit(FONT_SMALL, cx, cy, tl, strlen(tl), cw, COL(C_DESCRIPTION_FG));
	cy += font_m[FONT_SMALL].line;
	const char *rt = app.root ? app.root : "(no folder open)";
	draw_text_fit(FONT_SMALL, cx, cy, rt, strlen(rt), cw, COL(C_DESCRIPTION_FG));
	ui_tip(cx, cy, cw, font_m[FONT_SMALL].line, "Every path the tools take is inside this folder");
}

/* ---- the transcript ------------------------------------------------------------------------------ */

static UiMsg *ui_of(LlmMsg *m)
{
	if (!m->ui) {
		UiMsg *u = vx_alloc(sizeof *u);
		u->md = md_text_new();
		m->ui = u;
	}
	return m->ui;
}

/* the call a tool message answers */
static const LlmCall *call_of(const LlmChat *c, u32 i)
{
	const LlmMsg *t = &c->m[i];
	for (u32 k = i; k-- > 0;) {
		if (c->m[k].role == LLM_USER)
			break;
		for (u32 q = 0; q < c->m[k].ncalls; q++)
			if (t->call_id.b && c->m[k].calls[q].id.b && !strcmp(c->m[k].calls[q].id.b, t->call_id.b))
				return &c->m[k].calls[q];
	}
	return NULL;
}

static int mono_lines(const char *s, u32 n, u32 *total)
{
	u32 k = 0;
	for (u32 i = 0; i < n; i++)
		k += s[i] == '\n';
	if (n && s[n - 1] != '\n')
		k++;
	*total = k;
	return (int)VX_MIN(k, (u32)TOOL_LINES_MAX);
}

/* draw mono lines of s, at most max; returns the y after them */
static int mono_block(int x, int y, int w, const char *s, u32 n, int max, u32 col, bool paint)
{
	int lh = font_m[FONT_MONO].line;
	int k = 0;
	for (u32 i = 0; i < n && k < max; k++) {
		const char *e = memchr(s + i, '\n', n - i);
		u32 l = e ? (u32)(e - s) - i : n - i;
		if (paint)
			draw_text_fit(FONT_MONO, x, y + k * lh, s + i, VX_MIN(l, 400u), w, col);
		i += l + 1;
	}
	return y + k * lh;
}

static const char *const thinking_dots[4] = {"", ".", "..", "..."};

/* Lay out (and paint) message i at width w; returns its height. */
static int msg_layout(LlmChat *c, u32 i, int x, int y, int w, int clip_y, int clip_h, bool paint)
{
	LlmMsg *m = &c->m[i];
	UiMsg *u = ui_of(m);
	int cy = y;
	int sl = font_m[FONT_SMALL_BOLD].line;
	switch (m->role) {
	case LLM_USER: {
		char wn[40];
		when(m->t0, wn, sizeof wn);
		if (paint) {
			draw_text(FONT_SMALL_BOLD, x, cy, "You", 3, COL(C_LINK));
			draw_text(FONT_SMALL, x + font_width(FONT_SMALL_BOLD, "You", 3) + PX(8), cy, wn, strlen(wn),
			          COL(C_DESCRIPTION_FG));
		}
		cy += sl + PX(4);
		int pad = PX(10);
		int mh = md_text_layout(u->md, m->text.b ? m->text.b : "", m->text.n, w - 2 * pad);
		if (paint) {
			draw_rect(x, cy, w, mh + 2 * pad, COL(C_INPUT_BG));
			draw_rect(x, cy, PX(2), mh + 2 * pad, COL(C_LINK));
			md_text_draw(u->md, x + pad, cy + pad, clip_y, clip_h);
			Hit *hh = hit(x, cy, w, mh + 2 * pad, H_MD, i);
			if (hh)
				hh->ox = x + pad, hh->oy = cy + pad;
		}
		cy += mh + 2 * pad;
		break;
	}
	case LLM_ASSISTANT: {
		/* the label once a turn: after the question */
		bool first = i == 0 || c->m[i - 1].role == LLM_USER;
		if (first) {
			if (paint) {
				draw_icon(ICON_CHAT_S, x, cy, COL(C_FG));
				draw_text(FONT_SMALL_BOLD, x + PX(20), cy, "Agent", 5, COL(C_FG));
				if (c->model[0]) {
					int ax = x + PX(20) + font_width(FONT_SMALL_BOLD, "Agent", 5) + PX(8);
					draw_text(FONT_SMALL, ax, cy, c->model, strlen(c->model), COL(C_DESCRIPTION_FG));
				}
			}
			cy += sl + PX(6);
		}
		if (m->think.n) {
			bool open = m->ui_flags & MF_THINK;
			char lb[80];
			u32 words = 1;
			for (u32 k = 0; k < m->think.n; k++)
				words += m->think.b[k] == ' ';
			snprintf(lb, sizeof lb, "%s (%u words)", m->done || m->text.n || m->ncalls ? "Reasoning" : "Reasoning...", words);
			int lw = font_width(FONT_SMALL, lb, strlen(lb)) + PX(18);
			if (paint) {
				bool hov = in_r(app.hover_x, app.hover_y, x, cy, lw, sl);
				draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x - PX(2), cy + (sl - PX(16)) / 2, COL(C_DESCRIPTION_FG));
				draw_text(FONT_SMALL, x + PX(16), cy, lb, strlen(lb), hov ? COL(C_FG) : COL(C_DESCRIPTION_FG));
				hit(x, cy, lw, sl, H_THINK, i);
			}
			cy += sl + PX(4);
			if (open) {
				if (!u->think)
					u->think = md_text_new();
				int th = md_text_layout(u->think, m->think.b, m->think.n, w - PX(14));
				if (paint) {
					draw_rect(x + PX(3), cy, PX(2), th, COL(C_SECTION_HEADER_BORDER));
					md_text_draw(u->think, x + PX(14), cy, clip_y, clip_h);
				}
				cy += th + PX(6);
			}
		}
		if (m->text.n) {
			int mh = md_text_layout(u->md, m->text.b, m->text.n, w);
			if (paint) {
				md_text_draw(u->md, x, cy, clip_y, clip_h);
				Hit *hh = hit(x, cy, w, mh, H_MD, i);
				if (hh)
					hh->ox = x, hh->oy = cy;
				/* Copy, on hover */
				if (m->done && in_r(app.hover_x, app.hover_y, x, cy, w, mh)) {
					int cw = font_width(FONT_SMALL, "Copy", 4);
					int bx = x + w - cw - PX(4), by = cy;
					bool ch = in_r(app.hover_x, app.hover_y, bx - PX(4), by, cw + PX(8), sl + PX(4));
					draw_rect(bx - PX(4), by, cw + PX(8), sl + PX(4), ch ? COL(C_LIST_HOVER_BG) : COL(C_EDITOR_BG));
					draw_text(FONT_SMALL, bx, by + PX(2), "Copy", 4, ch ? COL(C_FG) : COL(C_LINK));
					hit(bx - PX(4), by, cw + PX(8), sl + PX(4), H_COPY, i);
					ui_tip(bx - PX(4), by, cw + PX(8), sl + PX(4), "Copy the answer as Markdown");
				}
			}
			cy += mh;
		} else if (!m->done && !m->think.n) {
			if (paint) {
				const char *d = thinking_dots[(SDL_GetTicks() / 300) % 4];
				char t[40];
				snprintf(t, sizeof t, "Thinking%s", d);
				draw_text(FONT_UI_ITALIC, x, cy, t, strlen(t), COL(C_DESCRIPTION_FG));
			}
			cy += font_m[FONT_UI].line;
		}
		break;
	}
	case LLM_TOOL: {
		if (!u->titled && m->name.n) {
			const LlmCall *k = call_of(c, i);
			llmt_title(m->name.b, k && k->args.b ? k->args.b : "", k ? k->args.n : 0, u->title, sizeof u->title);
			u->titled = true;
		}
		bool open = m->ui_flags & MF_OPEN;
		int rh = PX(24);
		if (paint) {
			bool hov = in_r(app.hover_x, app.hover_y, x, cy, w, rh);
			draw_rect(x, cy, w, rh, hov ? COL(C_LIST_HOVER_BG) : COL(C_PANEL_BG));
			draw_frame(x, cy, w, rh, COL(C_PANEL_BORDER));
			draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + PX(4), cy + (rh - PX(16)) / 2, COL(C_FG));
			/* the state: running, ok, failed */
			u32 sc = !m->done ? COL(C_WARNING) : m->error ? COL(C_ERROR) : COL(C_GIT_ADDED);
			draw_rect(x + PX(24), cy + rh / 2 - PX(3), PX(6), PX(6), sc);
			char right[64];
			if (!m->done)
				snprintf(right, sizeof right, "running%s", thinking_dots[(SDL_GetTicks() / 300) % 4]);
			else {
				u32 tot;
				mono_lines(m->text.b ? m->text.b : "", m->text.n, &tot);
				snprintf(right, sizeof right, "%s%u line%s  \xC2\xB7  %.1f s", m->error ? "failed  \xC2\xB7  " : "", tot,
				         tot == 1 ? "" : "s", (double)(m->t1 - m->t0) / 1000.0);
			}
			int rw = font_width(FONT_SMALL, right, strlen(right));
			draw_text(FONT_SMALL, x + w - rw - PX(10), ty(cy, rh, FONT_SMALL), right, strlen(right),
			          m->error ? COL(C_ERROR) : COL(C_DESCRIPTION_FG));
			draw_text_fit(FONT_MONO, x + PX(38), ty(cy, rh, FONT_MONO), u->title, strlen(u->title),
			              w - PX(58) - rw, COL(C_EDITOR_FG));
			hit(x, cy, w, rh, H_TOOL, i);
			ui_tip(x, cy, w, rh, open ? "Fold the call" : "Show the call's arguments and answer");
		}
		cy += rh;
		if (open) {
			const LlmCall *k = call_of(c, i);
			int pad = PX(8), lh = font_m[FONT_MONO].line;
			int bh = 0;
			u32 an = k ? k->args.n : 0, tot;
			int al = an ? VX_MIN(mono_lines(k->args.b, an, &tot), 12) : 0;
			int ol = mono_lines(m->text.b ? m->text.b : "", m->text.n, &tot);
			bh = pad + (al ? sl + al * lh + PX(6) : 0) + sl + ol * lh + (tot > (u32)ol ? sl : 0) + pad;
			if (paint) {
				draw_rect(x, cy, w, bh, COL(C_INPUT_BG));
				draw_frame(x, cy - PX(1), w, bh + PX(1), COL(C_PANEL_BORDER));
				int by = cy + pad;
				if (al) {
					draw_text(FONT_SMALL_BOLD, x + pad, by, "ARGUMENTS", 9, COL(C_SECTION_HEADER_FG));
					by = mono_block(x + pad, by + sl, w - 2 * pad, k->args.b, an, al, COL(C_EDITOR_FG), true) + PX(6);
				}
				draw_text(FONT_SMALL_BOLD, x + pad, by, m->error ? "FAILED" : "ANSWER", 6,
				          m->error ? COL(C_ERROR) : COL(C_SECTION_HEADER_FG));
				by = mono_block(x + pad, by + sl, w - 2 * pad, m->text.b ? m->text.b : "", m->text.n, ol, COL(C_EDITOR_FG),
				                true);
				if (tot > (u32)ol) {
					char more[80];
					snprintf(more, sizeof more, "... %u more lines (the model has all of them)", tot - (u32)ol);
					draw_text(FONT_SMALL, x + pad, by, more, strlen(more), COL(C_DESCRIPTION_FG));
				}
			}
			cy += bh;
		}
		break;
	}
	default: { /* a note */
		u32 col = m->error ? COL(C_ERROR) : COL(C_DESCRIPTION_FG);
		if (paint) {
			draw_icon(m->error ? ICON_ERROR : ICON_INFO, x, cy + (font_m[FONT_UI].line - PX(16)) / 2, col);
			draw_text_fit(FONT_UI, x + PX(22), cy, m->text.b ? m->text.b : "", m->text.n, w - PX(22), col);
			ui_tip(x, cy, w, font_m[FONT_UI].line, m->text.b ? m->text.b : "");
		}
		cy += font_m[FONT_UI].line;
	}
	}
	return cy - y;
}

static int gap_after(const LlmChat *c, u32 i)
{
	if (i + 1 >= c->n)
		return 0;
	int next = c->m[i + 1].role;
	/* a turn's pieces sit close; a new question is set apart */
	if (next == LLM_USER)
		return PX(28);
	if (c->m[i].role == LLM_TOOL && next == LLM_TOOL)
		return PX(4);
	return PX(10);
}

static const char *const examples[] = {
    "What is this project?",
    "Where is main defined?",
    "What does CMakeLists.txt do?",
    "Run `git status --short` and tell me what changed",
};

static void draw_welcome(int x, int y, int w)
{
	int cy = y + PX(8);
	draw_text(FONT_H2, x, cy, "Ask about this project", 22, COL(C_FG));
	cy += font_m[FONT_H2].line + PX(8);
	const char *p[] = {
	    "The agent reads, searches, edits and builds the open folder through its tools, the ones of mcp-fbsd built in: "
	    "read, write, edit, ls, stat, mv, mkdir, search and exec. Nothing outside the folder can be reached.",
	    "It talks to a local model server (llama.cpp's llama-server at 127.0.0.1:8080 unless you set another on the "
	    "left). Until one answers, a mock model stands in: it calls the tools for real and writes up what they say.",
	};
	for (size_t i = 0; i < VX_COUNT(p); i++) {
		/* wrapped by hand: two or three lines each */
		const char *s = p[i];
		size_t n = strlen(s);
		while (n) {
			size_t k = n;
			while (k && font_width(FONT_UI, s, k) > w)
				k--;
			if (k < n) {
				size_t b = k;
				while (b && s[b] != ' ')
					b--;
				if (b)
					k = b;
			}
			draw_text(FONT_UI, x, cy, s, k, COL(C_DESCRIPTION_FG));
			cy += font_m[FONT_UI].line + PX(2);
			s += k;
			n -= k;
			while (n && *s == ' ')
				s++, n--;
		}
		cy += PX(8);
	}
	cy += PX(6);
	cy = section(x, cy, "TRY");
	for (size_t i = 0; i < VX_COUNT(examples); i++) {
		int h = font_m[FONT_UI].line + PX(8);
		int tw = font_width(FONT_UI, examples[i], strlen(examples[i])) + PX(20);
		bool hov = in_r(app.hover_x, app.hover_y, x, cy, tw, h);
		draw_rect(x, cy, tw, h, hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
		draw_frame(x, cy, tw, h, COL(C_INPUT_BORDER));
		draw_text(FONT_UI, x + PX(10), cy + PX(4), examples[i], strlen(examples[i]), COL(C_FG));
		hit(x, cy, tw, h, H_EXAMPLE, (i64)i);
		cy += h + PX(6);
	}
}

static struct {
	int tr_x, tr_y, tr_w, tr_h;
	int in_x, in_y, in_w, in_reserve;
	int srv_x, srv_y, srv_w;
	int mdl_x, mdl_y, mdl_w;
	int list_x, list_y, list_w, list_h;
	i64 total;
} G;

static void draw_transcript(int x, int y, int w, int h)
{
	G.tr_x = x;
	G.tr_y = y;
	G.tr_w = w;
	G.tr_h = h;
	hit(x, y, w, h, H_TRANSCRIPT, 0);
	LlmChat *c = S.cur;
	if (!c || !c->n) {
		draw_clip(x, y, w, h);
		draw_welcome(x, y + PX(8), VX_MIN(w, PX(900)));
		draw_noclip();
		return;
	}
	pthread_mutex_lock(&S.mu);
	int top = PX(16);
	i64 total = top;
	for (u32 i = 0; i < c->n; i++)
		total += msg_layout(c, i, x, 0, w, 0, 0, false) + gap_after(c, i);
	total += PX(16);
	G.total = total;
	i64 maxs = VX_MAX(total - h, 0);
	if (S.stick)
		S.scroll = maxs;
	S.scroll = VX_CLAMP(S.scroll, 0, maxs);
	draw_clip(x, y, w + PX(10), h);
	i64 cy = top - S.scroll;
	for (u32 i = 0; i < c->n; i++) {
		int mh = msg_layout(c, i, x, 0, w, 0, 0, false);
		if (cy + mh > 0 && cy < h)
			msg_layout(c, i, x, y + (int)cy, w, y, h, true);
		cy += mh + gap_after(c, i);
	}
	pthread_mutex_unlock(&S.mu);
	draw_noclip();
	if (total > h) {
		int bh = VX_MAX((int)((i64)h * h / total), PX(20));
		int by = y + (int)((h - bh) * S.scroll / VX_MAX(maxs, 1));
		draw_rect(x + w + PX(6), by, PX(4), bh, COL(C_SCROLL_SLIDER));
	}
}

void llmview_open(void)
{
	if (!S.inited) {
		S.inited = true;
		pthread_mutex_init(&S.mu, NULL);
		S.last_used = -1;
		S.stick = true;
		load_chats();
		if (S.nchats && !S.chats[0]->n)
			S.cur = S.chats[0];
		/* a new chat to type into; the old ones are a click away */
		new_chat();
	}
	S.field = 0;
	app.focus = FOCUS_MAN_INPUT;
	app.dirty = true;
}

static void status_text(char *st, size_t cap, u32 *tint)
{
	int be = backend();
	*tint = COL(C_GIT_ADDED);
	if (S.running) {
		snprintf(st, cap, "working  \xC2\xB7  %s", S.run.used == LLM_SERVER ? "model server" : "mock model");
		*tint = COL(C_WARNING);
	} else if (be == LLM_MOCK) {
		snprintf(st, cap, "mock model");
		*tint = COL(C_INFO);
	} else if (S.last_used == LLM_MOCK && S.why_mock[0]) {
		snprintf(st, cap, "mock model  \xC2\xB7  no answer from %s", S.why_mock);
		*tint = COL(C_WARNING);
	} else if (S.last_used == LLM_SERVER)
		snprintf(st, cap, "model server  \xC2\xB7  %s", server_url());
	else
		snprintf(st, cap, "%s  \xC2\xB7  %s", be == LLM_SERVER ? "model server" : "auto", server_url());
}

void llmview_ws_draw(int x, int y, int w, int h)
{
	if (!S.inited)
		llmview_open();
	nhit = 0;
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	/* header */
	int hh = PX(48);
	draw_icon(ICON_CHAT_S, x + PX(16), y + (hh - PX(16)) / 2, COL(C_FG));
	draw_text(FONT_UI_BOLD, x + PX(40), y + PX(7), "LLM agent", 9, COL(C_FG));
	const char *sub = "A local model working on this project through the built-in mcp-fbsd tools";
	draw_text_fit(FONT_SMALL, x + PX(40), y + PX(7) + font_m[FONT_UI_BOLD].line, sub, strlen(sub), w / 2,
	              COL(C_DESCRIPTION_FG));
	{
		char st[400];
		u32 tint;
		status_text(st, sizeof st, &tint);
		int rx = x + w - PX(16);
		if (S.running)
			rx -= button(rx - font_width(FONT_UI, "Stop", 4) - PX(20), y + (hh - PX(24)) / 2, "Stop", H_STOP, 0, false,
			             "Stop the agent (Esc)") + PX(10);
		int sw = VX_MIN(font_width(FONT_SMALL, st, strlen(st)), w / 2);
		int sy = y + (hh - PX(22)) / 2;
		draw_rect(rx - sw - PX(16), sy, sw + PX(16), PX(22), (tint & 0xFFFFFF00u) | 0x40u);
		draw_text_fit(FONT_SMALL, rx - sw - PX(8), ty(sy, PX(22), FONT_SMALL), st, strlen(st), sw, COL(C_FG));
		ui_tip(rx - sw - PX(16), sy, sw + PX(16), PX(22), st);
	}
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	y += hh;
	h -= hh;
	/* the chats on the left; the transcript takes all the rest */
	int lw = VX_CLAMP(w / 6, PX(220), PX(300));
	draw_chats(x, y, lw, h);
	draw_rect(x + lw, y, PX(1), h, COL(C_SIDEBAR_BORDER));
	int pad = PX(24);
	int tx = x + lw + pad, tw = w - lw - 2 * pad;
	/* the question box at the bottom, with Send / Stop on its right */
	const char *bl = S.running ? "Stop" : "Send";
	int bw = font_width(FONT_UI, bl, strlen(bl)) + PX(20);
	int iw = tw - bw - PX(8);
	int ih = VX_MIN(input_height(&S.in, iw, 0), h / 2);
	int iy = y + h - PX(16) - ih;
	draw_transcript(tx, y, tw - PX(12), iy - PX(12) - y);
	draw_rect(x + lw + PX(1), iy - PX(12), w - lw - PX(1), PX(1), COL(C_PANEL_BORDER));
	const char *ph = S.running ? "The agent is working (Esc stops it)"
	                           : "Ask the agent about this project (Enter sends, Shift+Enter starts a new line)";
	draw_clip(tx, iy, iw, ih);
	input_draw_ex(&S.in, tx, iy, iw, 0, ph, app.focus == FOCUS_MAN_INPUT && S.field == 0);
	draw_noclip();
	hit(tx, iy, iw, ih, H_INPUT, 0);
	G.in_x = tx;
	G.in_y = iy;
	G.in_w = iw;
	button(tx + iw + PX(8), iy + ih - PX(24) - (ih > PX(26) ? 0 : PX(1)), bl, S.running ? H_STOP : H_SEND, 0, !S.running,
	       S.running ? "Stop the agent (Esc)" : "Send (Enter)");
}

/* ---- input ----------------------------------------------------------------------------------------- */

static Input *field_in(void) { return S.field == 1 ? &S.srv_in : S.field == 2 ? &S.model_in : &S.in; }

/* insert at the caret over the selection, newlines kept */
static void put_text(Input *in, const char *s, size_t n)
{
	if (!in->b)
		input_set(in, "", 0);
	u32 a = VX_MIN(in->cur, in->anchor), b = VX_MAX(in->cur, in->anchor);
	char *t = vx_alloc(in->n - (b - a) + n * 4 + 1);
	memcpy(t, in->b, a);
	u32 k = a;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\t') {
			for (int j = 0; j < 4; j++)
				t[k++] = ' ';
		} else if (s[i] != '\r' && (s[i] != '\n' || in == &S.in))
			t[k++] = s[i];
	u32 at = k;
	memcpy(t + k, in->b + b, in->n - b);
	k += in->n - b;
	input_set(in, t, k);
	in->cur = in->anchor = at;
	free(t);
}

/* the server or model box: applied and saved */
static void apply_field(void)
{
	if (S.field == 1) {
		char v[256];
		snprintf(v, sizeof v, "%.*s", (int)S.srv_in.n, S.srv_in.b ? S.srv_in.b : "");
		size_t l = strlen(v);
		while (l && v[l - 1] == ' ')
			v[--l] = 0;
		snprintf(app.llm_server, sizeof app.llm_server, "%s", strcmp(v, DEFAULT_SERVER) ? v : "");
		settings_save();
		ui_status("The model server: %s", server_url());
	} else if (S.field == 2) {
		snprintf(app.llm_model, sizeof app.llm_model, "%.*s", (int)S.model_in.n, S.model_in.b ? S.model_in.b : "");
		settings_save();
		ui_status("The model: %s", app.llm_model[0] ? app.llm_model : "the server's own");
	}
	S.field = 0;
}

static void open_link(const char *url)
{
	if (strstr(url, "://") || !strncmp(url, "mailto:", 7)) {
		if (!app.headless && !SDL_OpenURL(url))
			ui_status("Cannot open %s", url);
		return;
	}
	/* a file of the project, path or path:line */
	char rel[2048];
	snprintf(rel, sizeof rel, "%s", url);
	u32 line = UINT32_MAX;
	char *colon = strrchr(rel, ':');
	if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
		line = (u32)atoi(colon + 1);
		line = line ? line - 1 : 0;
		*colon = 0;
	}
	char *hash = strchr(rel, '#');
	if (hash)
		*hash = 0;
	char path[4096];
	if (rel[0] == '/')
		snprintf(path, sizeof path, "%s", rel);
	else
		snprintf(path, sizeof path, "%s/%s", app.root ? app.root : ".", rel);
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		ui_status("Not found: %s", rel);
		return;
	}
	app.side_view = SIDE_EXPLORER;
	open_file(path, false, line, 0);
}

static void do_hit(const Hit *h, const Event *e)
{
	switch (h->act) {
	case H_NEW: new_chat(); break;
	case H_CHAT:
		if ((u64)h->arg < S.nchats && S.cur != S.chats[h->arg]) {
			/* an empty chat left behind is dropped */
			LlmChat *was = S.cur;
			S.cur = S.chats[h->arg];
			if (was && !was->n && was != S.run_chat)
				for (u32 i = 0; i < S.nchats; i++)
					if (S.chats[i] == was) {
						memmove(S.chats + i, S.chats + i + 1, (S.nchats - i - 1) * sizeof *S.chats);
						S.nchats--;
						chat_free(was);
						break;
					}
			S.stick = true;
		}
		S.field = 0;
		app.focus = FOCUS_MAN_INPUT;
		break;
	case H_CHAT_DEL: delete_chat((u32)h->arg); break;
	case H_STOP: stop(); break;
	case H_SEND: {
		char *t = vx_strndup(S.in.b ? S.in.b : "", S.in.n);
		size_t n = S.in.n;
		if (n && !S.running) {
			input_set(&S.in, "", 0);
			ask(t, n);
		}
		free(t);
		break;
	}
	case H_SYSTEM: edit_system_prompt(); break;
	case H_BACKEND:
		app.llm_backend = (int)h->arg;
		settings_save();
		if (getenv("VXIDE_LLM_BACKEND"))
			ui_status("VXIDE_LLM_BACKEND is set: it decides");
		break;
	case H_READONLY:
		app.llm_read_only = !app.llm_read_only;
		settings_save();
		ui_status(app.llm_read_only ? "Read-only: the agent can read and search, not change or run"
		                            : "The agent can change files and run commands in the project");
		break;
	case H_SRV:
	case H_MODEL:
		if (S.field && S.field != (h->act == H_SRV ? 1 : 2))
			apply_field();
		S.field = h->act == H_SRV ? 1 : 2;
		app.focus = FOCUS_MAN_INPUT;
		input_click_ex(field_in(), h->x, h->y, h->w, 0, e->x, e->y, e->clicks);
		break;
	case H_INPUT:
		if (S.field)
			apply_field();
		S.field = 0;
		app.focus = FOCUS_MAN_INPUT;
		input_click_ex(&S.in, G.in_x, G.in_y, G.in_w, 0, e->x, e->y, e->clicks);
		break;
	case H_TOOL:
	case H_THINK:
		if (S.cur && (u64)h->arg < S.cur->n) {
			pthread_mutex_lock(&S.mu);
			S.cur->m[h->arg].ui_flags ^= h->act == H_TOOL ? MF_OPEN : MF_THINK;
			pthread_mutex_unlock(&S.mu);
			S.stick = false;
		}
		break;
	case H_COPY:
		if (S.cur && (u64)h->arg < S.cur->n) {
			pthread_mutex_lock(&S.mu);
			const JBuf *t = &S.cur->m[h->arg].text;
			ui_clipboard_set(t->b ? t->b : "", t->n);
			pthread_mutex_unlock(&S.mu);
			ui_status("Copied the answer (Markdown)");
		}
		break;
	case H_MD:
		if (S.cur && (u64)h->arg < S.cur->n) {
			pthread_mutex_lock(&S.mu);
			UiMsg *u = S.cur->m[h->arg].ui;
			const char *url = u ? md_text_link_at(u->md, h->ox, h->oy, e->x, e->y) : NULL;
			char *copy = url ? vx_strdup(url) : NULL;
			pthread_mutex_unlock(&S.mu);
			if (copy) {
				open_link(copy);
				free(copy);
			}
		}
		break;
	case H_EXAMPLE: ask(examples[h->arg], strlen(examples[h->arg])); break;
	default: break;
	}
}

static void scroll_by(i64 px)
{
	S.scroll += px;
	S.stick = S.scroll >= G.total - G.tr_h;
}

void llmview_ws_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	if (!S.inited)
		llmview_open();
	app.dirty = true;
	if (e->type == EV_TEXT) {
		app.focus = FOCUS_MAN_INPUT;
		put_text(field_in(), e->text, strlen(e->text));
		return;
	}
	if (e->type == EV_KEY) {
		int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
		bool enter = e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER;
		app.focus = FOCUS_MAN_INPUT;
		if (enter && S.field) {
			apply_field();
			return;
		}
		if (enter && mods == MOD_SHIFT) {
			put_text(&S.in, "\n", 1);
			return;
		}
		if (enter && (mods == 0 || mods == MOD_CTRL)) {
			Hit send = {0};
			send.act = H_SEND;
			do_hit(&send, e);
			return;
		}
		if (e->key == SDLK_ESCAPE) {
			if (S.field)
				S.field = 0; /* the edit is dropped */
			else if (S.running)
				stop();
			else if (S.in.n)
				input_set(&S.in, "", 0);
			return;
		}
		if (e->key == SDLK_PAGEUP || e->key == SDLK_PAGEDOWN) {
			scroll_by((e->key == SDLK_PAGEUP ? -1 : 1) * (i64)VX_MAX(G.tr_h - PX(40), PX(40)));
			return;
		}
		if (mods == MOD_CTRL && (e->key == SDLK_HOME || e->key == SDLK_END)) {
			S.scroll = e->key == SDLK_HOME ? 0 : G.total;
			S.stick = e->key == SDLK_END;
			return;
		}
		/* Up in an empty box: the last question, to edit and ask again */
		if (e->key == SDLK_UP && mods == 0 && !S.field && !S.in.n && S.cur) {
			pthread_mutex_lock(&S.mu);
			for (u32 i = S.cur->n; i-- > 0;)
				if (S.cur->m[i].role == LLM_USER) {
					input_set(&S.in, S.cur->m[i].text.b, S.cur->m[i].text.n);
					S.in.cur = S.in.anchor = S.in.n;
					break;
				}
			pthread_mutex_unlock(&S.mu);
			return;
		}
		if (e->key == SDLK_TAB)
			return;
		if (mods == MOD_CTRL && e->key == SDLK_V) {
			char *c = ui_clipboard_get();
			put_text(field_in(), c, strlen(c));
			free(c);
			return;
		}
		input_key(field_in(), e);
		return;
	}
	if (e->type == EV_WHEEL) {
		int d = (int)(e->wy * 3) * font_m[FONT_UI].line;
		if (e->x < G.tr_x - PX(24))
			S.list_scroll -= d;
		else
			scroll_by(-d);
		return;
	}
	if (e->type == EV_MMOVE) {
		for (int i = nhit; i-- > 0;)
			if (in_r(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
				const Hit *hh = &hits[i];
				if (hh->act == H_INPUT || hh->act == H_SRV || hh->act == H_MODEL)
					app.cursor = CURSOR_IBEAM;
				else if (hh->act == H_MD) {
					LlmChat *c = S.cur;
					pthread_mutex_lock(&S.mu);
					UiMsg *u = c && (u64)hh->arg < c->n ? c->m[hh->arg].ui : NULL;
					bool link = u && md_text_link_at(u->md, hh->ox, hh->oy, e->x, e->y);
					pthread_mutex_unlock(&S.mu);
					if (link)
						app.cursor = CURSOR_HAND;
				} else if (hh->act != H_TRANSCRIPT && hh->act != H_LIST)
					app.cursor = CURSOR_HAND;
				break;
			}
		return;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return;
	for (int i = nhit; i-- > 0;)
		if (in_r(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
			Hit hh = hits[i]; /* do_hit may draw nothing but can change the list */
			do_hit(&hh, e);
			return;
		}
	app.focus = FOCUS_MAN_INPUT;
}

/* ---- commands ------------------------------------------------------------------------------------- */

bool llmview_command(int cmd)
{
	switch (cmd) {
	case CMD_LLM_NEW:
		app.side_view = SIDE_LLM;
		llmview_open();
		new_chat();
		return true;
	case CMD_LLM_STOP: stop(); return true;
	case CMD_LLM_SYSTEM: edit_system_prompt(); return true;
	case CMD_LLM_ASK_SELECTION: {
		View *v = active_view();
		if (!v || ui_workspace()) {
			ui_status("Select some code in an editor first");
			return true;
		}
		size_t n = 0;
		char *s = ed_selection_text(v, &n);
		const char *p = app.docs.path[v->doc];
		const char *rel = p && app.root && !strncmp(p, app.root, strlen(app.root)) ? p + strlen(app.root) : p;
		while (rel && *rel == '/')
			rel++;
		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			n--;
		app.side_view = SIDE_LLM;
		llmview_open();
		JBuf b = {0};
		jb_raw(&b, "From `");
		jb_raw(&b, rel ? rel : "untitled");
		jb_raw(&b, "`:\n\n```\n");
		jb_rawn(&b, s, n);
		jb_raw(&b, "\n```\n\n");
		S.field = 0;
		put_text(&S.in, b.b, b.n);
		jb_free(&b);
		free(s);
		return true;
	}
	}
	return false;
}
