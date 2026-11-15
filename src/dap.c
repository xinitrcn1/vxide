/* Debug Adapter Protocol client: transport (Content-Length framed JSON
 * over the adapter's stdio, non-blocking both ways), the launch handshake,
 * and the session model behind dap.h. Strings that belong to one stop
 * (threads, frames, scopes, variables, evaluations) live in a pool that
 * is cleared when the program runs again. */
#define _POSIX_C_SOURCE 200809L
#include "dap.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NONE_U32 UINT32_MAX

enum {
	RQ_INIT = 1,
	RQ_LAUNCH,
	RQ_BPS,
	RQ_DBPS,
	RQ_EXC,
	RQ_CONFDONE,
	RQ_THREADS,
	RQ_STACK,
	RQ_SCOPES,
	RQ_VARS,
	RQ_EVAL,
	RQ_RUN, /* continue / next / stepIn / stepOut / pause */
	RQ_DISC,
	RQ_WINFO,
	RQ_WADDR,
	RQ_WSIZE,
	RQ_WTYPE,
	RQ_SLOTS,
	RQ_WGDB,  /* gdb: watch / rwatch / awatch through the console */
	RQ_QUIET, /* answers nobody reads */
};
enum { W_ADDR, W_SIZE, W_TYPE, W_INFO, W_READY, W_ERR };

struct Dap {
	pid_t pid;
	int in, out;
	bool dead;
	char adapter[64];
	bool lldb, gdb;
	/* transport */
	char *rb;
	u32 rn, rcap;
	JBuf wb;
	u32 woff;
	JBuf msg;
	Json js;
	i32 seq;
	/* requests in flight */
	u32 np, pcap;
	i32 *p_seq;
	u8 *p_kind;
	i64 *p_a, *p_b;
	/* session */
	int state;
	bool configured, caps_data_bp, disconnecting;
	u64 kill_at;
	char launch_json[16384];
	char reason[64], stoptext[512];
	int exit_code;
	u32 stops;
	u32 stops_at_run; /* stops seen when we last resumed */
	bool main_once;   /* a breakpoint on main until the first stop */
	u32 main_stop;    /* the stop it caused */
	bool changed;
	Pool pp; /* persistent strings */
	Pool sp; /* this stop's strings */
	/* breakpoints the UI asked for (to send once configured) and the results */
	u32 nbf, bfcap;
	u32 *bf_path;   /* pp */
	u32 *bf_json;   /* pp: the request's arguments */
	u8 *bf_sent;
	u32 nbr, brcap;
	u32 *br_path, *br_line, *br_actual;
	i64 *br_id;
	u8 *br_ok;
	/* threads */
	u32 nth, thcap;
	i64 *th_id;
	u32 *th_name;
	i64 cur_thread, stop_thread;
	/* frames of the selected thread */
	u32 nfr, frcap;
	i64 *fr_id;
	u32 *fr_name, *fr_path, *fr_line, *fr_col;
	u64 *fr_ip;
	u32 cur_frame;
	/* scopes of the selected frame */
	u32 nsc, sccap;
	u32 *sc_name;
	i64 *sc_ref;
	u8 *sc_exp;
	/* variable sets: ref -> nodes [first, first+count) */
	u32 nvs, vscap;
	i64 *vs_ref;
	u32 *vs_first, *vs_count;
	u8 *vs_state;
	u64 *vs_key; /* the container's path key */
	u32 nvn, vncap;
	u32 *vn_name, *vn_value, *vn_type, *vn_eval;
	i64 *vn_ref;
	u8 *vn_changed;
	u64 *vn_key; /* scope / parent / name: identifies a variable across stops */
	/* values at the last stop, to mark changes: sorted hashes */
	u32 nprev, prevcap;
	u64 *prev_key, *prev_val;
	/* evaluations */
	u32 nev, evcap;
	u8 *ev_state;
	u32 *ev_value, *ev_type;
	i64 *ev_ref;
	/* data breakpoints */
	u32 nw, wcap;
	u32 *w_expr, *w_desc, *w_dataid; /* pp */
	u8 *w_access, *w_state, *w_ok, *w_sw;
	i32 *w_gdbno; /* gdb's watchpoint number */
	u64 *w_addr;
	u32 *w_size;
	int hw_slots;
	bool slots_asked;
	/* output */
	u32 no, ocap, opop;
	u32 *o_cat, *o_text;
	Pool op;
};

#define P_COLS(d) SOA((d)->p_seq), SOA((d)->p_kind), SOA((d)->p_a), SOA((d)->p_b)
#define BF_COLS(d) SOA((d)->bf_path), SOA((d)->bf_json), SOA((d)->bf_sent)
#define BR_COLS(d) SOA((d)->br_path), SOA((d)->br_line), SOA((d)->br_actual), SOA((d)->br_id), SOA((d)->br_ok)
#define TH_COLS(d) SOA((d)->th_id), SOA((d)->th_name)
#define FR_COLS(d)                                                                                                     \
	SOA((d)->fr_id), SOA((d)->fr_name), SOA((d)->fr_path), SOA((d)->fr_line), SOA((d)->fr_col), SOA((d)->fr_ip)
#define SC_COLS(d) SOA((d)->sc_name), SOA((d)->sc_ref), SOA((d)->sc_exp)
#define VS_COLS(d) SOA((d)->vs_ref), SOA((d)->vs_first), SOA((d)->vs_count), SOA((d)->vs_state), SOA((d)->vs_key)
#define VN_COLS(d)                                                                                                     \
	SOA((d)->vn_name), SOA((d)->vn_value), SOA((d)->vn_type), SOA((d)->vn_eval), SOA((d)->vn_ref),                    \
	    SOA((d)->vn_changed), SOA((d)->vn_key)
#define PV_COLS(d) SOA((d)->prev_key), SOA((d)->prev_val)
#define EV_COLS(d) SOA((d)->ev_state), SOA((d)->ev_value), SOA((d)->ev_type), SOA((d)->ev_ref)
#define W_COLS(d)                                                                                                      \
	SOA((d)->w_expr), SOA((d)->w_desc), SOA((d)->w_dataid), SOA((d)->w_access), SOA((d)->w_state), SOA((d)->w_ok),    \
	    SOA((d)->w_addr), SOA((d)->w_size), SOA((d)->w_sw), SOA((d)->w_gdbno)
#define O_COLS(d) SOA((d)->o_cat), SOA((d)->o_text)

static u64 hstr(const char *s, u64 h)
{
	if (!h)
		h = 1469598103934665603ull;
	while (*s) {
		h ^= (u8)*s++;
		h *= 1099511628211ull;
	}
	return h;
}

static const char *sstr(const Dap *d, u32 off) { return d->sp.b ? d->sp.b + off : ""; }
static const char *pstr(const Dap *d, u32 off) { return d->pp.b ? d->pp.b + off : ""; }

/* ---- transport --------------------------------------------------------------------------- */

