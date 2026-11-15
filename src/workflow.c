/* Workflow side bar: build with CMake, run, and debug over DAP.
 *
 *   CMAKE        presets (or a build folder), Configure / Build, the cache
 *                options (booleans toggle, others edit; advanced on demand)
 *   RUN          an executable target (CMake File API) or any program, its
 *                arguments; Run (Ctrl+F5) or Debug (F5), building first
 *   while debugging: a toolbar (continue, pause, step over / into / out,
 *   restart, stop), CALL STACK (threads, frames), VARIABLES (scopes such as
 *   Locals and Registers, expandable, changed values marked), WATCH
 *   (expressions re-evaluated at every stop), WATCHPOINTS (hardware: break
 *   on write / read / both; software too with gdb), BREAKPOINTS.
 * In the editor: breakpoints in the gutter (click, F9), the execution line,
 * values under the pointer in the hover next to the language server's.
 * Builds and programs run as processes whose output goes to Output. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "cmake.h"
#include "dap.h"
#include "prof.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <SDL3/SDL.h>

extern char **environ;

#define NONE UINT32_MAX
enum { PICK_WF_PRESET = 420, PICK_WF_TARGET };
enum { PR_WF_OPTION = 270, PR_WF_WATCH, PR_WF_WATCHPOINT, PR_WF_WATCHPOINT_SW, PR_WF_CONDITION };
enum { S_CMAKE, S_RUN, S_STACK, S_VARS, S_WATCH, S_WPTS, S_BPS, S_COUNT };
enum { F_NONE, F_PROG, F_ARGS };
enum { J_NONE, J_CONFIGURE, J_BUILD, J_RUN };
enum { NX_NONE, NX_BUILD, NX_DEBUG, NX_RUN };

typedef struct {
	pid_t pid;
	int fd;
	int kind, next;
	char tag[16];
	char line[4096];
	u32 ln;
	u64 t0;
} Job;

#define BP_COLS SOA(W.bp_path), SOA(W.bp_line), SOA(W.bp_cond), SOA(W.bp_on)
#define WX_COLS SOA(W.w_expr), SOA(W.w_ev), SOA(W.w_last), SOA(W.w_chg)

static struct {
	bool inited;
	CMakeProj cm;
	u32 preset;
	char bdir[1024];
	u64 cm_mtime;
	char defs[64][320];
	u32 ndefs;
	bool show_adv, opts_open;
	u32 target; /* a target index, NONE: the program typed in */
	char want_preset[128], want_target[128];
	Input prog, args;
	int field;
	bool stop_entry, build_first, flags_loaded;
	Job job; /* configure / build / run */
	Dap *dap;
	u32 seen_stops;
	char last_cmdline[1200];
	/* breakpoints */
	u32 nbp, bpcap;
	u32 *bp_path, *bp_line, *bp_cond;
	u8 *bp_on;
	Pool bpool;
	/* watch expressions */
	u32 nw, wcap;
	u32 *w_expr;
	i32 *w_ev;
	u64 *w_last;
	u8 *w_chg;
	Pool wpool;
	/* expanded tree rows, by path ("Locals/p/x") */
	Pool open_pool;
	u32 *open, nopen, opencap;
	bool collapsed[S_COUNT];
	i32 scroll;
	int content_h;
	/* hover */
	u32 hv_doc, hv_off, hv_stops;
	i32 hv_ev;
	char hv_expr[256];
	/* context menus */
	char ctx_expr[512], ctx_value[1024];
	u32 ctx_bp;
	bool reveal; /* show the selected thread's frame once its stack is in */
} W;

/* ---- small helpers ------------------------------------------------------------------------- */

static void wf_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wf_status(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	ui_status("%s", b);
}

static void show_output(void)
{
	app.panel_visible = true;
	app.panel_view = PANEL_OUTPUT;
	ui_layout();
}

static char *canon(const char *p, char *out, size_t cap)
{
	char b[4096];
	snprintf(out, cap, "%s", realpath(p, b) ? b : p);
	return out;
}

static const char *base_of(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* Split like a shell: spaces, '...', "...", backslash escapes. */
static u32 split_args(const char *s, u32 n, char **out, u32 max, char *buf)
{
	u32 k = 0, b = 0, i = 0;
	while (i < n && k < max) {
		while (i < n && (s[i] == ' ' || s[i] == '\t'))
			i++;
		if (i >= n)
			break;
		out[k++] = buf + b;
		char q = 0;
		while (i < n && (q || (s[i] != ' ' && s[i] != '\t'))) {
			char ch = s[i++];
			if (!q && (ch == '\'' || ch == '"'))
				q = ch;
			else if (q && ch == q)
				q = 0;
			else if (ch == '\\' && q != '\'' && i < n)
				buf[b++] = s[i++];
			else
				buf[b++] = ch;
		}
		buf[b++] = 0;
	}
	return k;
}

/* ---- CMake ------------------------------------------------------------------------------- */

static u64 lists_mtime(void)
{
	char p[1200];
	struct stat st;
	u64 t = 0;
	const char *f[] = {"CMakeLists.txt", "CMakePresets.json", "CMakeUserPresets.json"};
	for (size_t i = 0; i < 3; i++) {
		snprintf(p, sizeof p, "%s/%s", app.root ? app.root : ".", f[i]);
		if (stat(p, &st) == 0)
			t += (u64)st.st_mtime;
	}
	return t;
}

static void cm_refresh(void)
{
	cmake_build_dir(&W.cm, W.preset, W.bdir, sizeof W.bdir);
	cmake_read_cache(&W.cm, W.bdir);
	cmake_read_targets(&W.cm, W.bdir);
	/* keep the chosen target by name */
	u32 ex[64];
	u32 nx = cmake_executables(&W.cm, ex, 64);
	if (W.want_target[0]) {
		W.target = NONE;
		for (u32 i = 0; i < nx; i++)
			if (!strcmp(cm_str(&W.cm, W.cm.tgt_name[ex[i]]), W.want_target))
				W.target = ex[i];
		if (!strcmp(W.want_target, "*custom*"))
			W.target = NONE;
	} else
		W.target = nx ? ex[0] : NONE;
	if (W.target != NONE)
		snprintf(W.want_target, sizeof W.want_target, "%s", cm_str(&W.cm, W.cm.tgt_name[W.target]));
}

static void cm_load(void)
{
	cmake_load(&W.cm, app.root ? app.root : ".");
	W.cm_mtime = lists_mtime();
	W.preset = NONE;
	for (u32 i = 0; i < W.cm.npre; i++)
		if (!W.want_preset[0] || !strcmp(cm_str(&W.cm, W.cm.pre_name[i]), W.want_preset)) {
			W.preset = i;
			break;
		}
	if (W.preset == NONE && W.cm.npre)
		W.preset = 0;
	if (W.preset != NONE)
		snprintf(W.want_preset, sizeof W.want_preset, "%s", cm_str(&W.cm, W.cm.pre_name[W.preset]));
	cm_refresh();
}

static void wf_init(void)
{
	if (W.inited)
		return;
	W.inited = true;
	if (!W.flags_loaded)
		W.build_first = true; /* the default, unless the session said otherwise */
	W.ctx_bp = NONE;
	W.hv_ev = -1;
	W.collapsed[S_WATCH] = W.collapsed[S_WPTS] = false;
	pool_add(&W.open_pool, "", 0);
	/* scopes that start open */
	const char *def[] = {"Locals", "Local", "Arguments"};
	for (size_t i = 0; i < VX_COUNT(def); i++) {
		VX_GROW(W.open, W.opencap, W.nopen + 1);
		W.open[W.nopen++] = pool_add(&W.open_pool, def[i], (u32)strlen(def[i]));
	}
	cm_load();
}

/* ---- jobs: configure, build, run ------------------------------------------------------------ */

static void job_line(Job *j, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n' || j->ln >= sizeof j->line - 1) {
			j->line[j->ln] = 0;
			ui_log("[%s] %s", j->tag, j->line);
			j->ln = 0;
			if (s[i] == '\n')
				continue;
		}
		if (s[i] != '\r')
			j->line[j->ln++] = s[i];
	}
}

static bool job_start(int kind, int next, char *const argv[], const char *tag)
{
	if (W.job.kind != J_NONE) {
		wf_status("Something is already running (Shift+F5 stops it)");
		return false;
	}
	int po[2];
	if (pipe(po) != 0)
		return false;
	char *full[200];
	int a = 0;
	full[a++] = "/bin/sh";
	full[a++] = "-c";
	full[a++] = "cd \"$1\" || exit 126; shift; exec \"$@\"";
	full[a++] = "sh";
	full[a++] = app.root ? app.root : ".";
	for (int i = 0; argv[i] && a < 198; i++)
		full[a++] = argv[i];
	full[a] = NULL;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_adddup2(&fa, po[1], 2);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0);
	int rc = posix_spawn(&W.job.pid, "/bin/sh", &fa, &at, full, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(po[1]);
	if (rc != 0) {
		close(po[0]);
		wf_status("Cannot start %s: %s", argv[0], strerror(rc));
		return false;
	}
	W.job.fd = po[0];
	fcntl(W.job.fd, F_SETFL, fcntl(W.job.fd, F_GETFL) | O_NONBLOCK);
	fcntl(W.job.fd, F_SETFD, FD_CLOEXEC);
	W.job.kind = kind;
	W.job.next = next;
	W.job.ln = 0;
	W.job.t0 = vx_now_ns();
	snprintf(W.job.tag, sizeof W.job.tag, "%s", tag);
	char cl[1200];
	u32 k = 0;
	for (int i = 0; argv[i] && k + 2 < sizeof cl; i++)
		k += (u32)snprintf(cl + k, sizeof cl - k, "%s%s", i ? " " : "", argv[i]);
	ui_log("[%s] $ %s", tag, cl);
	show_output();
	return true;
}

static void job_stop(void)
{
	if (W.job.kind != J_NONE && W.job.pid > 0)
		kill(-W.job.pid, SIGTERM);
}

/* ---- breakpoints ----------------------------------------------------------------------------- */

static u32 bp_find(const char *path, u32 line)
{
	for (u32 i = 0; i < W.nbp; i++)
		if (W.bp_line[i] == line && !strcmp(W.bpool.b + W.bp_path[i], path))
			return i;
	return NONE;
}

static void bp_send(const char *path)
{
	if (!W.dap)
		return;
	u32 lines[512];
	const char *cond[512];
	u32 n = 0;
	for (u32 i = 0; i < W.nbp && n < 512; i++)
		if (W.bp_on[i] && !strcmp(W.bpool.b + W.bp_path[i], path)) {
			lines[n] = W.bp_line[i];
			cond[n] = W.bpool.b + W.bp_cond[i];
			n++;
		}
	dap_set_breakpoints(W.dap, path, lines, cond, n);
}

static void bp_send_all(void)
{
	for (u32 i = 0; i < W.nbp; i++) {
		bool first = true;
		for (u32 k = 0; k < i; k++)
			if (!strcmp(W.bpool.b + W.bp_path[k], W.bpool.b + W.bp_path[i]))
				first = false;
		if (first)
			bp_send(W.bpool.b + W.bp_path[i]);
	}
}

