/* Profiles: massif, jemalloc heap, gmon (BSD file + synthetic), gcov,
 * gprof propagation, and an end-to-end gprof run when cc -pg works. */
#define _POSIX_C_SOURCE 200809L
#include "prof.h"
#include "tap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static char *load(const char *path, size_t *n)
{
	char *b = vx_read_file(path, n);
	if (!b)
		fprintf(stderr, "cannot read %s\n", path);
	return b;
}

static u32 fn_named(const Prof *p, const char *name)
{
	for (u32 f = 0; f < p->nfn; f++)
		if (!strcmp(prof_str(p, p->fn_name[f]), name))
			return f;
	return PROF_NONE;
}

static bool near(double a, double b) { return fabs(a - b) <= 1e-6 * VX_MAX(fabs(a), fabs(b)) + 1e-9; }

static void test_detect(void)
{
	CHECK(prof_detect("/x/massif.out.12", "desc: --x\ncmd: ./a\ntime_unit: i\n", 30) == PK_MASSIF);
	CHECK(prof_detect("/x/a.heap", "heap_v2/524288\n", 15) == PK_JEPROF);
	CHECK(prof_detect("/x/a.heap", "heap profile:   1: 2 [ 3: 4] @ heap_v2/524288\n", 46) == PK_JEPROF);
	CHECK(prof_detect("/x/prog.gmon", "\0\0\0", 3) == PK_GPROF);
	CHECK(prof_detect("/x/gmon.out", "gmon\1\0\0\0", 8) == PK_GPROF);
	CHECK(prof_detect("/x/hot.c.gcov", "        -:    0:Source:hot.c\n", 30) == PK_GCOV);
	CHECK(prof_detect("/x/a-hot.gcda", "adcg", 4) == PK_GCOV);
	CHECK(prof_detect("/x/hot.c", "#include <stdio.h>\n", 19) == PK_NONE);
	CHECK(prof_detect("/x/notes.gcov", "hello\n", 6) == PK_NONE);
	size_t n;
	char *g = load("tests/prof/hot.gmon", &n);
	CHECK(g && prof_detect("/x/renamed.bin", g, n) == PK_GPROF); /* by the BSD header version */
	free(g);
}