static void flush(Dap *d)
{
	while (d->in >= 0 && d->woff < d->wb.n) {
		ssize_t k = write(d->in, d->wb.b + d->woff, d->wb.n - d->woff);
		if (k > 0) {
			d->woff += (u32)k;
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		close(d->in);
		d->in = -1;
		d->dead = true;
		break;
	}
	if (d->woff == d->wb.n)
		d->wb.n = d->woff = 0;
}

/* requests: req_begin, then the arguments object, then req_end */
static i32 req_begin(Dap *d, const char *cmd)
{
	jb_reset(&d->msg);
	i32 s = ++d->seq;
	jb_raw(&d->msg, "{\"seq\":");
	jb_int(&d->msg, s);
	jb_raw(&d->msg, ",\"type\":\"request\",\"command\":");
	jb_str(&d->msg, cmd);
	jb_raw(&d->msg, ",\"arguments\":");
	return s;
}

static i32 req_end(Dap *d, int kind, i64 a, i64 b)
{
	jb_raw(&d->msg, "}");
	char hdr[48];
	int h = snprintf(hdr, sizeof hdr, "Content-Length: %u\r\n\r\n", d->msg.n);
	jb_rawn(&d->wb, hdr, (size_t)h);
	jb_rawn(&d->wb, d->msg.b, d->msg.n);
	SoaCol c[] = {P_COLS(d)};
	soa_grow(c, 4, &d->pcap, d->np + 1, d->np);
	d->p_seq[d->np] = d->seq;
	d->p_kind[d->np] = (u8)kind;
	d->p_a[d->np] = a;
	d->p_b[d->np] = b;
	d->np++;
	flush(d);
	return d->seq;
}

static i32 simple(Dap *d, const char *cmd, const char *args, int kind, i64 a)
{
	req_begin(d, cmd);
	jb_raw(&d->msg, args);
	return req_end(d, kind, a, 0);
}

static void out_add(Dap *d, const char *cat, const char *text, size_t n)
{
	SoaCol c[] = {O_COLS(d)};
	soa_grow(c, 2, &d->ocap, d->no + 1, d->no);
	d->o_cat[d->no] = pool_add(&d->op, cat, (u32)strlen(cat));
	d->o_text[d->no] = pool_add(&d->op, text, (u32)n);
	d->no++;
	d->changed = true;
}

static const char *const adapters[] = {"lldb-dap23", "lldb-dap21", "lldb-dap", "lldb-dap19", "lldb-dap-devel", "gdb"};

static pid_t spawn(char *const argv[], int *in, int *out)
{
	int pi[2], po[2];
	if (pipe(pi) != 0)
		return -1;
	if (pipe(po) != 0) {
		close(pi[0]);
		close(pi[1]);
		return -1;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pi[0], 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, pi[1]);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	pid_t pid;
	int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pi[0]);
	close(po[1]);
	if (rc != 0) {
		close(pi[1]);
		close(po[0]);
		return -1;
	}
	/* posix_spawnp succeeds before exec on some systems: a quick exit 127
	 * means the program was not found */
	struct timespec ts = {0, 30000000};
	nanosleep(&ts, NULL);
	int st;
	if (waitpid(pid, &st, WNOHANG) == pid) {
		close(pi[1]);
		close(po[0]);
		return -1;
	}
	fcntl(pi[1], F_SETFL, fcntl(pi[1], F_GETFL) | O_NONBLOCK);
	fcntl(po[0], F_SETFL, fcntl(po[0], F_GETFL) | O_NONBLOCK);
	fcntl(pi[1], F_SETFD, FD_CLOEXEC);
	fcntl(po[0], F_SETFD, FD_CLOEXEC);
	*in = pi[1];
	*out = po[0];
	return pid;
}

Dap *dap_start(const DapLaunch *l, char *err, size_t errcap)
{
	struct sigaction sa;
	if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sa, NULL);
	}
	Dap *d = vx_alloc(sizeof *d);
	d->in = d->out = -1;
	d->exit_code = -1;
	d->hw_slots = -1;
	d->cur_thread = d->stop_thread = -1;
	pool_add(&d->pp, "", 0);
	pool_add(&d->sp, "", 0);
	const char *env = getenv("VXIDE_DAP");
	const char *list[8];
	u32 nl = 0;
	if (l->adapter && *l->adapter)
		list[nl++] = l->adapter;
	else {
		if (env && *env)
			list[nl++] = env;
		for (size_t i = 0; i < VX_COUNT(adapters); i++)
			list[nl++] = adapters[i];
	}
	for (u32 i = 0; i < nl && d->pid <= 0; i++) {
		const char *base = strrchr(list[i], '/');
		base = base ? base + 1 : list[i];
		bool gdb = !strncmp(base, "gdb", 3);
		char *argv[5] = {(char *)list[i], NULL, NULL, NULL, NULL};
		if (gdb) {
			argv[1] = "-q";
			argv[2] = "-i";
			argv[3] = "dap";
		}
		pid_t p = spawn(argv, &d->in, &d->out);
		if (p > 0) {
			d->pid = p;
			snprintf(d->adapter, sizeof d->adapter, "%s", base);
			d->gdb = gdb;
			d->lldb = !gdb && strstr(base, "lldb");
		}
	}
	if (d->pid <= 0) {
		snprintf(err, errcap, "No debug adapter found (install lldb-dap or gdb 14+, or set VXIDE_DAP)");
		free(d);
		return NULL;
	}
	/* the launch arguments, sent after initialize */
	JBuf a = {0};
	jb_raw(&a, "{\"program\":");
	jb_str(&a, l->program);
	jb_raw(&a, ",\"args\":[");
	for (u32 i = 0; i < l->nargs; i++) {
		if (i)
			jb_raw(&a, ",");
		jb_str(&a, l->args[i]);
	}
	jb_raw(&a, "],\"cwd\":");
	jb_str(&a, l->cwd ? l->cwd : ".");
	/* "stop at main": gdb has a launch option; lldb's stopOnEntry stops in
	 * the dynamic loader instead, so it gets a one-shot breakpoint on main */
	jb_raw(&a, d->gdb ? ",\"stopAtBeginningOfMainSubprogram\":" : ",\"stopOnEntry\":");
	jb_raw(&a, l->stop_on_entry && d->gdb ? "true" : "false");
	d->main_once = l->stop_on_entry && !d->gdb;
	jb_raw(&a, "}");
	snprintf(d->launch_json, sizeof d->launch_json, "%.*s", (int)a.n, a.b);
	jb_free(&a);
	simple(d,
	       "initialize",
	       "{\"clientID\":\"vxide\",\"clientName\":\"vxide\",\"adapterID\":\"vxide\",\"linesStartAt1\":true,"
	       "\"columnsStartAt1\":true,\"pathFormat\":\"path\",\"supportsVariableType\":true,"
	       "\"supportsRunInTerminalRequest\":false}",
	       RQ_INIT, 0);
	d->state = DS_STARTING;
	return d;
}

bool dap_alive(Dap *d) { return d && d->pid > 0 && !d->dead; }
int dap_fd(Dap *d) { return d && !d->dead ? d->out : -1; }

static void reap(Dap *d)
{
	if (d->pid > 0) {
		int st;
		pid_t w = waitpid(d->pid, &st, WNOHANG);
		if (w == d->pid || (w < 0 && errno == ECHILD)) {
			d->pid = 0;
			d->dead = true;
		}
	}
}

void dap_stop(Dap *d)
{
	if (!d || d->dead)
		return;
	if (!d->disconnecting) {
		d->disconnecting = true;
		simple(d, "disconnect", "{\"terminateDebuggee\":true}", RQ_DISC, 0);
		d->kill_at = vx_now_ns() + 2000000000ull;
	}
}

void dap_free(Dap *d)
{
	if (!d)
		return;
	if (d->pid > 0) {
		kill(d->pid, SIGKILL);
		waitpid(d->pid, NULL, 0);
	}
	if (d->in >= 0)
		close(d->in);
	if (d->out >= 0)
		close(d->out);
	SoaCol a[] = {P_COLS(d)}, b[] = {BF_COLS(d)}, c[] = {BR_COLS(d)}, e[] = {TH_COLS(d)}, f[] = {FR_COLS(d)},
	       g[] = {SC_COLS(d)}, h[] = {VS_COLS(d)}, i[] = {VN_COLS(d)}, j[] = {PV_COLS(d)}, k[] = {EV_COLS(d)},
	       m[] = {W_COLS(d)}, o[] = {O_COLS(d)};
	soa_free(a, 4);
	soa_free(b, 3);
	soa_free(c, 5);
	soa_free(e, 2);
	soa_free(f, 6);
	soa_free(g, 3);
	soa_free(h, 5);
	soa_free(i, 7);
	soa_free(j, 2);
	soa_free(k, 4);
	soa_free(m, 10);
	soa_free(o, 2);
	free(d->rb);
	jb_free(&d->wb);
	jb_free(&d->msg);
	json_free(&d->js);
	pool_free(&d->pp);
	pool_free(&d->sp);
	pool_free(&d->op);
	free(d);
}

