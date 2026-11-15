/* Profiling workspace: hardware performance counters through FreeBSD's
 * hwpmc and pmcstat(8).
 *
 *   - checks that pmcstat works for this user (pmcstat -L); when it does
 *     not, says how to enable it (kldload hwpmc, loader.conf, the sysctl
 *     that lets users count their own processes)
 *   - lists the events of pmcstat -L, filterable, with toggles
 *   - runs a program with arguments, in the project's folder:
 *       Count  pmcstat -p EVENT... prog args: the totals in a table
 *       Sample pmcstat -P EVENT -O log prog args, then pmcstat -R log -a:
 *              every sample's callchain with file:line, opened as a
 *              profile (flame graph, callers, hot lines, source heat)
 * The check, the run and the conversion never block the UI thread. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "prof.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <SDL3/SDL.h>

extern char **environ;

enum { AV_OK, AV_NOTOOL, AV_NOMODULE, AV_ERROR, AV_NOTBSD };
enum { F_NONE, F_FILTER, F_PROG, F_ARGS, F_RATE };
enum { M_COUNT, M_SAMPLE };

#define EV_COLS SOA(S.ev), SOA(S.ev_sel), SOA(S.ev_hdr)

static struct {
	_Atomic int chk; /* 0 not yet, 1 checking, 2 done */
	pthread_t chk_th;
	int avail;
	char detail[600];
	bool proc_debug_off;
	char cpuid[96];
	/* events (SoA): name in the pool, selected, a class header (not an event) */
	Pool ev_pool;
	u32 nev, ev_cap;
	u32 *ev;
	u8 *ev_sel, *ev_hdr;
	u32 *vis, nvis, vis_cap; /* rows shown with the filter */
	u32 vis_hash;
	i64 ev_scroll;
	u32 ev_cur;
	Input filter, prog, args, rate;
	int field;
	int mode;
	bool children;
	bool inited;
	/* the run */
	pid_t pid;
	int fd;
	bool running;
	int status;
	bool exited;
	u64 t0, t1;
	char *out;
	u32 out_n, out_cap;
	i64 out_scroll; /* lines up from the bottom */
	char rundir[512], log[600], event[160], progname[256], cmdline[1024];
	int run_mode;
	/* count results */
	char res_name[32][128];
	double res_val[32];
	u32 nres;
	/* sample conversion */
	_Atomic int post; /* 0, 1 running, 2 done */
	pthread_t post_th;
	char post_path[700], post_err[300];
	char last_profile[700];
} S;

/* pmcstat, or VXIDE_PMCSTAT */
static char *pmc_tool(void)
{
	const char *e = getenv("VXIDE_PMCSTAT");
	return (char *)(e && *e ? e : "pmcstat");
}

/* ---- the check ------------------------------------------------------------------------ */

static void *chk_main(void *arg)
{
	(void)arg;
#ifndef __FreeBSD__
	S.avail = AV_NOTBSD;
#else
	char *argv[] = {pmc_tool(), "-L", NULL};
	size_t n = 0;
	int st = 0;
	char *out = prof_run(NULL, argv, &n, &st);
	if (!out) {
		S.avail = AV_NOTOOL;
	} else if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && n) {
		S.avail = AV_OK;
		/* events are indented; a line that is not names a class */
		const char *s = out, *e = out + n;
		while (s < e) {
			const char *nl = memchr(s, '\n', (size_t)(e - s));
			size_t len = (size_t)((nl ? nl : e) - s);
			bool hdr = len && s[0] != '\t' && s[0] != ' ';
			const char *t = s;
			size_t tn = len;
			while (tn && (*t == '\t' || *t == ' '))
				t++, tn--;
			while (tn && (t[tn - 1] == ' ' || t[tn - 1] == '\r'))
				tn--;
			if (tn) {
				SoaCol c[] = {EV_COLS};
				soa_grow(c, 3, &S.ev_cap, S.nev + 1, S.nev);
				S.ev[S.nev] = pool_add(&S.ev_pool, t, (u32)tn);
				S.ev_sel[S.nev] = 0;
				S.ev_hdr[S.nev] = hdr;
				S.nev++;
			}
			s = nl ? nl + 1 : e;
		}
	} else {
		/* the first line of pmcstat's complaint */
		const char *nl = memchr(out, '\n', n);
		snprintf(S.detail, sizeof S.detail, "%.*s", (int)(nl ? (size_t)(nl - out) : n), out);
		S.avail = strstr(out, "Initialization of the pmc(3) library failed") || strstr(out, "No such file") ||
		                  strstr(out, "hwpmc")
		              ? AV_NOMODULE
		              : AV_ERROR;
	}
	free(out);
	char *a2[] = {"sysctl", "-n", "security.bsd.unprivileged_proc_debug", NULL};
	char *o2 = prof_run(NULL, a2, &n, &st);
	S.proc_debug_off = o2 && n && o2[0] == '0';
	free(o2);
	char *a3[] = {"sysctl", "-n", "kern.hwpmc.cpuid", NULL};
	char *o3 = prof_run(NULL, a3, &n, &st);
	if (o3 && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		snprintf(S.cpuid, sizeof S.cpuid, "%.*s", (int)strcspn(o3, "\n"), o3);
	free(o3);
#endif
	atomic_store(&S.chk, 2);
	return NULL;
}

static void check_start(void)
{
	if (atomic_load(&S.chk) == 1)
		return;
	if (atomic_load(&S.chk) == 2)
		pthread_join(S.chk_th, NULL);
	SoaCol c[] = {EV_COLS};
	soa_free(c, 3);
	S.nev = S.ev_cap = 0;
	pool_free(&S.ev_pool);
	S.detail[0] = S.cpuid[0] = 0;
	S.vis_hash = 0;
	atomic_store(&S.chk, 1);
	if (pthread_create(&S.chk_th, NULL, chk_main, NULL) != 0) {
		chk_main(NULL);
		S.chk_th = pthread_self();
	}
}

