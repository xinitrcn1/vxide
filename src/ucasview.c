/* The ucas workspace: a REPL on ucas, the computer algebra system for
 * programmers (https://github.com/scarletnx/ucas), which is built into this
 * binary (cmake/ucas.cmake).
 *
 *   - the evaluator is vxide itself run as "ucas --json": ucas's own JSON
 *     frontend, in a process of its own (a long search can be cancelled, a
 *     crash or ucas's exit on a full heap cannot take the editor down)
 *   - every request runs under timeout_s, so ucas evaluates it in a forked
 *     child: Cancel (Esc) signals the group and only that child dies; the
 *     session's definitions stay (cmake/ucas-library.patch)
 *   - the transcript: each input with ucas's text, how far the answer is
 *     established (symbolic, exhaustive, SAT, numeric), its checks, notes and
 *     conditions, an ambiguity's readings as buttons, and its pictures
 *   - pictures (plots, surfaces, solids of revolution, shader runs: GIFs)
 *     inline, and the chosen one large in the View, animations playing
 *   - the session: the functions defined and the active cost model
 *   - English: "? sentence", or a line that does not parse and reads as one
 * Nothing here blocks the UI thread on ucas. */
#define _DEFAULT_SOURCE
#include "ui.h"
#include "ucasproto.h"
#include "ucasimg.h"
#include "syntax.h"
#include "theme.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <SDL3/SDL.h>

extern char **environ;

/* Under timeout_s ucas runs the request in a child it can kill: that is what
 * Cancel uses. A day: no real limit. */
#define TIMEOUT_S 86400.0
#define HIST_MAX 200
#define TEXT_LINES_MAX 600 /* of one answer, shown; Copy has all of it */

enum { CS_PENDING, CS_DONE };
enum { RQ_CELL, RQ_CELL_EN, RQ_STATE };

typedef struct {
	u32 src;      /* the input, in S.pool */
	bool english; /* asked as English ("? ...") */
	bool auto_en; /* English tried for it unasked (it reads as English, or did not parse) */
	bool sent_en; /* the answer shown is the English translator's */
	bool cancel_sent;
	int state;
	u64 t0, t1; /* running from / answered at */
	UcasResp r, alt; /* the answer; alt: the parse error while English is tried */
	u32 img0, nimg;
	int h, h_w; /* height at width h_w */
	/* a code block the statement is about (```glsl, ```spv, ```c ...) */
	bool has_code, code_open;
	u32 code, code_n, tag; /* in S.pool */
	int code_lang;         /* LANG_* for its colours */
	const char *ulang;     /* "glsl", "spv", "c" or NULL (static strings) */
	u8 file_open;          /* the returned source files shown (a bit each) */
	/* while it runs: the progress lines ucas writes to stderr (adamfit's
	 * "ucas: adamfit: ..."), the last LIVE_MAX of them */
	char *live[6];   /* candidates and notes, the latest */
	u32 nlive;
	char *prog;      /* the latest "... n/N shapes" line */
} Cell;
#define LIVE_MAX 6

typedef struct {
	u32 cell, file;
	int w, h;
	u8 *poster;     /* frame 0, RGBA */
	UcasImage all;  /* every frame, while it is the one in the View */
	u32 gen;        /* S.pic_gen when (u, v) is its place in the picture atlas */
	int u, v, up_frame;
	bool bad, tried;
} Img;

typedef struct {
	i64 id;
	int kind;
	u32 cell;
} Req;

static struct {
	bool inited;
	/* the worker */
	pid_t pid;
	int fin, fout, ferr;
	bool alive;
	char dead[400]; /* why it stopped, for the header */
	char *rx; /* stdout not yet a whole line */
	u32 rx_n, rx_cap;
	JBuf out;
	u32 out_off;
	char errlog[2048];
	u32 errlog_n;
	char eline[1024]; /* stderr's line being read */
	u32 eline_n;
	i64 next_id;
	Req q[256];
	u32 nq;
	bool state_queued;
	UcasResp tmp;
	/* the session */
	UcasResp state;
	/* the transcript */
	Pool pool;
	Cell *cells;
	u32 ncells, cells_cap;
	Img *img;
	u32 nimg, img_cap;
	i64 scroll; /* px of the transcript above the viewport */
	bool stick;  /* follow the end */
	i64 fn_scroll;
	/* the input and its history (newest last) */
	Input in;
	int in_top; /* the input's first row shown */
	u32 hist[HIST_MAX];
	u32 nhist;
	i64 hist_pos; /* nhist: not browsing */
	char *draft;  /* the unsent input while browsing */
	/* the View */
	i64 view;     /* an image, or -1 */
	bool playing;
	int frame;
	u64 next_at;
	/* the picture atlas: shelves */
	u32 pic_gen;
	int shelf_y[64], shelf_h[64], shelf_x[64];
	int nshelf, shelf_top;
} S;

/* ---- the worker process ------------------------------------------------------------------- */

#ifdef VXIDE_UCAS
static bool self_exe(char *buf, size_t cap)
{
#ifdef __FreeBSD__
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
	size_t len = cap;
	if (sysctl(mib, 4, buf, &len, NULL, 0) == 0 && len > 1)
		return true;
#endif
	ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
	if (n > 0) {
		buf[n] = 0;
		return true;
	}
	return false;
}
#endif

static void send_raw(void);
static i64 send_req(int kind, u32 cell, const char *cmd, const char *key, const char *text, size_t n,
                    const char *code, size_t cn, const char *lang, const char *akey, int aidx);

static void worker_start(void)
{
#ifndef VXIDE_UCAS
	snprintf(S.dead, sizeof S.dead, "this vxide was built without ucas (-DVXIDE_UCAS=OFF)");
	return;
#else
	if (S.alive)
		return;
	char exe[PATH_MAX];
	if (!self_exe(exe, sizeof exe)) {
		snprintf(S.dead, sizeof S.dead, "cannot find vxide's own binary to run ucas");
		return;
	}
	/* a dead worker's broken pipe is an error from write, not a signal */
	struct sigaction sa;
	if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
		sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sa, NULL);
	}
	int pi[2], po[2], pe[2];
	if (pipe(pi) != 0 || pipe(po) != 0 || pipe(pe) != 0) {
		snprintf(S.dead, sizeof S.dead, "pipe: %s", strerror(errno));
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pi[0], 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
	int ends[] = {pi[0], pi[1], po[0], po[1], pe[0], pe[1]};
	for (int i = 0; i < 6; i++)
		posix_spawn_file_actions_addclose(&fa, ends[i]);
#if defined(__FreeBSD__) || defined(__GLIBC__)
	/* relative paths (c optimize file.c, input file photo.jpg) are the project's */
	if (app.root)
		posix_spawn_file_actions_addchdir_np(&fa, app.root);
#endif
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	sigset_t none, def;
	sigemptyset(&none);
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	sigaddset(&def, SIGUSR1);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &def);
	/* its own process group: Cancel signals all of it (the server is a
	 * process ucas forked, whose pid we never learn) */
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	posix_spawnattr_setpgroup(&at, 0);
	/* argv[0] "ucas": the multi-call binary is ucas (main.c), and stays so
	 * when ucas's reset re-executes it */
	char *argv[] = {"ucas", "--json", "--files", "base64", NULL};
	int rc = posix_spawn(&S.pid, exe, &fa, &at, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(pi[0]);
	close(po[1]);
	close(pe[1]);
	if (rc != 0) {
		close(pi[1]);
		close(po[0]);
		close(pe[0]);
		snprintf(S.dead, sizeof S.dead, "cannot start ucas: %s", strerror(rc));
		return;
	}
	S.fin = pi[1];
	S.fout = po[0];
	S.ferr = pe[0];
	int fds[] = {S.fin, S.fout, S.ferr};
	for (int i = 0; i < 3; i++) {
		fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
		fcntl(fds[i], F_SETFD, FD_CLOEXEC);
	}
	S.alive = true;
	S.dead[0] = 0;
	S.rx_n = 0;
	S.errlog_n = 0;
	jb_reset(&S.out);
	S.out_off = 0;
	S.state_queued = true;
	send_req(RQ_STATE, 0, "state", NULL, NULL, 0, NULL, 0, NULL, NULL, 0);
#endif
}

static void finish_cell(u32 ci);

/* The worker is gone (or killed): what was asked of it is answered with why. */
static void worker_gone(const char *why)
{
	if (S.alive) {
		close(S.fin);
		close(S.fout);
		close(S.ferr);
		kill(-S.pid, SIGKILL);
		waitpid(S.pid, NULL, 0);
	}
	S.alive = false;
	S.state_queued = false;
	if (why)
		snprintf(S.dead, sizeof S.dead, "%s", why);
	for (u32 i = 0; i < S.nq; i++) {
		Req *r = &S.q[i];
		if (r->kind == RQ_STATE || r->cell >= S.ncells)
			continue;
		Cell *c = &S.cells[r->cell];
		if (c->state == CS_DONE)
			continue;
		ucas_parse(&c->r, "{}", 2);
		c->r.ok = false;
		c->r.err_code = pool_add(&c->r.str, "stopped", 7);
		char m[600];
		int k = snprintf(m, sizeof m, "%s", why ? why : "ucas stopped");
		if (S.errlog_n)
			snprintf(m + k, sizeof m - (size_t)k, "\n%.*s", (int)S.errlog_n, S.errlog);
		c->r.err_msg = pool_add(&c->r.str, m, (u32)strlen(m));
		finish_cell(r->cell);
	}
	S.nq = 0;
	ucas_resp_free(&S.state);
	app.dirty = true;
}

static i64 send_req(int kind, u32 cell, const char *cmd, const char *key, const char *text, size_t n,
                    const char *code, size_t cn, const char *lang, const char *akey, int aidx)
{
	if (!S.alive)
		worker_start();
	if (!S.alive || S.nq >= VX_COUNT(S.q))
		return -1;
	i64 id = ++S.next_id;
	ucas_request(&S.out, id, cmd, key, text, n, code, cn, lang, akey, aidx, TIMEOUT_S);
	S.q[S.nq++] = (Req){id, kind, cell};
	send_raw();
	return id;
}