/* ---- the model ------------------------------------------------------------------------- */

static void clear_stop(Dap *d, bool keep_prev)
{
	/* remember the values seen (this stop's override older ones) to mark
	 * what changed when they are looked at again */
	if (keep_prev && d->nvn) {
		u32 old = d->nprev;
		for (u32 i = 0; i < d->nvn && d->nprev < 50000; i++) {
			if (!d->vn_key[i])
				continue;
			SoaCol c[] = {PV_COLS(d)};
			soa_grow(c, 2, &d->prevcap, d->nprev + 1, d->nprev);
			d->prev_key[d->nprev] = d->vn_key[i];
			d->prev_val[d->nprev] = hstr(sstr(d, d->vn_value[i]), 0);
			d->nprev++;
		}
		/* stable sort by key, then keep the last (newest) of each key */
		for (u32 i = 1; i < d->nprev; i++)
			for (u32 j = i; j > 0 && d->prev_key[j - 1] > d->prev_key[j]; j--) {
				u64 tk = d->prev_key[j], tv = d->prev_val[j];
				d->prev_key[j] = d->prev_key[j - 1];
				d->prev_val[j] = d->prev_val[j - 1];
				d->prev_key[j - 1] = tk;
				d->prev_val[j - 1] = tv;
			}
		u32 k = 0;
		for (u32 i = 0; i < d->nprev; i++) {
			if (i + 1 < d->nprev && d->prev_key[i + 1] == d->prev_key[i])
				continue;
			d->prev_key[k] = d->prev_key[i];
			d->prev_val[k] = d->prev_val[i];
			k++;
		}
		d->nprev = k;
		(void)old;
	}
	d->nfr = d->nsc = d->nvs = d->nvn = d->nev = 0;
	d->cur_frame = 0;
	pool_clear(&d->sp);
	pool_add(&d->sp, "", 0);
	/* threads keep their ids; their names are re-read at the next stop */
	for (u32 i = 0; i < d->nth; i++)
		d->th_name[i] = 0;
	d->changed = true;
}

static i64 tok_i64(const Json *j, u32 t, i64 def) { return t == JSON_NIL ? def : json_int(j, t, def); }

static u32 tok_sp(Dap *d, u32 t) { return t == JSON_NIL ? 0 : json_str_pool(&d->js, t, &d->sp); }
static u32 tok_pp(Dap *d, u32 t) { return t == JSON_NIL ? 0 : json_str_pool(&d->js, t, &d->pp); }

static void request_stack(Dap *d, i64 thread)
{
	req_begin(d, "stackTrace");
	jb_raw(&d->msg, "{\"threadId\":");
	jb_int(&d->msg, thread);
	jb_raw(&d->msg, ",\"startFrame\":0,\"levels\":64}");
	req_end(d, RQ_STACK, thread, 0);
}

static void request_scopes(Dap *d)
{
	if (d->cur_frame >= d->nfr)
		return;
	req_begin(d, "scopes");
	jb_raw(&d->msg, "{\"frameId\":");
	jb_int(&d->msg, d->fr_id[d->cur_frame]);
	jb_raw(&d->msg, "}");
	req_end(d, RQ_SCOPES, d->fr_id[d->cur_frame], 0);
}

static void send_bps(Dap *d)
{
	for (u32 i = 0; i < d->nbf; i++)
		if (!d->bf_sent[i]) {
			d->bf_sent[i] = 1;
			req_begin(d, "setBreakpoints");
			jb_raw(&d->msg, pstr(d, d->bf_json[i]));
			req_end(d, RQ_BPS, (i64)d->bf_path[i], 0);
		}
}

static void push_watches(Dap *d)
{
	req_begin(d, "setDataBreakpoints");
	jb_raw(&d->msg, "{\"breakpoints\":[");
	bool first = true;
	for (u32 i = 0; i < d->nw; i++)
		if (d->w_state[i] == W_READY) {
			if (!first)
				jb_raw(&d->msg, ",");
			first = false;
			jb_raw(&d->msg, "{\"dataId\":");
			jb_str(&d->msg, pstr(d, d->w_dataid[i]));
			jb_raw(&d->msg, ",\"accessType\":");
			jb_str(&d->msg, d->w_access[i] == DW_READ ? "read" : d->w_access[i] == DW_READWRITE ? "readWrite" : "write");
			jb_raw(&d->msg, "}");
		}
	jb_raw(&d->msg, "]}");
	req_end(d, RQ_DBPS, 0, 0);
}

static i32 eval_req(Dap *d, const char *expr, const char *ctx, int kind, i64 a)
{
	req_begin(d, "evaluate");
	jb_raw(&d->msg, "{\"expression\":");
	jb_str(&d->msg, expr);
	jb_raw(&d->msg, ",\"context\":");
	jb_str(&d->msg, ctx);
	if (d->cur_frame < d->nfr) {
		jb_raw(&d->msg, ",\"frameId\":");
		jb_int(&d->msg, d->fr_id[d->cur_frame]);
	}
	jb_raw(&d->msg, "}");
	return req_end(d, kind, a, 0);
}

static void on_stopped(Dap *d, u32 body)
{
	Json *j = &d->js;
	clear_stop(d, true);
	d->state = DS_STOPPED;
	d->stops++;
	if (d->main_once) { /* one-shot: main is reached (or something stopped first) */
		d->main_once = false;
		d->main_stop = d->stops;
		simple(d, "setFunctionBreakpoints", "{\"breakpoints\":[]}", RQ_QUIET, 0);
	}
	char b[512];
	u32 r = json_get(j, body, "reason"), t = json_get(j, body, "description");
	if (t == JSON_NIL)
		t = json_get(j, body, "text");
	b[0] = 0;
	if (r != JSON_NIL)
		json_unescape(j->src + j->start[r], VX_MIN(j->end[r] - j->start[r], 60u), b);
	snprintf(d->reason, sizeof d->reason, "%s", b);
	b[0] = 0;
	if (t != JSON_NIL && j->end[t] - j->start[t] < sizeof b - 1)
		b[json_unescape(j->src + j->start[t], j->end[t] - j->start[t], b)] = 0;
	snprintf(d->stoptext, sizeof d->stoptext, "%s", b);
	d->stop_thread = tok_i64(j, json_get(j, body, "threadId"), d->stop_thread);
	d->cur_thread = d->stop_thread;
	simple(d, "threads", "{}", RQ_THREADS, 0);
	if (d->cur_thread >= 0)
		request_stack(d, d->cur_thread);
	if (d->lldb && !d->slots_asked) {
		d->slots_asked = true;
		eval_req(d, "watchpoint list", "repl", RQ_SLOTS, 0);
	}
}

static u32 frame_count_for(Dap *d) { return d->nfr; }