static bool checked(void)
{
	if (atomic_load(&S.chk) != 2)
		return false;
	if (!pthread_equal(S.chk_th, pthread_self())) {
		pthread_join(S.chk_th, NULL);
		S.chk_th = pthread_self(); /* joined */
		/* sensible defaults: cycles and instructions when the CPU has them */
		bool any = false;
		for (u32 i = 0; i < S.nev; i++) {
			const char *n = S.ev_pool.b + S.ev[i];
			if (!S.ev_hdr[i] && (!strcmp(n, "cpu_clk_unhalted.thread_p") || !strcmp(n, "inst_retired.any") ||
			                     !strcmp(n, "ls_not_halted_cyc") || !strcmp(n, "ex_ret_instr")))
				S.ev_sel[i] = any = true;
		}
		(void)any;
	}
	return true;
}

void pmcview_open(void)
{
	if (!S.inited) {
		S.inited = true;
		S.mode = M_COUNT;
		input_set(&S.rate, "65536", 5);
	}
	if (atomic_load(&S.chk) == 0)
		check_start();
	app.focus = FOCUS_WORKSPACE;
}

/* ---- arguments --------------------------------------------------------------------------- */

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

/* ---- running -------------------------------------------------------------------------------- */

static void out_add(const char *s, size_t n)
{
	enum { CAP = 512 * 1024 };
	if (S.out_n + n > CAP) {
		/* keep the tail */
		u32 drop = (u32)VX_MIN((size_t)S.out_n, S.out_n + n - CAP / 2);
		memmove(S.out, S.out + drop, S.out_n - drop);
		S.out_n -= drop;
	}
	VX_GROW(S.out, S.out_cap, S.out_n + (u32)n + 1);
	memcpy(S.out + S.out_n, s, n);
	S.out_n += (u32)n;
	S.out[S.out_n] = 0;
}

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...)
{
	char b[2048];
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	out_add(b, (size_t)VX_CLAMP(k, 0, (int)sizeof b - 1));
}

static u32 n_selected(void)
{
	u32 k = 0;
	for (u32 i = 0; i < S.nev; i++)
		k += S.ev_sel[i];
	return k;
}

static const char *first_selected(void)
{
	for (u32 i = 0; i < S.nev; i++)
		if (S.ev_sel[i])
			return S.ev_pool.b + S.ev[i];
	return NULL;
}