static void bp_add(const char *path, u32 line, bool on, const char *cond)
{
	if (!W.bpool.b)
		pool_add(&W.bpool, "", 0);
	SoaCol c[] = {BP_COLS};
	soa_grow(c, 4, &W.bpcap, W.nbp + 1, W.nbp);
	W.bp_path[W.nbp] = pool_add(&W.bpool, path, (u32)strlen(path));
	W.bp_line[W.nbp] = line;
	W.bp_cond[W.nbp] = pool_add(&W.bpool, cond ? cond : "", (u32)strlen(cond ? cond : ""));
	W.bp_on[W.nbp] = on;
	W.nbp++;
}

static void bp_remove(u32 i)
{
	if (i >= W.nbp)
		return;
	char path[4096];
	snprintf(path, sizeof path, "%s", W.bpool.b + W.bp_path[i]);
	W.nbp--;
	W.bp_path[i] = W.bp_path[W.nbp];
	W.bp_line[i] = W.bp_line[W.nbp];
	W.bp_cond[i] = W.bp_cond[W.nbp];
	W.bp_on[i] = W.bp_on[W.nbp];
	bp_send(path);
}

static void bp_toggle(const char *path, u32 line)
{
	u32 i = bp_find(path, line);
	if (i != NONE)
		bp_remove(i);
	else {
		bp_add(path, line, true, NULL);
		bp_send(path);
	}
	app.dirty = true;
}

bool dbg_gutter_click(View *v, u32 line)
{
	const char *p = app.docs.path[v->doc];
	if (!p)
		return false;
	char c[4096];
	bp_toggle(canon(p, c, sizeof c), line + 1);
	return true;
}

void dbg_shift_lines(u32 d, u32 line, u32 removed, u32 added)
{
	if (!W.nbp || !app.docs.path[d] || removed == added)
		return;
	char c[4096];
	canon(app.docs.path[d], c, sizeof c);
	bool moved = false;
	for (u32 i = 0; i < W.nbp; i++) {
		if (strcmp(W.bpool.b + W.bp_path[i], c))
			continue;
		u32 l0 = W.bp_line[i] - 1; /* 0-based */
		if (l0 <= line)
			continue;
		if (l0 <= line + removed) /* inside the removed lines: onto the edit */
			l0 = line + 1 <= line + added ? line + 1 : line;
		else
			l0 = l0 - removed + added;
		W.bp_line[i] = l0 + 1;
		moved = true;
	}
	if (moved)
		bp_send(c);
}

/* ---- the debug session ------------------------------------------------------------------------- */

static bool program_path(char *out, size_t cap)
{
	if (W.target != NONE && W.target < W.cm.ntgt) {
		snprintf(out, cap, "%s", cm_str(&W.cm, W.cm.tgt_path[W.target]));
		return out[0] != 0;
	}
	char p[1024];
	snprintf(p, sizeof p, "%.*s", (int)W.prog.n, W.prog.b ? W.prog.b : "");
	char *s = p;
	while (*s == ' ')
		s++;
	size_t n = strlen(s);
	while (n && s[n - 1] == ' ')
		s[--n] = 0;
	if (!*s)
		return false;
	struct stat st;
	char t[2200];
	if (s[0] == '/')
		snprintf(out, cap, "%s", s);
	else {
		snprintf(t, sizeof t, "%s/%s", app.root ? app.root : ".", s);
		if (stat(t, &st) == 0 || strchr(s, '/'))
			snprintf(out, cap, "%s", t);
		else {
			char *w = prof_which(s);
			snprintf(out, cap, "%s", w ? w : t);
			free(w);
		}
	}
	return true;
}

/* the Workflow pane, right of the editors (the left side bar stays) */
static void show_pane(void)
{
	app.right_visible = true;
	if (app.right_w < PX(340))
		app.right_w = PX(340);
	ui_layout();
}

static void dbg_launch(void)
{
	char prog[2200];
	if (!program_path(prog, sizeof prog)) {
		wf_status("Choose a target or type the program to debug (Workflow)");
		show_pane();
		return;
	}
	struct stat st;
	if (stat(prog, &st) != 0) {
		wf_status("%s does not exist (build it first)", prog);
		return;
	}
	static char abuf[8192];
	char *args[128];
	u32 na = split_args(W.args.b ? W.args.b : "", W.args.n, args, 128, abuf);
	DapLaunch l = {NULL, prog, (const char *const *)args, na, app.root ? app.root : ".", W.stop_entry};
	char err[300];
	W.dap = dap_start(&l, err, sizeof err);
	if (!W.dap) {
		ui_log("[debug] %s", err);
		wf_status("%s", err);
		show_output();
		return;
	}
	W.seen_stops = 0;
	bp_send_all();
	ui_log("[debug] %s: %s%s", dap_adapter(W.dap), prog, na ? " ..." : "");
	wf_status("Debugging %s with %s", base_of(prog), dap_adapter(W.dap));
	show_output();
	show_pane();
}

static void run_program(void)
{
	char prog[2200];
	if (!program_path(prog, sizeof prog)) {
		wf_status("Choose a target or type the program to run (Workflow)");
		return;
	}
	static char abuf[8192];
	char *argv[130];
	argv[0] = prog;
	u32 na = split_args(W.args.b ? W.args.b : "", W.args.n, argv + 1, 128, abuf);
	argv[na + 1] = NULL;
	job_start(J_RUN, NX_NONE, argv, "run");
}

static bool cm_active(void) { return W.cm.has_lists; }

static bool build_then(int next)
{
	char *argv[32], buf[4096];
	const char *tgt = W.target != NONE ? cm_str(&W.cm, W.cm.tgt_name[W.target]) : NULL;
	char cache[1200];
	snprintf(cache, sizeof cache, "%s/CMakeCache.txt", W.bdir);
	struct stat st;
	if (stat(cache, &st) != 0 || W.ndefs) {
		/* never configured (or options changed): configure, then build */
		cmake_query(W.bdir);
		const char *defs[64];
		for (u32 i = 0; i < W.ndefs; i++)
			defs[i] = W.defs[i];
		cmake_configure_argv(&W.cm, W.preset, W.bdir, defs, W.ndefs, argv, 32, buf, sizeof buf);
		return job_start(J_CONFIGURE, next == NX_NONE ? NX_BUILD : next + 100, argv, "cmake");
	}
	cmake_build_argv(&W.cm, W.bdir, tgt, argv, 32, buf, sizeof buf);
	return job_start(J_BUILD, next, argv, "build");
}

static void start(bool debug)
{
	wf_init();
	if (W.dap) {
		dap_continue(W.dap);
		return;
	}
	if (cm_active() && W.build_first && (W.target != NONE || !W.prog.n))
		build_then(debug ? NX_DEBUG : NX_RUN);
	else if (debug)
		dbg_launch();
	else
		run_program();
}

static void dbg_end(void)
{
	if (W.dap) {
		dap_stop(W.dap);
		/* freed by the tick once the adapter is gone */
	}
}

/* expression under offset: identifiers with . and -> before them */
static bool expr_at(Text *t, u32 off, char *out, size_t cap)
{
	u32 n = t->len;
	if (off >= n)
		return false;
#define WB(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '_')
	u8 c = text_byte(t, off);
	if (!WB(c))
		return false;
	u32 a = off, b = off;
	while (b < n && WB(text_byte(t, b)))
		b++;
	for (;;) {
		while (a > 0 && WB(text_byte(t, a - 1)))
			a--;
		if (a > 0 && text_byte(t, a - 1) == '.' && a > 1 && WB(text_byte(t, a - 2)))
			a -= 1;
		else if (a > 1 && text_byte(t, a - 1) == '>' && text_byte(t, a - 2) == '-' && a > 2 && WB(text_byte(t, a - 3)))
			a -= 2;
		else
			break;
	}
#undef WB
	if (b - a >= cap || (text_byte(t, a) >= '0' && text_byte(t, a) <= '9'))
		return false;
	text_copy(t, a, b - a, out);
	out[b - a] = 0;
	return true;
}

void dbg_hover_request(View *v, u32 off)
{
	if (!W.dap || dap_state(W.dap) != DS_STOPPED)
		return;
	char e[256];
	if (!expr_at(&app.docs.text[v->doc], off, e, sizeof e))
		return;
	W.hv_doc = v->doc;
	W.hv_off = off;
	W.hv_stops = dap_stop_count(W.dap);
	snprintf(W.hv_expr, sizeof W.hv_expr, "%s", e);
	W.hv_ev = dap_evaluate(W.dap, e, "hover");
}

const char *dbg_hover_text(u32 d, u32 off)
{
	static char b[1400];
	if (!W.dap || dap_state(W.dap) != DS_STOPPED || W.hv_ev < 0 || d != W.hv_doc || W.hv_stops != dap_stop_count(W.dap))
		return NULL;
	(void)off;
	const char *v, *t;
	i64 r;
	if (dap_eval_result(W.dap, W.hv_ev, &v, &t, &r) != DV_LOADED || !*v)
		return NULL;
	snprintf(b, sizeof b, "%s = %s%s%s%s", W.hv_expr, v, *t ? "   (" : "", t, *t ? ")" : "");
	return b;
}

/* ---- watch expressions ------------------------------------------------------------------------ */

static void watch_add(const char *e)
{
	if (!*e)
		return;
	if (!W.wpool.b)
		pool_add(&W.wpool, "", 0);
	SoaCol c[] = {WX_COLS};
	soa_grow(c, 4, &W.wcap, W.nw + 1, W.nw);
	W.w_expr[W.nw] = pool_add(&W.wpool, e, (u32)strlen(e));
	W.w_ev[W.nw] = W.dap ? dap_evaluate(W.dap, e, "watch") : -1;
	W.w_last[W.nw] = 0;
	W.w_chg[W.nw] = 0;
	W.nw++;
	W.collapsed[S_WATCH] = false;
}

static void watch_remove(u32 i)
{
	if (i >= W.nw)
		return;
	for (u32 k = i; k + 1 < W.nw; k++) {
		W.w_expr[k] = W.w_expr[k + 1];
		W.w_ev[k] = W.w_ev[k + 1];
		W.w_last[k] = W.w_last[k + 1];
		W.w_chg[k] = W.w_chg[k + 1];
	}
	W.nw--;
}

static u64 hs(const char *s)
{
	u64 h = 1469598103934665603ull;
	while (*s) {
		h ^= (u8)*s++;
		h *= 1099511628211ull;
	}
	return h | 1;
}

/* at a stop: re-evaluate the watches (their last values mark changes) */
static void on_stop(void)
{
	Dap *d = W.dap;
	for (u32 i = 0; i < W.nw; i++)
		W.w_ev[i] = dap_evaluate(d, W.wpool.b + W.w_expr[i], "watch");
	/* show where it stopped */
	u32 f = dap_current_frame(d);
	const char *p = dap_frame_path(d, f);
	u32 line = dap_frame_line(d, f);
	struct stat st;
	if (*p && line && stat(p, &st) == 0)
		open_file(p, false, line - 1, 0);
	const char *why = dap_stop_reason(d), *txt = dap_stop_text(d);
	wf_status("Paused%s%s%s in %s", *why ? " on " : "", *txt ? txt : why, "", dap_frame_name(d, f));
	app.raise = true; /* the program may have had the focus: come back */
	app.focus = FOCUS_EDITOR;
	app.dirty = true;
}