static void on_event(Dap *d, u32 root)
{
	Json *j = &d->js;
	u32 ev = json_get(j, root, "event"), body = json_get(j, root, "body");
	if (json_eq(j, ev, "initialized")) {
		send_bps(d);
		if (d->main_once)
			simple(d, "setFunctionBreakpoints", "{\"breakpoints\":[{\"name\":\"main\"}]}", RQ_QUIET, 0);
		simple(d, "setExceptionBreakpoints", "{\"filters\":[]}", RQ_EXC, 0);
		simple(d, "configurationDone", "{}", RQ_CONFDONE, 0);
		d->configured = true;
	} else if (json_eq(j, ev, "stopped")) {
		on_stopped(d, body);
	} else if (json_eq(j, ev, "continued")) {
		/* gdb may report "continued" after the stop that followed: a stop
		 * newer than our last resume wins */
		if (d->state == DS_STOPPED && d->stops > d->stops_at_run)
			return;
		if (d->state == DS_STOPPED)
			clear_stop(d, true); /* keep what was seen, to mark changes */
		d->state = DS_RUNNING;
		d->changed = true;
	} else if (json_eq(j, ev, "exited")) {
		d->exit_code = (int)tok_i64(j, json_get(j, body, "exitCode"), -1);
		d->changed = true;
	} else if (json_eq(j, ev, "terminated")) {
		if (d->state == DS_STOPPED)
			clear_stop(d, false);
		d->state = DS_EXITED;
		d->changed = true;
		dap_stop(d);
	} else if (json_eq(j, ev, "output")) {
		u32 c = json_get(j, body, "category"), o = json_get(j, body, "output");
		char cat[32] = "console";
		if (c != JSON_NIL && j->end[c] - j->start[c] < sizeof cat)
			cat[json_unescape(j->src + j->start[c], j->end[c] - j->start[c], cat)] = 0;
		if (strcmp(cat, "telemetry") && o != JSON_NIL) {
			u32 n = j->end[o] - j->start[o];
			char *t = vx_alloc(n + 1);
			u32 k = json_unescape(j->src + j->start[o], n, t);
			out_add(d, cat, t, k);
			free(t);
		}
	} else if (json_eq(j, ev, "thread")) {
		/* threads come and go while running; the list is re-read at each stop */
		u32 rs = json_get(j, body, "reason");
		i64 id = tok_i64(j, json_get(j, body, "threadId"), -1);
		if (json_eq(j, rs, "started") && id >= 0) {
			bool have = false;
			for (u32 i = 0; i < d->nth; i++)
				have |= d->th_id[i] == id;
			if (!have) {
				SoaCol c[] = {TH_COLS(d)};
				soa_grow(c, 2, &d->thcap, d->nth + 1, d->nth);
				d->th_id[d->nth] = id;
				d->th_name[d->nth] = 0;
				d->nth++;
			}
		} else if (json_eq(j, rs, "exited"))
			for (u32 i = 0; i < d->nth; i++)
				if (d->th_id[i] == id) {
					d->th_id[i] = d->th_id[d->nth - 1];
					d->th_name[i] = d->th_name[d->nth - 1];
					d->nth--;
					break;
				}
		d->changed = true;
	} else if (json_eq(j, ev, "breakpoint")) {
		u32 bp = json_get(j, body, "breakpoint");
		i64 id = tok_i64(j, json_get(j, bp, "id"), -1);
		u32 line = (u32)tok_i64(j, json_get(j, bp, "line"), 0);
		bool hit = false;
		for (u32 i = 0; i < d->nbr; i++)
			if (id >= 0 && d->br_id[i] == id) {
				d->br_ok[i] = json_bool(j, json_get(j, bp, "verified"), d->br_ok[i]);
				d->br_actual[i] = line ? line : d->br_actual[i];
				hit = true;
			}
		if (!hit && line) {
			/* gdb answers setBreakpoints before it knows the ids: match by place */
			char path[1024] = "";
			u32 pt = json_path(j, bp, "source.path");
			if (pt != JSON_NIL && j->end[pt] - j->start[pt] < sizeof path)
				path[json_unescape(j->src + j->start[pt], j->end[pt] - j->start[pt], path)] = 0;
			for (u32 i = 0; i < d->nbr; i++)
				if ((d->br_line[i] == line || d->br_actual[i] == line) && !strcmp(pstr(d, d->br_path[i]), path)) {
					d->br_id[i] = id;
					d->br_ok[i] = json_bool(j, json_get(j, bp, "verified"), d->br_ok[i]);
					d->br_actual[i] = line;
					break;
				}
		}
		d->changed = true;
	}
}

static int watch_size_of_type(const char *t)
{
	if (strchr(t, '*'))
		return 8;
	static const struct {
		const char *n;
		int s;
	} k[] = {{"char", 1}, {"bool", 1}, {"_Bool", 1}, {"int8", 1}, {"uint8", 1}, {"short", 2}, {"int16", 2},
	         {"uint16", 2}, {"float", 4}, {"int32", 4}, {"uint32", 4}, {"long double", 8}, {"double", 8},
	         {"long", 8}, {"int64", 8}, {"uint64", 8}, {"size_t", 8}, {"int", 4}, {"unsigned", 4}};
	for (size_t i = 0; i < VX_COUNT(k); i++)
		if (strstr(t, k[i].n))
			return k[i].s;
	return 8;
}

static void watch_ready(Dap *d, u32 w)
{
	u32 sz = d->w_size[w];
	u32 hw = sz <= 1 ? 1 : sz <= 2 ? 2 : sz <= 4 ? 4 : 8;
	char id[64], desc[200];
	snprintf(id, sizeof id, "%llX/%u", (unsigned long long)d->w_addr[w], hw);
	snprintf(desc, sizeof desc, "%u bytes at 0x%llx%s", hw, (unsigned long long)d->w_addr[w],
	         sz > 8 ? " (the first 8 of a larger object)" : "");
	d->w_dataid[w] = pool_add(&d->pp, id, (u32)strlen(id));
	d->w_desc[w] = pool_add(&d->pp, desc, (u32)strlen(desc));
	d->w_state[w] = W_READY;
	push_watches(d);
}

static void watch_fail(Dap *d, u32 w, const char *why)
{
	d->w_state[w] = W_ERR;
	d->w_ok[w] = 0;
	d->w_desc[w] = pool_add(&d->pp, why, (u32)strlen(why));
	d->changed = true;
}

