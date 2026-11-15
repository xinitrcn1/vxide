/* DAP client against a real adapter (lldb-dap or gdb): breakpoints,
 * stack, scopes, variables, evaluate, a hardware watchpoint, output, exit.
 * Skipped when no adapter or no C compiler is installed. */
#define _POSIX_C_SOURCE 200809L
#include "dap.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static const char *src =
    "#include <stdio.h>\n"                                        /* 1 */
    "#include <pthread.h>\n"                                      /* 2 */
    "struct pt { int x, y; };\n"                                  /* 3 */
    "static int counter;\n"                                       /* 4 */
    "static void *worker(void *a) { return a; }\n"                /* 5 */
    "int main(int argc, char **argv)\n"                           /* 6 */
    "{\n"                                                         /* 7 */
    "\tstruct pt p = {1, 2};\n"                                   /* 8 */
    "\tint arr[4] = {1, 2, 3, 4};\n"                              /* 9 */
    "\tpthread_t t;\n"                                            /* 10 */
    "\tpthread_create(&t, NULL, worker, NULL);\n"                 /* 11 */
    "\tcounter = 5;\n"                                            /* 12 */
    "\tp.x += counter;\n"                                         /* 13 */
    "\tpthread_join(t, NULL);\n"                                  /* 14 */
    "\tprintf(\"out %d %d %d\\n\", p.x, arr[2], argc);\n"         /* 15 */
    "\treturn 0;\n"                                               /* 16 */
    "}\n";

static Dap *d;
static char outbuf[8192];

/* pump until cond holds (or 20 s) */
static bool wait_for(bool (*cond)(void))
{
	for (int i = 0; i < 2000; i++) {
		dap_pump(d);
		const char *c, *t;
		while (dap_output(d, &c, &t))
			if (strlen(outbuf) + strlen(t) < sizeof outbuf - 1)
				strcat(outbuf, t);
		if (cond())
			return true;
		struct timespec ts = {0, 10000000};
		nanosleep(&ts, NULL);
	}
	return false;
}

static u32 want_stop = 1;
static bool stopped_with_frames(void)
{
	return dap_state(d) == DS_STOPPED && dap_stop_count(d) >= want_stop && dap_frame_count(d) > 0 &&
	       dap_scope_count(d) > 0 && dap_thread_count(d) > 0;
}
static bool exited(void) { return dap_state(d) == DS_EXITED; }
/* the cheap scopes' variables (lldb: Locals; gdb: Arguments and Locals) */
static u32 lf[4], ln[4], nls;
static bool locals_loaded(void)
{
	nls = 0;
	for (u32 i = 0; i < dap_scope_count(d) && nls < 4; i++)
		if (!dap_scope_expensive(d, i) && !strstr(dap_scope_name(d, i), "egister") &&
		    !strstr(dap_scope_name(d, i), "lobal")) {
			if (dap_vars(d, dap_scope_ref(d, i), &lf[nls], &ln[nls]) != DV_LOADED)
				return false;
			nls++;
		}
	return nls > 0;
}
static i32 ev_id;
static bool eval_done(void)
{
	const char *v, *t;
	i64 r;
	return dap_eval_result(d, ev_id, &v, &t, &r) >= DV_LOADED;
}
static bool watch_settled(void)
{
	return dap_watch_count(d) && strcmp(dap_watch_desc(d, 0), "Setting up...") != 0 &&
	       (dap_watch_ok(d, 0) || strstr(dap_watch_desc(d, 0), "rror"));
}

static u32 var_named(const char *n)
{
	for (u32 s = 0; s < nls; s++)
		for (u32 i = 0; i < ln[s]; i++)
			if (!strcmp(dap_var_name(d, lf[s] + i), n))
				return lf[s] + i;
	return UINT32_MAX;
}