/* ---- tick ------------------------------------------------------------------------------------- */

void workflow_tick(void)
{
	if (!W.inited)
		return;
	/* the job */
	if (W.job.kind != J_NONE) {
		char b[8192];
		ssize_t r;
		while ((r = read(W.job.fd, b, sizeof b)) > 0) {
			job_line(&W.job, b, (size_t)r);
			app.dirty = true;
		}
		int st;
		if (waitpid(W.job.pid, &st, WNOHANG) == W.job.pid) {
			while ((r = read(W.job.fd, b, sizeof b)) > 0)
				job_line(&W.job, b, (size_t)r);
			if (W.job.ln) {
				W.job.line[W.job.ln] = 0;
				ui_log("[%s] %s", W.job.tag, W.job.line);
			}
			close(W.job.fd);
			int kind = W.job.kind, next = W.job.next;
			bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
			int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
			double secs = (double)(vx_now_ns() - W.job.t0) / 1e9;
			W.job.kind = J_NONE;
			ui_log("[%s] %s (%d) after %.1f s", W.job.tag, ok ? "done" : "failed", code, secs);
			if (kind == J_CONFIGURE) {
				if (ok)
					W.ndefs = 0;
				cm_refresh();
				wf_status(ok ? "Configured" : "Configure failed (see Output)");
				if (ok && next != NX_NONE) {
					int after = next >= 100 ? next - 100 : NX_NONE;
					build_then(after);
				}
			} else if (kind == J_BUILD) {
				cm_refresh();
				wf_status(ok ? "Built" : "Build failed (see Output)");
				if (ok && next == NX_DEBUG)
					dbg_launch();
				else if (ok && next == NX_RUN)
					run_program();
			} else if (kind == J_RUN)
				wf_status("The program exited with %d", code);
			app.dirty = true;
		}
	}
	/* the debugger */
	if (W.dap) {
		if (dap_pump(W.dap))
			app.dirty = true;
		const char *cat, *txt;
		while (dap_output(W.dap, &cat, &txt)) {
			Job tmp = {0};
			snprintf(tmp.tag, sizeof tmp.tag, "%s", !strcmp(cat, "console") ? "debug" : "out");
			job_line(&tmp, txt, strlen(txt));
			if (tmp.ln) {
				tmp.line[tmp.ln] = 0;
				ui_log("[%s] %s", tmp.tag, tmp.line);
			}
			app.dirty = true;
		}
		if (dap_stop_count(W.dap) != W.seen_stops && dap_state(W.dap) == DS_STOPPED && dap_frame_count(W.dap)) {
			W.seen_stops = dap_stop_count(W.dap);
			on_stop();
		}
		if (W.reveal && dap_state(W.dap) == DS_STOPPED && dap_frame_count(W.dap)) {
			W.reveal = false;
			u32 f = dap_current_frame(W.dap);
			const char *p = dap_frame_path(W.dap, f);
			struct stat st;
			if (*p == '/' && dap_frame_line(W.dap, f) && stat(p, &st) == 0)
				open_file(p, false, dap_frame_line(W.dap, f) - 1, 0);
			for (u32 i = 0; i < W.nw; i++)
				W.w_ev[i] = dap_evaluate(W.dap, W.wpool.b + W.w_expr[i], "watch");
		}
		if (dap_state(W.dap) == DS_EXITED && !dap_alive(W.dap)) {
			int code = dap_exit_code(W.dap);
			if (code >= 0)
				ui_log("[debug] exited with code %d", code);
			else
				ui_log("[debug] session ended");
			if (code >= 0)
				wf_status("The program exited with %d", code);
			else
				wf_status("Debugging ended");
			dap_free(W.dap);
			W.dap = NULL;
			W.hv_ev = -1;
			app.dirty = true;
		}
	}
	/* CMakeLists / presets edited: read them again */
	static u64 last_check;
	if (vx_now_ns() - last_check > 2000000000ull && W.job.kind == J_NONE) {
		last_check = vx_now_ns();
		if (lists_mtime() != W.cm_mtime)
			cm_load();
	}
}

bool dbg_paused(void) { return W.dap && dap_state(W.dap) == DS_STOPPED; }

bool workflow_busy(void) { return W.job.kind != J_NONE || W.dap; }

void workflow_shutdown(void)
{
	job_stop();
	if (W.job.kind != J_NONE) {
		waitpid(W.job.pid, NULL, 0);
		close(W.job.fd);
		W.job.kind = J_NONE;
	}
	if (W.dap) {
		dap_free(W.dap); /* kills the adapter (and its debuggee) */
		W.dap = NULL;
	}
}

/* ---- the editor ---------------------------------------------------------------------------------- */

void dbg_gutter(u32 d, u32 first, u32 last, int x, int y0, int lh)
{
	const char *p = app.docs.path[d];
	if (!p)
		return;
	char c[4096];
	bool any = false;
	for (u32 i = 0; i < W.nbp && !any; i++)
		any = true;
	bool stopped = W.dap && dap_state(W.dap) == DS_STOPPED;
	if (!any && !stopped && !(app.hover_x >= x && app.hover_x < x + PX(18)))
		return;
	canon(p, c, sizeof c);
	for (u32 i = 0; i < W.nbp; i++) {
		if (strcmp(W.bpool.b + W.bp_path[i], c))
			continue;
		u32 l = W.bp_line[i] - 1;
		if (l < first || l >= last)
			continue;
		int ly = y0 + (int)(l - first) * lh;
		bool ver = true;
		u32 act = 0;
		if (W.dap && dap_bp_result(W.dap, c, W.bp_line[i], &ver, &act)) {
		} else
			ver = !W.dap;
		u32 col = !W.bp_on[i] ? (COL(C_ERROR) & 0xFFFFFF00u) | 0x50u : ver ? COL(C_ERROR) : COL(C_LINE_NUMBER);
		draw_icon(ICON_DOT, x + PX(1), ly + (lh - PX(16)) / 2, col);
		if (W.bpool.b[W.bp_cond[i]])
			draw_rect(x + PX(7), ly + lh / 2 - PX(1), PX(3), PX(2), COL(C_EDITOR_BG)); /* a condition: a dash */
	}
	/* a faint dot where a click would put one */
	if (app.hover_x >= x && app.hover_x < x + PX(18) && app.hover_y >= y0) {
		u32 l = first + (u32)((app.hover_y - y0) / lh);
		if (l < last && bp_find(c, l + 1) == NONE)
			draw_icon(ICON_DOT, x + PX(1), y0 + (int)(l - first) * lh + (lh - PX(16)) / 2,
			          (COL(C_ERROR) & 0xFFFFFF00u) | 0x40u);
	}
	/* the execution point */
	if (stopped) {
		u32 n = dap_frame_count(W.dap), cur = dap_current_frame(W.dap);
		for (u32 f = 0; f < n; f++) {
			const char *fp = dap_frame_path(W.dap, f);
			u32 fl = dap_frame_line(W.dap, f);
			if (!fl || !*fp || (f != cur && f != 0))
				continue;
			char fc[4096];
			if (strcmp(canon(fp, fc, sizeof fc), c))
				continue;
			u32 l = fl - 1;
			if (l < first || l >= last)
				continue;
			int ly = y0 + (int)(l - first) * lh;
			draw_icon(ICON_CHEVRON_RIGHT, x + PX(2), ly + (lh - PX(16)) / 2, f == 0 ? 0xFFCC00FFu : 0x4CC36BFFu);
		}
	}
}

void dbg_lines(u32 d, u32 first, u32 last, int text_x, int text_w, int y0, int lh)
{
	if (!W.dap || dap_state(W.dap) != DS_STOPPED || !app.docs.path[d])
		return;
	char c[4096];
	canon(app.docs.path[d], c, sizeof c);
	u32 n = dap_frame_count(W.dap), cur = dap_current_frame(W.dap);
	for (u32 f = 0; f < n; f++) {
		if (f != 0 && f != cur)
			continue;
		const char *fp = dap_frame_path(W.dap, f);
		u32 fl = dap_frame_line(W.dap, f);
		char fc[4096];
		if (!fl || !*fp || strcmp(canon(fp, fc, sizeof fc), c))
			continue;
		u32 l = fl - 1;
		if (l < first || l >= last)
			continue;
		/* the top frame in yellow, a selected caller in green (VSCodium) */
		draw_rect(text_x, y0 + (int)(l - first) * lh, text_w, lh, f == 0 ? 0xFFCC0033u : 0x4CC36B30u);
	}
}

void dbg_ruler(u32 d, int x, int y, int w, int h, u32 nl)
{
	if (!W.nbp || !app.docs.path[d] || !nl)
		return;
	char c[4096];
	canon(app.docs.path[d], c, sizeof c);
	for (u32 i = 0; i < W.nbp; i++)
		if (!strcmp(W.bpool.b + W.bp_path[i], c))
			draw_rect(x, y + (int)((i64)(W.bp_line[i] - 1) * h / (i64)nl), w, PX(3), COL(C_ERROR));
}

/* ---- the side bar ------------------------------------------------------------------------------- */

enum {
	H_NONE,
	H_SECTION,
	H_PRESET,
	H_CONFIGURE,
	H_BUILD,
	H_OPTS,
	H_ADV,
	H_OPT,
	H_TARGET,
	H_BUILDFIRST,
	H_STOPENTRY,
	H_RUN,
	H_DEBUG,
	H_STOP,
	H_TB, /* toolbar: arg = command */
	H_THREAD,
	H_FRAME,
	H_TOGGLE, /* tree row: arg = row */
	H_WATCH_ADD,
	H_WATCH_DEL,
	H_WPT_ADD,
	H_WPT_ADD_SW,
	H_WPT_DEL,
	H_WPT_ACCESS,
	H_BP_ON,
	H_BP_DEL,
	H_BP_GO,
	H_BP_CLEAR,
	H_FIELD,
};

typedef struct {
	int x, y, w, h, act, arg;
} Hit;
static Hit hits[512];
static int nhit;

/* rows of the variable trees (rebuilt per frame) */
static struct {
	u32 path; /* in rowpool */
	u32 node; /* DAP node, NONE for a scope */
	i64 ref;
} trow[1024];
static int ntrow;
static Pool rowpool;

static void hit(int x, int y, int w, int h, int act, int arg)
{
	if (nhit < (int)VX_COUNT(hits))
		hits[nhit++] = (Hit){x, y, w, h, act, arg};
}