static void test_massif(void)
{
	size_t n;
	char *s = load("tests/prof/massif.out.hot", &n);
	Prof p;
	CHECK(s && prof_parse_massif(&p, s, n) == 0);
	CHECK(p.nsnap == 69);
	CHECK_STR(prof_str(&p, p.cmd), "./hotm 100");
	CHECK_STR(prof_str(&p, p.time_unit), "i");
	CHECK(p.peak_snap == 67);
	CHECK(p.sn_heap[p.peak_snap] == 129936000);
	CHECK(p.sn_extra[p.peak_snap] == 32000);
	CHECK(near(prof_total(&p, 0, p.peak_snap), 129936000.0));
	/* the tree is inverted in massif (allocation site first); stacks are root first */
	ProfTree t = {0};
	prof_tree(&p, 0, p.peak_snap, false, &t);
	CHECK(t.child[0] != PROF_NONE);
	CHECK_STR(prof_str(&p, p.fn_name[t.fn[t.child[0]]]), "main");
	CHECK(t.maxdepth == 3);
	prof_tree(&p, 0, p.peak_snap, true, &t);
	CHECK_STR(prof_str(&p, p.fn_name[t.fn[t.child[0]]]), "grow");
	prof_tree_free(&t);
	double *self = calloc(p.nfn, sizeof(double)), *tot = calloc(p.nfn, sizeof(double));
	prof_fn_sums(&p, 0, p.peak_snap, self, tot);
	u32 grow = fn_named(&p, "grow"), mainf = fn_named(&p, "main");
	CHECK(grow != PROF_NONE && near(self[grow], 129936000.0));
	CHECK(mainf != PROF_NONE && near(tot[mainf], 129936000.0) && self[mainf] == 0);
	CHECK(p.fn_line[grow] == 15);
	free(self);
	free(tot);
	ProfLines l = {0};
	prof_lines(&p, 0, p.peak_snap, &l);
	bool l15 = false, l37 = false;
	for (u32 i = 0; i < l.n; i++) {
		const char *f = prof_str(&p, p.file_path[l.file[i]]);
		if (!strcmp(f, "hot.c") && l.line[i] == 15)
			l15 = near(l.self[i], 129936000.0);
		if (!strcmp(f, "hot.c") && l.line[i] == 37)
			l37 = l.self[i] == 0 && near(l.total[i], 129936000.0);
	}
	CHECK(l15);
	CHECK(l37);
	prof_lines_free(&l);
	/* callers of grow at the peak: leafy */
	u32 *nf;
	double *nv;
	u32 k = prof_neighbors(&p, 0, p.peak_snap, grow, true, &nf, &nv);
	CHECK(k == 1 && nf && !strcmp(prof_str(&p, p.fn_name[nf[0]]), "leafy"));
	free(nf);
	free(nv);
	prof_free(&p);
	free(s);

	/* below-threshold entries, C++ names with parentheses, unknown objects */
	const char *m = "desc: (none)\ncmd: ./x\ntime_unit: ms\n#-----------\nsnapshot=0\n#-----------\n"
	                "time=5\nmem_heap_B=300\nmem_heap_extra_B=8\nmem_stacks_B=0\nheap_tree=detailed\n"
	                "n3: 300 (heap allocation functions) malloc/new/new[], --alloc-fns, etc.\n"
	                " n1: 200 0x10: Foo::bar(int, char const*) (foo.cpp:42)\n"
	                "  n0: 200 0x20: main (main.cpp:7)\n"
	                " n0: 60 0x30: ??? (in /lib/libc.so.7)\n"
	                " n0: 40 in 2 places, all below massif's threshold (1.00%)\n";
	CHECK(prof_parse_massif(&p, m, strlen(m)) == 0);
	CHECK(p.peak_snap == 0);
	CHECK(fn_named(&p, "Foo::bar(int, char const*)") != PROF_NONE);
	CHECK(fn_named(&p, "??? [libc.so.7]") != PROF_NONE);
	CHECK(fn_named(&p, "(below massif's threshold)") != PROF_NONE);
	CHECK(near(prof_total(&p, 0, 0), 300));
	prof_free(&p);
}

static void test_jeprof(void)
{
	size_t n;
	char *s = load("tests/prof/app.heap", &n);
	Prof p;
	CHECK(s && prof_parse_jeprof(&p, s, n) == 0);
	CHECK(p.sample_period == 524288);
	CHECK(p.nst == 2);
	CHECK(p.nmet == 4); /* accumulated values present */
	CHECK(p.nmod == 1 && !strcmp(prof_str(&p, p.mod_path[0]), "/nonexistent/app"));
	/* unbiased: 2 MiB allocations at a 512 KiB period scale by 1/(1-e^-4) */
	double sc = 1.0 / (1.0 - exp(-4.0));
	CHECK(near(prof_total(&p, 0, PROF_NONE), (4194304.0 + 2097152.0) * sc));
	CHECK(near(prof_total(&p, 1, PROF_NONE), 3.0 * sc));
	/* root first: the outermost return address minus one */
	CHECK_STR(prof_str(&p, p.fn_name[p.fr_fn[p.sfr[p.st_off[0]]]]), "0x301f");
	CHECK_STR(prof_str(&p, p.fn_name[p.fr_fn[p.sfr[p.st_off[0] + 2]]]), "0x1000");
	ProfTree t = {0};
	prof_tree(&p, 0, PROF_NONE, false, &t);
	CHECK(t.child[0] != PROF_NONE && t.next[t.child[0]] == PROF_NONE); /* one root caller */
	u32 r = t.child[0];
	CHECK(t.child[r] != PROF_NONE && t.next[t.child[r]] != PROF_NONE); /* two branches */
	CHECK(t.val[t.child[r]] >= t.val[t.next[t.child[r]]]);         /* largest first */
	prof_tree_free(&t);
	prof_jeprof_symbolize(&p); /* the object does not exist: names stay addresses */
	CHECK(p.nst == 2 && p.st_len[0] == 3);
	prof_free(&p);
	free(s);

	const char *old = "heap profile:   3:  3072 [   3:  3072] @ heapprofile\n"
	                  "     2:     2048 [     2:     2048] @ 0x10 0x20\n"
	                  "     1:     1024 [     1:     1024] @ 0x10 0x30\n";
	CHECK(prof_parse_jeprof(&p, old, strlen(old)) == 0);
	CHECK(p.nst == 2 && near(prof_total(&p, 0, PROF_NONE), 3072)); /* no sampling: no scaling */
	prof_free(&p);
}