static void run_start(void)
{
	if (S.running || atomic_load(&S.post) == 1) {
		ui_status("A run is in progress");
		return;
	}
	if (!checked() || S.avail != AV_OK) {
		ui_status("pmcstat is not usable yet (see the instructions)");
		return;
	}
	char prog[1024];
	snprintf(prog, sizeof prog, "%.*s", (int)S.prog.n, S.prog.b ? S.prog.b : "");
	char *p = prog;
	while (*p == ' ')
		p++;
	size_t pl = strlen(p);
	while (pl && p[pl - 1] == ' ')
		p[--pl] = 0;
	if (!*p) {
		ui_status("Type the program to run");
		S.field = F_PROG;
		app.focus = FOCUS_MAN_INPUT;
		return;
	}
	u32 nsel = n_selected();
	if (!nsel) {
		ui_status("Select at least one event");
		return;
	}
	const char *root = app.root ? app.root : ".";
	/* a bare "prog": ./prog when the project has it, else found in PATH */
	char progpath[1100];
	if (!strchr(p, '/')) {
		char t[4200];
		snprintf(t, sizeof t, "%s/%s", root, p);
		char *w = NULL;
		if (access(t, X_OK) == 0)
			snprintf(progpath, sizeof progpath, "./%s", p);
		else if ((w = prof_which(p))) /* like `which prog` */
			snprintf(progpath, sizeof progpath, "%s", w);
		else {
			ui_status("No program \"%s\" in the project or in PATH", p);
			return;
		}
		free(w);
	} else
		snprintf(progpath, sizeof progpath, "%s", p);
	const char *base = strrchr(progpath, '/');
	snprintf(S.progname, sizeof S.progname, "%s", base ? base + 1 : progpath);

	char *argv[600];
	static char abuf[16384];
	int a = 0;
	argv[a++] = "/bin/sh";
	argv[a++] = "-c";
	argv[a++] = "cd \"$1\" || exit 126; shift; exec \"$@\"";
	argv[a++] = "sh";
	argv[a++] = (char *)root;
	argv[a++] = pmc_tool();
	if (S.children)
		argv[a++] = "-d";
	S.run_mode = S.mode;
	if (S.mode == M_COUNT) {
		for (u32 i = 0; i < S.nev && a < 500; i++)
			if (S.ev_sel[i]) {
				argv[a++] = "-p";
				argv[a++] = S.ev_pool.b + S.ev[i];
			}
	} else {
		/* one event: the log's samples are not told apart by event */
		snprintf(S.event, sizeof S.event, "%s", first_selected());
		const char *tmp = getenv("TMPDIR");
		snprintf(S.rundir, sizeof S.rundir, "%s/vxide-pmc-%d", tmp && *tmp ? tmp : "/tmp", (int)getuid());
		mkdir(S.rundir, 0700);
		snprintf(S.log, sizeof S.log, "%s/%s-%ld.pmclog", S.rundir, S.progname, (long)time(NULL));
		static char rate[32];
		snprintf(rate, sizeof rate, "%.*s", (int)VX_MIN(S.rate.n, 20u), S.rate.b ? S.rate.b : "65536");
		if (!atoi(rate))
			snprintf(rate, sizeof rate, "65536");
		argv[a++] = "-n";
		argv[a++] = rate;
		argv[a++] = "-P";
		argv[a++] = S.event;
		argv[a++] = "-O";
		argv[a++] = S.log;
	}
	int prog_at = a;
	argv[a++] = progpath;
	char *rest[64];
	u32 nr = split_args(S.args.b ? S.args.b : "", S.args.n, rest, 64, abuf);
	for (u32 i = 0; i < nr && a < 590; i++)
		argv[a++] = rest[i];
	argv[a] = NULL;
	/* the command, as it would be typed */
	u32 k = 0;
	for (int i = 5; i < a && k + 2 < sizeof S.cmdline; i++)
		k += (u32)snprintf(S.cmdline + k, sizeof S.cmdline - k, "%s%s", i > 5 ? " " : "", argv[i]);
	(void)prog_at;

	int po[2];
	if (pipe(po) != 0) {
		ui_status("pipe: %s", strerror(errno));
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_adddup2(&fa, po[1], 2);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0); /* its own group: Stop signals pmcstat and the program */
	int rc = posix_spawn(&S.pid, "/bin/sh", &fa, &at, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(po[1]);
	if (rc != 0) {
		close(po[0]);
		ui_status("Cannot start pmcstat: %s", strerror(rc));
		return;
	}
	S.fd = po[0];
	fcntl(S.fd, F_SETFL, fcntl(S.fd, F_GETFL) | O_NONBLOCK);
	S.running = true;
	S.exited = false;
	S.nres = 0;
	S.out_n = 0;
	S.out_scroll = 0;
	S.t0 = vx_now_ns();
	out_printf("$ cd %s\n$ %s\n", root, S.cmdline);
	app.dirty = true;
}

static void run_stop(void)
{
	if (S.running && S.pid > 0)
		kill(-S.pid, SIGINT); /* pmcstat prints what it has counted */
}

/* the counts pmcstat prints at exit:
 *   #  p/inst_retired.any p/cpu_clk_unhalted.thread_p
 *          92946591                   163252899      */
static void parse_counts(void)
{
	S.nres = 0;
	if (!S.out)
		return;
	char *hdr = NULL;
	for (char *l = S.out; l && *l;) {
		char *nl = strchr(l, '\n');
		if (l[0] == '#' && (strstr(l, " p/") || strstr(l, " s/")) && (!nl || strstr(l, " p/") < nl))
			hdr = l;
		l = nl ? nl + 1 : NULL;
	}
	if (!hdr)
		return;
	char *vals = strchr(hdr, '\n');
	if (!vals)
		return;
	vals++;
	char *h = hdr + 1;
	char *hend = vals - 1;
	while (S.nres < 32) {
		while (h < hend && *h == ' ')
			h++;
		if (h >= hend)
			break;
		char *e = h;
		while (e < hend && *e != ' ')
			e++;
		const char *nm = h;
		if (e - h > 2 && h[1] == '/')
			nm = h + 2;
		snprintf(S.res_name[S.nres], sizeof S.res_name[0], "%.*s", (int)(e - nm), nm);
		char *ve;
		S.res_val[S.nres] = strtod(vals, &ve);
		if (ve == vals)
			break;
		vals = ve;
		S.nres++;
		h = e;
	}
}

static void *post_main(void *arg)
{
	(void)arg;
	char tmp[760];
	snprintf(tmp, sizeof tmp, "%s.txt", S.log);
	char *argv[] = {pmc_tool(), "-R", S.log, "-a", tmp, NULL};
	size_t n;
	int st;
	char *o = prof_run(NULL, argv, &n, &st);
	free(o);
	size_t tn = 0;
	char *t = vx_read_file(tmp, &tn);
	if (!t || !tn) {
		snprintf(S.post_err, sizeof S.post_err, "pmcstat -R could not convert %s", S.log);
		free(t);
		unlink(tmp);
		atomic_store(&S.post, 2);
		return NULL;
	}
	/* a header for the profile view, then the samples */
	char hdr[1400];
	int hk = snprintf(hdr, sizeof hdr, "# pmcstat samples: event=%s rate=%.*s program=%s\n", S.event,
	                  (int)VX_MIN(S.rate.n, 20u), S.rate.b ? S.rate.b : "", S.cmdline);
	snprintf(S.post_path, sizeof S.post_path, "%s/%s.%s.%ld.pmcsamples", S.rundir, S.progname, S.event,
	         (long)time(NULL));
	char *all = vx_alloc((size_t)hk + tn + 1);
	memcpy(all, hdr, (size_t)hk);
	memcpy(all + hk, t, tn);
	if (vx_write_file(S.post_path, all, (size_t)hk + tn) != 0)
		snprintf(S.post_err, sizeof S.post_err, "Cannot write %s", S.post_path);
	free(all);
	free(t);
	unlink(tmp);
	atomic_store(&S.post, 2);
	return NULL;
}

void pmcview_tick(void)
{
	if (atomic_load(&S.chk) == 2 && !pthread_equal(S.chk_th, pthread_self())) {
		checked();
		app.dirty = true;
	}
	if (S.running) {
		char b[8192];
		for (;;) {
			ssize_t r = read(S.fd, b, sizeof b);
			if (r > 0) {
				out_add(b, (size_t)r);
				app.dirty = true;
				continue;
			}
			break;
		}
		int st;
		pid_t w = waitpid(S.pid, &st, WNOHANG);
		if (w == S.pid) {
			ssize_t r;
			while ((r = read(S.fd, b, sizeof b)) > 0)
				out_add(b, (size_t)r);
			close(S.fd);
			S.running = false;
			S.exited = true;
			S.status = st;
			S.t1 = vx_now_ns();
			int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
			out_printf("\n[exit %d after %.2f s]\n", code, (double)(S.t1 - S.t0) / 1e9);
			if (S.run_mode == M_COUNT)
				parse_counts();
			else {
				struct stat sb;
				if (stat(S.log, &sb) == 0 && sb.st_size > 0) {
					out_printf("Converting the samples (pmcstat -R %s -a)...\n", S.log);
					S.post_err[0] = 0;
					atomic_store(&S.post, 1);
					if (pthread_create(&S.post_th, NULL, post_main, NULL) != 0) {
						post_main(NULL);
						S.post_th = pthread_self();
					}
				} else
					out_printf("No samples were logged.\n");
			}
			app.dirty = true;
		}
	}
	if (atomic_load(&S.post) == 2) {
		if (!pthread_equal(S.post_th, pthread_self()))
			pthread_join(S.post_th, NULL);
		atomic_store(&S.post, 0);
		if (S.post_err[0]) {
			out_printf("%s\n", S.post_err);
		} else {
			out_printf("Profile: %s\n", S.post_path);
			snprintf(S.last_profile, sizeof S.last_profile, "%s", S.post_path);
			open_file(S.post_path, false, UINT32_MAX, 0); /* opens as a profile tab */
		}
		app.dirty = true;
	}
}

bool pmcview_busy(void) { return atomic_load(&S.chk) == 1 || S.running || atomic_load(&S.post) == 1; }

void pmcview_shutdown(void)
{
	if (S.running && S.pid > 0) {
		kill(-S.pid, SIGTERM);
		waitpid(S.pid, NULL, 0);
	}
}

/* ---- drawing --------------------------------------------------------------------------------- */

enum { H_NONE, H_RECHECK, H_MODE, H_CHILDREN, H_RUN, H_STOP, H_COPY, H_UNSEL, H_CLEAR };
typedef struct {
	int x, y, w, h, act, arg;
} Hit;
static Hit hits[96];
static int nhit;
static const char *copy_text[8];

static bool in_r(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static void hit(int x, int y, int w, int h, int act, int arg)
{
	if (nhit < (int)VX_COUNT(hits))
		hits[nhit++] = (Hit){x, y, w, h, act, arg};
}

static int ty(int y, int h, int font) { return y + (h - font_m[font].line) / 2; }

static int button(int x, int y, const char *label, int act, int arg, bool primary, const char *tip)
{
	int h = PX(26), w = font_width(FONT_UI, label, strlen(label)) + PX(24);
	bool hov = in_r(app.hover_x, app.hover_y, x, y, w, h);
	u32 bg = primary ? (hov ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG)) : hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG);
	draw_rect(x, y, w, h, bg);
	if (!primary)
		draw_frame(x, y, w, h, COL(C_INPUT_BORDER));
	draw_text(FONT_UI, x + PX(12), ty(y, h, FONT_UI), label, strlen(label), primary ? COL(C_BUTTON_FG) : COL(C_FG));
	hit(x, y, w, h, act, arg);
	if (tip)
		ui_tip(x, y, w, h, tip);
	return w;
}