int main(void)
{
	char dir[] = "/tmp/vxide-dap-XXXXXX";
	if (!mkdtemp(dir))
		return tap_done();
	char path[200], exe[200], cmd[600];
	snprintf(path, sizeof path, "%s/w.c", dir);
	snprintf(exe, sizeof exe, "%s/w", dir);
	FILE *f = fopen(path, "w");
	fputs(src, f);
	fclose(f);
	snprintf(cmd, sizeof cmd, "cc -g -O0 -pthread -o %s %s 2>/dev/null", exe, path);
	if (system(cmd) != 0) {
		fprintf(stderr, "# skipped: no C compiler\n");
		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		(void)system(cmd);
		return tap_done();
	}
	const char *args[] = {"a"};
	DapLaunch l = {NULL, exe, args, 1, dir, false};
	char err[200];
	d = dap_start(&l, err, sizeof err);
	if (!d) {
		fprintf(stderr, "# skipped: %s\n", err);
		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		(void)system(cmd);
		return tap_done();
	}
	fprintf(stderr, "# adapter: %s\n", dap_adapter(d));
	u32 line = 12;
	dap_set_breakpoints(d, path, &line, NULL, 1);
	CHECK(wait_for(stopped_with_frames));
	bool ver = false;
	u32 act = 0;
	CHECK(dap_bp_result(d, path, 12, &ver, &act) && ver && act == 12);
	u32 fr = dap_current_frame(d);
	CHECK(!strcmp(dap_frame_name(d, fr), "main"));
	CHECK(dap_frame_line(d, fr) == 12);
	CHECK(strstr(dap_frame_path(d, fr), "w.c") != NULL);
	CHECK(dap_thread_count(d) >= 1);
	bool regs = false;
	for (u32 i = 0; i < dap_scope_count(d); i++)
		regs |= strstr(dap_scope_name(d, i), "egister") != NULL;
	CHECK(regs || !dap_is_lldb(d));
	CHECK(wait_for(locals_loaded));
	u32 pv = var_named("p");
	CHECK(pv != UINT32_MAX && dap_var_ref(d, pv) > 0);
	CHECK(var_named("arr") != UINT32_MAX);
	/* children of p: x, y */
	if (pv != UINT32_MAX) {
		u32 f2, n2;
		dap_vars(d, dap_var_ref(d, pv), &f2, &n2);
		for (int i = 0; i < 500 && dap_vars(d, dap_var_ref(d, pv), &f2, &n2) == DV_PENDING; i++) {
			dap_pump(d);
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, NULL);
		}
		CHECK(n2 == 2 && !strcmp(dap_var_name(d, f2), "x") && !strcmp(dap_var_value(d, f2), "1"));
	}
	/* hover: a variable path; watch: any expression */
	ev_id = dap_evaluate(d, "p.x", "hover");
	CHECK(ev_id >= 0 && wait_for(eval_done));
	const char *v, *t;
	i64 r;
	dap_eval_result(d, ev_id, &v, &t, &r);
	CHECK_STR(v, "1");
	ev_id = dap_evaluate(d, "p.x + 41", "watch");
	CHECK(ev_id >= 0 && wait_for(eval_done));
	int es = dap_eval_result(d, ev_id, &v, &t, &r);
	if (es == DV_ERROR && dap_is_lldb(d) && (strstr(v, "JIT") || strstr(v, "materialize")))
		fprintf(stderr, "# this lldb cannot run JIT expressions (%s)\n", v);
	else
		CHECK_STR(v, "42");
	/* a hardware watchpoint on counter: the next write stops */
	dap_watch_add(d, "counter", DW_WRITE);
	CHECK(wait_for(watch_settled));
	CHECK(dap_watch_ok(d, 0));
	if (!dap_watch_ok(d, 0))
		fprintf(stderr, "# watch: %s\n", dap_watch_desc(d, 0));
	dap_continue(d);
	want_stop = 2;
	CHECK(wait_for(stopped_with_frames));
	CHECK(dap_frame_line(d, dap_current_frame(d)) == 13); /* stopped after the write on 12 */
	CHECK(strstr(dap_stop_text(d), "atchpoint") || strstr(dap_stop_reason(d), "data"));
	if (dap_is_lldb(d))
		CHECK(dap_hw_slots(d) > 0);
	/* step over line 13: p.x changes, and is marked */
	CHECK(wait_for(locals_loaded));
	dap_next(d);
	want_stop = 3;
	CHECK(wait_for(stopped_with_frames));
	CHECK(dap_frame_line(d, dap_current_frame(d)) == 14);
	CHECK(wait_for(locals_loaded));
	pv = var_named("p");
	if (pv != UINT32_MAX) {
		u32 f2, n2;
		for (int i = 0; i < 500 && dap_vars(d, dap_var_ref(d, pv), &f2, &n2) == DV_PENDING; i++) {
			dap_pump(d);
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, NULL);
		}
		CHECK(n2 == 2 && !strcmp(dap_var_value(d, f2), "6") && dap_var_changed(d, f2));
		CHECK(n2 == 2 && !dap_var_changed(d, f2 + 1)); /* y did not change */
	}
	dap_watch_remove(d, 0);
	dap_continue(d);
	CHECK(wait_for(exited));
	CHECK(dap_exit_code(d) == 0);
	CHECK(strstr(outbuf, "out 6 3 2") != NULL);
	if (!strstr(outbuf, "out 6 3 2"))
		fprintf(stderr, "# output: %s\n", outbuf);
	dap_stop(d);
	for (int i = 0; i < 300 && dap_alive(d); i++) {
		dap_pump(d);
		struct timespec ts = {0, 10000000};
		nanosleep(&ts, NULL);
	}
	dap_free(d);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)system(cmd);
	return tap_done();
}
