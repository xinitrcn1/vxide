/* Profiles: valgrind massif output, jemalloc heap profiles (heap_v2 and the
 * older gperftools-style "heap profile:" text), gprof's gmon data (BSD
 * gmonhdr and GNU "gmon" files, symbolised from the executable) and gcov
 * reports (.gcov text, or .gcda run through gcov).
 *
 * Everything becomes one model: functions, frames (a function at a source
 * line), and stacks of frames (root first) carrying up to four values
 * ("metrics": bytes, objects, samples...). Massif snapshots tag their
 * stacks; gprof's flat histogram and call arcs are turned into stacks the
 * way gprof propagates time (a callee's time is split among its callers by
 * call counts), so the flame graph, the callers/callees and the per-line
 * costs work the same for every kind. gcov keeps its per-line counts.
 *
 * Tables are coalesced SoA blocks (soa_grow), strings live in one pool. */
#ifndef VX_PROF_H
#define VX_PROF_H

#include "base.h"

enum { PK_NONE, PK_MASSIF, PK_JEPROF, PK_GPROF, PK_GCOV, PK_PMC };
enum { PU_BYTES, PU_COUNT, PU_SAMPLES };
enum { PSN_EMPTY, PSN_DETAILED, PSN_PEAK }; /* massif heap_tree= */
enum { PL_CODE = 1, PL_UNCOVERED = 2, PL_PARTIAL = 4 }; /* gcov line flags */

#define PROF_MAXMET 4
#define PROF_NONE UINT32_MAX

typedef struct {
	u32 cap, n;
	u32 *id; /* slot -> id + 1 (0 = empty) */
	u64 *h;
} ProfIx;

typedef struct {
	int kind;
	char err[256];
	Pool pool; /* offset 0 is "" */
	u32 title; /* massif desc:, jeprof header, gcov first source */
	u32 cmd;   /* massif cmd:, gprof/jeprof executable */
	u32 time_unit; /* massif: "i", "ms" or "B" */
	u32 nmet;
	u32 met_name[PROF_MAXMET];
	u8 met_unit[PROF_MAXMET];
	double sec_per_sample; /* gprof */
	u64 sample_period;     /* jemalloc: average bytes between samples */
	u64 hist_samples;      /* gprof: samples in the histogram */
	u64 total_calls;       /* gprof: calls recorded by mcount */
	u32 nunresolved;       /* frames symbolisation could not name */

	/* files */
	u32 nfile, file_cap;
	u32 *file_path;
	ProfIx file_ix;

	/* functions */
	u32 nfn, fn_cap;
	u32 *fn_name, *fn_file, *fn_line; /* line 0: unknown */
	u64 *fn_calls;                    /* gprof / gcov: times called */
	u8 *fn_blocks;                    /* gcov: % of blocks executed (255 unknown) */
	ProfIx fn_ix;

	/* frames: a function at a line (or an address before symbolisation) */
	u32 nfr, fr_cap;
	u32 *fr_fn, *fr_file, *fr_line, *fr_mod;
	u64 *fr_addr;
	ProfIx fr_ix;

	/* stacks: sfr[st_off .. st_off+st_len), root first */
	u32 nst, st_cap;
	u32 *st_off, *st_len, *st_snap;
	double *st_v0, *st_v1, *st_v2, *st_v3;
	u32 nsfr, sfr_cap;
	u32 *sfr;

	/* massif snapshots */
	u32 nsnap, snap_cap;
	u64 *sn_time, *sn_heap, *sn_extra, *sn_stacks;
	u8 *sn_tree;
	u32 peak_snap; /* PROF_NONE when none */

	/* gprof call arcs: call-site frame -> callee function */
	u32 narc, arc_cap;
	u32 *arc_from, *arc_to;
	u64 *arc_count;

	/* jemalloc: mapped objects (for symbolisation) */
	u32 nmod, mod_cap;
	u64 *mod_lo, *mod_hi, *mod_off;
	u32 *mod_path;

	/* gcov: per-line counts */
	u32 ngl, gl_cap;
	u32 *gl_file, *gl_line;
	u64 *gl_count;
	u8 *gl_flags;
	u16 *gl_br_tot, *gl_br_hit;
} Prof;

/* What kind of profile a file is (PK_NONE: not one), from its name and head. */
int prof_detect(const char *path, const char *head, size_t n);

/* Parse a profile. 0 on success, -1 with p->err set (what parsed is kept).
 * gmon needs the executable: exe may be NULL to look next to the file
 * (prog.gmon -> prog; gmon.out -> an executable in the same folder whose
 * code covers the histogram). Symbolisation runs addr2line (llvm-addr2line,
 * addr2line; VXIDE_ADDR2LINE overrides) on the executable / mapped objects,
 * so call this off the UI thread. gcda files run gcov -t. */