static int checkbox(int x, int y, int h, bool on, const char *label, int act, int arg)
{
	int bs = PX(14), by = y + (h - bs) / 2;
	draw_frame(x, by, bs, bs, on ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
	if (on)
		draw_rect(x + PX(3), by + PX(3), bs - PX(6), bs - PX(6), COL(C_FOCUS_BORDER));
	int w = bs + PX(8);
	if (label)
		w += draw_text(FONT_UI, x + bs + PX(8), ty(y, h, FONT_UI), label, strlen(label), COL(C_FG)) - (x + bs + PX(8));
	if (act)
		hit(x, y, w, h, act, arg);
	return w;
}

static int section(int x, int y, const char *t)
{
	draw_text(FONT_SMALL_BOLD, x, y, t, strlen(t), COL(C_SECTION_HEADER_FG));
	return y + font_m[FONT_SMALL_BOLD].line + PX(6);
}

/* geometry for the mouse */
static struct {
	int filt_x, filt_y, filt_w;
	int prog_x, prog_y, prog_w;
	int args_x, args_y, args_w;
	int rate_x, rate_y, rate_w;
	int list_x, list_y, list_w, list_h;
	int out_x, out_y, out_w, out_h;
} G;

static void refilter(void)
{
	u32 h = 2166136261u ^ S.nev;
	for (u32 i = 0; i < S.filter.n; i++)
		h = (h ^ (u8)S.filter.b[i]) * 16777619u;
	if (h == S.vis_hash && S.vis)
		return;
	S.vis_hash = h;
	S.nvis = 0;
	char q[256];
	snprintf(q, sizeof q, "%.*s", (int)VX_MIN(S.filter.n, 255u), S.filter.b ? S.filter.b : "");
	for (u32 i = 0; i < S.nev; i++) {
		const char *n = S.ev_pool.b + S.ev[i];
		if (q[0] && (S.ev_hdr[i] || !strstr(n, q)))
			continue;
		VX_GROW(S.vis, S.vis_cap, S.nvis + 1);
		S.vis[S.nvis++] = i;
	}
	if (S.ev_cur >= S.nvis)
		S.ev_cur = S.nvis ? S.nvis - 1 : 0;
	S.ev_scroll = 0;
}

static int cmd_line(int x, int y, int w, const char *cmd, int idx)
{
	/* a command in a box with a Copy button */
	int h = PX(28);
	draw_rect(x, y, w, h, COL(C_INPUT_BG));
	draw_frame(x, y, w, h, COL(C_INPUT_BORDER));
	draw_text_fit(FONT_MONO, x + PX(10), ty(y, h, FONT_MONO), cmd, strlen(cmd), w - PX(90), COL(C_EDITOR_FG));
	const char *c = "Copy";
	int cw = font_width(FONT_SMALL, c, 4) + PX(16);
	int bx = x + w - cw - PX(4);
	bool hov = in_r(app.hover_x, app.hover_y, bx, y + PX(3), cw, h - PX(6));
	draw_rect(bx, y + PX(3), cw, h - PX(6), hov ? COL(C_LIST_HOVER_BG) : COL(C_SIDEBAR_BG));
	draw_text(FONT_SMALL, bx + PX(8), ty(y + PX(3), h - PX(6), FONT_SMALL), c, 4, COL(C_FG));
	copy_text[idx] = cmd;
	hit(bx, y + PX(3), cw, h - PX(6), H_COPY, idx);
	return y + h + PX(8);
}

static int para(int x, int y, int w, const char *t, u32 col)
{
	/* wrapped small paragraph */
	int lh = font_m[FONT_UI].line + PX(3);
	const char *s = t;
	while (*s) {
		const char *e = s, *brk = NULL;
		while (*e && *e != '\n') {
			if (*e == ' ')
				brk = e;
			if (font_width(FONT_UI, s, (size_t)(e - s + 1)) > w && brk)
				break;
			e++;
		}
		const char *stop = (*e && *e != '\n' && brk) ? brk : e;
		draw_text(FONT_UI, x, y, s, (size_t)(stop - s), col);
		y += lh;
		s = *stop ? stop + 1 : stop;
	}
	return y;
}

static void draw_unavailable(int x, int y, int w)
{
	int cx = x + PX(32), cw = VX_MIN(w - PX(64), PX(820));
	int cy = y + PX(24);
	draw_icon(ICON_WARNING, cx, cy + PX(2), COL(C_WARNING));
	const char *t = S.avail == AV_NOTBSD    ? "Profiling uses FreeBSD's hwpmc (pmcstat); this system is not FreeBSD."
	                : S.avail == AV_NOTOOL  ? "pmcstat was not found."
	                : S.avail == AV_NOMODULE ? "The hwpmc driver is not loaded, so pmcstat cannot reach the counters."
	                                        : "pmcstat cannot use the performance counters.";
	draw_text(FONT_UI_BOLD, cx + PX(26), cy, t, strlen(t), COL(C_FG));
	cy += PX(30);
	if (S.detail[0]) {
		char d[700];
		snprintf(d, sizeof d, "pmcstat said: %s", S.detail);
		cy = para(cx, cy, cw, d, COL(C_DESCRIPTION_FG)) + PX(10);
	}
	if (S.avail == AV_NOTBSD) {
		cy = para(cx, cy, cw, "Other systems (Linux perf) are not supported yet.", COL(C_DESCRIPTION_FG));
	} else if (S.avail == AV_NOTOOL) {
		cy = para(cx, cy, cw,
		          "pmcstat is part of the FreeBSD base system (/usr/sbin/pmcstat). Make sure /usr/sbin is in PATH, or "
		          "install the base system's lib32 / profiling components if they were left out.",
		          COL(C_FG)) +
		     PX(8);
	} else {
		cy = section(cx, cy, "1. LOAD THE DRIVER NOW (AS ROOT)");
		cy = cmd_line(cx, cy, cw, "kldload hwpmc", 0);
		cy = section(cx, cy, "2. LOAD IT AT EVERY BOOT");
		cy = cmd_line(cx, cy, cw, "sysrc -f /boot/loader.conf hwpmc_load=YES", 1);
		cy = section(cx, cy, "3. LET USERS COUNT THEIR OWN PROCESSES (THE DEFAULT)");
		cy = cmd_line(cx, cy, cw, "sysctl security.bsd.unprivileged_proc_debug=1", 2);
		cy = para(cx, cy, cw,
		          "Counting and sampling a program you start works as your user; system-wide modes (-s, -S) need root. "
		          "Virtual machines and jails often hide the hardware counters: run on the host, or enable "
		          "performance counter passthrough in the hypervisor.",
		          COL(C_DESCRIPTION_FG)) +
		     PX(10);
	}
	button(cx, cy + PX(6), "Check Again", H_RECHECK, 0, true, "Run pmcstat -L again");
}

static void draw_events(int x, int y, int w, int h)
{
	refilter();
	char t[96];
	snprintf(t, sizeof t, "EVENTS  (%u, %u selected)", S.nev, n_selected());
	int cy = section(x, y, t);
	int ih = input_draw_ex(&S.filter, x, cy, w, 0, "Filter events (Ctrl+F)", app.focus == FOCUS_MAN_INPUT && S.field == F_FILTER);
	G.filt_x = x;
	G.filt_y = cy;
	G.filt_w = w;
	cy += ih + PX(8);
	/* the selected events as chips: click to remove */
	int chx = x, chh = PX(22);
	bool any = false;
	for (u32 i = 0; i < S.nev; i++) {
		if (!S.ev_sel[i])
			continue;
		any = true;
		const char *n = S.ev_pool.b + S.ev[i];
		char lb[200];
		snprintf(lb, sizeof lb, "%s  \xC3\x97", n);
		int cw = font_width(FONT_SMALL, lb, strlen(lb)) + PX(14);
		if (chx + cw > x + w && chx > x) {
			chx = x;
			cy += chh + PX(4);
		}
		bool hov = in_r(app.hover_x, app.hover_y, chx, cy, cw, chh);
		draw_rect(chx, cy, cw, chh, hov ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG));
		draw_text(FONT_SMALL, chx + PX(7), ty(cy, chh, FONT_SMALL), lb, strlen(lb), COL(C_BUTTON_FG));
		hit(chx, cy, cw, chh, H_UNSEL, (int)i);
		chx += cw + PX(4);
	}
	if (any) {
		const char *c = "Clear";
		int cw = font_width(FONT_SMALL, c, 5) + PX(14);
		if (chx + cw > x + w) {
			chx = x;
			cy += chh + PX(4);
		}
		draw_text(FONT_SMALL, chx + PX(6), ty(cy, chh, FONT_SMALL), c, 5, COL(C_LINK));
		hit(chx, cy, cw, chh, H_CLEAR, 0);
		cy += chh + PX(8);
	}
	int rh = app.row_h;
	int lh = y + h - cy;
	G.list_x = x;
	G.list_y = cy;
	G.list_w = w;
	G.list_h = lh;
	int vis = VX_MAX(lh / rh, 1);
	S.ev_scroll = VX_CLAMP(S.ev_scroll, 0, VX_MAX((i64)S.nvis - vis, 0));
	draw_rect(x, cy, w, lh, COL(C_SIDEBAR_BG));
	draw_clip(x, cy, w, lh);
	for (int r = 0; r <= vis && (u32)(S.ev_scroll + r) < S.nvis; r++) {
		u32 k = (u32)S.ev_scroll + (u32)r, i = S.vis[k];
		int ry = cy + r * rh;
		const char *n = S.ev_pool.b + S.ev[i];
		if (S.ev_hdr[i]) {
			draw_text(FONT_SMALL_BOLD, x + PX(8), ty(ry, rh, FONT_SMALL_BOLD), n, strlen(n), COL(C_SECTION_HEADER_FG));
			continue;
		}
		bool cur = k == S.ev_cur && app.focus == FOCUS_WORKSPACE && S.field == F_NONE;
		if (cur)
			draw_rect(x, ry, w, rh, COL(C_LIST_ACTIVE_BG));
		else if (in_r(app.hover_x, app.hover_y, x, ry, w, rh))
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		checkbox(x + PX(8), ry, rh, S.ev_sel[i], NULL, 0, 0);
		draw_text_fit(FONT_UI, x + PX(32), ty(ry, rh, FONT_UI), n, strlen(n), w - PX(40),
		              cur ? COL(C_LIST_ACTIVE_FG) : COL(C_FG));
	}
	if (!S.nvis) {
		const char *m = "No event matches";
		draw_text(FONT_UI, x + PX(10), cy + PX(6), m, strlen(m), COL(C_DESCRIPTION_FG));
	}
	draw_noclip();
}