static void on_response(Dap *d, u32 root)
{
	Json *j = &d->js;
	i32 rs = (i32)tok_i64(j, json_get(j, root, "request_seq"), -1);
	u32 pi = NONE_U32;
	for (u32 i = 0; i < d->np; i++)
		if (d->p_seq[i] == rs) {
			pi = i;
			break;
		}
	if (pi == NONE_U32)
		return;
	int kind = d->p_kind[pi];
	i64 a = d->p_a[pi];
	d->p_seq[pi] = d->p_seq[d->np - 1];
	d->p_kind[pi] = d->p_kind[d->np - 1];
	d->p_a[pi] = d->p_a[d->np - 1];
	d->p_b[pi] = d->p_b[d->np - 1];
	d->np--;
	bool ok = json_bool(j, json_get(j, root, "success"), false);
	u32 body = json_get(j, root, "body");
	u32 msg = json_get(j, root, "message");
	char err[400] = "";
	if (!ok) {
		/* "message", or (newer lldb-dap) body.error.format */
		u32 e = json_path(j, body, "error.format");
		if (e == JSON_NIL)
			e = msg;
		if (e != JSON_NIL) {
			u32 n = VX_MIN(j->end[e] - j->start[e], (u32)sizeof err - 1);
			err[json_unescape(j->src + j->start[e], n, err)] = 0;
			/* one line is enough */
			for (char *c = err; *c; c++)
				if (*c == '\n' && c[1]) {
					*c = 0;
					break;
				}
		}
	}
	d->changed = true;
	switch (kind) {
	case RQ_INIT:
		d->caps_data_bp = json_bool(j, json_get(j, body, "supportsDataBreakpoints"), false);
		req_begin(d, "launch");
		jb_raw(&d->msg, d->launch_json);
		req_end(d, RQ_LAUNCH, 0, 0);
		break;
	case RQ_LAUNCH:
		if (!ok) {
			char m[600];
			int k = snprintf(m, sizeof m, "Launch failed: %s\n", err[0] ? err : "(no reason given)");
			out_add(d, "console", m, (size_t)k);
			snprintf(d->stoptext, sizeof d->stoptext, "%s", err);
			d->state = DS_EXITED;
			dap_stop(d);
		} else if (d->state == DS_STARTING)
			d->state = DS_RUNNING;
		break;
	case RQ_BPS: {
		/* results for one file: replace that file's rows */
		const char *path = pstr(d, (u32)a);
		for (u32 i = 0; i < d->nbr;) {
			if (!strcmp(pstr(d, d->br_path[i]), path)) {
				d->nbr--;
				d->br_path[i] = d->br_path[d->nbr];
				d->br_line[i] = d->br_line[d->nbr];
				d->br_actual[i] = d->br_actual[d->nbr];
				d->br_id[i] = d->br_id[d->nbr];
				d->br_ok[i] = d->br_ok[d->nbr];
			} else
				i++;
		}
		/* the request's lines, in order, pair with the answer's breakpoints */
		u32 bf = NONE_U32;
		for (u32 i = 0; i < d->nbf; i++)
			if (d->bf_path[i] == (u32)a)
				bf = i;
		u32 arr = json_get(j, body, "breakpoints");
		Json q = {0};
		const char *aj = bf != NONE_U32 ? pstr(d, d->bf_json[bf]) : "{}";
		if (json_parse(&q, aj, (u32)strlen(aj)) == 0) {
			u32 ql = json_get(&q, 0, "breakpoints");
			for (u32 i = 0; i < q.size[ql == JSON_NIL ? 0 : ql] && ql != JSON_NIL; i++) {
				u32 line = (u32)json_int(&q, json_get(&q, json_at(&q, ql, i), "line"), 0);
				u32 r = json_at(j, arr, i);
				SoaCol c[] = {BR_COLS(d)};
				soa_grow(c, 5, &d->brcap, d->nbr + 1, d->nbr);
				d->br_path[d->nbr] = (u32)a;
				d->br_line[d->nbr] = line;
				d->br_actual[d->nbr] = r != JSON_NIL ? (u32)tok_i64(j, json_get(j, r, "line"), line) : line;
				d->br_id[d->nbr] = r != JSON_NIL ? tok_i64(j, json_get(j, r, "id"), -1) : -1;
				d->br_ok[d->nbr] = r != JSON_NIL && json_bool(j, json_get(j, r, "verified"), false);
				d->nbr++;
			}
		}
		json_free(&q);
		break;
	}
	case RQ_THREADS: {
		u32 arr = json_get(j, body, "threads");
		d->nth = 0;
		for (u32 i = 0; arr != JSON_NIL && i < j->size[arr]; i++) {
			u32 t = json_at(j, arr, i);
			SoaCol c[] = {TH_COLS(d)};
			soa_grow(c, 2, &d->thcap, d->nth + 1, d->nth);
			d->th_id[d->nth] = tok_i64(j, json_get(j, t, "id"), -1);
			d->th_name[d->nth] = tok_sp(d, json_get(j, t, "name"));
			d->nth++;
		}
		/* lldb on FreeBSD may leave the thread that stopped out of the list */
		i64 must[2] = {d->stop_thread, d->cur_thread};
		for (int k = 0; k < 2; k++) {
			bool have = must[k] < 0;
			for (u32 i = 0; i < d->nth && !have; i++)
				have = d->th_id[i] == must[k];
			if (!have) {
				SoaCol c[] = {TH_COLS(d)};
				soa_grow(c, 2, &d->thcap, d->nth + 1, d->nth);
				d->th_id[d->nth] = must[k];
				d->th_name[d->nth] = pool_add(&d->sp, "thread", 6);
				d->nth++;
			}
		}
		if (d->cur_thread < 0 && d->nth) {
			d->cur_thread = d->th_id[0];
			request_stack(d, d->cur_thread);
		}
		break;
	}
	case RQ_STACK: {
		if (a != d->cur_thread || d->state != DS_STOPPED)
			break;
		u32 arr = json_get(j, body, "stackFrames");
		d->nfr = 0;
		for (u32 i = 0; arr != JSON_NIL && i < j->size[arr]; i++) {
			u32 f = json_at(j, arr, i);
			SoaCol c[] = {FR_COLS(d)};
			soa_grow(c, 6, &d->frcap, d->nfr + 1, d->nfr);
			u32 k = d->nfr++;
			d->fr_id[k] = tok_i64(j, json_get(j, f, "id"), 0);
			d->fr_name[k] = tok_sp(d, json_get(j, f, "name"));
			d->fr_path[k] = tok_sp(d, json_path(j, f, "source.path"));
			d->fr_line[k] = (u32)tok_i64(j, json_get(j, f, "line"), 0);
			d->fr_col[k] = (u32)tok_i64(j, json_get(j, f, "column"), 0);
			char ip[40] = "";
			u32 it = json_get(j, f, "instructionPointerReference");
			if (it != JSON_NIL && j->end[it] - j->start[it] < sizeof ip)
				ip[json_unescape(j->src + j->start[it], j->end[it] - j->start[it], ip)] = 0;
			d->fr_ip[k] = strtoull(ip, NULL, 16);
		}
		if (d->main_stop && d->main_stop == d->stops && d->nfr && !strcmp(sstr(d, d->fr_name[0]), "main"))
			snprintf(d->stoptext, sizeof d->stoptext, "the start of main");
		/* the first frame with real source is the interesting one (lldb names
		 * disassembly "libsys.so.7`_umtx_op_err": not a file) */
		d->cur_frame = 0;
		for (u32 i = 0; i < d->nfr; i++)
			if (sstr(d, d->fr_path[i])[0] == '/' && d->fr_line[i] && access(sstr(d, d->fr_path[i]), R_OK) == 0) {
				d->cur_frame = i;
				break;
			}
		d->nsc = d->nvs = d->nvn = 0;
		request_scopes(d);
		break;
	}
	case RQ_SCOPES: {
		if (d->cur_frame >= d->nfr || a != d->fr_id[d->cur_frame])
			break;
		u32 arr = json_get(j, body, "scopes");
		d->nsc = 0;
		for (u32 i = 0; arr != JSON_NIL && i < j->size[arr]; i++) {
			u32 s = json_at(j, arr, i);
			SoaCol c[] = {SC_COLS(d)};
			soa_grow(c, 3, &d->sccap, d->nsc + 1, d->nsc);
			d->sc_name[d->nsc] = tok_sp(d, json_get(j, s, "name"));
			d->sc_ref[d->nsc] = tok_i64(j, json_get(j, s, "variablesReference"), 0);
			d->sc_exp[d->nsc] = json_bool(j, json_get(j, s, "expensive"), false);
			d->nsc++;
		}
		/* the first cheap scope (locals) loads at once */
		for (u32 i = 0; i < d->nsc; i++)
			if (!d->sc_exp[i]) {
				u32 f, n;
				dap_vars(d, d->sc_ref[i], &f, &n);
				break;
			}
		break;
	}
	case RQ_VARS: {
		u32 vs = NONE_U32;
		for (u32 i = 0; i < d->nvs; i++)
			if (d->vs_ref[i] == a)
				vs = i;
		if (vs == NONE_U32)
			break;
		if (!ok) {
			d->vs_state[vs] = DV_ERROR;
			break;
		}
		u32 arr = json_get(j, body, "variables");
		d->vs_first[vs] = d->nvn;
		d->vs_count[vs] = 0;
		for (u32 i = 0; arr != JSON_NIL && i < j->size[arr] && i < 2000; i++) {
			u32 v = json_at(j, arr, i);
			SoaCol c[] = {VN_COLS(d)};
			soa_grow(c, 7, &d->vncap, d->nvn + 1, d->nvn);
			u32 k = d->nvn++;
			d->vn_name[k] = tok_sp(d, json_get(j, v, "name"));
			d->vn_value[k] = tok_sp(d, json_get(j, v, "value"));
			d->vn_type[k] = tok_sp(d, json_get(j, v, "type"));
			d->vn_eval[k] = tok_sp(d, json_get(j, v, "evaluateName"));
			d->vn_ref[k] = tok_i64(j, json_get(j, v, "variablesReference"), 0);
			/* its key: the container's key and its name (evaluateName is not
			 * given by every adapter) */
			d->vn_key[k] = d->vs_key[vs] ? hstr(sstr(d, d->vn_name[k]), d->vs_key[vs] ^ 0x9E3779B97F4A7C15ull) : 0;
			/* changed since it was last seen? */
			d->vn_changed[k] = 0;
			if (d->vn_key[k] && d->nprev) {
				u64 h = d->vn_key[k];
				u32 lo = 0, hi = d->nprev;
				while (lo < hi) {
					u32 mid = (lo + hi) / 2;
					if (d->prev_key[mid] < h)
						lo = mid + 1;
					else
						hi = mid;
				}
				if (lo < d->nprev && d->prev_key[lo] == h)
					d->vn_changed[k] = d->prev_val[lo] != hstr(sstr(d, d->vn_value[k]), 0);
			}
			d->vs_count[vs]++;
		}
		d->vs_state[vs] = DV_LOADED;
		break;
	}
	case RQ_EVAL: {
		if ((u32)a >= d->nev)
			break;
		u32 e = (u32)a;
		if (!ok) {
			d->ev_state[e] = DV_ERROR;
			d->ev_value[e] = pool_add(&d->sp, err, (u32)strlen(err));
			break;
		}
		d->ev_state[e] = DV_LOADED;
		d->ev_value[e] = tok_sp(d, json_get(j, body, "result"));
		d->ev_type[e] = tok_sp(d, json_get(j, body, "type"));
		d->ev_ref[e] = tok_i64(j, json_get(j, body, "variablesReference"), 0);
		break;
	}
	case RQ_SLOTS: {
		u32 r = json_get(j, body, "result");
		if (r != JSON_NIL) {
			char b[4096];
			u32 n = VX_MIN(j->end[r] - j->start[r], (u32)sizeof b - 1);
			b[json_unescape(j->src + j->start[r], n, b)] = 0;
			const char *k = strstr(b, "hardware watchpoints:");
			if (k)
				d->hw_slots = atoi(k + 21);
		}
		break;
	}
	case RQ_WADDR:
	case RQ_WSIZE:
	case RQ_WTYPE: {
		u32 w = (u32)a;
		if (w >= d->nw || d->w_state[w] == W_ERR)
			break;
		char b[512] = "";
		u32 r = json_get(j, body, kind == RQ_WTYPE ? "type" : "result");
		if (r != JSON_NIL && j->end[r] - j->start[r] < sizeof b)
			b[json_unescape(j->src + j->start[r], j->end[r] - j->start[r], b)] = 0;
		if (kind == RQ_WADDR) {
			const char *x = strstr(b, "0x");
			if (!ok || !x) {
				watch_fail(d, w, err[0] ? err : "Not an lvalue: cannot take its address");
				break;
			}
			d->w_addr[w] = strtoull(x, NULL, 16);
			char e2[600];
			snprintf(e2, sizeof e2, "sizeof(%s)", pstr(d, d->w_expr[w]));
			d->w_state[w] = W_SIZE;
			eval_req(d, e2, "watch", RQ_WSIZE, w);
		} else if (kind == RQ_WSIZE) {
			if (ok && atoi(b) > 0) {
				d->w_size[w] = (u32)atoi(b);
				watch_ready(d, w);
			} else {
				/* some lldb builds cannot run sizeof: guess from the type */
				d->w_state[w] = W_TYPE;
				eval_req(d, pstr(d, d->w_expr[w]), "watch", RQ_WTYPE, w);
			}
		} else {
			d->w_size[w] = ok ? (u32)watch_size_of_type(b) : 4;
			watch_ready(d, w);
		}
		break;
	}
	case RQ_WINFO: {
		u32 w = (u32)a;
		if (w >= d->nw)
			break;
		u32 id = json_get(j, body, "dataId");
		if (!ok || json_type(j, id) != JS_STR) {
			u32 ds = json_get(j, body, "description");
			char b[300] = "";
			if (ds != JSON_NIL && j->end[ds] - j->start[ds] < sizeof b)
				b[json_unescape(j->src + j->start[ds], j->end[ds] - j->start[ds], b)] = 0;
			watch_fail(d, w, err[0] ? err : b[0] ? b : "The adapter cannot watch this");
			break;
		}
		d->w_dataid[w] = tok_pp(d, id);
		d->w_desc[w] = tok_pp(d, json_get(j, body, "description"));
		d->w_state[w] = W_READY;
		push_watches(d);
		break;
	}
	case RQ_WGDB: {
		u32 w = (u32)a;
		if (w >= d->nw)
			break;
		char b[600] = "";
		u32 r = json_get(j, body, "result");
		if (r != JSON_NIL && j->end[r] - j->start[r] < sizeof b)
			b[json_unescape(j->src + j->start[r], j->end[r] - j->start[r], b)] = 0;
		/* "Hardware watchpoint 2: counter", "Watchpoint 3: x", "Hardware read watchpoint 4: y" */
		const char *k = strstr(b, "atchpoint ");
		if (!ok || !k) {
			watch_fail(d, w, err[0] ? err : b[0] ? b : "gdb did not set the watchpoint");
			break;
		}
		d->w_gdbno[w] = atoi(k + 10);
		d->w_sw[w] = !strstr(b, "Hardware");
		char desc[200];
		snprintf(desc, sizeof desc, "%s watchpoint %d", d->w_sw[w] ? "Software" : "Hardware", d->w_gdbno[w]);
		d->w_desc[w] = pool_add(&d->pp, desc, (u32)strlen(desc));
		d->w_state[w] = W_READY;
		d->w_ok[w] = 1;
		break;
	}
	case RQ_DBPS: {
		u32 arr = json_get(j, body, "breakpoints");
		u32 k = 0;
		for (u32 i = 0; i < d->nw; i++)
			if (d->w_state[i] == W_READY) {
				u32 r = json_at(j, arr, k++);
				d->w_ok[i] = ok && r != JSON_NIL && json_bool(j, json_get(j, r, "verified"), true);
				if (!d->w_ok[i]) {
					u32 m = json_get(j, r, "message");
					if (m != JSON_NIL)
						d->w_desc[i] = tok_pp(d, m);
					else if (!ok)
						d->w_desc[i] = pool_add(&d->pp, err, (u32)strlen(err));
				}
			}
		break;
	}
	case RQ_RUN:
		if (!ok && err[0]) {
			char m[500];
			int k = snprintf(m, sizeof m, "%s\n", err);
			out_add(d, "console", m, (size_t)k);
		}
		break;
	case RQ_DISC:
		break;
	default: break;
	}
}