int prof_load(Prof *p, int kind, const char *path, const u8 *data, size_t n, const char *exe);
void prof_free(Prof *p);
/* The tool's own text for a profile (gprof -b, gcov -t) or the file
 * itself; malloc'd, NULL when the tool failed. Runs tools: not on the UI
 * thread. */
char *prof_raw_report(int kind, const char *path, const char *exe, size_t *n);
/* Run argv (PATH lookup) in cwd (NULL: here), stdout and stderr captured
 * together; NULL when it could not start (exit 127). Blocks: worker
 * threads only. */
char *prof_run(const char *cwd, char *const argv[], size_t *len, int *status);

/* Parsers on their own (no files read, no tools run): for tests. */
int prof_parse_massif(Prof *p, const char *s, size_t n);
int prof_parse_jeprof(Prof *p, const char *s, size_t n); /* frames stay addresses */
int prof_parse_gcov(Prof *p, const char *s, size_t n);
/* pmcstat -R log -a out: one callchain per sample, leaf first, frames
 * "0xADDR function file:line", samples separated by "--" (a "# ..." header
 * line names the event). Identical stacks are merged. */
int prof_parse_pmc(Prof *p, const char *s, size_t n);
/* Symbolise jemalloc frames, then drop allocator frames at the leaves. */
void prof_jeprof_symbolize(Prof *p);

/* gmon histogram and arcs, before symbolisation. */
typedef struct {
	u64 lowpc, highpc;
	u32 nbins;
	u64 *bins;
	u32 rate; /* samples per second */
	u32 narc;
	u64 *arc_from, *arc_self, *arc_count;
	bool gnu;
	char err[128];
} Gmon;
int gmon_parse(const u8 *d, size_t n, int ptr_size, Gmon *g);
void gmon_free(Gmon *g);
/* Build the profile from gmon data with a symboliser: for each address,
 * the function name, its start, the file and line. */
typedef struct {
	u32 name; /* pool offset in p */
	u32 file; /* pool offset, 0 = unknown */
	u32 line;
	u64 fstart; /* function start address, UINT64_MAX unknown */
	u32 alt;    /* the debug info's (demangled) function name, 0 = none */
} ProfLoc;
typedef void (*ProfResolveFn)(void *ctx, Prof *p, const u64 *addr, u32 n, ProfLoc *out);
void prof_from_gmon(Prof *p, const Gmon *g, ProfResolveFn resolve, void *ctx);

/* Strings and helpers */
static inline const char *prof_str(const Prof *p, u32 off) { return p->pool.b ? p->pool.b + off : ""; }
static inline double prof_sv(const Prof *p, u32 m, u32 s)
{
	return m == 0 ? p->st_v0[s] : m == 1 ? p->st_v1[s] : m == 2 ? p->st_v2[s] : p->st_v3[s];
}
const char *prof_kind_name(int kind);
/* A value of metric m as text: "1.4 MiB", "0.31 s", "12,345". */
void prof_fmt(const Prof *p, u32 m, double v, char *buf, size_t cap);
void prof_fmt_bytes(double v, char *buf, size_t cap);
void prof_fmt_count(double v, char *buf, size_t cap);

/* ---- aggregation for one metric over the stacks of one snapshot ----------------
 * (snap = PROF_NONE: all stacks; massif stacks always have a snapshot) */
double prof_total(const Prof *p, u32 m, u32 snap);
/* per function: self (as leaf) and total (once per stack); arrays of p->nfn */
void prof_fn_sums(const Prof *p, u32 m, u32 snap, double *self, double *total);

/* Call tree: node 0 is the root (fn PROF_NONE). Top-down (callers first,
 * the flame graph) or bottom-up (leaf functions first: who allocated /
 * where the time went, then their callers). Children are ordered by
 * value, largest first. */
typedef struct {
	u32 n, cap;
	u32 *fn, *parent, *child, *next, *depth;
	double *val, *self;
	u32 maxdepth;
} ProfTree;
void prof_tree(const Prof *p, u32 m, u32 snap, bool bottom_up, ProfTree *t);
void prof_tree_free(ProfTree *t);

/* Source lines with a cost: self = cost while the line runs (samples at
 * the line, bytes allocated there), total = also what it calls (a call
 * site carries the callee's time). gcov: the execution count in both,
 * flags PL_*. Unsorted. */
typedef struct {
	u32 n, cap;
	u32 *file, *line, *fn;
	double *self, *total;
	u8 *flags;
	u16 *br_tot, *br_hit;
} ProfLines;
void prof_lines(const Prof *p, u32 m, u32 snap, ProfLines *out);
void prof_lines_free(ProfLines *l);

/* Callers (or callees) of function f with the value flowing through each
 * edge, largest first. Returns the count; *fn / *val are malloc'd. */
u32 prof_neighbors(const Prof *p, u32 m, u32 snap, u32 f, bool callers, u32 **fn, double **val);
/* gprof: calls from g to f according to the arcs (0 when none) */
u64 prof_arc_calls(const Prof *p, u32 from_fn, u32 to_fn);

#endif