static void fmt_big(double v, char *b, size_t cap)
{
	char t[40];
	int k = snprintf(t, sizeof t, "%.0f", v);
	size_t o = 0;
	for (int i = 0; i < k && o + 2 < cap; i++) {
		if (i && (k - i) % 3 == 0)
			b[o++] = ',';
		b[o++] = t[i];
	}
	b[o] = 0;
}

static double res_of(const char *a, const char *b2)
{
	for (u32 i = 0; i < S.nres; i++)
		if (!strcmp(S.res_name[i], a) || (b2 && !strcmp(S.res_name[i], b2)))
			return S.res_val[i];
	return -1;
}

static void draw_run(int x, int y, int w, int h)
{
	int cy = section(x, y, "PROGRAM");
	bool foc = app.focus == FOCUS_MAN_INPUT;
	int ih = input_draw_ex(&S.prog, x, cy, w, 0, "./build/app  (relative to the project, or absolute)", foc && S.field == F_PROG);
	G.prog_x = x;
	G.prog_y = cy;
	G.prog_w = w;
	cy += ih + PX(10);
	cy = section(x, cy, "ARGUMENTS");
	ih = input_draw_ex(&S.args, x, cy, w, 0, "--flag \"quoted arg\"", foc && S.field == F_ARGS);
	G.args_x = x;
	G.args_y = cy;
	G.args_w = w;
	cy += ih + PX(6);
	char wd[1200];
	snprintf(wd, sizeof wd, "Runs in %s", app.root ? app.root : ".");
	draw_text_fit(FONT_SMALL, x, cy, wd, strlen(wd), w, COL(C_DESCRIPTION_FG));
	cy += font_m[FONT_SMALL].line + PX(12);
	/* mode */
	int rh = PX(26);
	int bx = x;
	const char *mn[2] = {"Count", "Sample"};
	for (int m = 0; m < 2; m++) {
		int bw = font_width(FONT_UI, mn[m], strlen(mn[m])) + PX(24);
		bool on = S.mode == m;
		draw_rect(bx, cy, bw, rh, on ? COL(C_BUTTON_BG) : COL(C_INPUT_BG));
		draw_frame(bx, cy, bw, rh, on ? COL(C_BUTTON_BG) : COL(C_INPUT_BORDER));
		draw_text(FONT_UI, bx + PX(12), ty(cy, rh, FONT_UI), mn[m], strlen(mn[m]), on ? COL(C_BUTTON_FG) : COL(C_FG));
		hit(bx, cy, bw, rh, H_MODE, m);
		ui_tip(bx, cy, bw, rh, m ? "Sample: pmcstat -P, then a profile with callchains and source lines"
		                          : "Count: pmcstat -p, totals for every selected event");
		bx += bw;
	}
	bx += PX(16);
	bx += checkbox(bx, cy, rh, S.children, "Child processes (-d)", H_CHILDREN, 0) + PX(16);
	if (S.mode == M_SAMPLE) {
		const char *e = "every";
		bx = draw_text(FONT_UI, bx, ty(cy, rh, FONT_UI), e, 5, COL(C_FG)) + PX(6);
		input_draw_ex(&S.rate, bx, cy, PX(90), 0, "65536", foc && S.field == F_RATE);
		G.rate_x = bx;
		G.rate_y = cy;
		G.rate_w = PX(90);
		draw_text(FONT_UI, bx + PX(96), ty(cy, rh, FONT_UI), "events", 6, COL(C_FG));
	} else
		G.rate_w = 0;
	cy += rh + PX(12);
	/* run / stop */
	if (S.running) {
		button(x, cy, "Stop", H_STOP, 0, false, "Interrupt pmcstat and the program (it still prints its counts)");
		char t[96];
		snprintf(t, sizeof t, "Running, %.1f s", (double)(vx_now_ns() - S.t0) / 1e9);
		draw_text(FONT_UI, x + PX(80), ty(cy, PX(26), FONT_UI), t, strlen(t), COL(C_DESCRIPTION_FG));
	} else {
		int bw = button(x, cy, "Run", H_RUN, 0, true, "Run the program under pmcstat (Ctrl+Enter)");
		const char *note = NULL;
		char nb[300];
		if (atomic_load(&S.post) == 1)
			note = "Converting samples...";
		else if (S.mode == M_SAMPLE && first_selected()) {
			snprintf(nb, sizeof nb, "Samples %s%s; the result opens as a profile", first_selected(),
			         n_selected() > 1 ? " (the first selected event)" : "");
			note = nb;
		}
		if (note)
			draw_text_fit(FONT_SMALL, x + bw + PX(12), ty(cy, PX(26), FONT_SMALL), note, strlen(note),
			              w - bw - PX(12), COL(C_DESCRIPTION_FG));
	}
	cy += PX(26) + PX(14);
	/* counts */
	if (S.nres) {
		cy = section(x, cy, "COUNTS");
		for (u32 i = 0; i < S.nres; i++) {
			char v[48];
			fmt_big(S.res_val[i], v, sizeof v);
			draw_text_fit(FONT_UI, x, cy, S.res_name[i], strlen(S.res_name[i]), w - PX(180), COL(C_FG));
			int vw = font_width(FONT_MONO, v, strlen(v));
			draw_text(FONT_MONO, x + w - vw, cy, v, strlen(v), COL(C_FG));
			cy += app.row_h;
		}
		/* instructions per cycle, when both were counted */
		double ins = res_of("inst_retired.any", "instructions"), cyc = res_of("cpu_clk_unhalted.thread_p", "cpu_clk_unhalted.thread");
		if (ins < 0)
			ins = res_of("ex_ret_instr", NULL);
		if (cyc < 0)
			cyc = res_of("ls_not_halted_cyc", "unhalted-cycles");
		if (ins > 0 && cyc > 0) {
			char ipc[64];
			snprintf(ipc, sizeof ipc, "%.2f", ins / cyc);
			draw_text(FONT_UI_BOLD, x, cy, "Instructions per cycle", 22, COL(C_FG));
			int vw = font_width(FONT_MONO_BOLD, ipc, strlen(ipc));
			draw_text(FONT_MONO_BOLD, x + w - vw, cy, ipc, strlen(ipc), COL(C_FG));
			cy += app.row_h;
		}
		cy += PX(10);
	}
	/* output */
	cy = section(x, cy, "OUTPUT");
	int oh = y + h - cy;
	G.out_x = x;
	G.out_y = cy;
	G.out_w = w;
	G.out_h = oh;
	draw_rect(x, cy, w, oh, COL(C_PANEL_BG));
	draw_frame(x, cy, w, oh, COL(C_PANEL_BORDER));
	if (!S.out_n) {
		const char *m = "The program's output and pmcstat's report appear here.";
		draw_text(FONT_SMALL, x + PX(10), cy + PX(8), m, strlen(m), COL(C_DESCRIPTION_FG));
		return;
	}
	/* the last lines that fit, minus the scroll */
	int lh = font_m[FONT_MONO].line;
	int vis = VX_MAX((oh - PX(12)) / lh, 1);
	u32 starts[4096];
	u32 ns = 0;
	/* walk back from the end to find line starts */
	u32 total = 0;
	for (u32 i = 0; i < S.out_n; i++)
		total += S.out[i] == '\n';
	S.out_scroll = VX_CLAMP(S.out_scroll, 0, VX_MAX((i64)total - vis + 1, 0));
	i64 skip = S.out_scroll;
	u32 e = S.out_n;
	if (e && S.out[e - 1] == '\n')
		e--;
	while (ns < (u32)vis + (u32)skip && ns < 4096) {
		u32 s = e;
		while (s > 0 && S.out[s - 1] != '\n')
			s--;
		starts[ns++] = s;
		if (!s)
			break;
		e = s - 1;
	}
	draw_clip(x + PX(1), cy + PX(1), w - PX(2), oh - PX(2));
	int row = 0;
	for (i64 k = (i64)ns - 1; k >= skip; k--) {
		u32 s = starts[k], en = s;
		while (en < S.out_n && S.out[en] != '\n')
			en++;
		bool cmd = S.out[s] == '$';
		draw_text(FONT_MONO, x + PX(8), cy + PX(6) + row * lh, S.out + s, en - s,
		          cmd ? COL(C_LINK) : COL(C_EDITOR_FG));
		row++;
	}
	draw_noclip();
}