static void send_raw(void)
{
	while (S.alive && S.out_off < S.out.n) {
		ssize_t w = write(S.fin, S.out.b + S.out_off, S.out.n - S.out_off);
		if (w > 0) {
			S.out_off += (u32)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		worker_gone("ucas stopped reading its requests");
		return;
	}
	if (S.out_off == S.out.n) {
		jb_reset(&S.out);
		S.out_off = 0;
	}
}

static bool running(void)
{
	for (u32 i = 0; i < S.nq; i++)
		if (S.q[i].kind != RQ_STATE)
			return true;
	return false;
}

static void cancel(void)
{
	if (!S.alive || !running())
		return;
	/* the server kills the request's child and answers "cancelled"; the
	 * waiter and the child ignore the signal (ucas-library.patch) */
	kill(-S.pid, SIGUSR1);
	for (u32 i = 0; i < S.nq; i++)
		if (S.q[i].kind != RQ_STATE && S.q[i].cell < S.ncells)
			S.cells[S.q[i].cell].cancel_sent = true;
	ui_status("Cancelling the evaluation (definitions are kept)");
}

/* ---- the transcript ---------------------------------------------------------------------------- */

static const char *cell_src(const Cell *c) { return S.pool.b + c->src; }

static void img_drop_frames(Img *im)
{
	ucasimg_free(&im->all);
	if (im->gen == S.pic_gen)
		im->up_frame = -1; /* the atlas may hold a later frame: put the poster back */
}

static void view_set(i64 k)
{
	if (S.view >= 0 && S.view < (i64)S.nimg && S.view != k)
		img_drop_frames(&S.img[S.view]);
	S.view = k;
	S.frame = 0;
	S.playing = true;
	S.next_at = 0;
	app.dirty = true;
}

static void live_clear(Cell *c);

static void finish_cell(u32 ci)
{
	Cell *c = &S.cells[ci];
	c->state = CS_DONE;
	c->t1 = vx_now_ns();
	live_clear(c); /* the answer's text has the same lines */
	c->h_w = 0;
	c->img0 = S.nimg;
	c->nimg = 0;
	for (u32 f = 0; f < c->r.nfiles; f++) {
		const char *mime = ucas_s(&c->r, c->r.files[f].mime);
		if (strncmp(mime, "image/", 6) != 0)
			continue;
		VX_GROW(S.img, S.img_cap, S.nimg + 1);
		Img *im = &S.img[S.nimg];
		memset(im, 0, sizeof *im);
		im->cell = ci;
		im->file = f;
		im->up_frame = -1;
		if (!ucasimg_size(c->r.files[f].data, c->r.files[f].n, &im->w, &im->h))
			im->bad = true;
		S.nimg++;
		c->nimg++;
	}
	if (c->nimg)
		view_set(c->img0 + c->nimg - 1); /* the newest picture */
	app.dirty = true;
}

static u32 new_cell(const char *s, size_t n, bool english)
{
	VX_GROW(S.cells, S.cells_cap, S.ncells + 1);
	Cell *c = &S.cells[S.ncells];
	memset(c, 0, sizeof *c);
	char *t = vx_alloc(n + 1); /* s may lie in S.pool, which pool_add can move */
	memcpy(t, s, n);
	c->src = pool_add(&S.pool, t, (u32)n);
	free(t);
	c->english = english;
	c->state = CS_PENDING;
	c->t0 = vx_now_ns();
	S.stick = true;
	return S.ncells++;
}

static void ask_cell(u32 ci, const char *akey, int aidx, bool en)
{
	Cell *c = &S.cells[ci];
	const char *s = cell_src(c);
	if (c->has_code)
		/* a statement with a code block: ucas's text frontend (the english
		 * command) takes them as the CLI does, ``` and all; statements it
		 * does not translate run as written */
		send_req(RQ_CELL_EN, ci, "english", "text", s, strlen(s), S.pool.b + c->code, c->code_n, c->ulang, akey,
		         aidx);
	else if (en)
		send_req(RQ_CELL_EN, ci, "english", "text", s, strlen(s), NULL, 0, NULL, akey, aidx);
	else
		send_req(RQ_CELL, ci, "ucas", "source", s, strlen(s), NULL, 0, NULL, akey, aidx);
	if (!S.alive) {
		/* it would not start: say so in the cell */
		ucas_parse(&c->r, "{}", 2);
		c->r.err_code = pool_add(&c->r.str, "stopped", 7);
		c->r.err_msg = pool_add(&c->r.str, S.dead, (u32)strlen(S.dead));
		finish_cell(ci);
	}
}

static void hist_add(const char *s, size_t n)
{
	if (S.nhist && !strcmp(S.pool.b + S.hist[S.nhist - 1], s))
		return;
	if (S.nhist == HIST_MAX) {
		memmove(S.hist, S.hist + 1, sizeof S.hist[0] * (HIST_MAX - 1));
		S.nhist--;
	}
	S.hist[S.nhist++] = pool_add(&S.pool, s, (u32)n);
}

static void submit(const char *s, size_t n)
{
	while (n && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
		s++, n--;
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
		n--;
	if (!n)
		return;
	/* tabs as four spaces: the code's columns, and the box's */
	u32 tabs = 0;
	for (size_t i = 0; i < n; i++)
		tabs += s[i] == '\t';
	char *t = vx_alloc(n + tabs * 3 + 1);
	size_t tn = 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\t')
			for (int k = 0; k < 4; k++)
				t[tn++] = ' ';
		else
			t[tn++] = s[i];
	hist_add(t, tn);
	S.hist_pos = S.nhist;
	free(S.draft);
	S.draft = NULL;
	/* statements, and statements with a code block: a cell each */
	UcasSeg seg[16];
	u32 ns = ucas_split(t, (u32)tn, seg, VX_COUNT(seg));
	for (u32 k = 0; k < ns; k++) {
		const char *b = seg[k].stmt;
		size_t bn = seg[k].stmt_n;
		while (bn && (*b == ' ' || *b == '\n' || *b == '\r'))
			b++, bn--;
		while (bn && (b[bn - 1] == ' ' || b[bn - 1] == '\n' || b[bn - 1] == '\r'))
			bn--;
		bool en = !seg[k].code && bn && b[0] == '?';
		if (en) {
			b++, bn--;
			while (bn && *b == ' ')
				b++, bn--;
		}
		u32 ci = new_cell(b, bn, en);
		Cell *c = &S.cells[ci];
		if (seg[k].code) {
			c->has_code = true;
			c->code_n = seg[k].code_n;
			c->code = pool_add(&S.pool, seg[k].code, seg[k].code_n);
			c->tag = pool_add(&S.pool, seg[k].tag, seg[k].tag_n);
			c->code_lang = seg[k].lang;
			if (c->code_lang == LANG_NONE) /* untagged: colours from what ucas would guess */
				c->code_lang = strstr(S.pool.b + c->code, "Op") && strchr(S.pool.b + c->code, '%') ? LANG_SPIRV
				                                                                                  : LANG_GLSL;
			c->ulang = seg[k].ucas_lang;
		}
		/* as ucas's own REPL does: a line that reads as English goes through
		 * the translator first (which runs statements as they are when it
		 * does not understand them); when it fails, the line runs as
		 * statements */
		c->auto_en = !en && !c->has_code && ucas_looks_english(cell_src(c), strlen(cell_src(c)));
		ask_cell(ci, NULL, 0, en || c->auto_en);
	}
	free(t);
}

static void sel_clear(void);

static void clear_transcript(void)
{
	if (running()) {
		ui_status("An evaluation is running: cancel it first (Esc)");
		return;
	}
	for (u32 i = 0; i < S.ncells; i++) {
		ucas_resp_free(&S.cells[i].r);
		ucas_resp_free(&S.cells[i].alt);
		live_clear(&S.cells[i]);
	}
	for (u32 i = 0; i < S.nimg; i++) {
		free(S.img[i].poster);
		ucasimg_free(&S.img[i].all);
	}
	S.ncells = S.nimg = 0;
	sel_clear();
	S.view = -1;
	S.scroll = 0;
	S.stick = true;
	S.nq = 0;
	/* the pool keeps the history: rebuild it with only that */
	Pool p = {0};
	for (u32 i = 0; i < S.nhist; i++) {
		const char *h = S.pool.b + S.hist[i];
		S.hist[i] = pool_add(&p, h, (u32)strlen(h));
	}
	pool_free(&S.pool);
	S.pool = p;
	app.dirty = true;
}

static void restart(void)
{
	worker_gone("restarted");
	S.dead[0] = 0;
	worker_start();
	ui_status("ucas restarted: the definitions are cleared");
}

/* ---- responses ------------------------------------------------------------------------------------ */

static bool is(const UcasResp *r, u32 off, const char *s) { return off != UCAS_NONE && !strcmp(r->str.b + off, s); }

static void handle_line(const char *line, u32 n)
{
	if (!ucas_parse(&S.tmp, line, n))
		return; /* not an answer (a stray line): nothing waits for it */
	u32 k = 0;
	while (k < S.nq && S.q[k].id != S.tmp.id)
		k++;
	if (k == S.nq)
		return;
	Req rq = S.q[k];
	memmove(S.q + k, S.q + k + 1, sizeof S.q[0] * (S.nq - k - 1));
	S.nq--;
	/* the next one runs from now */
	for (u32 i = 0; i < S.nq; i++)
		if (S.q[i].kind != RQ_STATE && S.q[i].cell < S.ncells) {
			S.cells[S.q[i].cell].t0 = vx_now_ns();
			break;
		}
	app.dirty = true;
	if (rq.kind == RQ_STATE) {
		UcasResp t = S.state;
		S.state = S.tmp;
		S.tmp = t;
		S.state_queued = false;
		return;
	}
	if (rq.cell >= S.ncells)
		return; /* cleared meanwhile */
	Cell *c = &S.cells[rq.cell];
	UcasResp t = c->r;
	c->r = S.tmp;
	S.tmp = t;
	const char *s = cell_src(c);
	if (rq.kind == RQ_CELL && !c->r.ok && is(&c->r, c->r.err_code, "parse_error") && !c->english && !c->auto_en &&
	    ucas_looks_english(s, strlen(s))) {
		/* statements that do not parse may still be English */
		UcasResp a = c->alt;
		c->alt = c->r;
		c->r = a;
		c->auto_en = true;
		send_req(RQ_CELL_EN, rq.cell, "english", "text", s, strlen(s), NULL, 0, NULL, NULL, 0);
		return;
	}
	if (rq.kind == RQ_CELL_EN && c->auto_en && !c->has_code && !c->r.ok) {
		if (c->alt.parsed) {
			/* not English either: the statements' own error says more */
			UcasResp a = c->r;
			c->r = c->alt;
			c->alt = a;
		} else {
			/* English first did not work: as statements, then */
			UcasResp a = c->alt;
			c->alt = c->r;
			c->r = a;
			send_req(RQ_CELL, rq.cell, "ucas", "source", s, strlen(s), NULL, 0, NULL, NULL, 0);
			return;
		}
	}
	c->sent_en = rq.kind == RQ_CELL_EN;
	finish_cell(rq.cell);
	if (!S.state_queued) {
		S.state_queued = true;
		send_req(RQ_STATE, 0, "state", NULL, NULL, 0, NULL, 0, NULL, NULL, 0);
	}
}

static void live_clear(Cell *c)
{
	for (u32 i = 0; i < c->nlive; i++)
		free(c->live[i]);
	c->nlive = 0;
	free(c->prog);
	c->prog = NULL;
}

/* a progress line for the cell being evaluated */
static void live_add(const char *l)
{
	for (u32 i = 0; i < S.nq; i++) {
		if (S.q[i].kind == RQ_STATE || S.q[i].cell >= S.ncells)
			continue;
		Cell *c = &S.cells[S.q[i].cell];
		while (*l == ' ')
			l++;
		if (!strncmp(l, "...", 3)) {
			/* progress: only the latest, under the candidates */
			free(c->prog);
			c->prog = vx_strdup(l);
			app.dirty = true;
			return;
		}
		if (!strncmp(l, "stage 2", 7)) {
			free(c->prog); /* stage 1's progress is over */
			c->prog = NULL;
		}
		if (c->nlive == LIVE_MAX) {
			free(c->live[0]);
			memmove(c->live, c->live + 1, sizeof c->live[0] * (LIVE_MAX - 1));
			c->nlive--;
		}
		c->live[c->nlive++] = vx_strdup(l);
		app.dirty = true;
		return;
	}
}

void ucasview_tick(void)
{
	if (!S.alive)
		return;
	send_raw();
	char b[65536];
	for (;;) {
		ssize_t r = read(S.fout, b, sizeof b);
		if (r > 0) {
			VX_GROW(S.rx, S.rx_cap, S.rx_n + (u32)r + 1);
			memcpy(S.rx + S.rx_n, b, (size_t)r);
			S.rx_n += (u32)r;
			continue;
		}
		if (r == 0) {
			worker_gone("ucas exited");
			return;
		}
		break;
	}
	u32 start = 0;
	for (u32 i = 0; i < S.rx_n; i++)
		if (S.rx[i] == '\n') {
			handle_line(S.rx + start, i - start);
			start = i + 1;
			if (!S.alive)
				return;
		}
	if (start) {
		memmove(S.rx, S.rx + start, S.rx_n - start);
		S.rx_n -= start;
	}
	ssize_t r;
	while ((r = read(S.ferr, b, sizeof b)) > 0) {
		/* progress lines for the running cell */
		for (ssize_t j = 0; j < r; j++) {
			if (b[j] != '\n') {
				if (S.eline_n < sizeof S.eline - 1)
					S.eline[S.eline_n++] = b[j];
				continue;
			}
			S.eline[S.eline_n] = 0;
			static const char pre[] = "ucas: adamfit: ";
			if (!strncmp(S.eline, pre, sizeof pre - 1))
				live_add(S.eline + sizeof pre - 1);
			S.eline_n = 0;
		}
		/* keep the tail: shown when ucas stops */
		u32 k = (u32)VX_MIN((size_t)r, sizeof S.errlog);
		if (S.errlog_n + k > sizeof S.errlog) {
			u32 drop = S.errlog_n + k - (u32)sizeof S.errlog;
			memmove(S.errlog, S.errlog + drop, S.errlog_n - drop);
			S.errlog_n -= drop;
		}
		memcpy(S.errlog + S.errlog_n, b + r - k, k);
		S.errlog_n += k;
	}
	int st;
	if (waitpid(S.pid, &st, WNOHANG) == S.pid) {
		char why[96];
		if (WIFSIGNALED(st))
			snprintf(why, sizeof why, "ucas was killed by signal %d", WTERMSIG(st));
		else
			snprintf(why, sizeof why, "ucas exited with status %d", WEXITSTATUS(st));
		S.alive = false; /* reaped: worker_gone must not wait for it */
		close(S.fin);
		close(S.fout);
		close(S.ferr);
		kill(-S.pid, SIGKILL);
		S.alive = true;
		S.fin = S.fout = S.ferr = -1;
		worker_gone(why);
	}
	/* the View's animation */
	if (app.side_view == SIDE_UCAS && S.playing && S.view >= 0 && S.img[S.view].all.nframes > 1 &&
	    vx_now_ns() >= S.next_at) {
		Img *im = &S.img[S.view];
		S.frame = (S.frame + 1) % im->all.nframes;
		S.next_at = vx_now_ns() + (u64)im->all.delay[S.frame] * 1000000ull;
		app.dirty = true;
	}
}

bool ucasview_busy(void) { return S.alive && S.nq > 0; }

u64 ucasview_wait_hint(void)
{
	if (app.side_view != SIDE_UCAS || !S.playing || S.view < 0 || S.img[S.view].all.nframes < 2)
		return 1000000;
	u64 now = vx_now_ns();
	return S.next_at > now ? (S.next_at - now) / 1000000ull + 1 : 1;
}

void ucasview_shutdown(void)
{
	if (S.alive) {
		close(S.fin);
		close(S.fout);
		close(S.ferr);
		kill(-S.pid, SIGKILL);
		waitpid(S.pid, NULL, 0);
		S.alive = false;
	}
}

/* ---- pictures ---------------------------------------------------------------------------------- */

static void shelves_reset(void)
{
	S.pic_gen++;
	S.nshelf = 0;
	S.shelf_top = 0;
	app.dirty = true; /* pictures drawn this frame may have lost their place */
}

static bool shelf_alloc(int w, int h, int *u, int *v)
{
	w += 2;
	h += 2; /* a texel apart: bilinear taps never reach a neighbour */
	if (w > PIC_SIZE || h > PIC_SIZE)
		return false;
	for (int i = 0; i < S.nshelf; i++)
		if (S.shelf_h[i] >= h && S.shelf_h[i] <= h + h / 3 + 8 && S.shelf_x[i] + w <= PIC_SIZE) {
			*u = S.shelf_x[i];
			*v = S.shelf_y[i];
			S.shelf_x[i] += w;
			return true;
		}
	if (S.shelf_top + h > PIC_SIZE || S.nshelf == (int)VX_COUNT(S.shelf_y))
		return false;
	int i = S.nshelf++;
	S.shelf_y[i] = S.shelf_top;
	S.shelf_h[i] = h;
	S.shelf_x[i] = w;
	S.shelf_top += h;
	*u = 0;
	*v = S.shelf_y[i];
	return true;
}

static const UcasFile *img_file(const Img *im) { return &S.cells[im->cell].r.files[im->file]; }

/* Make image k's frame drawable from the atlas; false when it cannot be. */
static bool pic_ready(u32 k, int frame)
{
	Img *im = &S.img[k];
	if (im->bad)
		return false;
	if (!im->poster) {
		if (im->tried)
			return false;
		im->tried = true;
		UcasImage one;
		const UcasFile *f = img_file(im);
		if (!ucasimg_decode(f->data, f->n, true, &one) || one.w > PIC_SIZE - 2 || one.h > PIC_SIZE - 2) {
			ucasimg_free(&one);
			im->bad = true;
			return false;
		}
		im->w = one.w;
		im->h = one.h;
		im->poster = one.rgba;
		free(one.delay);
	}
	if (frame && (!im->all.rgba || frame >= im->all.nframes))
		frame = 0;
	if (im->gen != S.pic_gen || !S.pic_gen) {
		if (!S.pic_gen)
			S.pic_gen = 1;
		if (!shelf_alloc(im->w, im->h, &im->u, &im->v)) {
			shelves_reset();
			if (!shelf_alloc(im->w, im->h, &im->u, &im->v))
				return false;
		}
		im->gen = S.pic_gen;
		im->up_frame = -1;
	}
	if (im->up_frame != frame) {
		u8 *dst = gpu_pic_pixels();
		const u8 *src = frame ? im->all.rgba + (size_t)frame * im->w * im->h * 4 : im->poster;
		for (int y = 0; y < im->h; y++)
			memcpy(dst + ((size_t)(im->v + y) * PIC_SIZE + (size_t)im->u) * 4, src + (size_t)y * im->w * 4,
			       (size_t)im->w * 4);
		gpu_pic_dirty(im->u, im->v, im->w, im->h);
		im->up_frame = frame;
	}
	return true;
}

/* every frame of the View's picture (an animation) */
static void view_frames(void)
{
	if (S.view < 0)
		return;
	Img *im = &S.img[S.view];
	if (im->all.rgba || im->bad)
		return;
	const UcasFile *f = img_file(im);
	if (f->n < 4 || memcmp(f->data, "GIF8", 4) != 0)
		return;
	if (!ucasimg_decode(f->data, f->n, false, &im->all) || im->all.w != im->w || im->all.h != im->h)
		ucasimg_free(&im->all);
	else if (im->all.nframes < 2)
		ucasimg_free(&im->all); /* a still: the poster is all of it */
	S.next_at = vx_now_ns() + (im->all.nframes > 1 ? (u64)im->all.delay[0] * 1000000ull : 0);
}

/* a returned file into the project, under a name not taken */
static void save_file(u32 ci, u32 fi)
{
	const UcasFile *f = &S.cells[ci].r.files[fi];
	const char *name = ucas_s(&S.cells[ci].r, f->name);
	if (!*name)
		name = "ucas.out";
	const char *dot = strrchr(name, '.');
	int bl = dot ? (int)(dot - name) : (int)strlen(name);
	char path[PATH_MAX];
	struct stat st;
	for (int i = 1; i < 1000; i++) {
		if (i == 1)
			snprintf(path, sizeof path, "%s/%s", app.root ? app.root : ".", name);
		else
			snprintf(path, sizeof path, "%s/%.*s-%d%s", app.root ? app.root : ".", bl, name, i, dot ? dot : "");
		if (stat(path, &st) != 0)
			break;
	}
	if (vx_write_file(path, (const char *)f->data, f->n) != 0)
		ui_status("Cannot write %s: %s", path, strerror(errno));
	else
		ui_status("Saved %s", path);
}

static void save_img(u32 k) { save_file(S.img[k].cell, S.img[k].file); }

static bool is_text_file(const UcasResp *r, const UcasFile *f)
{
	const char *m = ucas_s(r, f->mime);
	return !strncmp(m, "text/", 5) || memchr(f->data, 0, f->n) == NULL;
}

/* a returned source file (simplified.glsl ...) in an editor tab, read-only,
 * coloured by its name */
static void open_file_tab(u32 ci, u32 fi)
{
	const UcasResp *r = &S.cells[ci].r;
	const UcasFile *f = &r->files[fi];
	const char *name = ucas_s(r, f->name);
	int lang = syn_lang_for_path(name, (const char *)f->data, f->n);
	u32 d = doc_new_virtual(name, (const char *)f->data, f->n, lang);
	extern void show_doc_public(u32 d);
	if (d != UINT32_MAX)
		show_doc_public(d);
}

/* ---- drawing helpers ------------------------------------------------------------------------- */

enum {
	H_NONE,
	H_CANCEL,
	H_ASSUME,
	H_VIEW,
	H_SAVE,
	H_COPY,
	H_EDIT,
	H_EXAMPLE,
	H_PLAY,
	H_FIRST,
	H_SEEK,
	H_FUNC,
	H_CLEAR,
	H_RESTART,
	H_INPUT,
	H_CODE_TOGGLE,
	H_FILE_SHOW,
	H_FILE_OPEN,
	H_FILE_SAVE
};
typedef struct {
	int x, y, w, h, act;
	i64 arg;
} Hit;
static Hit hits[512];
static int nhit;

static bool in_r(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static void hit(int x, int y, int w, int h, int act, i64 arg)
{
	if (nhit < (int)VX_COUNT(hits))
		hits[nhit++] = (Hit){x, y, w, h, act, arg};
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

/* a small link-coloured word; returns the x after it */
static int ulink(int x, int y, const char *label, int act, i64 arg, const char *tip)
{
	int w = font_width(FONT_SMALL, label, strlen(label));
	int h = font_m[FONT_SMALL].line;
	bool hov = in_r(app.hover_x, app.hover_y, x, y, w, h);
	draw_text(FONT_SMALL, x, y, label, strlen(label), hov ? COL(C_FG) : COL(C_LINK));
	hit(x, y, w, h, act, arg);
	if (tip)
		ui_tip(x, y, w, h, tip);
	return x + w;
}

static int section(int x, int y, const char *t)
{
	draw_text(FONT_SMALL_BOLD, x, y, t, strlen(t), COL(C_SECTION_HEADER_FG));
	return y + font_m[FONT_SMALL_BOLD].line + PX(6);
}

static int mono_cw(void) { return VX_MAX(font_width(FONT_MONO, "M", 1), 1); }

/* The end of one display row of s[i..n) at most cols columns wide (a
 * newline ends it too); *next is where the next row starts. */
static u32 row_end(const char *s, u32 n, u32 i, int cols, u32 *next)
{
	int c = 0;
	u32 j = i;
	while (j < n && s[j] != '\n' && c < cols) {
		u32 cp;
		j += utf8_decode(s + j, n - j, &cp);
		c++;
	}
	*next = j < n && s[j] == '\n' ? j + 1 : j;
	return j;
}

static int rows_of(const char *s, u32 n, int cols)
{
	if (!n)
		return 0;
	int r = 0;
	for (u32 i = 0; i < n;) {
		u32 nx;
		row_end(s, n, i, cols, &nx);
		r++;
		if (nx == i)
			break;
		i = nx;
	}
	return r;
}

/* the colour of one line of ucas's text */
static u32 line_col(const char *s, u32 n, bool first)
{
	while (n && *s == ' ')
		s++, n--;
	if (n >= 2 && s[0] == '>' && s[1] == ' ')
		return COL(C_DESCRIPTION_FG); /* the statement, echoed */
	if (n >= 5 && !memcmp(s, "note:", 5))
		return COL(C_DESCRIPTION_FG);
	if (n >= 6 && (!memcmp(s, "error:", 6) || memmem(s, n, ": error:", 8)))
		return COL(C_ERROR);
	(void)first;
	return COL(C_EDITOR_FG);
}

/* Syntax colours (syntax.c, the editor's lexers) for one logical line,
 * then row [rs, re) of it painted at x, y, column by column. */
typedef struct {
	u32 start[256], len[256];
	u8 cls[256];
	u32 n;
} HlLine;

static u32 hl_lex(HlLine *h, int lang, u32 state, const char *l, u32 n)
{
	SynSpans o = {h->start, h->len, h->cls, 0, 256};
	state = syn_line(lang, state, l, n, &o);
	h->n = o.count;
	return state;
}

static void hl_row(const HlLine *h, int x, int y, const char *l, u32 rs, u32 re, u32 dflt)
{
	int cw = mono_cw();
	const u32 *tok = themes[app.theme].tok;
	u32 pos = rs;
	int col = 0;
	for (u32 k = 0; k <= h->n && pos < re; k++) {
		/* the gap before span k (plain text), then span k */
		u32 ss = k < h->n ? h->start[k] : re, se = k < h->n ? h->start[k] + h->len[k] : re;
		for (int part = 0; part < 2; part++) {
			u32 a = part ? VX_MAX(ss, pos) : pos, b = part ? VX_MIN(se, re) : VX_MIN(ss, re);
			if (b <= a)
				continue;
			u32 c = part ? tok[h->cls[k < h->n ? k : 0]] : dflt;
			int font = FONT_MONO;
			if (part && k < h->n) {
				u8 st = tk_style[h->cls[k]];
				font = (st & TS_BOLD) ? FONT_MONO_BOLD : (st & TS_ITALIC) ? FONT_MONO_ITALIC : FONT_MONO;
			}
			draw_text(font, x + col * cw, y, l + a, b - a, c);
			for (u32 j = a; j < b;) {
				u32 cp;
				j += utf8_decode(l + j, b - j, &cp);
				col++;
			}
			pos = b;
		}
	}
}

/* Wrapped mono text; draws when draw, returns the height. At most max_lines
 * display rows (*cut set when there were more). */
/* The lexer for a line of ucas's text, beyond its own colours: a SPIR-V
 * instruction ("%30 = OpExtInst ..."), a line of a diff it shows (the GLSL
 * view: "--- original.glsl" ... "+    v = x;"), or of the C it wrote (under
 * "optimised C:"), indented as ucas indents. */
static int text_lang(const char *s, u32 n, u32 *ind, bool *in_diff, u32 *diff_ind, int *sec, u32 *sec_ind)
{
	u32 k = 0;
	while (k < n && s[k] == ' ')
		k++;
	*ind = k;
	const char *t = s + k;
	u32 tn = n - k;
	if (*sec != LANG_NONE) {
		if (tn && k > *sec_ind)
			return *sec;
		*sec = LANG_NONE;
	}
	if (tn == 12 && !memcmp(t, "optimised C:", 12)) {
		*sec = LANG_C; /* the lines below it, indented further */
		*sec_ind = k;
		return LANG_NONE;
	}
	if (tn >= 4 && !memcmp(t, "--- ", 4)) {
		*in_diff = true;
		*diff_ind = k;
		return LANG_DIFF;
	}
	if (*in_diff) {
		if (!tn || k < *diff_ind)
			*in_diff = false;
		else {
			*ind = *diff_ind; /* + and - in the diff's own column */
			return LANG_DIFF;
		}
	}
	if (tn > 1 && t[0] == '%') {
		u32 e = 1;
		while (e < tn && t[e] != ' ')
			e++;
		if (e + 5 < tn && !memcmp(t + e, " = Op", 5))
			return LANG_SPIRV;
	}
	if (tn > 3 && t[0] == 'O' && t[1] == 'p' && t[2] >= 'A' && t[2] <= 'Z')
		return LANG_SPIRV;
	return LANG_NONE;
}

/* ---- selecting the transcript's text ----------------------------------------------------------
 *
 * Each row of text a frame paints is kept with what it shows: its cell, the
 * region (the input, its code block, ucas's text, an error's message) and
 * the bytes [a, b) of that region's text.  A selection is two positions
 * (cell, region, byte) in the transcript's order; rows inside it get the
 * selection colour under their text, and Ctrl+C copies it. */

enum { RG_NONE, RG_SRC, RG_CODE, RG_TEXT, RG_ERR };
typedef struct {
	u32 cell;
	u8 rg;
	u32 off;
} TPos;
typedef struct {
	u32 cell;
	u8 rg;
	u32 a, b;
	int x, y;
} TRow;
static TRow trow[4096];
static int ntrow;
static u32 cur_cell;
static u8 cur_rg; /* what mono_text / code_text paint now */
static struct {
	bool on, drag;
	TPos a, h; /* anchor, head */
} SEL;

static int tpos_cmp(TPos x, TPos y)
{
	if (x.cell != y.cell)
		return x.cell < y.cell ? -1 : 1;
	if (x.rg != y.rg)
		return x.rg < y.rg ? -1 : 1;
	return x.off < y.off ? -1 : x.off > y.off;
}

static bool sel_any(void) { return SEL.on && tpos_cmp(SEL.a, SEL.h) != 0; }

static void sel_clear(void)
{
	SEL.on = SEL.drag = false;
}

/* a region's text and length ("" when the cell has none) */
static const char *region_str(u32 ci, int rg, u32 *n)
{
	const Cell *c = &S.cells[ci];
	const char *t = "";
	*n = 0;
	if (rg == RG_SRC)
		t = cell_src(c);
	else if (rg == RG_CODE && c->has_code) {
		*n = c->code_n;
		return S.pool.b + c->code;
	} else if (rg == RG_TEXT && c->state == CS_DONE) {
		t = ucas_s(&c->r, c->r.text);
		u32 k = (u32)strlen(t);
		while (k && t[k - 1] == '\n')
			k--;
		*n = k;
		return t;
	} else if (rg == RG_ERR && c->state == CS_DONE && !c->r.ok)
		t = ucas_s(&c->r, c->r.err_msg);
	*n = (u32)strlen(t);
	return t;
}

/* columns from a to b of s */
static int ncols(const char *s, u32 a, u32 b)
{
	int c = 0;
	for (u32 j = a; j < b;) {
		u32 cp;
		j += utf8_decode(s + j, b - j, &cp);
		c++;
	}
	return c;
}

/* a row being painted: kept, and the selection under it */
static void sel_row(int x, int y, const char *s, u32 a, u32 b)
{
	if (cur_rg == RG_NONE)
		return;
	if (ntrow < (int)VX_COUNT(trow))
		trow[ntrow++] = (TRow){cur_cell, cur_rg, a, b, x, y};
	if (!sel_any())
		return;
	TPos lo = SEL.a, hi = SEL.h;
	if (tpos_cmp(lo, hi) > 0) {
		TPos t = lo;
		lo = hi;
		hi = t;
	}
	TPos ra = {cur_cell, cur_rg, a}, rb = {cur_cell, cur_rg, b};
	if (tpos_cmp(hi, ra) <= 0 || tpos_cmp(lo, rb) > 0)
		return;
	u32 sa = tpos_cmp(lo, ra) > 0 ? lo.off : a, sb = tpos_cmp(hi, rb) < 0 ? hi.off : b;
	int cw = mono_cw(), lh = font_m[FONT_MONO].line;
	int c0 = ncols(s, a, sa), c1 = ncols(s, a, sb);
	if (tpos_cmp(hi, rb) > 0)
		c1++; /* the line break is selected too */
	if (c1 > c0)
		draw_rect(x + c0 * cw, y, (c1 - c0) * cw, lh, COL(C_SELECTION));
}

/* the position under the pointer: the row there, else the end of the row
 * above it, else the first row's start */
static bool pos_at(int mx, int my, TPos *p)
{
	int lh = font_m[FONT_MONO].line, cw = mono_cw(), above = -1;
	for (int i = 0; i < ntrow; i++) {
		const TRow *r = &trow[i];
		if (my >= r->y && my < r->y + lh && mx >= r->x - PX(24)) {
			u32 n, j = r->a;
			const char *s = region_str(r->cell, r->rg, &n);
			int col = mx <= r->x ? 0 : (mx - r->x + cw / 2) / cw;
			for (int c = 0; c < col && j < r->b; c++) {
				u32 cp;
				j += utf8_decode(s + j, r->b - j, &cp);
			}
			*p = (TPos){r->cell, r->rg, j};
			return true;
		}
		if (r->y + lh <= my && (above < 0 || r->y >= trow[above].y))
			above = i;
	}
	if (above >= 0) {
		*p = (TPos){trow[above].cell, trow[above].rg, trow[above].b};
		return true;
	}
	if (ntrow) {
		*p = (TPos){trow[0].cell, trow[0].rg, trow[0].a};
		return true;
	}
	return false;
}

static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '%'; }

/* double click: the word; triple: the line */
static void sel_unit(TPos p, int clicks)
{
	u32 n;
	const char *s = region_str(p.cell, p.rg, &n);
	u32 a = p.off, b = p.off;
	if (clicks >= 3) {
		while (a > 0 && s[a - 1] != '\n')
			a--;
		while (b < n && s[b] != '\n')
			b++;
	} else {
		while (a > 0 && word_ch(s[a - 1]))
			a--;
		while (b < n && word_ch(s[b]))
			b++;
	}
	SEL.a = (TPos){p.cell, p.rg, a};
	SEL.h = (TPos){p.cell, p.rg, b};
	SEL.on = true;
}

static void sel_all(void)
{
	if (!S.ncells)
		return;
	u32 n;
	SEL.a = (TPos){0, RG_SRC, 0};
	region_str(S.ncells - 1, RG_ERR, &n);
	SEL.h = (TPos){S.ncells - 1, RG_ERR, n};
	SEL.on = true;
}

/* the selected text: each region's part, regions on lines of their own */
static void sel_copy(void)
{
	TPos lo = SEL.a, hi = SEL.h;
	if (tpos_cmp(lo, hi) > 0) {
		TPos t = lo;
		lo = hi;
		hi = t;
	}
	JBuf b = {0};
	bool first = true;
	for (u32 ci = lo.cell; ci <= hi.cell && ci < S.ncells; ci++)
		for (int rg = RG_SRC; rg <= RG_ERR; rg++) {
			TPos ra = {ci, (u8)rg, 0};
			u32 n;
			const char *s = region_str(ci, rg, &n);
			TPos rb = {ci, (u8)rg, n};
			if (!n || tpos_cmp(rb, lo) <= 0 || tpos_cmp(ra, hi) >= 0)
				continue;
			u32 a = tpos_cmp(lo, ra) > 0 ? lo.off : 0, e = tpos_cmp(hi, rb) < 0 ? hi.off : n;
			if (e <= a)
				continue;
			if (!first)
				jb_raw(&b, "\n");
			jb_rawn(&b, s + a, e - a);
			first = false;
		}
	if (b.n) {
		ui_clipboard_set(b.b, b.n);
		ui_status("Copied %u characters from the ucas transcript", b.n);
	}
	jb_free(&b);
}

static int mono_text(int x, int y, int cols, const char *s, u32 n, bool paint, int max_lines, bool *cut)
{
	int lh = font_m[FONT_MONO].line;
	int row = 0;
	bool first = true, in_diff = false;
	u32 diff_ind = 0, sec_ind = 0;
	int sec = LANG_NONE;
	static HlLine hl;
	if (cut)
		*cut = false;
	for (u32 i = 0; i < n;) {
		/* a logical line: its colour (or its lexer's), then its rows */
		u32 le = i;
		while (le < n && s[le] != '\n')
			le++;
		u32 col = paint ? line_col(s + i, le - i, first) : 0;
		int font = first ? FONT_MONO_BOLD : FONT_MONO;
		int lang = LANG_NONE;
		if (paint && !first) {
			u32 ind;
			lang = text_lang(s + i, le - i, &ind, &in_diff, &diff_ind, &sec, &sec_ind);
			if (lang != LANG_NONE) {
				hl_lex(&hl, lang, 0, s + i + ind, le - i - ind);
				for (u32 k = 0; k < hl.n; k++)
					hl.start[k] += i + ind;
			}
		}
		u32 j = i;
		do {
			if (row >= max_lines) {
				if (cut)
					*cut = true;
				return row * lh;
			}
			u32 nx;
			u32 e = row_end(s, le, j, cols, &nx);
			if (paint && y + row * lh < draw.y1 && y + (row + 1) * lh > draw.y0) {
				sel_row(x, y + row * lh, s, j, e);
				if (lang != LANG_NONE)
					hl_row(&hl, x, y + row * lh, s, j, e, col);
				else
					draw_text(font, x, y + row * lh, s + j, e - j, col);
			}
			row++;
			j = nx == j ? le : nx;
		} while (j < le);
		first = false;
		i = le + 1;
	}
	return row * lh;
}

/* Code with its colours, wrapped at cols; at most max_rows rows. Its height
 * (painted when paint). */
static int code_text(int x, int y, int cols, int lang, const char *s, u32 n, bool paint, int max_rows, int *rows_out)
{
	int lh = font_m[FONT_MONO].line, row = 0;
	u32 state = 0;
	static HlLine h;
	for (u32 i = 0; i <= n && row < max_rows;) {
		u32 le = i;
		while (le < n && s[le] != '\n')
			le++;
		if (paint)
			state = hl_lex(&h, lang, state, s + i, le - i);
		u32 j = i;
		do {
			if (row >= max_rows)
				break;
			u32 nx, e = row_end(s, le, j, cols, &nx);
			if (paint && y + row * lh < draw.y1 && y + (row + 1) * lh > draw.y0) {
				sel_row(x, y + row * lh, s, j, e);
				/* spans are relative to the line */
				HlLine sh = h;
				for (u32 k = 0; k < sh.n; k++)
					sh.start[k] += i;
				hl_row(&sh, x, y + row * lh, s, j, e, COL(C_EDITOR_FG));
			}
			row++;
			j = nx == j ? le : nx;
		} while (j < le);
		if (le >= n)
			break;
		i = le + 1;
	}
	if (rows_out)
		*rows_out = row;
	return row * lh;
}

static int count_rows(const char *s, u32 n, int cols)
{
	int r = 0;
	code_text(0, 0, cols, LANG_NONE, s, n, false, 1 << 30, &r);
	return r;
}

#define CODE_ROWS 14   /* of a code block shown before "Show all" */
#define FILE_ROWS 40   /* of a returned file shown inline */

/* A code box: a header (the language, its size, links), the code coloured.
 * Its height. */
static int code_box(int x, int y, int w, const char *label, int lang, const char *s, u32 n, bool open, int max_open,
                    bool paint, int act_toggle, i64 arg, int act2, const char *act2_label, int act3,
                    const char *act3_label)
{
	int cw = mono_cw(), lh = font_m[FONT_MONO].line, sh = font_m[FONT_SMALL].line;
	int cols = VX_MAX((w - PX(16)) / cw, 8);
	int total = count_rows(s, n, cols);
	int lim = open ? max_open : CODE_ROWS;
	int shown = VX_MIN(total, lim);
	int hh = sh + PX(8);
	int bh = hh + shown * lh + PX(8);
	if (paint) {
		draw_rect(x, y, w, bh, COL(C_PANEL_BG));
		draw_frame(x, y, w, bh, COL(C_PANEL_BORDER));
		char t[160];
		u32 lines = 1;
		for (u32 i = 0; i < n; i++)
			lines += s[i] == '\n';
		snprintf(t, sizeof t, "%s  \xC2\xB7  %u line%s", label, lines, lines == 1 ? "" : "s");
		int lx = draw_text(FONT_SMALL, x + PX(8), y + PX(4), t, strlen(t), COL(C_DESCRIPTION_FG)) + PX(12);
		if (total > CODE_ROWS && act_toggle) {
			char m[48];
			if (open)
				snprintf(m, sizeof m, "Collapse");
			else
				snprintf(m, sizeof m, "Show all %d rows", total);
			lx = ulink(lx, y + PX(4), m, act_toggle, arg, NULL) + PX(12);
		}
		if (act2)
			lx = ulink(lx, y + PX(4), act2_label, act2, arg, NULL) + PX(12);
		if (act3)
			ulink(lx, y + PX(4), act3_label, act3, arg, NULL);
		/* inside the box and the transcript's clip, then that clip again */
		int ox0 = draw.x0, oy0 = draw.y0, ox1 = draw.x1, oy1 = draw.y1;
		draw.x0 = VX_MAX(ox0, x + PX(1));
		draw.y0 = VX_MAX(oy0, y + hh);
		draw.x1 = VX_MIN(ox1, x + w - PX(1));
		draw.y1 = VX_MIN(oy1, y + hh + shown * lh + PX(6));
		if (draw.x0 < draw.x1 && draw.y0 < draw.y1)
			code_text(x + PX(8), y + hh + PX(2), cols, lang, s, n, true, shown, NULL);
		draw.x0 = ox0;
		draw.y0 = oy0;
		draw.x1 = ox1;
		draw.y1 = oy1;
		if (total > shown)
			draw_rect(x + PX(1), y + bh - PX(3), w - PX(2), PX(2), COL(C_FOCUS_BORDER));
	}
	return bh;
}

/* ---- a cell ----------------------------------------------------------------------------------------- */

static const char *proof_label(int k)
{
	switch (k) {
	case UCAS_PROOF_SYMBOLIC: return "symbolic";
	case UCAS_PROOF_EXHAUSTIVE: return "exhaustive";
	case UCAS_PROOF_SAT: return "proved by SAT";
	case UCAS_PROOF_NUMERIC: return "numeric, not a proof";
	}
	return NULL;
}

static int chip(int x, int y, const char *t, u32 tint, const char *tip)
{
	int h = font_m[FONT_SMALL].line + PX(4), w = font_width(FONT_SMALL, t, strlen(t)) + PX(12);
	draw_rect(x, y, w, h, (tint & 0xFFFFFF00u) | 0x38u);
	draw_text(FONT_SMALL, x + PX(6), y + PX(2), t, strlen(t), COL(C_FG));
	if (tip)
		ui_tip(x, y, w, h, tip);
	return x + w + PX(6);
}

/* the size a picture is shown at in a box of w x h (never past its own
 * size times the UI scale) */
static void fit(const Img *im, int w, int h, int *dw, int *dh)
{
	int iw = VX_MAX(im->w, 1), ih = VX_MAX(im->h, 1);
	double s = VX_MIN((double)w / iw, (double)h / ih);
	s = VX_MIN(s, (double)app.scale);
	*dw = VX_MAX((int)(iw * s), 1);
	*dh = VX_MAX((int)(ih * s), 1);
}

static void draw_pic(u32 k, int frame, int x, int y, int dw, int dh)
{
	Img *im = &S.img[k];
	if (pic_ready(k, frame))
		draw_picture(x, y, dw, dh, im->u, im->v, im->w, im->h, 255);
	else {
		draw_rect(x, y, dw, dh, COL(C_INPUT_BG));
		const char *m = im->bad ? "cannot show this picture (Save writes the file)" : "";
		draw_text_fit(FONT_SMALL, x + PX(8), y + PX(8), m, strlen(m), dw - PX(16), COL(C_DESCRIPTION_FG));
	}
}

/* Lays out (and when draw, draws) cell ci at x, y, w wide; its height. */
static int cell_layout(u32 ci, int x, int y, int w, bool paint)
{
	Cell *c = &S.cells[ci];
	UcasResp *r = &c->r;
	int cw = mono_cw(), lh = font_m[FONT_MONO].line, sh = font_m[FONT_SMALL].line;
	int y0 = y;
	y += PX(8);
	/* the input: a prompt, the source, and on the right its time or state */
	int pw = cw * 2;
	int meta_w = PX(150);
	int cols = VX_MAX((w - pw - meta_w) / cw, 8);
	const char *src = cell_src(c);
	u32 sn = (u32)strlen(src);
	int rows = VX_MAX(rows_of(src, sn, cols), 1);
	if (paint) {
		const char *pr = c->english ? "?" : "\xE2\x80\xBA"; /* › */
		draw_text(FONT_MONO_BOLD, x, y, pr, strlen(pr), COL(C_LINK));
		cur_cell = ci;
		cur_rg = RG_SRC;
		mono_text(x + pw, y, cols, src, sn, true, 1000, NULL);
		cur_rg = RG_NONE;
		char meta[96];
		int mx = x + w;
		if (c->state == CS_PENDING) {
			bool head = false;
			for (u32 i = 0; i < S.nq; i++)
				if (S.q[i].kind != RQ_STATE) {
					head = S.q[i].cell == ci;
					break;
				}
			if (head && !c->cancel_sent) {
				mx = ulink(mx - font_width(FONT_SMALL, "Cancel", 6), y, "Cancel", H_CANCEL, 0,
				          "Stop this evaluation (Esc); definitions are kept") -
				     font_width(FONT_SMALL, "Cancel", 6) - PX(10);
			}
			snprintf(meta, sizeof meta, "%s %.1f s", head ? (c->cancel_sent ? "cancelling" : "running") : "queued",
			         head ? (double)(vx_now_ns() - c->t0) / 1e9 : 0.0);
			if (!head)
				snprintf(meta, sizeof meta, "queued");
		} else {
			double t = (double)(c->t1 - c->t0) / 1e9;
			snprintf(meta, sizeof meta, t < 0.01 ? "%.3f s" : t < 10 ? "%.2f s" : "%.1f s", t);
			int ex = mx - font_width(FONT_SMALL, "Edit", 4);
			ulink(ex, y, "Edit", H_EDIT, ci, "Put this input back in the box");
			mx = ex - PX(10);
			if (r->text != UCAS_NONE && *ucas_s(r, r->text)) {
				int cx = mx - font_width(FONT_SMALL, "Copy", 4);
				ulink(cx, y, "Copy", H_COPY, ci, "Copy ucas's answer as text");
				mx = cx - PX(10);
			}
		}
		int mw = font_width(FONT_SMALL, meta, strlen(meta));
		draw_text(FONT_SMALL, mx - mw, y, meta, strlen(meta), COL(C_DESCRIPTION_FG));
	}
	y += rows * lh + PX(4);
	/* its code block, coloured by the editor's lexers */
	if (c->has_code) {
		const char *tag = S.pool.b + c->tag;
		char lb[64];
		snprintf(lb, sizeof lb, "%s%s%s", syn_lang_name(c->code_lang), *tag ? "  ```" : "", tag);
		cur_cell = ci;
		cur_rg = RG_CODE;
		y += code_box(x + pw, y, w - pw, lb, c->code_lang, S.pool.b + c->code, c->code_n, c->code_open, 2000, paint,
		              H_CODE_TOGGLE, ci, 0, NULL, 0, NULL) +
		     PX(6);
		cur_rg = RG_NONE;
	}
	/* English: ucas's text says what it was read as ("ucas: ..." lines) */
	if (c->state != CS_DONE) {
		/* what ucas says while it works (adamfit's candidates as found) */
		for (u32 i = 0; i < c->nlive; i++) {
			if (paint) {
				const char *l = c->live[i];
				draw_text_fit(FONT_MONO, x + pw, y, l, strlen(l), w - pw,
				              i + 1 == c->nlive ? COL(C_EDITOR_FG) : COL(C_DESCRIPTION_FG));
			}
			y += lh;
		}
		if (c->prog) {
			if (paint)
				draw_text_fit(FONT_MONO, x + pw, y, c->prog, strlen(c->prog), w - pw, COL(C_DESCRIPTION_FG));
			y += lh;
		}
		return y - y0 + PX(8);
	}
	int ox = x + pw, ow = w - pw;
	int ocols = VX_MAX(ow / cw, 8);
	/* ucas's text */
	const char *txt = ucas_s(r, r->text);
	u32 tn = (u32)strlen(txt);
	while (tn && txt[tn - 1] == '\n')
		tn--;
	if (tn) {
		bool cut;
		cur_cell = ci;
		cur_rg = RG_TEXT;
		y += mono_text(ox, y, ocols, txt, tn, paint, TEXT_LINES_MAX, &cut) + PX(4);
		cur_rg = RG_NONE;
		if (cut) {
			if (paint)
				ulink(ox, y, "The rest is cut here: Copy has all of it", H_COPY, ci, NULL);
			y += sh + PX(4);
		}
	}
	/* an error (not when ucas's text shows it already, with its caret) */
	if (!r->ok && r->err_code != UCAS_NONE && !(r->nread == 0 && *ucas_s(r, r->err_msg) && strstr(txt, ucas_s(r, r->err_msg)))) {
		const char *code = ucas_s(r, r->err_code), *msg = ucas_s(r, r->err_msg), *how = ucas_s(r, r->err_how);
		/* ucas's hint speaks of the JSON request (args.assume ...): here the
		 * readings are buttons */
		if (r->nread)
			how = "Pick a reading to run it again with it:";
		else if (strstr(how, "args."))
			how = "";
		char head[300];
		if (r->err_line)
			snprintf(head, sizeof head, "%s (line %d, column %d)", code, r->err_line, r->err_col);
		else
			snprintf(head, sizeof head, "%s", code);
		u32 mn = (u32)strlen(msg);
		int bh = PX(8) + font_m[FONT_UI_BOLD].line + rows_of(msg, mn, ocols - 2) * lh + PX(8);
		u32 hn = (u32)strlen(how);
		int hrows = hn ? VX_MIN(rows_of(how, hn, ocols - 2), 6) : 0;
		bh += hrows * lh + (hrows ? PX(4) : 0);
		bh += r->nread ? (int)r->nread * (PX(24) + PX(4)) + PX(4) : 0;
		if (paint) {
			draw_rect(ox, y, ow, bh, (COL(C_ERROR) & 0xFFFFFF00u) | 0x22u);
			draw_rect(ox, y, PX(2), bh, COL(C_ERROR));
			int iy = y + PX(8);
			draw_text(FONT_UI_BOLD, ox + PX(10), iy, head, strlen(head), COL(C_ERROR));
			iy += font_m[FONT_UI_BOLD].line;
			cur_cell = ci;
			cur_rg = RG_ERR;
			iy += mono_text(ox + PX(10), iy, ocols - 2, msg, mn, true, 1000, NULL);
			cur_rg = RG_NONE;
			if (hrows) {
				iy += PX(4);
				for (int rr = 0, i = 0; rr < hrows && (u32)i < hn; rr++) {
					u32 nx, e = row_end(how, hn, (u32)i, ocols - 2, &nx);
					draw_text(FONT_MONO, ox + PX(10), iy, how + i, e - (u32)i, COL(C_DESCRIPTION_FG));
					iy += lh;
					i = (int)nx;
				}
			}
			if (r->nread)
				iy += PX(4);
			for (u32 i = 0; i < r->nread; i++) {
				char lb[400];
				snprintf(lb, sizeof lb, "%d. %s%s", r->read_index[i], ucas_s(r, r->read_text[i]),
				         r->read_default[i] ? "  (the default)" : "");
				button(ox + PX(10), iy, lb, H_ASSUME, (i64)ci * 16 + i, i == 0, "Run it again with this reading");
				iy += PX(24) + PX(4);
			}
		}
		y += bh + PX(6);
	}
	/* pictures */
	for (u32 k = c->img0; k < c->img0 + c->nimg; k++) {
		Img *im = &S.img[k];
		if (!im->w || !im->h) {
			im->w = 320;
			im->h = 40;
		}
		int dw, dh;
		fit(im, VX_MIN(ow, PX(560)), PX(300), &dw, &dh);
		if (paint && y + dh > draw.y0 && y < draw.y1) {
			bool v = (i64)k == S.view;
			draw_pic(k, v ? S.frame : 0, ox, y, dw, dh);
			draw_frame(ox - PX(1), y - PX(1), dw + PX(2), dh + PX(2), v ? COL(C_FOCUS_BORDER) : COL(C_PANEL_BORDER));
			hit(ox, y, dw, dh, H_VIEW, k);
			ui_tip(ox, y, dw, dh, v ? "In the View" : "Show in the View");
		}
		y += dh + PX(4);
		if (paint) {
			const UcasFile *f = img_file(im);
			char cap[200];
			snprintf(cap, sizeof cap, "%s  %d\xC3\x97%d  %u KB", ucas_s(r, f->name), im->w, im->h, (f->n + 1023) / 1024);
			int ex = draw_text(FONT_SMALL, ox, y, cap, strlen(cap), COL(C_DESCRIPTION_FG)) + PX(12);
			ulink(ex, y, "Save", H_SAVE, k, "Write the file into the project");
		}
		y += sh + PX(8);
	}
	/* source files it returned (simplified.glsl, simplified.spvasm, the C
	 * rewritten ...): Show (coloured here), Open (an editor tab), Save */
	for (u32 f = 0; f < r->nfiles && f < 8; f++) {
		const UcasFile *uf = &r->files[f];
		if (!strncmp(ucas_s(r, uf->mime), "image/", 6) || !is_text_file(r, uf))
			continue;
		const char *name = ucas_s(r, uf->name);
		bool open = c->file_open & (1u << f);
		if (open) {
			int lang = syn_lang_for_path(name, (const char *)uf->data, uf->n);
			char lb[200];
			snprintf(lb, sizeof lb, "%s  %s", name, syn_lang_name(lang));
			y += code_box(ox, y, ow, lb, lang, (const char *)uf->data, uf->n, true, FILE_ROWS, paint, 0, (i64)ci * 16 + f,
			              H_FILE_SHOW, "Hide", H_FILE_OPEN, "Open in an editor") +
			     PX(4);
			if (paint)
				ulink(ox, y, "Save", H_FILE_SAVE, (i64)ci * 16 + f, "Write the file into the project");
			y += sh + PX(8);
			continue;
		}
		if (paint) {
			char cap[200];
			snprintf(cap, sizeof cap, "%s  %u KB", name, (uf->n + 1023) / 1024);
			int ex = draw_text(FONT_SMALL, ox, y, cap, strlen(cap), COL(C_DESCRIPTION_FG)) + PX(12);
			ex = ulink(ex, y, "Show", H_FILE_SHOW, (i64)ci * 16 + f, "Show it here, coloured") + PX(10);
			ex = ulink(ex, y, "Open", H_FILE_OPEN, (i64)ci * 16 + f, "Open it in an editor tab") + PX(10);
			ulink(ex, y, "Save", H_FILE_SAVE, (i64)ci * 16 + f, "Write the file into the project");
		}
		y += sh + PX(6);
	}
	/* proof, checks, conditions */
	const char *pl = proof_label(r->proof);
	if (pl || r->nchecks || r->nconds) {
		if (paint) {
			int cx = ox;
			if (pl)
				cx = chip(cx, y, pl, r->proof == UCAS_PROOF_NUMERIC ? COL(C_WARNING) : COL(C_GIT_ADDED),
				          ucas_s(r, r->proof_text));
			if (r->nchecks) {
				u32 bad = 0;
				char tip[1500];
				u32 k = 0;
				tip[0] = 0;
				for (u32 i = 0; i < r->nchecks; i++) {
					bad += !r->check_passed[i];
					if (k < sizeof tip - 8)
						k += (u32)snprintf(tip + k, sizeof tip - k, "%s%s %s", i ? "\n" : "",
						                   r->check_passed[i] ? "\xE2\x9C\x93" : "\xE2\x9C\x97", ucas_s(r, r->check_text[i]));
				}
				char t[64];
				if (bad)
					snprintf(t, sizeof t, "\xE2\x9C\x97 %u of %u checks failed", bad, r->nchecks);
				else
					snprintf(t, sizeof t, "\xE2\x9C\x93 %u check%s passed", r->nchecks, r->nchecks == 1 ? "" : "s");
				cx = chip(cx, y, t, bad ? COL(C_ERROR) : COL(C_GIT_ADDED), tip);
			}
			if (r->nconds) {
				char t[600];
				u32 k = (u32)snprintf(t, sizeof t, "holds where ");
				for (u32 i = 0; i < r->nconds && k < sizeof t - 4; i++)
					k += (u32)snprintf(t + k, sizeof t - k, "%s%s", i ? ", " : "", ucas_s(r, r->cond[i]));
				draw_text_fit(FONT_SMALL, cx, y + PX(2), t, strlen(t), ox + ow - cx, COL(C_DESCRIPTION_FG));
			}
		}
		y += sh + PX(4) + PX(6);
	}
	/* notes that are not in the text already */
	for (u32 i = 0; i < r->nnotes; i++) {
		const char *t = ucas_s(r, r->note_text[i]);
		if (strstr(txt, t))
			continue;
		if (paint) {
			char b[700];
			snprintf(b, sizeof b, "%s: %s", ucas_s(r, r->note_kind[i]), t);
			char *j = strstr(b, " (args.");
			if (j)
				*j = 0; /* the JSON request's key: not this REPL's */
			draw_text_fit(FONT_SMALL, ox, y, b, strlen(b), ow, COL(C_DESCRIPTION_FG));
			ui_tip(ox, y, ow, sh, b);
		}
		y += sh + PX(2);
	}
	return y - y0 + PX(8);
}

static int cell_h(u32 ci, int w)
{
	Cell *c = &S.cells[ci];
	if (c->state == CS_DONE && c->h_w == w)
		return c->h;
	int h = cell_layout(ci, 0, 0, w, false);
	if (c->state == CS_DONE) {
		c->h = h;
		c->h_w = w;
	}
	return h;
}

/* ---- the input box ------------------------------------------------------------------------------ */

static struct {
	int x, y, w, h, cols; /* the text area */
	int tr_x, tr_y, tr_w, tr_h; /* the transcript */
	int fn_x, fn_y, fn_w, fn_h; /* the functions list */
	int sl_x, sl_y, sl_w;       /* the frame slider */
} G;

static int input_rows(int cols)
{
	if (!S.in.b)
		input_set(&S.in, "", 0);
	return VX_CLAMP(rows_of(S.in.b, S.in.n, cols) + (S.in.n && S.in.b[S.in.n - 1] == '\n'), 1, 16);
}

/* row and column of byte offset off in the wrapped input */
static void input_rc(u32 off, int cols, int *row, int *col)
{
	int r = 0;
	u32 i = 0;
	const char *s = S.in.b;
	u32 n = S.in.n;
	for (;;) {
		u32 nx, e = row_end(s, n, i, cols, &nx);
		if (off <= e && (off < e || nx == e || s[e] == '\n' || nx >= n)) {
			int c = 0;
			for (u32 j = i; j < off;) {
				u32 cp;
				j += utf8_decode(s + j, n - j, &cp);
				c++;
			}
			*row = r;
			*col = c;
			return;
		}
		if (nx >= n && off >= n) {
			*row = r + (n && s[n - 1] == '\n');
			*col = 0;
			return;
		}
		r++;
		i = nx;
	}
}

/* byte offset at row, column */
static u32 input_off(int row, int col, int cols)
{
	const char *s = S.in.b;
	u32 n = S.in.n, i = 0;
	for (int r = 0; r < row; r++) {
		u32 nx;
		row_end(s, n, i, cols, &nx);
		if (nx >= n)
			return n;
		i = nx;
	}
	u32 nx, e = row_end(s, n, i, cols, &nx);
	u32 j = i;
	for (int c = 0; c < col && j < e; c++) {
		u32 cp;
		j += utf8_decode(s + j, n - j, &cp);
	}
	return j;
}

static void draw_input(int x, int y, int w, int h, bool focused)
{
	int cw = mono_cw(), lh = font_m[FONT_MONO].line;
	draw_rect(x, y, w, h, COL(C_INPUT_BG));
	draw_frame(x, y, w, h, focused ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
	int tx = x + PX(8) + cw * 2, tyy = y + PX(6);
	int cols = VX_MAX((w - PX(16) - cw * 2) / cw, 8);
	G.x = tx;
	G.y = tyy;
	G.w = cols * cw;
	G.h = h - PX(12);
	G.cols = cols;
	hit(x, y, w, h, H_INPUT, 0);
	const char *pr = S.in.n && S.in.b[0] == '?' ? "?" : "\xE2\x80\xBA";
	draw_text(FONT_MONO_BOLD, x + PX(8), tyy, pr, strlen(pr), COL(C_LINK));
	draw_clip(x + PX(1), y + PX(1), w - PX(2), h - PX(2));
	if (!S.in.n) {
		const char *ph = "ucas statements, ```glsl / ```spv / ```c blocks after one (Enter runs), or ? and English";
		draw_text_fit(FONT_MONO, tx, tyy, ph, strlen(ph), cols * cw, COL(C_INPUT_PLACEHOLDER));
	}
	/* selection, text, caret */
	u32 a = VX_MIN(S.in.cur, S.in.anchor), b = VX_MAX(S.in.cur, S.in.anchor);
	/* the rows shown: the caret's always among them */
	int vis = VX_MAX((h - PX(12)) / lh, 1), cr, cc;
	input_rc(S.in.cur, cols, &cr, &cc);
	if (cr < S.in_top)
		S.in_top = cr;
	if (cr >= S.in_top + vis)
		S.in_top = cr - vis + 1;
	S.in_top = VX_MAX(S.in_top, 0);
	/* fenced code coloured as it is typed (the editor's lexers) */
	enum { L_STMT, L_FENCE, L_CODE };
	int kind = L_STMT, lang = LANG_NONE;
	bool in_fence = false;
	u32 state = 0;
	static HlLine hl;
	u32 i = 0;
	for (int r = 0; i <= S.in.n; r++) {
		if (i == 0 || S.in.b[i - 1] == '\n') {
			u32 le = i;
			while (le < S.in.n && S.in.b[le] != '\n')
				le++;
			const char *tag;
			u32 tn;
			if (!in_fence && ucas_fence_open(S.in.b + i, le - i, &tag, &tn)) {
				kind = L_FENCE;
				in_fence = true;
				lang = tn ? syn_block_lang(tag, tn) : LANG_NONE;
				if (lang == LANG_NONE)
					lang = LANG_GLSL;
				state = 0;
			} else if (in_fence && ucas_fence_close(S.in.b + i, le - i)) {
				kind = L_FENCE;
				in_fence = false;
			} else if (in_fence) {
				kind = L_CODE;
				state = hl_lex(&hl, lang, state, S.in.b + i, le - i);
				for (u32 k = 0; k < hl.n; k++)
					hl.start[k] += i; /* relative to the whole input */
			} else
				kind = L_STMT;
		}
		u32 nx, e = row_end(S.in.b, S.in.n, i, cols, &nx);
		int yy = tyy + (r - S.in_top) * lh;
		if (b > a && a < e + 1 && b > i) {
			int c0 = 0, c1 = 0;
			for (u32 j = i; j < e && j < a;) {
				u32 cp;
				j += utf8_decode(S.in.b + j, S.in.n - j, &cp);
				c0++;
			}
			for (u32 j = i; j < e && j < b;) {
				u32 cp;
				j += utf8_decode(S.in.b + j, S.in.n - j, &cp);
				c1++;
			}
			if (b > e)
				c1++;
			draw_rect(tx + c0 * cw, yy, (c1 - c0) * cw, lh, COL(C_SELECTION));
		}
		if (kind == L_CODE)
			hl_row(&hl, tx, yy, S.in.b, i, e, COL(C_INPUT_FG));
		else
			draw_text(FONT_MONO, tx, yy, S.in.b + i, e - i, kind == L_FENCE ? COL(C_DESCRIPTION_FG) : COL(C_INPUT_FG));
		if (nx >= S.in.n || nx == i)
			break;
		i = nx;
	}
	if (focused)
		draw_rect(tx + cc * cw, tyy + (cr - S.in_top) * lh, PX(2), lh, COL(C_CURSOR));
	draw_noclip();
}

/* ---- the workspace ------------------------------------------------------------------------------ */

static const char *examples[] = {
    "plot sin x, cos x",
    "range (x: u8) / 3",
    "avg(x: u32, y: u32) = (x & y) + ((x xor y) >> 1)",
    "equiv avg u32((u64(x) + u64(y)) >> 1)",
    "integrate x^2 sin(x) from 0 to pi",
    "plot3d sin(x) * cos(y)",
    "revolve x^2 from 0 to 1",
    "find (x: u32 in 0..1000) * 4 % 16 3",
    "? the area under x^2 from 0 to 4",
    "adamfit sin(x) in 0..1.5707963 count 600",
    "simplify\n```comp\n#version 450\nlayout(local_size_x = 64) in;\nlayout(std430, binding = 0) buffer B { uint v[]; };\n"
    "void main()\n{\n    uint i = gl_GlobalInvocationID.x;\n    uint x = v[i] & 255u;\n"
    "    v[i] = min(x, 300u) * 8u / 4u;\n}\n```",
    "c optimize nofile\n```c\nunsigned avg(unsigned x, unsigned y)\n{\n    return (unsigned)(((unsigned long long)x "
    "+ y) >> 1);\n}\n```",
    "animate exec of this seed 1\n```comp\n#version 450\nlayout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0, rgba8) uniform writeonly image2D img;\nvoid main()\n{\n"
    "    vec2 p = vec2(gl_GlobalInvocationID.xy) / 64.0;\n"
    "    imageStore(img, ivec2(gl_GlobalInvocationID.xy), vec4(p, 0.5 + 0.5 * sin(p.x * 9.0), 1));\n}\n```",
};

static void draw_welcome(int x, int y, int w)
{
	int cy = y + PX(8);
	draw_text(FONT_UI_BOLD, x, cy, "A computer algebra system for programmers", 41, COL(C_FG));
	cy += font_m[FONT_UI_BOLD].line + PX(6);
	const char *p[] = {
	    "Prove integer formulas equal on their whole domain (enumerated or by SAT), find where an expression wraps,",
	    "get exact ranges, search cheaper equivalent forms under CPU and GPU cost models, and do calculus, algebra,",
	    "number theory and transforms. Plots, surfaces and solids of revolution show here and in the View.",
	};
	for (size_t i = 0; i < VX_COUNT(p); i++) {
		draw_text_fit(FONT_UI, x, cy, p[i], strlen(p[i]), w, COL(C_DESCRIPTION_FG));
		cy += font_m[FONT_UI].line + PX(2);
	}
	cy += PX(12);
	cy = section(x, cy, "TRY");
	for (size_t i = 0; i < VX_COUNT(examples); i++) {
		int h = font_m[FONT_MONO].line + PX(6);
		const char *e = examples[i];
		/* a block: its statement and what the block is */
		char lb[120];
		const char *fence = strstr(e, "\n```");
		if (fence) {
			const char *tg = fence + 4;
			snprintf(lb, sizeof lb, "%.*s  + a ```%.*s block", (int)(fence - e), e, (int)strcspn(tg, "\n"), tg);
			e = lb;
		}
		int tw = font_width(FONT_MONO, e, strlen(e)) + PX(16);
		bool hov = in_r(app.hover_x, app.hover_y, x, cy, tw, h);
		draw_rect(x, cy, tw, h, hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
		draw_text(FONT_MONO, x + PX(8), cy + PX(3), e, strlen(e), COL(C_EDITOR_FG));
		hit(x, cy, tw, h, H_EXAMPLE, (i64)i);
		ui_tip(x, cy, tw, h, fence ? examples[i] : "Run it");
		cy += h + PX(4);
	}
	cy += PX(10);
	const char *k = "Esc cancels a running evaluation (definitions are kept), Up/Down walk the history, Ctrl+L "
	                "clears. In an editor, \"ucas: Run Selection or Line in the ucas Tab\" sends code here.";
	draw_text_fit(FONT_SMALL, x, cy, k, strlen(k), w, COL(C_DESCRIPTION_FG));
}

static void draw_transcript(int x, int y, int w, int h)
{
	ntrow = 0; /* the rows this frame paints, for the selection */
	G.tr_x = x;
	G.tr_y = y;
	G.tr_w = w;
	G.tr_h = h;
	if (!S.ncells) {
		draw_clip(x, y, w, h);
		draw_welcome(x, y, w);
		draw_noclip();
		return;
	}
	i64 total = 0;
	for (u32 i = 0; i < S.ncells; i++)
		total += cell_h(i, w);
	i64 maxs = VX_MAX(total - h, 0);
	if (S.stick)
		S.scroll = maxs;
	S.scroll = VX_CLAMP(S.scroll, 0, maxs);
	draw_clip(x, y, w, h);
	i64 cy = -S.scroll;
	for (u32 i = 0; i < S.ncells; i++) {
		int ch = cell_h(i, w);
		if (cy + ch > 0 && cy < h)
			cell_layout(i, x, y + (int)cy, w, true);
		cy += ch;
		if (i + 1 < S.ncells && cy > 0 && cy < h)
			draw_rect(x, y + (int)cy - PX(1), w, PX(1), COL(C_PANEL_BORDER));
	}
	draw_noclip();
	/* a scrollbar when it does not fit */
	if (total > h) {
		int bh = VX_MAX((int)((i64)h * h / total), PX(20));
		int by = y + (int)((h - bh) * S.scroll / VX_MAX(maxs, 1));
		draw_rect(x + w + PX(4), by, PX(4), bh, COL(C_SCROLL_SLIDER));
	}
}

static void draw_view(int x, int y, int w, int h)
{
	int cy = section(x, y, "VIEW");
	if (S.view < 0 || S.view >= (i64)S.nimg) {
		const char *m = "Plots, surfaces and animations show here.";
		draw_text_fit(FONT_UI, x, cy + PX(4), m, strlen(m), w, COL(C_DESCRIPTION_FG));
		return;
	}
	view_frames();
	Img *im = &S.img[S.view];
	int ch = h - (cy - y) - PX(40);
	int dw, dh;
	pic_ready((u32)S.view, 0); /* its size */
	fit(im, w, ch, &dw, &dh);
	int px = x + (w - dw) / 2;
	draw_rect(x, cy, w, dh, COL(C_PANEL_BG));
	draw_pic((u32)S.view, S.frame, px, cy, dw, dh);
	cy += dh + PX(8);
	/* the animation's controls */
	int n = VX_MAX(im->all.nframes, 1);
	int bx = x;
	if (n > 1) {
		bx += button(bx, cy, S.playing ? "Pause" : "Play", H_PLAY, 0, false, "Play or pause the animation") + PX(6);
		bx += button(bx, cy, "First", H_FIRST, 0, false, "The first frame") + PX(10);
		char fr[32];
		snprintf(fr, sizeof fr, "%d / %d", S.frame + 1, n);
		int fw = font_width(FONT_SMALL, fr, strlen(fr));
		int sw = VX_MAX(x + w - bx - fw - PX(70), PX(40));
		/* the slider: click to seek */
		int sy = cy + PX(11);
		draw_rect(bx, sy, sw, PX(2), COL(C_INPUT_BORDER));
		int kx = bx + (int)((i64)sw * S.frame / VX_MAX(n - 1, 1));
		draw_rect(kx - PX(3), sy - PX(5), PX(6), PX(12), COL(C_FOCUS_BORDER));
		G.sl_x = bx;
		G.sl_y = cy;
		G.sl_w = sw;
		hit(bx, cy, sw, PX(24), H_SEEK, 0);
		bx += sw + PX(10);
		draw_text(FONT_SMALL, bx, ty(cy, PX(24), FONT_SMALL), fr, strlen(fr), COL(C_DESCRIPTION_FG));
		bx += fw + PX(10);
	}
	button(x + w - font_width(FONT_UI, "Save", 4) - PX(20), cy, "Save", H_SAVE, S.view, false,
	       "Write the file into the project");
}

static void draw_session(int x, int y, int w, int h)
{
	int cy = section(x, y, "SESSION");
	const UcasResp *st = &S.state;
	char m[160];
	snprintf(m, sizeof m, "Cost model: %s", st->model != UCAS_NONE ? ucas_s(st, st->model) : "-");
	draw_text_fit(FONT_UI, x, cy, m, strlen(m), w, COL(C_FG));
	ui_tip(x, cy, w, font_m[FONT_UI].line, "find and cost price with it; change it with: use zen4 (x86 models lists them)");
	cy += font_m[FONT_UI].line + PX(8);
	int bh = PX(24) + PX(8);
	int lh = font_m[FONT_MONO].line + PX(4);
	int lt = cy, lhgt = y + h - bh - lt;
	G.fn_x = x;
	G.fn_y = lt;
	G.fn_w = w;
	G.fn_h = lhgt;
	if (!st->nfuncs) {
		const char *t = "No functions yet: f(x: u8) = x * 3 defines one.";
		draw_text_fit(FONT_SMALL, x, cy, t, strlen(t), w, COL(C_DESCRIPTION_FG));
	} else {
		int vis = VX_MAX(lhgt / lh, 1);
		S.fn_scroll = VX_CLAMP(S.fn_scroll, 0, VX_MAX((i64)st->nfuncs - vis, 0));
		draw_clip(x, lt, w, lhgt);
		for (int r = 0; r < vis && (u32)(S.fn_scroll + r) < st->nfuncs; r++) {
			u32 i = (u32)S.fn_scroll + (u32)r;
			int ry = lt + r * lh;
			char b[600];
			snprintf(b, sizeof b, "%s = %s", ucas_s(st, st->func_sig[i]), ucas_s(st, st->func_body[i]));
			bool hov = in_r(app.hover_x, app.hover_y, x, ry, w, lh);
			if (hov)
				draw_rect(x, ry, w, lh, COL(C_LIST_HOVER_BG));
			draw_text_fit(FONT_MONO, x + PX(4), ry + PX(2), b, strlen(b), w - PX(8), COL(C_EDITOR_FG));
			hit(x, ry, w, lh, H_FUNC, i);
			ui_tip(x, ry, w, lh, b);
		}
		draw_noclip();
	}
	int by = y + h - PX(24);
	int bx = x;
	bx += button(bx, by, "Clear Output", H_CLEAR, 0, false, "Clear the transcript (Ctrl+L); definitions stay") + PX(6);
	button(bx, by, "Restart ucas", H_RESTART, 0, false, "A fresh ucas: every definition is cleared");
}

void ucasview_open(void)
{
	if (!S.inited) {
		S.inited = true;
		S.view = -1;
		S.stick = true;
		S.hist_pos = S.nhist;
	}
	if (!S.alive)
		worker_start();
	app.focus = FOCUS_MAN_INPUT;
	app.dirty = true;
}

void ucasview_ws_draw(int x, int y, int w, int h)
{
	nhit = 0;
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	/* header */
	int hh = PX(52);
	draw_icon(ICON_SIGMA_S, x + PX(24), y + PX(18), COL(C_FG));
	draw_text(FONT_UI_BOLD, x + PX(48), y + PX(10), "ucas", 4, COL(C_FG));
	const char *sub = "Computer algebra for programmers: proofs, ranges, costs, calculus and plots";
	draw_text_fit(FONT_SMALL, x + PX(48), y + PX(28), sub, strlen(sub), w / 2, COL(C_DESCRIPTION_FG));
	{
		char st[480];
		u32 tint = COL(C_GIT_ADDED);
		if (!S.alive && S.dead[0]) {
			snprintf(st, sizeof st, "stopped: %s", S.dead);
			tint = COL(C_ERROR);
		} else if (!S.alive)
			snprintf(st, sizeof st, "not started");
		else if (running()) {
			snprintf(st, sizeof st, "evaluating");
			tint = COL(C_WARNING);
		} else
			snprintf(st, sizeof st, "ready");
		int sw = VX_MIN(font_width(FONT_SMALL, st, strlen(st)), w / 3);
		draw_rect(x + w - sw - PX(36), y + PX(14), sw + PX(16), PX(22), (tint & 0xFFFFFF00u) | 0x40u);
		draw_text_fit(FONT_SMALL, x + w - sw - PX(28), ty(y + PX(14), PX(22), FONT_SMALL), st, strlen(st), sw,
		              COL(C_FG));
		ui_tip(x + w - sw - PX(36), y + PX(14), sw + PX(16), PX(22), st);
	}
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	y += hh;
	h -= hh;
	int pad = PX(20), gap = PX(24);
	bool side = w >= PX(900);
	int rw = side ? VX_CLAMP((w - 2 * pad - gap) * 2 / 5, PX(320), PX(760)) : 0;
	int lw = w - 2 * pad - (side ? rw + gap : 0);
	int lx = x + pad;
	/* the input at the bottom of the left column */
	int cw = mono_cw();
	int cols = VX_MAX((lw - PX(16) - cw * 2) / cw, 8);
	int ih = input_rows(cols) * font_m[FONT_MONO].line + PX(12);
	int iy = y + h - pad - ih;
	draw_transcript(lx, y + PX(12), lw - PX(10), iy - PX(12) - (y + PX(12)));
	draw_input(lx, iy, lw, ih, app.focus == FOCUS_MAN_INPUT || app.focus == FOCUS_WORKSPACE);
	if (side) {
		int rx = x + w - pad - rw;
		draw_rect(rx - gap / 2, y, PX(1), h, COL(C_PANEL_BORDER));
		int vh = (h - 2 * PX(12)) * 3 / 5;
		draw_view(rx, y + PX(12), rw, vh);
		draw_session(rx, y + PX(12) + vh + PX(16), rw, h - vh - PX(40) - pad + PX(12));
	}
}

/* ---- input --------------------------------------------------------------------------------------- */

/* Insert at the caret over the selection, newlines kept (Input's own
 * input_text is single-line); \r dropped. */
static void input_put(const char *s, size_t n)
{
	if (!S.in.b)
		input_set(&S.in, "", 0);
	u32 a = VX_MIN(S.in.cur, S.in.anchor), b = VX_MAX(S.in.cur, S.in.anchor);
	char *t = vx_alloc(S.in.n - (b - a) + n + 1);
	if (memchr(s, '\t', n))
		t = realloc(t, S.in.n + n * 4 + 1);
	memcpy(t, S.in.b, a);
	u32 k = a;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\t') {
			for (int j = 0; j < 4; j++) /* a tab: four spaces (room made above) */
				t[k++] = ' ';
		} else if (s[i] != '\r')
			t[k++] = s[i];
	u32 at = k;
	memcpy(t + k, S.in.b + b, S.in.n - b);
	k += S.in.n - b;
	input_set(&S.in, t, k);
	S.in.cur = S.in.anchor = at;
	free(t);
}

/* Where the input stands: inside a code block not yet closed (*fence), and
 * the brackets left open in its statements (not in its code) */
static void input_state(bool *fence, int *depth)
{
	*fence = false;
	*depth = 0;
	const char *s = S.in.b;
	u32 n = S.in.n;
	for (u32 i = 0; i < n;) {
		u32 e = i;
		while (e < n && s[e] != '\n')
			e++;
		const char *tag;
		u32 tn;
		if (!*fence && ucas_fence_open(s + i, e - i, &tag, &tn))
			*fence = true;
		else if (*fence && ucas_fence_close(s + i, e - i))
			*fence = false;
		else if (!*fence)
			*depth += ucas_open_depth(s + i, e - i);
		i = e + 1;
	}
}

static void hist_go(int d)
{
	if (!S.nhist)
		return;
	if (S.hist_pos == (i64)S.nhist && d < 0) {
		free(S.draft);
		S.draft = vx_alloc(S.in.n + 1);
		memcpy(S.draft, S.in.b, S.in.n);
	}
	i64 p = VX_CLAMP(S.hist_pos + d, 0, (i64)S.nhist);
	if (p == S.hist_pos)
		return;
	S.hist_pos = p;
	const char *t = p == (i64)S.nhist ? (S.draft ? S.draft : "") : S.pool.b + S.hist[p];
	input_set(&S.in, t, strlen(t));
	S.in.cur = S.in.anchor = S.in.n;
}

static void run_input(void)
{
	if (!S.in.b || !S.in.n)
		return;
	char *t = vx_alloc(S.in.n + 1);
	memcpy(t, S.in.b, S.in.n);
	size_t n = S.in.n;
	input_set(&S.in, "", 0);
	submit(t, n);
	free(t);
}

static void do_hit(const Hit *h, const Event *e)
{
	switch (h->act) {
	case H_CANCEL: cancel(); break;
	case H_ASSUME: {
		u32 ci = (u32)(h->arg / 16), ri = (u32)(h->arg % 16);
		if (ci >= S.ncells || running())
			break;
		Cell *c = &S.cells[ci];
		if (ri >= c->r.nread)
			break;
		char key[64];
		snprintf(key, sizeof key, "%s", ucas_s(&c->r, c->r.err_key));
		int idx = c->r.read_index[ri];
		bool en = c->sent_en;
		u32 nc = new_cell(cell_src(c), strlen(cell_src(c)), c->english);
		c = &S.cells[ci]; /* new_cell may move the cells */
		Cell *d = &S.cells[nc];
		d->auto_en = c->auto_en;
		d->has_code = c->has_code;
		d->code = c->code;
		d->code_n = c->code_n;
		d->tag = c->tag;
		d->code_lang = c->code_lang;
		d->ulang = c->ulang;
		ask_cell(nc, key, idx, en);
		break;
	}
	case H_VIEW: view_set(h->arg); break;
	case H_SAVE:
		if ((u64)h->arg < S.nimg)
			save_img((u32)h->arg);
		break;
	case H_COPY: {
		const UcasResp *r = &S.cells[h->arg].r;
		const char *t = ucas_s(r, r->text);
		ui_clipboard_set(t, strlen(t));
		ui_status("Copied ucas's answer");
		break;
	}
	case H_EDIT: {
		const Cell *c = &S.cells[h->arg];
		const char *tag = c->has_code ? S.pool.b + c->tag : "";
		char *t = vx_alloc(strlen(cell_src(c)) + strlen(tag) + c->code_n + 16);
		if (c->has_code)
			sprintf(t, "%s\n```%s\n%.*s\n```", cell_src(c), tag, (int)c->code_n, S.pool.b + c->code);
		else
			sprintf(t, "%s%s", c->english ? "? " : "", cell_src(c));
		input_set(&S.in, t, strlen(t));
		S.in.cur = S.in.anchor = S.in.n;
		free(t);
		app.focus = FOCUS_MAN_INPUT;
		break;
	}
	case H_EXAMPLE: submit(examples[h->arg], strlen(examples[h->arg])); break;
	case H_PLAY:
		S.playing = !S.playing;
		S.next_at = vx_now_ns();
		break;
	case H_FIRST:
		S.frame = 0;
		S.playing = false;
		break;
	case H_SEEK:
		if (S.view >= 0 && S.img[S.view].all.nframes > 1) {
			int n = S.img[S.view].all.nframes;
			S.frame = VX_CLAMP((int)((i64)(e->x - G.sl_x) * (n - 1) / VX_MAX(G.sl_w, 1)), 0, n - 1);
			S.playing = false;
		}
		break;
	case H_FUNC: {
		/* its name, at the caret */
		const char *sig = ucas_s(&S.state, S.state.func_sig[h->arg]);
		size_t l = strcspn(sig, "(");
		input_put(sig, l);
		app.focus = FOCUS_MAN_INPUT;
		break;
	}
	case H_CODE_TOGGLE:
		if ((u64)h->arg < S.ncells) {
			S.cells[h->arg].code_open = !S.cells[h->arg].code_open;
			S.cells[h->arg].h_w = 0;
			S.stick = false;
		}
		break;
	case H_FILE_SHOW:
	case H_FILE_OPEN:
	case H_FILE_SAVE: {
		u32 ci = (u32)(h->arg / 16), fi = (u32)(h->arg % 16);
		if (ci >= S.ncells || fi >= S.cells[ci].r.nfiles)
			break;
		if (h->act == H_FILE_SHOW) {
			S.cells[ci].file_open ^= (u8)(1u << fi);
			S.cells[ci].h_w = 0;
			S.stick = false;
		} else if (h->act == H_FILE_OPEN)
			open_file_tab(ci, fi);
		else
			save_file(ci, fi);
		break;
	}
	case H_CLEAR: clear_transcript(); break;
	case H_RESTART: restart(); break;
	case H_INPUT: {
		app.focus = FOCUS_MAN_INPUT;
		int cw = mono_cw(), lh = font_m[FONT_MONO].line;
		int row = VX_MAX((e->y - G.y) / lh, 0), col = VX_MAX((e->x - G.x + cw / 2) / cw, 0);
		u32 off = input_off(row + S.in_top, col, G.cols);
		S.in.cur = off;
		if (!(e->mods & MOD_SHIFT))
			S.in.anchor = off;
		break;
	}
	}
}

static void scroll_by(i64 px)
{
	S.scroll += px;
	S.stick = false;
	/* back at the end: follow it again (draw clamps; this checks) */
	i64 total = 0;
	for (u32 i = 0; i < S.ncells; i++)
		total += cell_h(i, G.tr_w);
	if (S.scroll >= total - G.tr_h)
		S.stick = true;
}

void ucasview_ws_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	app.dirty = true;
	if (e->type == EV_TEXT) {
		app.focus = FOCUS_MAN_INPUT;
		input_put(e->text, strlen(e->text));
		return;
	}
	if (e->type == EV_KEY) {
		int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
		bool enter = e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER;
		app.focus = FOCUS_MAN_INPUT;
		if (enter && mods == MOD_SHIFT) {
			input_put("\n", 1);
			return;
		}
		if (enter && (mods == 0 || mods == MOD_CTRL)) {
			bool fence;
			int depth;
			input_state(&fence, &depth);
			if (mods == 0 && fence) {
				/* in a code block: a new line, indented as this one; the
				 * closing ``` and Enter runs it */
				u32 ls = S.in.cur;
				while (ls && S.in.b[ls - 1] != '\n')
					ls--;
				u32 k = ls;
				while (k < S.in.n && k < S.in.cur && S.in.b[k] == ' ')
					k++;
				char t[130] = "\n";
				u32 ind = VX_MIN(k - ls, 120u);
				memset(t + 1, ' ', ind);
				input_put(t, ind + 1);
				return;
			}
			/* an open bracket goes on to the next line, indented: ucas reads an
			 * indented line as the statement going on */
			if (mods == 0 && depth > 0) {
				input_put("\n  ", 3);
				return;
			}
			run_input();
			return;
		}
		if (mods == MOD_CTRL && e->key == SDLK_C && sel_any() && S.in.anchor == S.in.cur) {
			sel_copy(); /* the transcript's selection (the box's own when it has one) */
			return;
		}
		if (mods == MOD_CTRL && e->key == SDLK_A && !S.in.n) {
			sel_all(); /* an empty box: the whole transcript */
			return;
		}
		if (e->key == SDLK_ESCAPE) {
			if (sel_any())
				SEL.on = false;
			else if (running())
				cancel();
			else if (S.in.n)
				input_set(&S.in, "", 0);
			return;
		}
		if (mods == MOD_CTRL && e->key == SDLK_L) {
			clear_transcript();
			return;
		}
		if (e->key == SDLK_PAGEUP || e->key == SDLK_PAGEDOWN) {
			scroll_by((e->key == SDLK_PAGEUP ? -1 : 1) * (i64)VX_MAX(G.tr_h - PX(40), PX(40)));
			return;
		}
		if ((e->key == SDLK_UP || e->key == SDLK_DOWN) && mods == 0) {
			int r, c, rn, cn;
			input_rc(S.in.cur, G.cols ? G.cols : 80, &r, &c);
			input_rc(S.in.n, G.cols ? G.cols : 80, &rn, &cn);
			bool up = e->key == SDLK_UP;
			if ((up && r == 0) || (!up && r == rn)) {
				hist_go(up ? -1 : 1);
				return;
			}
			S.in.cur = S.in.anchor = input_off(r + (up ? -1 : 1), c, G.cols ? G.cols : 80);
			return;
		}
		if (e->key == SDLK_TAB)
			return; /* nothing to complete yet; keep it out of the text */
		if (mods == MOD_CTRL && e->key == SDLK_V) {
			char *c = ui_clipboard_get(); /* statements span lines: keep them */
			input_put(c, strlen(c));
			free(c);
			return;
		}
		input_key(&S.in, e);
		return;
	}
	if (e->type == EV_WHEEL) {
		int d = (int)(e->wy * 3) * font_m[FONT_MONO].line;
		if (in_r(e->x, e->y, G.fn_x, G.fn_y, G.fn_w, G.fn_h))
			S.fn_scroll -= (int)(e->wy * 3);
		else
			scroll_by(-d);
		return;
	}
	if (e->type == EV_MUP) {
		SEL.drag = false;
		return;
	}
	if (e->type == EV_MMOVE) {
		TPos p;
		if (SEL.drag && app.mouse_down) {
			/* dragging past the transcript's edge scrolls it */
			int lh = font_m[FONT_MONO].line;
			if (e->y < G.tr_y)
				scroll_by(-lh);
			else if (e->y > G.tr_y + G.tr_h)
				scroll_by(lh);
			if (pos_at(e->x, VX_CLAMP(e->y, G.tr_y, G.tr_y + G.tr_h - 1), &p))
				SEL.h = p;
		} else if (in_r(e->x, e->y, G.tr_x, G.tr_y, G.tr_w, G.tr_h)) {
			for (int i = 0; i < ntrow; i++)
				if (e->y >= trow[i].y && e->y < trow[i].y + font_m[FONT_MONO].line &&
				    e->x >= trow[i].x) {
					app.cursor = CURSOR_IBEAM;
					break;
				}
		}
		return;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return;
	for (int i = nhit; i-- > 0;)
		if (in_r(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
			do_hit(&hits[i], e);
			return;
		}
	if (in_r(e->x, e->y, G.tr_x, G.tr_y, G.tr_w, G.tr_h)) {
		/* select: press and drag; two clicks a word, three a line;
		 * shift extends */
		TPos p;
		if (pos_at(e->x, e->y, &p)) {
			if (e->clicks >= 2)
				sel_unit(p, e->clicks);
			else if ((e->mods & MOD_SHIFT) && SEL.on)
				SEL.h = p;
			else {
				SEL.a = SEL.h = p;
				SEL.on = true;
			}
			SEL.drag = e->clicks < 2;
		} else
			SEL.on = false;
	}
	app.focus = FOCUS_MAN_INPUT;
}

/* ---- commands, the session file ----------------------------------------------------------------- */

void ucasview_run_text(const char *src, size_t n)
{
	app.side_view = SIDE_UCAS;
	ucasview_open();
	submit(src, n);
}

bool ucasview_command(int cmd)
{
	switch (cmd) {
	case CMD_UCAS_RUN_SELECTION: {
		View *v = active_view();
		if (!v || ui_workspace()) {
			ui_status("Select ucas statements in an editor first");
			return true;
		}
		size_t n = 0;
		char *s = ed_selection_text(v, &n);
		/* code from a shader or C file: what ucas does with code is simplify
		 * it (C: its C frontend); a selection with its own ``` runs as it is */
		int lang = app.docs.lang[v->doc];
		const char *tag = lang == LANG_GLSL ? "glsl" : lang == LANG_SPIRV ? "spv" : lang == LANG_C ? "c" : NULL;
		if (tag && !strstr(s, "```")) {
			while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
				n--;
			char *t = vx_alloc(n + 40);
			int k = snprintf(t, n + 40, "simplify\n```%s\n%.*s\n```", tag, (int)n, s);
			ucasview_run_text(t, (size_t)k);
			free(t);
		} else
			ucasview_run_text(s, n);
		free(s);
		return true;
	}
	case CMD_UCAS_CANCEL: cancel(); return true;
	case CMD_UCAS_RESTART: restart(); return true;
	case CMD_UCAS_SAVE_PICTURE:
		if (S.view >= 0 && S.view < (i64)S.nimg)
			save_img((u32)S.view);
		else
			ui_status("No picture in the ucas View");
		return true;
	}
	return false;
}

void ucasview_session_save(void *file)
{
	FILE *f = file;
	u32 from = S.nhist > 100 ? S.nhist - 100 : 0;
	for (u32 i = from; i < S.nhist; i++) {
		fputs("ucas_hist ", f);
		for (const char *p = S.pool.b + S.hist[i]; *p; p++) {
			if (*p == '\\')
				fputs("\\\\", f);
			else if (*p == '\n')
				fputs("\\n", f);
			else if (*p != '\r')
				fputc(*p, f);
		}
		fputc('\n', f);
	}
}

bool ucasview_session_line(const char *l)
{
	if (strncmp(l, "ucas_hist ", 10) != 0)
		return false;
	if (!S.inited) {
		S.inited = true;
		S.view = -1;
		S.stick = true;
	}
	const char *s = l + 10;
	char *t = vx_alloc(strlen(s) + 1);
	size_t k = 0;
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			t[k++] = *s == 'n' ? '\n' : *s;
		} else
			t[k++] = *s;
	}
	hist_add(t, k);
	S.hist_pos = S.nhist;
	free(t);
	return true;
}