static bool in_r(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static int ty(int y, int h, int f) { return y + (h - font_m[f].line) / 2; }

static bool is_open(const char *path)
{
	for (u32 i = 0; i < W.nopen; i++)
		if (!strcmp(W.open_pool.b + W.open[i], path))
			return true;
	return false;
}

static void set_open(const char *path, bool on)
{
	for (u32 i = 0; i < W.nopen; i++)
		if (!strcmp(W.open_pool.b + W.open[i], path)) {
			if (!on)
				W.open[i] = W.open[--W.nopen];
			return;
		}
	if (on) {
		VX_GROW(W.open, W.opencap, W.nopen + 1);
		W.open[W.nopen++] = pool_add(&W.open_pool, path, (u32)strlen(path));
	}
}

typedef struct {
	int x, w; /* the side bar's columns */
	int y;    /* pen (content coordinates, scrolled) */
	int top, bottom;
} Pen;

static bool visible(Pen *p, int h) { return p->y + h > p->top && p->y < p->bottom; }

static void section(Pen *p, int s, const char *title, const char *extra)
{
	int h = app.row_h;
	if (visible(p, h)) {
		bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h);
		if (hov)
			draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
		draw_icon(W.collapsed[s] ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, p->x + PX(4), p->y + (h - PX(16)) / 2,
		          COL(C_FG));
		int ex = draw_text(FONT_SMALL_BOLD, p->x + PX(22), ty(p->y, h, FONT_SMALL_BOLD), title, strlen(title),
		                   COL(C_SECTION_HEADER_FG));
		if (extra)
			draw_text_fit(FONT_SMALL, ex + PX(8), ty(p->y, h, FONT_SMALL), extra, strlen(extra),
			              p->x + p->w - ex - PX(60), COL(C_DESCRIPTION_FG));
		hit(p->x, p->y, p->w, h, H_SECTION, s);
	}
	p->y += h;
}

/* a small text button at the right end of the row just drawn */
static int row_button(Pen *p, int rx, int y, const char *label, int act, int arg, const char *tip)
{
	int w = font_width(FONT_SMALL, label, strlen(label)) + PX(12);
	int x = rx - w;
	if (y + app.row_h > p->top && y < p->bottom) {
		bool hov = in_r(app.hover_x, app.hover_y, x, y + PX(2), w, app.row_h - PX(4));
		draw_rect(x, y + PX(2), w, app.row_h - PX(4), hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
		draw_text(FONT_SMALL, x + PX(6), ty(y, app.row_h, FONT_SMALL), label, strlen(label), COL(C_FG));
		hit(x, y + PX(2), w, app.row_h - PX(4), act, arg);
		if (tip)
			ui_tip(x, y + PX(2), w, app.row_h - PX(4), tip);
	}
	return x - PX(4);
}

static int button(Pen *p, int x, const char *label, int act, int arg, bool primary, const char *tip)
{
	int h = PX(24), w = font_width(FONT_UI, label, strlen(label)) + PX(20);
	if (visible(p, h)) {
		bool hov = in_r(app.hover_x, app.hover_y, x, p->y, w, h);
		u32 bg = primary ? (hov ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG)) : hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG);
		draw_rect(x, p->y, w, h, bg);
		if (!primary)
			draw_frame(x, p->y, w, h, COL(C_INPUT_BORDER));
		draw_text(FONT_UI, x + PX(10), ty(p->y, h, FONT_UI), label, strlen(label), primary ? COL(C_BUTTON_FG) : COL(C_FG));
		hit(x, p->y, w, h, act, arg);
		if (tip)
			ui_tip(x, p->y, w, h, tip);
	}
	return x + w + PX(6);
}

static void checkbox(Pen *p, int x, bool on, const char *label, int act, int arg, const char *tip)
{
	int h = app.row_h, bs = PX(13), by = p->y + (h - bs) / 2;
	if (visible(p, h)) {
		draw_frame(x, by, bs, bs, on ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
		if (on)
			draw_rect(x + PX(3), by + PX(3), bs - PX(6), bs - PX(6), COL(C_FOCUS_BORDER));
		int ex = draw_text(FONT_UI, x + bs + PX(8), ty(p->y, h, FONT_UI), label, strlen(label), COL(C_FG));
		hit(x, p->y, ex - x, h, act, arg);
		if (tip)
			ui_tip(x, p->y, ex - x, h, tip);
	}
}

/* "Label  [value v]" choice row */
static void choice(Pen *p, const char *label, const char *value, int act, const char *tip)
{
	int h = app.row_h;
	if (visible(p, h)) {
		draw_text(FONT_UI, p->x + PX(12), ty(p->y, h, FONT_UI), label, strlen(label), COL(C_DESCRIPTION_FG));
		int vx = p->x + PX(80), vw = p->w - PX(92);
		bool hov = in_r(app.hover_x, app.hover_y, vx, p->y + PX(1), vw, h - PX(2));
		draw_rect(vx, p->y + PX(1), vw, h - PX(2), hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
		draw_frame(vx, p->y + PX(1), vw, h - PX(2), COL(C_INPUT_BORDER));
		draw_text_fit(FONT_UI, vx + PX(8), ty(p->y, h, FONT_UI), value, strlen(value), vw - PX(30), COL(C_FG));
		draw_icon(ICON_CHEVRON_DOWN, vx + vw - PX(20), p->y + (h - PX(16)) / 2, COL(C_FG));
		hit(vx, p->y, vw, h, act, 0);
		if (tip)
			ui_tip(vx, p->y, vw, h, tip);
	}
	p->y += h + PX(2);
}

static void text_row(Pen *p, int indent, const char *s, u32 col, int font)
{
	int h = app.row_h;
	if (visible(p, h))
		draw_text_fit(font, p->x + indent, ty(p->y, h, font), s, strlen(s), p->w - indent - PX(8), col);
	p->y += h;
}

static void field(Pen *p, Input *in, int f, const char *ph)
{
	bool foc = app.focus == FOCUS_SIDE_INPUT && W.field == f;
	int h = input_height(in, p->w - PX(24), 0);
	if (visible(p, h))
		input_draw_ex(in, p->x + PX(12), p->y, p->w - PX(24), 0, ph, foc);
	hit(p->x + PX(12), p->y, p->w - PX(24), h, H_FIELD, f);
	p->y += h + PX(6);
}

static void draw_toolbar(Pen *p)
{
	Dap *d = W.dap;
	int h = PX(28);
	bool stopped = dap_state(d) == DS_STOPPED;
	struct {
		int icon, cmd;
		const char *tip;
		bool on;
	} b[] = {
	    {stopped ? ICON_DBG_CONTINUE : ICON_DBG_PAUSE, stopped ? CMD_DBG_START : CMD_DBG_PAUSE,
	     stopped ? "Continue (F5)" : "Pause (F6)", true},
	    {ICON_DBG_STEP_OVER, CMD_DBG_STEP_OVER, "Step Over (F10)", stopped},
	    {ICON_DBG_STEP_INTO, CMD_DBG_STEP_INTO, "Step Into (F11)", stopped},
	    {ICON_DBG_STEP_OUT, CMD_DBG_STEP_OUT, "Step Out (Shift+F11)", stopped},
	    {ICON_DBG_RESTART, CMD_DBG_RESTART, "Restart (Ctrl+Shift+F5)", true},
	    {ICON_DBG_STOP, CMD_DBG_STOP, "Stop (Shift+F5)", true},
	};
	if (visible(p, h)) {
		draw_rect(p->x, p->y, p->w, h, COL(C_WIDGET_BG));
		int bx = p->x + PX(8);
		for (size_t i = 0; i < VX_COUNT(b); i++) {
			bool hov = in_r(app.hover_x, app.hover_y, bx, p->y + PX(2), PX(26), h - PX(4));
			if (hov && b[i].on)
				draw_rect(bx, p->y + PX(2), PX(26), h - PX(4), COL(C_LIST_HOVER_BG));
			u32 c = !b[i].on ? COL(C_DESCRIPTION_FG)
			        : b[i].cmd == CMD_DBG_STOP ? COL(C_ERROR)
			        : b[i].cmd == CMD_DBG_START ? 0x4CC36BFFu
			                                   : COL(C_LINK);
			draw_icon(b[i].icon, bx + PX(5), p->y + (h - PX(16)) / 2, c);
			if (b[i].on)
				hit(bx, p->y + PX(2), PX(26), h - PX(4), H_TB, b[i].cmd);
			ui_tip(bx, p->y + PX(2), PX(26), h - PX(4), b[i].tip);
			bx += PX(28);
		}
	}
	p->y += h;
	char st[400];
	int s = dap_state(d);
	if (s == DS_STARTING)
		snprintf(st, sizeof st, "Starting %s...", dap_adapter(d));
	else if (s == DS_RUNNING)
		snprintf(st, sizeof st, "Running");
	else if (s == DS_STOPPED) {
		const char *t = dap_stop_text(d);
		snprintf(st, sizeof st, "Paused: %s", *t ? t : dap_stop_reason(d));
	} else
		snprintf(st, sizeof st, "Ended");
	text_row(p, PX(12), st, s == DS_STOPPED ? 0xE5B000FFu : COL(C_DESCRIPTION_FG), FONT_SMALL);
}

static void tree(Pen *p, const char *path, i64 ref, int depth);

/* one variable row (and its children when open) */
static void var_row(Pen *p, const char *path, u32 node, int depth)
{
	Dap *d = W.dap;
	int h = app.row_h;
	i64 ref = dap_var_ref(d, node);
	bool open = ref > 0 && is_open(path);
	int ri = ntrow < (int)VX_COUNT(trow) ? ntrow++ : -1;
	if (ri >= 0) {
		trow[ri].path = pool_add(&rowpool, path, (u32)strlen(path));
		trow[ri].node = node;
		trow[ri].ref = ref;
	}
	if (visible(p, h)) {
		int ix = p->x + PX(10) + depth * PX(12);
		bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h);
		if (hov)
			draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
		if (ref > 0)
			draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ix, p->y + (h - PX(16)) / 2, COL(C_FG));
		const char *nm = dap_var_name(d, node), *val = dap_var_value(d, node);
		int ex = draw_text(FONT_MONO, ix + PX(16), ty(p->y, h, FONT_MONO), nm, strlen(nm), TOK(TK_VARIABLE));
		if (*val) /* groups (register sets) have no value */
			ex = draw_text(FONT_MONO, ex, ty(p->y, h, FONT_MONO), ": ", 2, COL(C_FG));
		bool chg = dap_var_changed(d, node);
		if (chg)
			draw_rect(ex - PX(2), p->y + PX(2), VX_MIN(font_width(FONT_MONO, val, strlen(val)) + PX(4), p->x + p->w - ex),
			          h - PX(4), 0xE5B00040u);
		draw_text_fit(FONT_MONO, ex, ty(p->y, h, FONT_MONO), val, strlen(val), p->x + p->w - ex - PX(6),
		              chg ? 0xE5B000FFu : TOK(TK_NUMBER));
		const char *t = dap_var_type(d, node);
		char tip[1600];
		snprintf(tip, sizeof tip, "%s%s%s = %s%s", *t ? t : "", *t ? " " : "", nm, val,
		         chg ? "  (changed)" : "");
		ui_tip(p->x, p->y, p->w, h, tip);
		if (ri >= 0)
			hit(p->x, p->y, p->w, h, H_TOGGLE, ri);
	}
	p->y += h;
	if (open)
		tree(p, path, ref, depth + 1);
}