static void handle(Dap *d, const char *s, u32 n)
{
	if (json_parse(&d->js, s, n) != 0)
		return;
	u32 t = json_get(&d->js, 0, "type");
	if (json_eq(&d->js, t, "event"))
		on_event(d, 0);
	else if (json_eq(&d->js, t, "response"))
		on_response(d, 0);
	/* reverse requests (runInTerminal) are not supported: we said so */
}

bool dap_pump(Dap *d)
{
	if (!d)
		return false;
	d->changed = false;
	flush(d);
	if (d->out >= 0) {
		for (;;) {
			VX_GROW(d->rb, d->rcap, d->rn + 65536);
			ssize_t k = read(d->out, d->rb + d->rn, d->rcap - d->rn - 1);
			if (k > 0) {
				d->rn += (u32)k;
				continue;
			}
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
				close(d->out);
				d->out = -1;
				d->dead = true;
				if (d->state != DS_EXITED) {
					d->state = DS_EXITED;
					d->changed = true;
				}
			}
			break;
		}
	}
	/* frames: "Content-Length: N\r\n...\r\n\r\n" + N bytes */
	u32 off = 0;
	for (;;) {
		char *hdr_end = NULL;
		for (u32 i = off; i + 3 < d->rn; i++)
			if (d->rb[i] == '\r' && d->rb[i + 1] == '\n' && d->rb[i + 2] == '\r' && d->rb[i + 3] == '\n') {
				hdr_end = d->rb + i;
				break;
			}
		if (!hdr_end)
			break;
		u32 len = 0;
		for (char *p = d->rb + off; p < hdr_end; p++)
			if (!strncasecmp(p, "Content-Length:", 15)) {
				len = (u32)strtoul(p + 15, NULL, 10);
				break;
			}
		u32 body = (u32)(hdr_end - d->rb) + 4;
		if (body + len > d->rn)
			break;
		handle(d, d->rb + body, len);
		off = body + len;
	}
	if (off) {
		memmove(d->rb, d->rb + off, d->rn - off);
		d->rn -= off;
	}
	flush(d);
	/* a session that will not end: kill the adapter */
	if (d->disconnecting && d->pid > 0) {
		u64 now = vx_now_ns();
		reap(d);
		if (d->pid > 0 && now > d->kill_at) {
			kill(d->pid, SIGKILL);
			waitpid(d->pid, NULL, 0);
			d->pid = 0;
			d->dead = true;
		}
	} else
		reap(d);
	if (d->dead && d->state != DS_EXITED) {
		d->state = DS_EXITED;
		d->changed = true;
	}
	return d->changed;
}