static void test_gcov(void)
{
	size_t n;
	char *s = load("tests/prof/hot.c.gcov", &n);
	Prof p;
	CHECK(s && prof_parse_gcov(&p, s, n) == 0);
	CHECK(p.nfile == 1 && !strcmp(prof_str(&p, p.file_path[0]), "hot.c"));
	u32 w = fn_named(&p, "work");
	CHECK(w != PROF_NONE && p.fn_calls[w] == 200 && p.fn_line[w] == 5 && p.fn_blocks[w] == 100);
	ProfLines l = {0};
	prof_lines(&p, 0, PROF_NONE, &l);
	bool l25 = false, l24 = false;
	for (u32 i = 0; i < l.n; i++) {
		if (l.line[i] == 25)
			l25 = l.self[i] == 2002000 && l.fn[i] == fn_named(&p, "hotloop");
		if (l.line[i] == 24)
			l24 = l.br_tot[i] == 2 && l.br_hit[i] == 2;
	}
	CHECK(l25);
	CHECK(l24);
	prof_lines_free(&l);
	prof_free(&p);
	free(s);
	const char *g = "        -:    0:Source:/src/a.c\n"
	                "function f called 0 returned 0% blocks executed 0%\n"
	                "    #####:    3:int f(void) {\n"
	                "     1.2k*:    4:  x();\n"
	                "branch  0 taken 0\n"
	                "branch  1 never executed\n"
	                "        -:    5:}\n"
	                "------------------\n"
	                "    #####:    3:int f(void) {\n"; /* an instantiation block repeats lines */
	CHECK(prof_parse_gcov(&p, g, strlen(g)) == 0);
	CHECK(p.ngl == 2);
	CHECK(p.gl_flags[0] & PL_UNCOVERED);
	CHECK(p.gl_count[1] == 1200 && (p.gl_flags[1] & PL_PARTIAL));
	CHECK(p.gl_br_tot[1] == 2 && p.gl_br_hit[1] == 0);
	prof_free(&p);
}

/* gmon: main(0x1000) calls a(0x1040) once and b(0x1080) three times, a calls
 * b once. b has 40 samples, a 10. Lines: (addr - start) / 4 + 10 in t.c. */
static void fake_resolve(void *ctx, Prof *p, const u64 *addr, u32 n, ProfLoc *out)
{
	(void)ctx;
	for (u32 i = 0; i < n; i++) {
		u64 a = addr[i];
		const char *nm = a < 0x1040 ? "main" : a < 0x1080 ? "a" : "b";
		u64 st = a < 0x1040 ? 0x1000 : a < 0x1080 ? 0x1040 : 0x1080;
		out[i].name = pool_add(&p->pool, nm, (u32)strlen(nm));
		out[i].file = pool_add(&p->pool, "t.c", 3);
		out[i].line = (u32)((a - st) / 4 + 10);
		out[i].fstart = st;
		out[i].alt = 0;
	}
}