static void tree(Pen *p, const char *path, i64 ref, int depth)
{
	u32 f, n;
	int st = dap_vars(W.dap, ref, &f, &n);
	if (st == DV_PENDING) {
		text_row(p, PX(26) + depth * PX(12), "Loading...", COL(C_DESCRIPTION_FG), FONT_SMALL);
		return;
	}
	if (st != DV_LOADED)
		return;
	for (u32 i = 0; i < n && i < 500; i++) {
		char sub[600];
		snprintf(sub, sizeof sub, "%s/%s", path, dap_var_name(W.dap, f + i));
		var_row(p, sub, f + i, depth);
		if (p->y > p->bottom + PX(2000))
			break; /* far below: stop drawing (and loading) */
	}
}

static void draw_stack(Pen *p)
{
	Dap *d = W.dap;
	u32 nt = dap_thread_count(d);
	char ex[48];
	snprintf(ex, sizeof ex, "%u thread%s", nt, nt == 1 ? "" : "s");
	section(p, S_STACK, "CALL STACK", ex);
	if (W.collapsed[S_STACK])
		return;
	if (dap_state(d) != DS_STOPPED) {
		text_row(p, PX(22), "Not paused", COL(C_DESCRIPTION_FG), FONT_SMALL);
		return;
	}
	int h = app.row_h;
	i64 cur = dap_current_thread(d), stopped = dap_stopped_thread(d);
	for (u32 t = 0; t < nt; t++) {
		i64 id = dap_thread_id(d, t);
		bool sel = id == cur;
		if (visible(p, h)) {
			if (sel)
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_INACTIVE_BG));
			else if (in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h))
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
			draw_icon(sel ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, p->x + PX(10), p->y + (h - PX(16)) / 2, COL(C_FG));
			char lb[300];
			const char *nm = dap_thread_name(d, t);
			snprintf(lb, sizeof lb, "%s #%lld%s", *nm ? nm : "thread", (long long)id,
			         id == stopped ? "   paused here" : "");
			draw_text_fit(FONT_UI, p->x + PX(28), ty(p->y, h, FONT_UI), lb, strlen(lb), p->w - PX(36),
			              id == stopped ? COL(C_FG) : COL(C_DESCRIPTION_FG));
			hit(p->x, p->y, p->w, h, H_THREAD, (int)t);
			ui_tip(p->x, p->y, p->w, h, "Switch to this thread");
		}
		p->y += h;
		if (!sel)
			continue;
		u32 nf = dap_frame_count(d), cf = dap_current_frame(d);
		if (!nf)
			text_row(p, PX(40), "Loading...", COL(C_DESCRIPTION_FG), FONT_SMALL);
		for (u32 f = 0; f < nf && f < 64; f++) {
			if (visible(p, h)) {
				if (f == cf)
					draw_rect(p->x, p->y, p->w, h, app.focus == FOCUS_RIGHT ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
				else if (in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h))
					draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
				const char *fp = dap_frame_path(d, f);
				u32 fl = dap_frame_line(d, f);
				bool src = *fp && fl;
				int ex = draw_text_fit(FONT_UI, p->x + PX(40), ty(p->y, h, FONT_UI), dap_frame_name(d, f),
				                       strlen(dap_frame_name(d, f)), p->w / 2, src ? COL(C_FG) : COL(C_DESCRIPTION_FG));
				char loc[600];
				if (src)
					snprintf(loc, sizeof loc, "%s:%u", base_of(fp), fl);
				else
					snprintf(loc, sizeof loc, "0x%llx", (unsigned long long)dap_frame_ip(d, f));
				draw_text_fit(FONT_SMALL, ex + PX(10), ty(p->y, h, FONT_SMALL), loc, strlen(loc), p->x + p->w - ex - PX(16),
				              COL(C_DESCRIPTION_FG));
				hit(p->x, p->y, p->w, h, H_FRAME, (int)f);
				if (src)
					ui_tip(p->x, p->y, p->w, h, fp);
			}
			p->y += h;
		}
	}
}

static void draw_vars(Pen *p)
{
	section(p, S_VARS, "VARIABLES", NULL);
	if (W.collapsed[S_VARS])
		return;
	Dap *d = W.dap;
	if (dap_state(d) != DS_STOPPED) {
		text_row(p, PX(22), "Not paused", COL(C_DESCRIPTION_FG), FONT_SMALL);
		return;
	}
	int h = app.row_h;
	u32 ns = dap_scope_count(d);
	if (!ns)
		text_row(p, PX(22), "Loading...", COL(C_DESCRIPTION_FG), FONT_SMALL);
	for (u32 s = 0; s < ns; s++) {
		const char *nm = dap_scope_name(d, s);
		bool open = is_open(nm);
		int ri = ntrow < (int)VX_COUNT(trow) ? ntrow++ : -1;
		if (ri >= 0) {
			trow[ri].path = pool_add(&rowpool, nm, (u32)strlen(nm));
			trow[ri].node = NONE;
			trow[ri].ref = dap_scope_ref(d, s);
		}
		if (visible(p, h)) {
			if (in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h))
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
			draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, p->x + PX(10), p->y + (h - PX(16)) / 2, COL(C_FG));
			draw_text(FONT_UI_BOLD, p->x + PX(28), ty(p->y, h, FONT_UI_BOLD), nm, strlen(nm), COL(C_FG));
			if (ri >= 0)
				hit(p->x, p->y, p->w, h, H_TOGGLE, ri);
		}
		p->y += h;
		if (open)
			tree(p, nm, dap_scope_ref(d, s), 1);
	}
}

static void draw_watch(Pen *p)
{
	section(p, S_WATCH, "WATCH", W.nw ? NULL : "expressions, re-evaluated at every stop");
	int sy = p->y - app.row_h;
	row_button(p, p->x + p->w - PX(6), sy, "+ Add", H_WATCH_ADD, 0, "Add an expression to watch");
	if (W.collapsed[S_WATCH])
		return;
	int h = app.row_h;
	for (u32 i = 0; i < W.nw; i++) {
		const char *e = W.wpool.b + W.w_expr[i];
		const char *v = "", *t = "";
		i64 ref = 0;
		int st = W.dap && dap_state(W.dap) == DS_STOPPED ? dap_eval_result(W.dap, W.w_ev[i], &v, &t, &ref) : DV_NONE;
		if (st == DV_LOADED || st == DV_ERROR) {
			u64 hh = hs(v);
			if (W.w_last[i] != hh) {
				W.w_chg[i] = W.w_last[i] != 0 && st == DV_LOADED;
				W.w_last[i] = hh;
			}
		}
		char path[300];
		snprintf(path, sizeof path, "watch:%u", i);
		bool open = ref > 0 && is_open(path);
		int ri = ntrow < (int)VX_COUNT(trow) ? ntrow++ : -1;
		if (ri >= 0) {
			trow[ri].path = pool_add(&rowpool, path, (u32)strlen(path));
			trow[ri].node = NONE;
			trow[ri].ref = ref;
		}
		if (visible(p, h)) {
			bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h);
			if (hov)
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
			if (ref > 0)
				draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, p->x + PX(10), p->y + (h - PX(16)) / 2, COL(C_FG));
			int ex = draw_text(FONT_MONO, p->x + PX(28), ty(p->y, h, FONT_MONO), e, strlen(e), TOK(TK_VARIABLE));
			ex = draw_text(FONT_MONO, ex, ty(p->y, h, FONT_MONO), ": ", 2, COL(C_FG));
			const char *shown = !W.dap ? "(not debugging)" : st == DV_PENDING ? "..." : st == DV_NONE ? "(not paused)" : v;
			u32 vc = st == DV_ERROR ? COL(C_ERROR) : W.w_chg[i] ? 0xE5B000FFu : TOK(TK_NUMBER);
			int rx = p->x + p->w - PX(26);
			draw_text_fit(FONT_MONO, ex, ty(p->y, h, FONT_MONO), shown, strlen(shown), rx - ex - PX(4), vc);
			char tip[1400];
			snprintf(tip, sizeof tip, "%s = %s%s%s", e, shown, *t ? "  " : "", t);
			ui_tip(p->x, p->y, rx - p->x, h, tip);
			if (ri >= 0)
				hit(p->x, p->y, rx - p->x, h, H_TOGGLE, ri);
			if (hov) {
				draw_icon(ICON_CLOSE, rx + PX(4), p->y + (h - PX(16)) / 2, COL(C_FG));
				hit(rx, p->y, PX(24), h, H_WATCH_DEL, (int)i);
			}
		}
		p->y += h;
		if (open)
			tree(p, path, ref, 2);
	}
	if (!W.nw)
		text_row(p, PX(22), "Right-click a variable, or select an expression: Debug: Add to Watch",
		         COL(C_DESCRIPTION_FG), FONT_SMALL);
}

static void draw_watchpoints(Pen *p)
{
	Dap *d = W.dap;
	char ex[96] = "";
	u32 used = 0;
	for (u32 i = 0; d && i < dap_watch_count(d); i++)
		used += dap_watch_ok(d, i) && dap_watch_hw(d, i);
	if (d && dap_hw_slots(d) > 0)
		snprintf(ex, sizeof ex, "hardware: %u of %d in use", used, dap_hw_slots(d));
	section(p, S_WPTS, "WATCHPOINTS", ex[0] ? ex : "break when memory is read or written");
	int sy = p->y - app.row_h;
	int bx = row_button(p, p->x + p->w - PX(6), sy, "+ Add", H_WPT_ADD, 0,
	                    "A hardware watchpoint: break when this lvalue is written");
	if (d && dap_supports_sw_watch(d))
		row_button(p, bx, sy, "+ Software", H_WPT_ADD_SW, 0,
		           "A software watchpoint (gdb): no slot limit, but the program single-steps (slow)");
	if (W.collapsed[S_WPTS])
		return;
	int h = app.row_h;
	u32 n = d ? dap_watch_count(d) : 0;
	for (u32 i = 0; i < n; i++) {
		if (visible(p, h * 2)) {
			bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h * 2);
			if (hov)
				draw_rect(p->x, p->y, p->w, h * 2, COL(C_LIST_HOVER_BG));
			static const char *const acc[] = {"W", "R", "RW"};
			static const char *const acc_tip[] = {"Breaks on writes (click: reads)", "Breaks on reads (click: reads and writes)",
			                                      "Breaks on reads and writes (click: writes)"};
			int a = dap_watch_access(d, i);
			int cw = PX(30), cx = p->x + PX(12);
			draw_rect(cx, p->y + PX(3), cw, h - PX(6), COL(C_BUTTON_BG));
			const char *al = acc[a];
			draw_text(FONT_SMALL_BOLD, cx + (cw - font_width(FONT_SMALL_BOLD, al, strlen(al))) / 2,
			          ty(p->y, h, FONT_SMALL_BOLD), al, strlen(al), COL(C_BUTTON_FG));
			hit(cx, p->y, cw, h, H_WPT_ACCESS, (int)i);
			ui_tip(cx, p->y, cw, h, acc_tip[a]);
			bool ok = dap_watch_ok(d, i);
			const char *e = dap_watch_name(d, i);
			int rx = p->x + p->w - PX(26);
			int ex2 = draw_text_fit(FONT_MONO, cx + cw + PX(8), ty(p->y, h, FONT_MONO), e, strlen(e), rx - cx - cw - PX(14),
			                        ok ? TOK(TK_VARIABLE) : COL(C_DESCRIPTION_FG));
			(void)ex2;
			char desc[400];
			snprintf(desc, sizeof desc, "%s%s", dap_watch_hw(d, i) ? "Hardware. " : "Software. ", dap_watch_desc(d, i));
			draw_text_fit(FONT_SMALL, cx + cw + PX(8), ty(p->y + h, h, FONT_SMALL), desc, strlen(desc),
			              rx - cx - cw - PX(14), ok ? COL(C_DESCRIPTION_FG) : COL(C_ERROR));
			if (hov) {
				draw_icon(ICON_CLOSE, rx + PX(4), p->y + (h - PX(16)) / 2, COL(C_FG));
				hit(rx, p->y, PX(24), h, H_WPT_DEL, (int)i);
			}
		}
		p->y += h * 2;
	}
	if (!n)
		text_row(p, PX(22), d ? "Right-click a variable: Break on Write" : "Available while debugging",
		         COL(C_DESCRIPTION_FG), FONT_SMALL);
}