void pmcview_ws_draw(int x, int y, int w, int h)
{
	nhit = 0;
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	/* header */
	int hh = PX(52);
	draw_icon(ICON_PULSE_S, x + PX(24), y + PX(18), COL(C_FG));
	draw_text(FONT_UI_BOLD, x + PX(48), y + PX(10), "Profiling", 9, COL(C_FG));
	const char *sub = "Hardware performance counters: FreeBSD hwpmc through pmcstat(8)";
	draw_text(FONT_SMALL, x + PX(48), y + PX(28), sub, strlen(sub), COL(C_DESCRIPTION_FG));
	if (checked() && S.avail == AV_OK) {
		char st[160];
		snprintf(st, sizeof st, "hwpmc ready%s%s", S.cpuid[0] ? "  -  " : "", S.cpuid);
		int sw = font_width(FONT_SMALL, st, strlen(st));
		draw_rect(x + w - sw - PX(36), y + PX(14), sw + PX(16), PX(22), (COL(C_GUTTER_ADDED) & 0xFFFFFF00u) | 0x40u);
		draw_text(FONT_SMALL, x + w - sw - PX(28), ty(y + PX(14), PX(22), FONT_SMALL), st, strlen(st), COL(C_FG));
	}
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	y += hh;
	h -= hh;
	if (!checked()) {
		const char *m = "Checking pmcstat...";
		draw_text(FONT_UI, x + PX(32), y + PX(24), m, strlen(m), COL(C_DESCRIPTION_FG));
		return;
	}
	if (S.avail != AV_OK) {
		draw_unavailable(x, y, w);
		return;
	}
	if (S.proc_debug_off) {
		int bh = PX(30);
		draw_rect(x, y, w, bh, (COL(C_WARNING) & 0xFFFFFF00u) | 0x30u);
		const char *m = "security.bsd.unprivileged_proc_debug is 0: counting your own processes needs root. As root:";
		int ex = draw_text(FONT_SMALL, x + PX(16), ty(y, bh, FONT_SMALL), m, strlen(m), COL(C_FG));
		cmd_line(ex + PX(8), y + PX(1), VX_MIN(PX(460), x + w - ex - PX(24)), "sysctl security.bsd.unprivileged_proc_debug=1", 0);
		y += bh + PX(4);
		h -= bh + PX(4);
	}
	int pad = PX(24), gap = PX(28);
	int lw = VX_CLAMP((w - 2 * pad - gap) * 2 / 5, PX(260), PX(520));
	int rx = x + pad + lw + gap, rw = x + w - pad - rx;
	draw_events(x + pad, y + PX(16), lw, h - PX(32));
	draw_run(rx, y + PX(16), rw, h - PX(32));
}