/* ---- accessors ---------------------------------------------------------------------------- */

int dap_state(Dap *d) { return d ? d->state : DS_EXITED; }
const char *dap_adapter(Dap *d) { return d ? d->adapter : ""; }
const char *dap_stop_reason(Dap *d) { return d ? d->reason : ""; }
const char *dap_stop_text(Dap *d) { return d ? d->stoptext : ""; }
int dap_exit_code(Dap *d) { return d ? d->exit_code : -1; }
u32 dap_stop_count(Dap *d) { return d ? d->stops : 0; }
bool dap_supports_data_bp(Dap *d) { return d && (d->caps_data_bp || d->lldb); }
bool dap_is_lldb(Dap *d) { return d && d->lldb; }

static void run_cmd(Dap *d, const char *cmd, bool need_stop)
{
	if (!d || d->dead || d->disconnecting || (need_stop && d->state != DS_STOPPED))
		return;
	i64 t = d->cur_thread >= 0 ? d->cur_thread : d->stop_thread;
	if (need_stop) {
		/* running from now on: the stop data is stale */
		d->stops_at_run = d->stops;
		clear_stop(d, true);
		d->state = DS_RUNNING;
	}
	req_begin(d, cmd);
	jb_raw(&d->msg, "{\"threadId\":");
	jb_int(&d->msg, t >= 0 ? t : 0);
	jb_raw(&d->msg, "}");
	req_end(d, RQ_RUN, 0, 0);
}

void dap_continue(Dap *d) { run_cmd(d, "continue", true); }
void dap_pause(Dap *d)
{
	if (d && d->state == DS_RUNNING)
		run_cmd(d, "pause", false);
}
void dap_next(Dap *d) { run_cmd(d, "next", true); }
void dap_step_in(Dap *d) { run_cmd(d, "stepIn", true); }
void dap_step_out(Dap *d) { run_cmd(d, "stepOut", true); }

void dap_set_breakpoints(Dap *d, const char *path, const u32 *lines, const char *const *cond, u32 n)
{
	if (!d)
		return;
	JBuf a = {0};
	jb_raw(&a, "{\"source\":{\"path\":");
	jb_str(&a, path);
	jb_raw(&a, "},\"breakpoints\":[");
	for (u32 i = 0; i < n; i++) {
		if (i)
			jb_raw(&a, ",");
		jb_raw(&a, "{\"line\":");
		jb_int(&a, lines[i]);
		if (cond && cond[i] && *cond[i]) {
			jb_raw(&a, ",\"condition\":");
			jb_str(&a, cond[i]);
		}
		jb_raw(&a, "}");
	}
	jb_raw(&a, "]}");
	/* one entry per file: the latest set wins */
	u32 f = NONE_U32;
	for (u32 i = 0; i < d->nbf; i++)
		if (!strcmp(pstr(d, d->bf_path[i]), path))
			f = i;
	if (f == NONE_U32) {
		SoaCol c[] = {BF_COLS(d)};
		soa_grow(c, 3, &d->bfcap, d->nbf + 1, d->nbf);
		f = d->nbf++;
		d->bf_path[f] = pool_add(&d->pp, path, (u32)strlen(path));
	}
	d->bf_json[f] = pool_add(&d->pp, a.b, a.n);
	d->bf_sent[f] = 0;
	jb_free(&a);
	if (d->configured && !d->dead)
		send_bps(d);
}

bool dap_bp_result(Dap *d, const char *path, u32 line, bool *verified, u32 *actual)
{
	if (!d)
		return false;
	for (u32 i = 0; i < d->nbr; i++)
		if (d->br_line[i] == line && !strcmp(pstr(d, d->br_path[i]), path)) {
			*verified = d->br_ok[i];
			*actual = d->br_actual[i];
			return true;
		}
	return false;
}

u32 dap_thread_count(Dap *d) { return d ? d->nth : 0; }
i64 dap_thread_id(Dap *d, u32 i) { return i < d->nth ? d->th_id[i] : -1; }
const char *dap_thread_name(Dap *d, u32 i) { return i < d->nth ? sstr(d, d->th_name[i]) : ""; }
i64 dap_current_thread(Dap *d) { return d ? d->cur_thread : -1; }
i64 dap_stopped_thread(Dap *d) { return d ? d->stop_thread : -1; }

void dap_select_thread(Dap *d, i64 id)
{
	if (!d || d->state != DS_STOPPED || id == d->cur_thread)
		return;
	d->cur_thread = id;
	d->nfr = d->nsc = 0;
	d->changed = true;
	request_stack(d, id);
}

u32 dap_frame_count(Dap *d) { return d ? frame_count_for(d) : 0; }
const char *dap_frame_name(Dap *d, u32 i) { return i < d->nfr ? sstr(d, d->fr_name[i]) : ""; }
const char *dap_frame_path(Dap *d, u32 i) { return i < d->nfr ? sstr(d, d->fr_path[i]) : ""; }
u32 dap_frame_line(Dap *d, u32 i) { return i < d->nfr ? d->fr_line[i] : 0; }
u32 dap_frame_col(Dap *d, u32 i) { return i < d->nfr ? d->fr_col[i] : 0; }
u64 dap_frame_ip(Dap *d, u32 i) { return i < d->nfr ? d->fr_ip[i] : 0; }
u32 dap_current_frame(Dap *d) { return d ? d->cur_frame : 0; }

void dap_select_frame(Dap *d, u32 i)
{
	if (!d || i >= d->nfr || i == d->cur_frame)
		return;
	d->cur_frame = i;
	d->nsc = 0;
	/* variables belong to frames: drop the loaded ones (evaluations too) */
	d->nvs = d->nvn = d->nev = 0;
	d->changed = true;
	request_scopes(d);
}

u32 dap_scope_count(Dap *d) { return d ? d->nsc : 0; }
const char *dap_scope_name(Dap *d, u32 i) { return i < d->nsc ? sstr(d, d->sc_name[i]) : ""; }
i64 dap_scope_ref(Dap *d, u32 i) { return i < d->nsc ? d->sc_ref[i] : 0; }
bool dap_scope_expensive(Dap *d, u32 i) { return i < d->nsc && d->sc_exp[i]; }

int dap_vars(Dap *d, i64 ref, u32 *first, u32 *count)
{
	*first = *count = 0;
	if (!d || ref <= 0 || d->state != DS_STOPPED)
		return DV_NONE;
	for (u32 i = 0; i < d->nvs; i++)
		if (d->vs_ref[i] == ref) {
			*first = d->vs_first[i];
			*count = d->vs_count[i];
			return d->vs_state[i];
		}
	SoaCol c[] = {VS_COLS(d)};
	soa_grow(c, 5, &d->vscap, d->nvs + 1, d->nvs);
	u32 k = d->nvs++;
	d->vs_ref[k] = ref;
	/* the key of what holds these: a variable's, or a scope's name (+ frame) */
	d->vs_key[k] = 0;
	for (u32 i = d->nvn; i-- > 0;)
		if (d->vn_ref[i] == ref) {
			d->vs_key[k] = d->vn_key[i];
			break;
		}
	for (u32 i = 0; i < d->nsc && !d->vs_key[k]; i++)
		if (d->sc_ref[i] == ref)
			d->vs_key[k] = hstr(sstr(d, d->sc_name[i]),
			                    hstr(d->cur_frame < d->nfr ? sstr(d, d->fr_name[d->cur_frame]) : "", 0));
	d->vs_first[k] = d->vs_count[k] = 0;
	d->vs_state[k] = DV_PENDING;
	req_begin(d, "variables");
	jb_raw(&d->msg, "{\"variablesReference\":");
	jb_int(&d->msg, ref);
	jb_raw(&d->msg, "}");
	req_end(d, RQ_VARS, ref, 0);
	return DV_PENDING;
}