static void draw_bps(Pen *p)
{
	char ex[32];
	snprintf(ex, sizeof ex, "%u", W.nbp);
	section(p, S_BPS, "BREAKPOINTS", W.nbp ? ex : "click left of a line number, or F9");
	if (W.nbp)
		row_button(p, p->x + p->w - PX(6), p->y - app.row_h, "Remove All", H_BP_CLEAR, 0, NULL);
	if (W.collapsed[S_BPS])
		return;
	int h = app.row_h;
	for (u32 i = 0; i < W.nbp; i++) {
		if (visible(p, h)) {
			bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h);
			if (hov)
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
			int bs = PX(13), cx = p->x + PX(12), by = p->y + (h - bs) / 2;
			draw_frame(cx, by, bs, bs, W.bp_on[i] ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
			if (W.bp_on[i])
				draw_rect(cx + PX(3), by + PX(3), bs - PX(6), bs - PX(6), COL(C_FOCUS_BORDER));
			hit(cx - PX(2), p->y, bs + PX(6), h, H_BP_ON, (int)i);
			const char *path = W.bpool.b + W.bp_path[i];
			bool ver = true;
			u32 act = 0;
			if (W.dap && !dap_bp_result(W.dap, path, W.bp_line[i], &ver, &act))
				ver = false;
			draw_icon(ICON_DOT, cx + bs + PX(4), p->y + (h - PX(16)) / 2,
			          !W.bp_on[i] ? (COL(C_ERROR) & 0xFFFFFF00u) | 0x50u : ver ? COL(C_ERROR) : COL(C_LINE_NUMBER));
			char lb[1200];
			const char *cond = W.bpool.b + W.bp_cond[i];
			snprintf(lb, sizeof lb, "%s:%u%s%s", base_of(path), W.bp_line[i], *cond ? "   if " : "", cond);
			int rx = p->x + p->w - PX(26);
			int tx = cx + bs + PX(22);
			draw_text_fit(FONT_UI, tx, ty(p->y, h, FONT_UI), lb, strlen(lb), rx - tx - PX(4), COL(C_FG));
			hit(tx, p->y, rx - tx, h, H_BP_GO, (int)i);
			char tip[1400];
			snprintf(tip, sizeof tip, "%s:%u%s", path, W.bp_line[i],
			         W.dap && !ver ? "  (not resolved yet: no code at this line?)" : "");
			ui_tip(tx, p->y, rx - tx, h, tip);
			if (hov) {
				draw_icon(ICON_CLOSE, rx + PX(4), p->y + (h - PX(16)) / 2, COL(C_FG));
				hit(rx, p->y, PX(24), h, H_BP_DEL, (int)i);
			}
		}
		p->y += h;
	}
}

static void draw_cmake(Pen *p)
{
	section(p, S_CMAKE, "CMAKE", W.cm.has_lists ? NULL : "no CMakeLists.txt here");
	if (W.collapsed[S_CMAKE] || !W.cm.has_lists)
		return;
	if (W.cm.npre) {
		char v[400];
		snprintf(v, sizeof v, "%s", W.preset != NONE ? cm_str(&W.cm, W.cm.pre_display[W.preset]) : "(none)");
		choice(p, "Preset", v, H_PRESET, "Configure preset (CMakePresets.json)");
	}
	char bd[1200];
	const char *rel = W.bdir;
	size_t rl = app.root ? strlen(app.root) : 0;
	if (rl && !strncmp(W.bdir, app.root, rl) && W.bdir[rl] == '/')
		rel = W.bdir + rl + 1;
	snprintf(bd, sizeof bd, "Build folder: %s%s", rel, W.cm.from_cache ? "" : "  (not configured)");
	text_row(p, PX(12), bd, COL(C_DESCRIPTION_FG), FONT_SMALL);
	int bx = p->x + PX(12);
	bool busy = W.job.kind != J_NONE;
	bx = button(p, bx, W.job.kind == J_CONFIGURE ? "Configuring..." : "Configure", busy ? H_NONE : H_CONFIGURE, 0,
	            W.ndefs > 0, "cmake (with the options changed below)");
	button(p, bx, W.job.kind == J_BUILD ? "Building..." : "Build", busy ? H_NONE : H_BUILD, 0, false,
	       "cmake --build (the selected target)");
	p->y += PX(24) + PX(6);
	if (W.ndefs) {
		char m[80];
		snprintf(m, sizeof m, "%u option%s changed: Configure applies %s", W.ndefs, W.ndefs == 1 ? "" : "s",
		         W.ndefs == 1 ? "it" : "them");
		text_row(p, PX(12), m, COL(C_WARNING), FONT_SMALL);
	}
	/* options */
	u32 nopt = 0;
	for (u32 i = 0; i < W.cm.nent; i++)
		nopt += !W.cm.ent_adv[i] || W.show_adv;
	int h = app.row_h;
	if (visible(p, h)) {
		draw_icon(W.opts_open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, p->x + PX(10), p->y + (h - PX(16)) / 2,
		          COL(C_FG));
		char t[80];
		snprintf(t, sizeof t, "Options (%u)%s", nopt, W.cm.from_cache ? "" : " from CMakeLists.txt");
		draw_text(FONT_UI, p->x + PX(28), ty(p->y, h, FONT_UI), t, strlen(t), COL(C_FG));
		hit(p->x, p->y, p->w - PX(90), h, H_OPTS, 0);
	}
	if (W.cm.from_cache)
		row_button(p, p->x + p->w - PX(6), p->y, W.show_adv ? "Hide advanced" : "Advanced", H_ADV, 0,
		           "Show the advanced cache entries");
	p->y += h;
	if (!W.opts_open)
		return;
	for (u32 i = 0; i < W.cm.nent; i++) {
		if (W.cm.ent_adv[i] && !W.show_adv)
			continue;
		const char *nm = cm_str(&W.cm, W.cm.ent_name[i]), *val = cm_str(&W.cm, W.cm.ent_value[i]);
		/* a pending change shows instead */
		char pend[320] = "";
		for (u32 k = 0; k < W.ndefs; k++) {
			const char *eq = strchr(W.defs[k], '='), *co = strchr(W.defs[k], ':');
			size_t nl = (size_t)((co && co < eq ? co : eq) - W.defs[k]);
			if (eq && nl == strlen(nm) && !strncmp(W.defs[k], nm, nl))
				snprintf(pend, sizeof pend, "%s", eq + 1);
		}
		const char *shown = pend[0] ? pend : val;
		if (visible(p, h)) {
			bool hov = in_r(app.hover_x, app.hover_y, p->x, p->y, p->w, h);
			if (hov)
				draw_rect(p->x, p->y, p->w, h, COL(C_LIST_HOVER_BG));
			int x0 = p->x + PX(28);
			if (W.cm.ent_type[i] == CV_BOOL) {
				bool on = !strcasecmp(shown, "ON") || !strcasecmp(shown, "TRUE") || !strcasecmp(shown, "YES") ||
				          !strcmp(shown, "1") || !strcasecmp(shown, "Y");
				int bs = PX(13), by = p->y + (h - bs) / 2;
				draw_frame(x0, by, bs, bs, on ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
				if (on)
					draw_rect(x0 + PX(3), by + PX(3), bs - PX(6), bs - PX(6), COL(C_FOCUS_BORDER));
				draw_text_fit(FONT_UI, x0 + bs + PX(8), ty(p->y, h, FONT_UI), nm, strlen(nm), p->w - bs - PX(44),
				              pend[0] ? COL(C_WARNING) : COL(C_FG));
			} else {
				int ex = draw_text_fit(FONT_UI, x0, ty(p->y, h, FONT_UI), nm, strlen(nm), p->w / 2,
				                       pend[0] ? COL(C_WARNING) : COL(C_FG));
				draw_text_fit(FONT_MONO, ex + PX(10), ty(p->y, h, FONT_MONO), shown, strlen(shown),
				              p->x + p->w - ex - PX(18), TOK(TK_STRING));
			}
			char tip[1600];
			snprintf(tip, sizeof tip, "%s = %s\n%s", nm, shown, cm_str(&W.cm, W.cm.ent_help[i]));
			ui_tip(p->x, p->y, p->w, h, tip);
			hit(p->x, p->y, p->w, h, H_OPT, (int)i);
		}
		p->y += h;
	}
}

static void draw_run(Pen *p)
{
	section(p, S_RUN, "RUN", NULL);
	if (W.collapsed[S_RUN])
		return;
	u32 ex[64];
	u32 nx = cmake_executables(&W.cm, ex, 64);
	if (W.cm.has_lists && nx) {
		const char *v = W.target != NONE ? cm_str(&W.cm, W.cm.tgt_name[W.target]) : "Another program...";
		choice(p, "Target", v, H_TARGET, "The executable to run or debug (CMake targets)");
	} else if (W.cm.has_lists)
		text_row(p, PX(12), W.cm.from_cache ? "No executable targets" : "Configure to list the executables",
		         COL(C_DESCRIPTION_FG), FONT_SMALL);
	if (W.target == NONE) {
		text_row(p, PX(12), "Program", COL(C_DESCRIPTION_FG), FONT_SMALL);
		field(p, &W.prog, F_PROG, "./app, or a name in PATH");
	}
	text_row(p, PX(12), "Arguments", COL(C_DESCRIPTION_FG), FONT_SMALL);
	field(p, &W.args, F_ARGS, "--flag \"quoted arg\"");
	char wd[1200];
	snprintf(wd, sizeof wd, "Runs in %s", app.root ? app.root : ".");
	text_row(p, PX(12), wd, COL(C_DESCRIPTION_FG), FONT_SMALL);
	if (W.cm.has_lists) {
		checkbox(p, p->x + PX(12), W.build_first, "Build first", H_BUILDFIRST, 0, "cmake --build before running");
		p->y += app.row_h;
	}
	checkbox(p, p->x + PX(12), W.stop_entry, "Stop at the start of main", H_STOPENTRY, 0,
	         "Pause on main's first line (and show it), before anything runs");
	p->y += app.row_h + PX(4);
	int bx = p->x + PX(12);
	if (W.job.kind == J_RUN || W.dap)
		button(p, bx, "Stop", H_STOP, 0, false, "Shift+F5");
	else {
		bx = button(p, bx, "Run", W.job.kind ? H_NONE : H_RUN, 0, false, "Run without the debugger (Ctrl+F5)");
		button(p, bx, "Debug", W.job.kind ? H_NONE : H_DEBUG, 0, true, "Start debugging (F5)");
	}
	p->y += PX(24) + PX(10);
}

void workflow_draw(int x, int y, int w, int h)
{
	wf_init();
	nhit = 0;
	ntrow = 0;
	pool_clear(&rowpool);
	pool_add(&rowpool, "", 0);
	int th = view_title(x, y, w, "WORKFLOW");
	int top = y + th;
	Pen p = {x, w, top - W.scroll, top, y + h};
	draw_clip(x, top, w, h - th);
	if (W.dap)
		draw_toolbar(&p);
	draw_cmake(&p);
	draw_run(&p);
	if (W.dap) {
		draw_stack(&p);
		draw_vars(&p);
	}
	draw_watch(&p);
	draw_watchpoints(&p);
	draw_bps(&p);
	draw_noclip();
	W.content_h = p.y + W.scroll - top;
	int maxs = VX_MAX(W.content_h - (h - th) + PX(20), 0);
	W.scroll = VX_CLAMP(W.scroll, 0, maxs);
}

/* ---- side bar input ----------------------------------------------------------------------------- */

static void prompt(int kind, const char *title, const char *init)
{
	app.quick = QUICK_PROMPT;
	app.quick_prompt_kind = kind;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, init ? init : "", init ? strlen(init) : 0);
	app.focus = FOCUS_QUICK;
}