/* ---- input ----------------------------------------------------------------------------------- */

static Input *field_input(int f)
{
	return f == F_FILTER ? &S.filter : f == F_PROG ? &S.prog : f == F_ARGS ? &S.args : f == F_RATE ? &S.rate : NULL;
}

static void focus_field(int f)
{
	S.field = f;
	app.focus = f == F_NONE ? FOCUS_WORKSPACE : FOCUS_MAN_INPUT;
}

static void toggle_cur(void)
{
	if (S.ev_cur < S.nvis && !S.ev_hdr[S.vis[S.ev_cur]])
		S.ev_sel[S.vis[S.ev_cur]] ^= 1;
}

static void hit_run(int act, int arg)
{
	switch (act) {
	case H_RECHECK: check_start(); break;
	case H_MODE: S.mode = arg; break;
	case H_CHILDREN: S.children = !S.children; break;
	case H_RUN: run_start(); break;
	case H_STOP: run_stop(); break;
	case H_COPY:
		if (arg >= 0 && arg < 8 && copy_text[arg]) {
			ui_clipboard_set(copy_text[arg], strlen(copy_text[arg]));
			ui_status("Copied: %s", copy_text[arg]);
		}
		break;
	case H_UNSEL:
		if ((u32)arg < S.nev)
			S.ev_sel[arg] = 0;
		break;
	case H_CLEAR:
		for (u32 i = 0; i < S.nev; i++)
			S.ev_sel[i] = 0;
		break;
	}
}