const char *dap_var_name(Dap *d, u32 n) { return n < d->nvn ? sstr(d, d->vn_name[n]) : ""; }
const char *dap_var_value(Dap *d, u32 n) { return n < d->nvn ? sstr(d, d->vn_value[n]) : ""; }
const char *dap_var_type(Dap *d, u32 n) { return n < d->nvn ? sstr(d, d->vn_type[n]) : ""; }
const char *dap_var_eval_name(Dap *d, u32 n) { return n < d->nvn ? sstr(d, d->vn_eval[n]) : ""; }
i64 dap_var_ref(Dap *d, u32 n) { return n < d->nvn ? d->vn_ref[n] : 0; }
bool dap_var_changed(Dap *d, u32 n) { return n < d->nvn && d->vn_changed[n]; }

i32 dap_evaluate(Dap *d, const char *expr, const char *context)
{
	if (!d || d->state != DS_STOPPED || !expr || !*expr)
		return -1;
	SoaCol c[] = {EV_COLS(d)};
	soa_grow(c, 4, &d->evcap, d->nev + 1, d->nev);
	u32 e = d->nev++;
	d->ev_state[e] = DV_PENDING;
	d->ev_value[e] = d->ev_type[e] = 0;
	d->ev_ref[e] = 0;
	eval_req(d, expr, context, RQ_EVAL, e);
	return (i32)e;
}

int dap_eval_result(Dap *d, i32 id, const char **value, const char **type, i64 *ref)
{
	*value = *type = "";
	*ref = 0;
	if (!d || id < 0 || (u32)id >= d->nev)
		return DV_NONE;
	*value = sstr(d, d->ev_value[id]);
	*type = sstr(d, d->ev_type[id]);
	*ref = d->ev_ref[id];
	return d->ev_state[id];
}

static void gdb_watch(Dap *d, u32 w)
{
	char c[700];
	const char *cmd = d->w_access[w] == DW_READ ? "rwatch" : d->w_access[w] == DW_READWRITE ? "awatch" : "watch";
	if (d->w_sw[w]) /* gdb makes software watchpoints when it may not use the hardware */
		eval_req(d, "set can-use-hw-watchpoints 0", "repl", RQ_QUIET, 0);
	snprintf(c, sizeof c, "%s %s", d->w_sw[w] ? "watch" : cmd, pstr(d, d->w_expr[w]));
	eval_req(d, c, "repl", RQ_WGDB, w);
	if (d->w_sw[w])
		eval_req(d, "set can-use-hw-watchpoints 1", "repl", RQ_QUIET, 0);
}

static void gdb_unwatch(Dap *d, u32 w)
{
	if (d->w_gdbno[w] > 0) {
		char c[40];
		snprintf(c, sizeof c, "delete %d", d->w_gdbno[w]);
		eval_req(d, c, "repl", RQ_QUIET, 0);
		d->w_gdbno[w] = 0;
	}
}

bool dap_supports_sw_watch(Dap *d) { return d && d->gdb; }
bool dap_watch_hw(Dap *d, u32 i) { return i < d->nw && !d->w_sw[i]; }

void dap_watch_add(Dap *d, const char *expr, int access) { dap_watch_add_ex(d, expr, access, false); }

void dap_watch_add_ex(Dap *d, const char *expr, int access, bool software)
{
	if (!d || !expr || !*expr)
		return;
	SoaCol c[] = {W_COLS(d)};
	soa_grow(c, 10, &d->wcap, d->nw + 1, d->nw);
	u32 w = d->nw++;
	d->w_expr[w] = pool_add(&d->pp, expr, (u32)strlen(expr));
	d->w_desc[w] = d->w_dataid[w] = 0;
	d->w_access[w] = (u8)access;
	d->w_ok[w] = 0;
	d->w_addr[w] = 0;
	d->w_size[w] = 0;
	d->w_sw[w] = software && d->gdb;
	d->w_gdbno[w] = 0;
	d->changed = true;
	if (software && !d->gdb) {
		watch_fail(d, w, "Software watchpoints need gdb as the debug adapter (lldb only has hardware ones)");
		return;
	}
	if (d->state != DS_STOPPED) {
		watch_fail(d, w, "Pause the program first: watchpoints are set on a stopped program");
		return;
	}
	if (d->gdb) {
		d->w_state[w] = W_INFO;
		gdb_watch(d, w);
	} else {
		d->w_state[w] = W_ADDR;
		/* "&p.x" is a variable path lldb resolves itself; "&(...)" would
		 * need the expression JIT, which not every platform has */
		bool path = true;
		for (const char *c = expr; *c; c++)
			path &= (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
			        *c == '.' || *c == '[' || *c == ']' || *c == '-' || *c == '>' || (*c == '*' && c == expr);
		char e[600];
		snprintf(e, sizeof e, path ? "&%s" : "&(%s)", expr);
		eval_req(d, e, "watch", RQ_WADDR, w);
	}
}

void dap_watch_remove(Dap *d, u32 i)
{
	if (!d || i >= d->nw)
		return;
	bool live = d->w_state[i] == W_READY && !d->gdb;
	if (d->gdb && !d->dead)
		gdb_unwatch(d, i);
	for (u32 k = i; k + 1 < d->nw; k++) {
		d->w_sw[k] = d->w_sw[k + 1];
		d->w_gdbno[k] = d->w_gdbno[k + 1];
		d->w_expr[k] = d->w_expr[k + 1];
		d->w_desc[k] = d->w_desc[k + 1];
		d->w_dataid[k] = d->w_dataid[k + 1];
		d->w_access[k] = d->w_access[k + 1];
		d->w_state[k] = d->w_state[k + 1];
		d->w_ok[k] = d->w_ok[k + 1];
		d->w_addr[k] = d->w_addr[k + 1];
		d->w_size[k] = d->w_size[k + 1];
	}
	d->nw--;
	d->changed = true;
	if (live && !d->dead)
		push_watches(d);
}

void dap_watch_set_access(Dap *d, u32 i, int access)
{
	if (!d || i >= d->nw || d->w_access[i] == access)
		return;
	d->w_access[i] = (u8)access;
	d->changed = true;
	if (d->gdb && !d->dead && d->w_state[i] == W_READY) {
		gdb_unwatch(d, i); /* gdb: delete it and set the new kind */
		d->w_state[i] = W_INFO;
		gdb_watch(d, i);
	} else if (d->w_state[i] == W_READY && !d->dead)
		push_watches(d);
}

u32 dap_watch_count(Dap *d) { return d ? d->nw : 0; }
const char *dap_watch_name(Dap *d, u32 i) { return i < d->nw ? pstr(d, d->w_expr[i]) : ""; }
const char *dap_watch_desc(Dap *d, u32 i)
{
	if (i >= d->nw)
		return "";
	if (d->w_state[i] != W_READY && d->w_state[i] != W_ERR)
		return "Setting up...";
	return pstr(d, d->w_desc[i]);
}
int dap_watch_access(Dap *d, u32 i) { return i < d->nw ? d->w_access[i] : DW_WRITE; }
bool dap_watch_ok(Dap *d, u32 i) { return i < d->nw && d->w_state[i] == W_READY && d->w_ok[i]; }
int dap_hw_slots(Dap *d) { return d ? d->hw_slots : -1; }

bool dap_output(Dap *d, const char **category, const char **text)
{
	if (!d || d->opop >= d->no) {
		if (d && d->no && d->opop >= d->no) {
			d->no = d->opop = 0;
			pool_clear(&d->op);
		}
		return false;
	}
	*category = d->op.b + d->o_cat[d->opop];
	*text = d->op.b + d->o_text[d->opop];
	d->opop++;
	return true;
}