static void open_bp(u32 i)
{
	if (i < W.nbp)
		open_file(W.bpool.b + W.bp_path[i], false, W.bp_line[i] - 1, 0);
}

static void set_def(const char *name, int type, const char *value)
{
	static const char *const tn[] = {"BOOL", "STRING", "PATH", "FILEPATH", "STRING"};
	char d[320];
	snprintf(d, sizeof d, "%s:%s=%s", name, tn[type], value);
	for (u32 k = 0; k < W.ndefs; k++) {
		const char *co = strchr(W.defs[k], ':');
		if (co && (size_t)(co - W.defs[k]) == strlen(name) && !strncmp(W.defs[k], name, strlen(name))) {
			snprintf(W.defs[k], sizeof W.defs[k], "%s", d);
			return;
		}
	}
	if (W.ndefs < 64)
		snprintf(W.defs[W.ndefs++], sizeof W.defs[0], "%s", d);
}

static void hit_run(int act, int arg, const Event *e)
{
	(void)e;
	switch (act) {
	case H_SECTION: W.collapsed[arg] = !W.collapsed[arg]; break;
	case H_PRESET: workflow_command(CMD_CMAKE_PRESET); break;
	case H_TARGET: workflow_command(CMD_CMAKE_TARGET); break;
	case H_CONFIGURE: workflow_command(CMD_CMAKE_CONFIGURE); break;
	case H_BUILD: workflow_command(CMD_CMAKE_BUILD); break;
	case H_OPTS: W.opts_open = !W.opts_open; break;
	case H_ADV: W.show_adv = !W.show_adv; break;
	case H_OPT: {
		u32 i = (u32)arg;
		const char *nm = cm_str(&W.cm, W.cm.ent_name[i]);
		if (W.cm.ent_type[i] == CV_BOOL) {
			const char *cur = cm_str(&W.cm, W.cm.ent_value[i]);
			for (u32 k = 0; k < W.ndefs; k++)
				if (!strncmp(W.defs[k], nm, strlen(nm)) && W.defs[k][strlen(nm)] == ':')
					cur = strchr(W.defs[k], '=') + 1;
			bool on = !strcasecmp(cur, "ON") || !strcasecmp(cur, "TRUE") || !strcmp(cur, "1") || !strcasecmp(cur, "YES");
			set_def(nm, CV_BOOL, on ? "OFF" : "ON");
		} else {
			W.ctx_bp = i;
			char t[300];
			snprintf(t, sizeof t, "%s (%s)", nm, cm_str(&W.cm, W.cm.ent_help[i]));
			prompt(PR_WF_OPTION, t, cm_str(&W.cm, W.cm.ent_value[i]));
		}
		break;
	}
	case H_BUILDFIRST: W.build_first = !W.build_first; break;
	case H_STOPENTRY: W.stop_entry = !W.stop_entry; break;
	case H_RUN: start(false); break;
	case H_DEBUG: start(true); break;
	case H_STOP: workflow_command(CMD_DBG_STOP); break;
	case H_TB: run_command(arg); break;
	case H_THREAD:
		if (W.dap) {
			dap_select_thread(W.dap, dap_thread_id(W.dap, (u32)arg));
			W.reveal = true;
		}
		break;
	case H_FRAME:
		if (W.dap) {
			dap_select_frame(W.dap, (u32)arg);
			const char *fp = dap_frame_path(W.dap, (u32)arg);
			u32 fl = dap_frame_line(W.dap, (u32)arg);
			struct stat st;
			if (*fp && fl && stat(fp, &st) == 0)
				open_file(fp, false, fl - 1, 0);
			for (u32 i = 0; i < W.nw; i++) /* watches belong to the frame */
				W.w_ev[i] = dap_evaluate(W.dap, W.wpool.b + W.w_expr[i], "watch");
		}
		break;
	case H_TOGGLE:
		if (arg >= 0 && arg < ntrow && trow[arg].ref > 0) {
			const char *path = rowpool.b + trow[arg].path;
			set_open(path, !is_open(path));
		}
		break;
	case H_WATCH_ADD: prompt(PR_WF_WATCH, "Expression to watch", NULL); break;
	case H_WATCH_DEL: watch_remove((u32)arg); break;
	case H_WPT_ADD:
	case H_WPT_ADD_SW:
		if (!W.dap || dap_state(W.dap) != DS_STOPPED)
			wf_status("Watchpoints are set while the program is paused");
		else
			prompt(act == H_WPT_ADD ? PR_WF_WATCHPOINT : PR_WF_WATCHPOINT_SW,
			       act == H_WPT_ADD ? "Break when written: an lvalue (counter, p->x, arr[3])"
			                        : "Software watchpoint (gdb): an expression", NULL);
		break;
	case H_WPT_DEL:
		if (W.dap)
			dap_watch_remove(W.dap, (u32)arg);
		break;
	case H_WPT_ACCESS:
		if (W.dap)
			dap_watch_set_access(W.dap, (u32)arg, (dap_watch_access(W.dap, (u32)arg) + 1) % 3);
		break;
	case H_BP_ON:
		if ((u32)arg < W.nbp) {
			W.bp_on[arg] ^= 1;
			bp_send(W.bpool.b + W.bp_path[arg]);
		}
		break;
	case H_BP_DEL: bp_remove((u32)arg); break;
	case H_BP_GO: open_bp((u32)arg); break;
	case H_BP_CLEAR: workflow_command(CMD_DBG_CLEAR_BPS); break;
	case H_FIELD:
		W.field = arg;
		app.focus = FOCUS_SIDE_INPUT;
		break;
	}
	app.dirty = true;
}

static void var_menu(int row, int mx, int my)
{
	if (!W.dap || row < 0 || row >= ntrow || trow[row].node == NONE)
		return;
	u32 n = trow[row].node;
	const char *ev = dap_var_eval_name(W.dap, n);
	snprintf(W.ctx_expr, sizeof W.ctx_expr, "%s", *ev ? ev : dap_var_name(W.dap, n));
	snprintf(W.ctx_value, sizeof W.ctx_value, "%s", dap_var_value(W.dap, n));
	menu_open(mx, my);
	menu_item("Add to Watch", CMD_DBG_VAR_WATCH, true);
	menu_item("Copy Value", CMD_DBG_VAR_COPY, true);
	menu_sep();
	menu_item("Break on Write (hardware watchpoint)", CMD_DBG_VAR_BREAK_W, true);
	menu_item("Break on Read", CMD_DBG_VAR_BREAK_R, true);
	menu_item("Break on Read or Write", CMD_DBG_VAR_BREAK_RW, true);
}

void workflow_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)w;
	(void)h;
	(void)y;
	app.dirty = true;
	if (e->type == EV_WHEEL) {
		W.scroll -= (i32)(e->wy * 3 * app.row_h);
		return;
	}
	if (e->type == EV_TEXT && app.focus == FOCUS_SIDE_INPUT && W.field) {
		input_text(W.field == F_PROG ? &W.prog : &W.args, e->text, strlen(e->text));
		return;
	}
	if (e->type == EV_KEY && app.focus == FOCUS_SIDE_INPUT && W.field) {
		Input *in = W.field == F_PROG ? &W.prog : &W.args;
		if (e->key == SDLK_ESCAPE) {
			W.field = F_NONE;
			app.focus = FOCUS_RIGHT;
		} else if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
			W.field = F_NONE;
			app.focus = FOCUS_EDITOR;
			start(!(e->mods & MOD_CTRL));
		} else if (e->key == SDLK_TAB)
			W.field = W.field == F_PROG ? F_ARGS : W.target == NONE ? F_PROG : F_ARGS;
		else
			input_key(in, e);
		return;
	}
	if (e->type != EV_MDOWN)
		return;
	for (int i = nhit; i-- > 0;)
		if (in_r(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
			if (e->button == 3) {
				if (hits[i].act == H_TOGGLE)
					var_menu(hits[i].arg, e->x, e->y);
				else if (hits[i].act == H_BP_GO || hits[i].act == H_BP_ON) {
					W.ctx_bp = (u32)hits[i].arg;
					menu_open(e->x, e->y);
					menu_item("Edit Condition...", CMD_DBG_BP_CONDITION, true);
					menu_item("Remove All Breakpoints", CMD_DBG_CLEAR_BPS, true);
				}
				return;
			}
			if (e->button != 1)
				return;
			if (hits[i].act == H_FIELD) {
				Input *in = hits[i].arg == F_PROG ? &W.prog : &W.args;
				W.field = hits[i].arg;
				app.focus = FOCUS_SIDE_INPUT;
				input_click_ex(in, hits[i].x, hits[i].y, hits[i].w, 0, e->x, e->y, e->clicks);
				return;
			}
			if (W.field)
				W.field = F_NONE;
			app.focus = FOCUS_RIGHT;
			hit_run(hits[i].act, hits[i].arg, e);
			return;
		}
	W.field = F_NONE;
	app.focus = FOCUS_RIGHT;
}

/* ---- commands --------------------------------------------------------------------------------- */

static bool godot_fallback(void)
{
	/* a Godot project with nothing else to run keeps Godot's keys */
	return godot_dir() && !W.cm.has_lists && !W.prog.n && !W.dap && W.job.kind == J_NONE;
}