void pmcview_ws_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	app.dirty = true;
	Input *in = app.focus == FOCUS_MAN_INPUT ? field_input(S.field) : NULL;
	if (e->type == EV_TEXT) {
		if (!in) {
			/* typing in the list filters it */
			focus_field(F_FILTER);
			in = &S.filter;
		}
		input_text(in, e->text, strlen(e->text));
		return;
	}
	if (e->type == EV_KEY) {
		int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
		if (mods == MOD_CTRL && (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER)) {
			run_start();
			return;
		}
		if (mods == MOD_CTRL && (e->key == SDLK_F || e->key == SDLK_L)) {
			focus_field(F_FILTER);
			return;
		}
		if (e->key == SDLK_TAB) {
			static const int order[] = {F_FILTER, F_PROG, F_ARGS, F_RATE};
			int n = S.mode == M_SAMPLE ? 4 : 3, k = 0;
			for (int i = 0; i < n; i++)
				if (order[i] == S.field)
					k = i;
			k = (k + ((mods & MOD_SHIFT) ? n - 1 : 1)) % n;
			focus_field(order[k]);
			return;
		}
		if (in) {
			if (e->key == SDLK_ESCAPE) {
				if (S.field == F_FILTER && S.filter.n)
					input_set(&S.filter, "", 0);
				else
					focus_field(F_NONE);
				return;
			}
			if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
				if (S.field == F_FILTER) {
					/* Enter toggles the only (or the highlighted) match */
					refilter();
					toggle_cur();
				} else
					run_start();
				return;
			}
			if (S.field == F_FILTER && (e->key == SDLK_DOWN || e->key == SDLK_UP)) {
				focus_field(F_NONE);
				return;
			}
			input_key(in, e);
			return;
		}
		/* the event list */
		i64 vis = VX_MAX(G.list_h / app.row_h, 1), c = S.ev_cur;
		switch (e->key) {
		case SDLK_UP: c--; break;
		case SDLK_DOWN: c++; break;
		case SDLK_PAGEUP: c -= vis; break;
		case SDLK_PAGEDOWN: c += vis; break;
		case SDLK_HOME: c = 0; break;
		case SDLK_END: c = (i64)S.nvis - 1; break;
		case SDLK_SPACE:
		case SDLK_RETURN:
		case SDLK_KP_ENTER: toggle_cur(); return;
		case SDLK_F5: run_start(); return;
		default: return;
		}
		if (!S.nvis)
			return;
		S.ev_cur = (u32)VX_CLAMP(c, 0, (i64)S.nvis - 1);
		if ((i64)S.ev_cur < S.ev_scroll)
			S.ev_scroll = S.ev_cur;
		if ((i64)S.ev_cur >= S.ev_scroll + vis)
			S.ev_scroll = (i64)S.ev_cur - vis + 1;
		return;
	}
	if (e->type == EV_WHEEL) {
		int d = (int)(e->wy * 3);
		if (in_r(e->x, e->y, G.list_x, G.list_y, G.list_w, G.list_h))
			S.ev_scroll -= d;
		else if (in_r(e->x, e->y, G.out_x, G.out_y, G.out_w, G.out_h))
			S.out_scroll += d;
		return;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return;
	for (int i = nhit; i-- > 0;)
		if (in_r(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
			hit_run(hits[i].act, hits[i].arg);
			return;
		}
	struct {
		int f, x, y, w;
	} boxes[] = {{F_FILTER, G.filt_x, G.filt_y, G.filt_w},
	             {F_PROG, G.prog_x, G.prog_y, G.prog_w},
	             {F_ARGS, G.args_x, G.args_y, G.args_w},
	             {F_RATE, G.rate_x, G.rate_y, G.rate_w}};
	for (size_t i = 0; i < VX_COUNT(boxes); i++) {
		Input *bi = field_input(boxes[i].f);
		if (boxes[i].w && in_r(e->x, e->y, boxes[i].x, boxes[i].y, boxes[i].w, input_height(bi, boxes[i].w, 0))) {
			focus_field(boxes[i].f);
			input_click_ex(bi, boxes[i].x, boxes[i].y, boxes[i].w, 0, e->x, e->y, e->clicks);
			return;
		}
	}
	if (in_r(e->x, e->y, G.list_x, G.list_y, G.list_w, G.list_h)) {
		i64 r = S.ev_scroll + (e->y - G.list_y) / app.row_h;
		focus_field(F_NONE);
		if (r >= 0 && (u32)r < S.nvis) {
			S.ev_cur = (u32)r;
			toggle_cur();
		}
		return;
	}
	focus_field(F_NONE);
}