static void test_gprof_synth(void)
{
	Gmon g = {0};
	g.lowpc = 0x1000;
	g.highpc = 0x1100;
	g.nbins = 64;
	g.bins = calloc(64, sizeof(u64));
	g.bins[(0x1080 - 0x1000) / 4] = 40;
	g.bins[(0x1060 - 0x1000) / 4] = 10;
	g.rate = 100;
	u64 from[] = {0x1011, 0x1021, 0x1051}, self[] = {0x1040, 0x1080, 0x1080}, cnt[] = {1, 3, 1};
	g.narc = 3;
	g.arc_from = from;
	g.arc_self = self;
	g.arc_count = cnt;
	Prof p;
	memset(&p, 0, sizeof p);
	pool_add(&p.pool, "", 0);
	p.kind = PK_GPROF;
	p.peak_snap = PROF_NONE;
	prof_from_gmon(&p, &g, fake_resolve, NULL);
	free(g.bins);
	CHECK(p.hist_samples == 50 && p.total_calls == 5);
	CHECK(near(p.sec_per_sample, 0.01));
	CHECK(near(prof_total(&p, 0, PROF_NONE), 50));
	u32 mf = fn_named(&p, "main"), af = fn_named(&p, "a"), bf = fn_named(&p, "b");
	CHECK(mf != PROF_NONE && af != PROF_NONE && bf != PROF_NONE);
	CHECK(p.fn_calls[bf] == 4 && p.fn_calls[af] == 1);
	CHECK(p.fn_line[bf] == 10);
	double *se = calloc(p.nfn, sizeof(double)), *to = calloc(p.nfn, sizeof(double));
	prof_fn_sums(&p, 0, PROF_NONE, se, to);
	CHECK(near(to[mf], 50) && near(se[mf], 0));
	CHECK(near(to[af], 20) && near(se[af], 10)); /* a: 10 own + a quarter of b */
	CHECK(near(se[bf], 40) && near(to[bf], 40));
	free(se);
	free(to);
	/* callers of b: main 30, a 10 */
	u32 *nf;
	double *nv;
	u32 k = prof_neighbors(&p, 0, PROF_NONE, bf, true, &nf, &nv);
	CHECK(k == 2 && nf[0] == mf && near(nv[0], 30) && nf[1] == af && near(nv[1], 10));
	free(nf);
	free(nv);
	CHECK(prof_arc_calls(&p, mf, bf) == 3);
	/* the call site main+0x10 (line 14) carries a's time */
	ProfLines l = {0};
	prof_lines(&p, 0, PROF_NONE, &l);
	bool l13 = false;
	for (u32 i = 0; i < l.n; i++)
		if (l.line[i] == 14 && l.fn[i] == mf)
			l13 = near(l.total[i], 20) && l.self[i] == 0;
	CHECK(l13);
	prof_lines_free(&l);
	char b[32];
	prof_fmt(&p, 0, 50, b, sizeof b);
	CHECK_STR(b, "500.0 ms");
	prof_free(&p);
}