bool workflow_command(int cmd)
{
	View *v = active_view();
	switch (cmd) {
	case CMD_VIEW_WORKFLOW:
		/* the tab toggles the pane */
		wf_init();
		if (app.right_visible) {
			app.right_visible = false;
			if (app.focus == FOCUS_RIGHT || app.focus == FOCUS_SIDE_INPUT)
				app.focus = FOCUS_EDITOR;
			ui_layout();
		} else {
			show_pane();
			app.focus = FOCUS_RIGHT;
		}
		return true;
	case CMD_DBG_START:
		wf_init();
		if (godot_fallback()) {
			run_command(CMD_GODOT_RUN);
			return true;
		}
		start(true);
		return true;
	case CMD_DBG_RUN:
		wf_init();
		if (W.dap)
			wf_status("Already debugging");
		else
			start(false);
		return true;
	case CMD_DBG_STOP:
		if (W.dap)
			dbg_end();
		else if (W.job.kind != J_NONE)
			job_stop();
		else if (godot_dir())
			run_command(CMD_GODOT_STOP);
		return true;
	case CMD_DBG_RESTART:
		if (W.dap) {
			dap_free(W.dap); /* the old session goes at once */
			W.dap = NULL;
			dbg_launch();
		}
		return true;
	case CMD_DBG_PAUSE:
		if (W.dap)
			dap_pause(W.dap);
		else if (godot_dir())
			run_command(CMD_GODOT_RUN_SCENE);
		return true;
	case CMD_DBG_STEP_OVER:
		if (W.dap)
			dap_next(W.dap);
		return true;
	case CMD_DBG_STEP_INTO:
		if (W.dap)
			dap_step_in(W.dap);
		return true;
	case CMD_DBG_STEP_OUT:
		if (W.dap)
			dap_step_out(W.dap);
		return true;
	case CMD_DBG_TOGGLE_BP:
		if (v && app.docs.path[v->doc] && !app.docs.bin[v->doc]) {
			wf_init();
			dbg_gutter_click(v, text_line_of(&app.docs.text[v->doc], v->head[0]));
		}
		return true;
	case CMD_DBG_BP_CONDITION:
		if (W.ctx_bp < W.nbp)
			prompt(PR_WF_CONDITION, "Break only when this is true (empty: always)", W.bpool.b + W.bp_cond[W.ctx_bp]);
		return true;
	case CMD_DBG_CLEAR_BPS: {
		char paths[64][1024];
		u32 np = 0;
		for (u32 i = 0; i < W.nbp && np < 64; i++)
			snprintf(paths[np++], sizeof paths[0], "%s", W.bpool.b + W.bp_path[i]);
		W.nbp = 0;
		for (u32 i = 0; i < np; i++)
			bp_send(paths[i]);
		return true;
	}
	case CMD_DBG_ADD_WATCH:
	case CMD_DBG_WATCHPOINT: {
		if (!v)
			return true;
		wf_init();
		Text *t = &app.docs.text[v->doc];
		char e[256] = "";
		u32 a = VX_MIN(v->head[0], v->anchor[0]), b = VX_MAX(v->head[0], v->anchor[0]);
		if (b > a && b - a < sizeof e) {
			text_copy(t, a, b - a, e);
			e[b - a] = 0;
		} else if (!expr_at(t, app.ctx == CTX_EDITOR ? app.ctx_off : v->head[0], e, sizeof e))
			e[0] = 0;
		if (!e[0]) {
			prompt(cmd == CMD_DBG_ADD_WATCH ? PR_WF_WATCH : PR_WF_WATCHPOINT,
			       cmd == CMD_DBG_ADD_WATCH ? "Expression to watch" : "Break when written: an lvalue", NULL);
			return true;
		}
		if (cmd == CMD_DBG_ADD_WATCH)
			watch_add(e);
		else if (W.dap && dap_state(W.dap) == DS_STOPPED) {
			dap_watch_add(W.dap, e, DW_WRITE);
			W.collapsed[S_WPTS] = false;
		} else
			wf_status("Watchpoints are set while the program is paused");
		return true;
	}
	case CMD_DBG_VAR_WATCH: watch_add(W.ctx_expr); return true;
	case CMD_DBG_VAR_COPY: ui_clipboard_set(W.ctx_value, strlen(W.ctx_value)); return true;
	case CMD_DBG_VAR_BREAK_W:
	case CMD_DBG_VAR_BREAK_R:
	case CMD_DBG_VAR_BREAK_RW:
		if (W.dap) {
			dap_watch_add(W.dap, W.ctx_expr,
			              cmd == CMD_DBG_VAR_BREAK_W ? DW_WRITE : cmd == CMD_DBG_VAR_BREAK_R ? DW_READ : DW_READWRITE);
			W.collapsed[S_WPTS] = false;
		}
		return true;
	case CMD_CMAKE_CONFIGURE: {
		wf_init();
		if (!W.cm.has_lists) {
			wf_status("No CMakeLists.txt in the project");
			return true;
		}
		char *argv[80], buf[16384];
		const char *defs[64];
		for (u32 i = 0; i < W.ndefs; i++)
			defs[i] = W.defs[i];
		cmake_query(W.bdir);
		cmake_configure_argv(&W.cm, W.preset, W.bdir, defs, W.ndefs, argv, 80, buf, sizeof buf);
		job_start(J_CONFIGURE, NX_NONE, argv, "cmake");
		return true;
	}
	case CMD_CMAKE_BUILD:
		wf_init();
		if (!W.cm.has_lists)
			wf_status("No CMakeLists.txt in the project");
		else
			build_then(NX_NONE);
		return true;
	case CMD_CMAKE_PRESET:
		wf_init();
		if (!W.cm.npre) {
			wf_status("No configure presets (CMakePresets.json)");
			return true;
		}
		pick_begin(PICK_WF_PRESET, "Configure preset", NULL);
		for (u32 i = 0; i < W.cm.npre; i++) {
			char bd[1100];
			cmake_build_dir(&W.cm, i, bd, sizeof bd);
			pick_add(cm_str(&W.cm, W.cm.pre_display[i]), bd, i);
		}
		pick_show();
		return true;
	case CMD_CMAKE_TARGET: {
		wf_init();
		u32 ex[64];
		u32 nx = cmake_executables(&W.cm, ex, 64);
		pick_begin(PICK_WF_TARGET, "Target to run or debug", NULL);
		for (u32 i = 0; i < nx; i++)
			pick_add(cm_str(&W.cm, W.cm.tgt_name[ex[i]]), cm_str(&W.cm, W.cm.tgt_path[ex[i]]), ex[i]);
		pick_add("Another program...", "type its path in Workflow > Run", NONE);
		pick_show();
		return true;
	}
	default: return false;
	}
}

void workflow_pick_accept(int kind, u32 val)
{
	if (kind == PICK_WF_PRESET && val < W.cm.npre) {
		W.preset = val;
		snprintf(W.want_preset, sizeof W.want_preset, "%s", cm_str(&W.cm, W.cm.pre_name[val]));
		cm_refresh();
	} else if (kind == PICK_WF_TARGET) {
		W.target = val < W.cm.ntgt ? val : NONE;
		snprintf(W.want_target, sizeof W.want_target, "%s",
		         W.target != NONE ? cm_str(&W.cm, W.cm.tgt_name[W.target]) : "*custom*");
		if (W.target == NONE) {
			W.field = F_PROG;
			show_pane();
			app.focus = FOCUS_SIDE_INPUT;
		}
	}
	app.dirty = true;
}

void workflow_prompt_accept(int kind, const char *text)
{
	switch (kind) {
	case PR_WF_OPTION:
		if (W.ctx_bp < W.cm.nent)
			set_def(cm_str(&W.cm, W.cm.ent_name[W.ctx_bp]), W.cm.ent_type[W.ctx_bp], text);
		break;
	case PR_WF_WATCH: watch_add(text); break;
	case PR_WF_WATCHPOINT:
	case PR_WF_WATCHPOINT_SW:
		if (W.dap && *text) {
			dap_watch_add_ex(W.dap, text, DW_WRITE, kind == PR_WF_WATCHPOINT_SW);
			W.collapsed[S_WPTS] = false;
		}
		break;
	case PR_WF_CONDITION:
		if (W.ctx_bp < W.nbp) {
			W.bp_cond[W.ctx_bp] = pool_add(&W.bpool, text, (u32)strlen(text));
			bp_send(W.bpool.b + W.bp_path[W.ctx_bp]);
		}
		break;
	}
	app.dirty = true;
}

/* ---- session --------------------------------------------------------------------------------- */

void workflow_session_save(void *file)
{
	FILE *f = file;
	for (u32 i = 0; i < W.nbp; i++)
		fprintf(f, "bp\t%u\t%d\t%s\t%s\n", W.bp_line[i], W.bp_on[i], W.bpool.b + W.bp_path[i], W.bpool.b + W.bp_cond[i]);
	for (u32 i = 0; i < W.nw; i++)
		fprintf(f, "wfwatch\t%s\n", W.wpool.b + W.w_expr[i]);
	if (W.want_preset[0])
		fprintf(f, "wfpreset\t%s\n", W.want_preset);
	if (W.want_target[0])
		fprintf(f, "wftarget\t%s\n", W.want_target);
	if (W.prog.n)
		fprintf(f, "wfprog\t%.*s\n", (int)W.prog.n, W.prog.b);
	if (W.args.n)
		fprintf(f, "wfargs\t%.*s\n", (int)W.args.n, W.args.b);
	fprintf(f, "wfflags\t%d\t%d\n", W.build_first, W.stop_entry);
	fprintf(f, "wfpane\t%d\t%d\n", app.right_visible, app.right_w);
}

bool workflow_session_line(const char *l)
{
	char b[4200];
	snprintf(b, sizeof b, "%s", l);
	size_t n = strlen(b);
	while (n && (b[n - 1] == '\n' || b[n - 1] == '\r'))
		b[--n] = 0;
	char *f[6] = {0};
	int k = 0;
	for (char *s = b; s && k < 6;) {
		f[k++] = s;
		s = strchr(s, '\t');
		if (s)
			*s++ = 0;
	}
	if (!strcmp(f[0], "bp") && k >= 4) {
		bp_add(f[3], (u32)atoi(f[1]), atoi(f[2]) != 0, k >= 5 ? f[4] : "");
		return true;
	}
	if (!strcmp(f[0], "wfwatch") && k >= 2) {
		watch_add(f[1]);
		return true;
	}
	if (!strcmp(f[0], "wfpreset") && k >= 2) {
		snprintf(W.want_preset, sizeof W.want_preset, "%s", f[1]);
		return true;
	}
	if (!strcmp(f[0], "wftarget") && k >= 2) {
		snprintf(W.want_target, sizeof W.want_target, "%s", f[1]);
		return true;
	}
	if (!strcmp(f[0], "wfprog") && k >= 2) {
		input_set(&W.prog, f[1], strlen(f[1]));
		return true;
	}
	if (!strcmp(f[0], "wfargs") && k >= 2) {
		input_set(&W.args, f[1], strlen(f[1]));
		return true;
	}
	if (!strcmp(f[0], "wfpane") && k >= 3) {
		app.right_visible = atoi(f[1]) != 0;
		app.right_w = atoi(f[2]);
		return true;
	}
	if (!strcmp(f[0], "wfflags") && k >= 3) {
		W.build_first = atoi(f[1]) != 0;
		W.stop_entry = atoi(f[2]) != 0;
		W.flags_loaded = true;
		return true;
	}
	return false;
}