static void test_gmon_file(void)
{
	size_t n;
	u8 *d = (u8 *)load("tests/prof/hot.gmon", &n);
	Gmon g;
	CHECK(d && gmon_parse(d, n, 8, &g) == 0);
	CHECK(!g.gnu && g.highpc > g.lowpc && g.nbins > 0);
	u64 sum = 0;
	for (u32 i = 0; i < g.nbins; i++)
		sum += g.bins[i];
	CHECK(sum > 0);
	CHECK(g.narc == 5);
	bool c2000 = false, c200 = false;
	for (u32 a = 0; a < g.narc; a++) {
		c2000 |= g.arc_count[a] == 2000;
		c200 |= g.arc_count[a] == 200;
	}
	CHECK(c2000 && c200);
	gmon_free(&g);
	free(d);
	/* GNU format: header, one histogram, one arc */
	u8 gnu[200] = "gmon";
	u32 ver = 1;
	memcpy(gnu + 4, &ver, 4);
	size_t o = 20;
	gnu[o++] = 0;
	u64 lo = 0x400000, hi = 0x400010;
	u32 hs = 4, rate = 1000;
	memcpy(gnu + o, &lo, 8), o += 8;
	memcpy(gnu + o, &hi, 8), o += 8;
	memcpy(gnu + o, &hs, 4), o += 4;
	memcpy(gnu + o, &rate, 4), o += 4;
	memcpy(gnu + o, "seconds", 7), o += 15;
	gnu[o++] = 's';
	u16 bins[4] = {1, 0, 7, 0};
	memcpy(gnu + o, bins, 8), o += 8;
	gnu[o++] = 1;
	u64 f = 0x400004, s = 0x400008;
	u32 c = 9;
	memcpy(gnu + o, &f, 8), o += 8;
	memcpy(gnu + o, &s, 8), o += 8;
	memcpy(gnu + o, &c, 4), o += 4;
	CHECK(gmon_parse(gnu, o, 8, &g) == 0);
	CHECK(g.gnu && g.lowpc == lo && g.nbins == 4 && g.bins[2] == 7 && g.rate == 1000);
	CHECK(g.narc == 1 && g.arc_count[0] == 9 && g.arc_self[0] == s);
	gmon_free(&g);
}

/* Compile tests/prof/hot.c with -pg, run it and load what it wrote. */
static void test_gprof_e2e(void)
{
	char dir[] = "/tmp/vxide-prof-XXXXXX";
	if (!mkdtemp(dir))
		return;
	char cmd[1024], src[4096];
	if (!realpath("tests/prof/hot.c", src))
		return;
	snprintf(cmd, sizeof cmd,
	         "cd %s && cc -O1 -fno-inline -g -pg -o hot %s >/dev/null 2>&1 && ./hot 300000 >/dev/null 2>&1 && "
	         "(test -f hot.gmon || test -f gmon.out)",
	         dir, src);
	if (system(cmd) != 0) {
		fprintf(stderr, "# gprof end-to-end skipped (cc -pg unavailable)\n");
		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		(void)system(cmd);
		return;
	}
	char gm[1100];
	snprintf(gm, sizeof gm, "%s/hot.gmon", dir);
	if (access(gm, R_OK) != 0)
		snprintf(gm, sizeof gm, "%s/gmon.out", dir);
	size_t n;
	char *d = load(gm, &n);
	Prof p;
	int rc = prof_load(&p, PK_GPROF, gm, (const u8 *)d, n, NULL);
	CHECK(rc == 0);
	if (rc)
		fprintf(stderr, "# %s\n", p.err);
	double *se = calloc(p.nfn + 1, sizeof(double)), *to = calloc(p.nfn + 1, sizeof(double));
	prof_fn_sums(&p, 0, PROF_NONE, se, to);
	u32 hot = fn_named(&p, "hotloop"), mf = fn_named(&p, "main"), grow = fn_named(&p, "grow");
	CHECK(hot != PROF_NONE && mf != PROF_NONE);
	u32 top = 0;
	for (u32 f = 1; f < p.nfn; f++)
		if (se[f] > se[top])
			top = f;
	CHECK(top == hot);
	CHECK(mf != PROF_NONE && near(to[mf], prof_total(&p, 0, PROF_NONE)));
	CHECK(grow != PROF_NONE && p.fn_calls[grow] == 2001);
	/* the entry's line: the signature or its opening brace */
	CHECK(hot != PROF_NONE && (p.fn_line[hot] == 20 || p.fn_line[hot] == 21));
	/* the hot lines are inside hotloop's loops (23-25) */
	ProfLines l = {0};
	prof_lines(&p, 0, PROF_NONE, &l);
	u32 best = 0;
	for (u32 i = 1; i < l.n; i++)
		if (l.self[i] > l.self[best])
			best = i;
	CHECK(l.n && l.line[best] >= 23 && l.line[best] <= 25);
	prof_lines_free(&l);
	free(se);
	free(to);
	prof_free(&p);
	free(d);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)system(cmd);
}

static void test_pmc(void)
{
	const char *a = "# pmcstat samples: event=cpu_clk_unhalted.thread_p rate=65536 program=./hot 10\n"
	                " #samples/total 3\n"
	                "0x201a1f hotloop /tmp/x/hot.c:25\n"
	                "0x2018cb main /tmp/x/hot.c:0\n"
	                "0x82208d3e8 ?? ??:0\n"
	                "0x3c1363e03008 <unknown> ??:0\n"
	                "--\n"
	                "0x201a1f hotloop /tmp/x/hot.c:25\n"
	                "0x2018cb main /tmp/x/hot.c:0\n"
	                "0x55 <unknown> ??:0\n"
	                "--\n"
	                "0x201a30 operator new(unsigned long) /usr/include/c++/new:12\n"
	                "0x201a23 hotloop /tmp/x/hot.c:24\n"
	                "0x2018cb main /tmp/x/hot.c:0\n";
	Prof p;
	CHECK(prof_detect("/t/hot.cpu.pmcsamples", a, strlen(a)) == PK_PMC);
	CHECK(prof_parse_pmc(&p, a, strlen(a)) == 0);
	CHECK(p.hist_samples == 3);
	CHECK(p.nst == 2);                          /* the first two samples are one stack */
	CHECK(near(prof_total(&p, 0, PROF_NONE), 3));
	CHECK_STR(prof_str(&p, p.met_name[0]), "Samples (cpu_clk_unhalted.thread_p)");
	CHECK_STR(prof_str(&p, p.cmd), "./hot 10");
	u32 mf = fn_named(&p, "main"), hf = fn_named(&p, "hotloop"), nf = fn_named(&p, "operator new(unsigned long)");
	CHECK(mf != PROF_NONE && hf != PROF_NONE && nf != PROF_NONE);
	/* root junk stripped: every stack starts at main */
	for (u32 st = 0; st < p.nst; st++)
		CHECK(p.fr_fn[p.sfr[p.st_off[st]]] == mf);
	double *se = calloc(p.nfn, sizeof(double)), *to = calloc(p.nfn, sizeof(double));
	prof_fn_sums(&p, 0, PROF_NONE, se, to);
	CHECK(near(se[hf], 2) && near(to[hf], 3) && near(to[mf], 3) && near(se[nf], 1));
	free(se);
	free(to);
	ProfLines l = {0};
	prof_lines(&p, 0, PROF_NONE, &l);
	bool l25 = false, l24 = false;
	for (u32 i = 0; i < l.n; i++) {
		if (l.line[i] == 25)
			l25 = near(l.self[i], 2);
		if (l.line[i] == 24)
			l24 = l.self[i] == 0 && near(l.total[i], 1); /* the call site of operator new */
	}
	CHECK(l25 && l24);
	prof_lines_free(&l);
	prof_free(&p);
}

static void test_fmt(void)
{
	char b[32];
	prof_fmt_bytes(512, b, sizeof b);
	CHECK_STR(b, "512 B");
	prof_fmt_bytes(1536, b, sizeof b);
	CHECK_STR(b, "1.50 KiB");
	prof_fmt_bytes(129936000, b, sizeof b);
	CHECK_STR(b, "123.9 MiB");
	prof_fmt_count(1234567, b, sizeof b);
	CHECK_STR(b, "1,234,567");
	prof_fmt_count(12, b, sizeof b);
	CHECK_STR(b, "12");
}

int main(void)
{
	test_detect();
	test_massif();
	test_jeprof();
	test_gcov();
	test_gprof_synth();
	test_gmon_file();
	test_gprof_e2e();
	test_pmc();
	test_fmt();
	return tap_done();
}
