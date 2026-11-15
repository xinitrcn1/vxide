/* Profiles: parsers (massif, jemalloc heap, gmon, gcov), symbolisation
 * through addr2line, gprof time propagation, and the aggregations the
 * profile views draw (per function, call trees, per line, callers). */
#define _POSIX_C_SOURCE 200809L
#include "prof.h"
#include "binfile.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NONE PROF_NONE

/* ---- small helpers ------------------------------------------------------------------ */

static u64 hbytes(const void *s, size_t n, u64 h)
{
	const u8 *b = s;
	if (!h)
		h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h | 1;
}

/* memmem (not in POSIX) */
static const char *mfind(const void *hay, size_t n, const char *needle, size_t k)
{
	const char *h = hay;
	if (!k || k > n)
		return k ? NULL : h;
	for (size_t i = 0; i + k <= n; i++)
		if (h[i] == needle[0] && !memcmp(h + i, needle, k))
			return h + i;
	return NULL;
}

typedef bool (*EqFn)(const Prof *p, u32 id, const void *key);

static void ix_free(ProfIx *x)
{
	free(x->id);
	free(x->h);
	memset(x, 0, sizeof *x);
}

static void ix_put(ProfIx *x, u64 h, u32 id)
{
	u32 m = x->cap - 1, i = (u32)h & m;
	while (x->id[i])
		i = (i + 1) & m;
	x->id[i] = id + 1;
	x->h[i] = h;
}

static void ix_add(ProfIx *x, u64 h, u32 id)
{
	if ((x->n + 1) * 2 > x->cap) {
		ProfIx o = *x;
		x->cap = o.cap ? o.cap * 2 : 64;
		x->id = vx_alloc(x->cap * sizeof(u32));
		x->h = vx_alloc(x->cap * sizeof(u64));
		for (u32 i = 0; i < o.cap; i++)
			if (o.id[i])
				ix_put(x, o.h[i], o.id[i] - 1);
		free(o.id);
		free(o.h);
	}
	ix_put(x, h, id);
	x->n++;
}

static u32 ix_find(const ProfIx *x, u64 h, const Prof *p, EqFn eq, const void *key)
{
	if (!x->cap)
		return NONE;
	u32 m = x->cap - 1, i = (u32)h & m;
	while (x->id[i]) {
		if (x->h[i] == h && eq(p, x->id[i] - 1, key))
			return x->id[i] - 1;
		i = (i + 1) & m;
	}
	return NONE;
}

typedef struct {
	const char *s;
	size_t n;
} Str;

#define FILE_COLS(p) SOA((p)->file_path)
#define FN_COLS(p) SOA((p)->fn_name), SOA((p)->fn_file), SOA((p)->fn_line), SOA((p)->fn_calls), SOA((p)->fn_blocks)
#define FR_COLS(p) SOA((p)->fr_fn), SOA((p)->fr_file), SOA((p)->fr_line), SOA((p)->fr_mod), SOA((p)->fr_addr)
#define ST_COLS(p)                                                                                                     \
	SOA((p)->st_off), SOA((p)->st_len), SOA((p)->st_snap), SOA((p)->st_v0), SOA((p)->st_v1), SOA((p)->st_v2),          \
	    SOA((p)->st_v3)
#define SN_COLS(p) SOA((p)->sn_time), SOA((p)->sn_heap), SOA((p)->sn_extra), SOA((p)->sn_stacks), SOA((p)->sn_tree)
#define ARC_COLS(p) SOA((p)->arc_from), SOA((p)->arc_to), SOA((p)->arc_count)
#define MOD_COLS(p) SOA((p)->mod_lo), SOA((p)->mod_hi), SOA((p)->mod_off), SOA((p)->mod_path)
#define GL_COLS(p)                                                                                                     \
	SOA((p)->gl_file), SOA((p)->gl_line), SOA((p)->gl_count), SOA((p)->gl_flags), SOA((p)->gl_br_tot),                 \
	    SOA((p)->gl_br_hit)

static u32 str_add(Prof *p, const char *s, size_t n) { return pool_add(&p->pool, s, (u32)n); }

static bool eq_file(const Prof *p, u32 id, const void *k)
{
	const Str *s = k;
	const char *a = prof_str(p, p->file_path[id]);
	return strlen(a) == s->n && !memcmp(a, s->s, s->n);
}

static u32 file_of(Prof *p, const char *s, size_t n)
{
	if (!n || (n == 2 && !memcmp(s, "??", 2)))
		return NONE;
	u64 h = hbytes(s, n, 0);
	Str k = {s, n};
	u32 id = ix_find(&p->file_ix, h, p, eq_file, &k);
	if (id != NONE)
		return id;
	SoaCol c[] = {FILE_COLS(p)};
	soa_grow(c, 1, &p->file_cap, p->nfile + 1, p->nfile);
	id = p->nfile++;
	p->file_path[id] = str_add(p, s, n);
	ix_add(&p->file_ix, h, id);
	return id;
}

static bool eq_fn(const Prof *p, u32 id, const void *k)
{
	const Str *s = k;
	const char *a = prof_str(p, p->fn_name[id]);
	return strlen(a) == s->n && !memcmp(a, s->s, s->n);
}

static u32 fn_of(Prof *p, const char *s, size_t n)
{
	u64 h = hbytes(s, n, 0);
	Str k = {s, n};
	u32 id = ix_find(&p->fn_ix, h, p, eq_fn, &k);
	if (id != NONE)
		return id;
	SoaCol c[] = {FN_COLS(p)};
	soa_grow(c, 5, &p->fn_cap, p->nfn + 1, p->nfn);
	id = p->nfn++;
	p->fn_name[id] = str_add(p, s, n);
	p->fn_file[id] = NONE;
	p->fn_line[id] = 0;
	p->fn_calls[id] = 0;
	p->fn_blocks[id] = 255;
	ix_add(&p->fn_ix, h, id);
	return id;
}

static u32 fn_ofz(Prof *p, const char *s) { return fn_of(p, s, strlen(s)); }

typedef struct {
	u32 fn, file, line, mod;
	u64 addr;
} FrKey;

static bool eq_fr(const Prof *p, u32 id, const void *k)
{
	const FrKey *f = k;
	return p->fr_fn[id] == f->fn && p->fr_file[id] == f->file && p->fr_line[id] == f->line &&
	       p->fr_addr[id] == f->addr && p->fr_mod[id] == f->mod;
}

static u32 frame_of(Prof *p, u32 fn, u32 file, u32 line, u64 addr, u32 mod)
{
	FrKey k = {fn, file, line, mod, addr};
	u64 h = hbytes(&k, sizeof k, 0);
	u32 id = ix_find(&p->fr_ix, h, p, eq_fr, &k);
	if (id != NONE)
		return id;
	SoaCol c[] = {FR_COLS(p)};
	soa_grow(c, 5, &p->fr_cap, p->nfr + 1, p->nfr);
	id = p->nfr++;
	p->fr_fn[id] = fn;
	p->fr_file[id] = file;
	p->fr_line[id] = line;
	p->fr_mod[id] = mod;
	p->fr_addr[id] = addr;
	ix_add(&p->fr_ix, h, id);
	if (file != NONE && line) {
		if (p->fn_file[fn] == NONE)
			p->fn_file[fn] = file;
		if (p->fn_file[fn] == file && (!p->fn_line[fn] || line < p->fn_line[fn]))
			p->fn_line[fn] = line;
	}
	return id;
}

static void stack_add(Prof *p, const u32 *fr, u32 n, u32 snap, const double *v)
{
	if (!n)
		return;
	SoaCol c[] = {ST_COLS(p)};
	soa_grow(c, 7, &p->st_cap, p->nst + 1, p->nst);
	VX_GROW(p->sfr, p->sfr_cap, p->nsfr + n);
	u32 s = p->nst++;
	p->st_off[s] = p->nsfr;
	p->st_len[s] = n;
	p->st_snap[s] = snap;
	p->st_v0[s] = v[0];
	p->st_v1[s] = v[1];
	p->st_v2[s] = v[2];
	p->st_v3[s] = v[3];
	memcpy(p->sfr + p->nsfr, fr, n * sizeof(u32));
	p->nsfr += n;
}

static void prof_begin(Prof *p, int kind)
{
	memset(p, 0, sizeof *p);
	p->kind = kind;
	pool_add(&p->pool, "", 0);
	p->peak_snap = NONE;
}

static void set_met(Prof *p, u32 m, const char *name, u8 unit)
{
	p->met_name[m] = pool_add(&p->pool, name, (u32)strlen(name));
	p->met_unit[m] = unit;
	if (p->nmet <= m)
		p->nmet = m + 1;
}

/* line iteration */
#define FOR_LINES(s, n, l, len)                                                                                        \
	for (const char *l = (s), *l##_e = (s) + (n), *l##_nl; l < l##_e; l = l##_nl ? l##_nl + 1 : l##_e)              \
		for (size_t len = (size_t)(((l##_nl = memchr(l, '\n', (size_t)(l##_e - l))) ? l##_nl : l##_e) - l),          \
		            len##_once = 1;                                                                                   \
		     len##_once; len##_once = 0)

static bool starts(const char *l, size_t n, const char *pre)
{
	size_t k = strlen(pre);
	return n >= k && !memcmp(l, pre, k);
}

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t'))
		(*s)++, (*n)--;
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\r' || (*s)[*n - 1] == '\t'))
		(*n)--;
}

static u64 num_at(const char *s, size_t n, size_t *used)
{
	u64 v = 0;
	size_t i = 0;
	while (i < n && s[i] >= '0' && s[i] <= '9')
		v = v * 10 + (u64)(s[i++] - '0');
	if (used)
		*used = i;
	return v;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* ---- detection, names, formatting --------------------------------------------------- */

static bool ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcmp(s + a - b, suf);
}

int prof_detect(const char *path, const char *head, size_t n)
{
	const char *b = path ? base_name(path) : "";
	if (n >= 5 && !memcmp(head, "desc:", 5) && mfind(head, n, "\ntime_unit:", 11))
		return PK_MASSIF;
	if (!strncmp(b, "massif.out", 10) && n >= 5 && !memcmp(head, "desc:", 5))
		return PK_MASSIF;
	if ((n >= 8 && !memcmp(head, "heap_v2/", 8)) || (n >= 13 && !memcmp(head, "heap profile:", 13)))
		return PK_JEPROF;
	if (n >= 4 && !memcmp(head, "gmon", 4) && (ends_with(b, ".out") || ends_with(b, ".gmon") || strstr(b, "gmon")))
		return PK_GPROF;
	if (n >= 24) {
		u32 v64, v32;
		memcpy(&v64, head + 20, 4);
		memcpy(&v32, head + 12, 4);
		if (v64 == 0x00051879u || v32 == 0x00051879u)
			return PK_GPROF; /* BSD gmonhdr (GMONVERSION) */
	}
	if (ends_with(b, ".gmon") || !strcmp(b, "gmon.out"))
		return PK_GPROF;
	if (ends_with(b, ".gcov") && n && mfind(head, n, ":Source:", 8))
		return PK_GCOV;
	if (ends_with(b, ".gcda"))
		return PK_GCOV;
	return PK_NONE;
}

const char *prof_kind_name(int kind)
{
	switch (kind) {
	case PK_MASSIF: return "Massif heap profile";
	case PK_JEPROF: return "jemalloc heap profile";
	case PK_GPROF: return "gprof profile";
	case PK_GCOV: return "gcov coverage";
	default: return "Profile";
	}
}

void prof_fmt_bytes(double v, char *buf, size_t cap)
{
	double a = fabs(v);
	if (a < 1024)
		snprintf(buf, cap, "%.0f B", v);
	else if (a < 1024.0 * 1024)
		snprintf(buf, cap, "%.*f KiB", a < 10240 ? 2 : 1, v / 1024);
	else if (a < 1024.0 * 1024 * 1024)
		snprintf(buf, cap, "%.*f MiB", a < 10485760.0 ? 2 : 1, v / (1024.0 * 1024));
	else
		snprintf(buf, cap, "%.2f GiB", v / (1024.0 * 1024 * 1024));
}

void prof_fmt_count(double v, char *buf, size_t cap)
{
	if (fabs(v) >= 1e15) {
		snprintf(buf, cap, "%.3g", v);
		return;
	}
	if (v != floor(v) && fabs(v) < 100) {
		snprintf(buf, cap, "%.2f", v);
		return;
	}
	char t[32];
	int k = snprintf(t, sizeof t, "%.0f", fabs(v));
	size_t o = 0;
	if (v < 0 && o + 1 < cap)
		buf[o++] = '-';
	for (int i = 0; i < k && o + 2 < cap; i++) {
		if (i && (k - i) % 3 == 0)
			buf[o++] = ',';
		buf[o++] = t[i];
	}
	buf[o] = 0;
}

void prof_fmt(const Prof *p, u32 m, double v, char *buf, size_t cap)
{
	u8 u = m < p->nmet ? p->met_unit[m] : PU_COUNT;
	if (u == PU_BYTES)
		prof_fmt_bytes(v, buf, cap);
	else if (u == PU_SAMPLES && p->sec_per_sample > 0) {
		double s = v * p->sec_per_sample;
		if (s == 0)
			snprintf(buf, cap, "0 s");
		else if (s >= 1)
			snprintf(buf, cap, "%.2f s", s);
		else if (s >= 0.001)
			snprintf(buf, cap, "%.1f ms", s * 1e3);
		else
			snprintf(buf, cap, "%.0f us", s * 1e6);
	} else
		prof_fmt_count(v, buf, cap);
}

/* ---- running tools --------------------------------------------------------------------- */

/* Run argv (PATH lookup), stdout captured, stdin/stderr /dev/null, in
 * cwd when given (through sh: posix_spawn has no portable chdir).
 * NULL when it could not be started or exited 127 (not found). */
static char *run_capture_in(const char *cwd, char *const argv[], size_t *len, int *status);
static char *run_capture(char *const argv[], size_t *len, int *status) { return run_capture_in(NULL, argv, len, status); }

static char *run_capture_in(const char *cwd, char *const argv_in[], size_t *len, int *status)
{
	char *wrap[64];
	char *const *argv = argv_in;
	if (cwd) {
		int k = 0;
		wrap[k++] = "/bin/sh";
		wrap[k++] = "-c";
		wrap[k++] = "cd \"$1\" || exit 127; shift; exec \"$@\"";
		wrap[k++] = "sh";
		wrap[k++] = (char *)cwd;
		for (int i = 0; argv_in[i] && k < 63; i++)
			wrap[k++] = argv_in[i];
		wrap[k] = NULL;
		argv = wrap;
	}
	int po[2];
	if (pipe(po) != 0)
		return NULL;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	pid_t pid;
	int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(po[1]);
	if (rc != 0) {
		close(po[0]);
		return NULL;
	}
	char *b = NULL;
	u32 n = 0, cap = 0;
	for (;;) {
		VX_GROW(b, cap, n + 65536);
		ssize_t r = read(po[0], b + n, cap - n - 1);
		if (r <= 0)
			break;
		n += (u32)r;
	}
	close(po[0]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
		free(b);
		return NULL;
	}
	if (status)
		*status = st;
	b[n] = 0;
	*len = n;
	return b;
}

/* addr2line on obj: function (demangled) and file:line per address. */
static void addr2line_batch(Prof *p, const char *obj, const u64 *addr, u32 n, ProfLoc *out)
{
	for (u32 i = 0; i < n; i++) {
		out[i].file = 0;
		out[i].line = 0;
		out[i].alt = 0;
	}
	const char *env = getenv("VXIDE_ADDR2LINE");
	const char *tools[] = {env && *env ? env : "llvm-addr2line", "llvm-addr2line", "addr2line"};
	static int which = -1; /* the first tool that worked (all calls agree) */
	enum { CHUNK = 256 };
	for (u32 base = 0; base < n; base += CHUNK) {
		u32 k = VX_MIN(n - base, (u32)CHUNK);
		char *argv[CHUNK + 8];
		static char abuf[CHUNK][24];
		u32 a = 0;
		argv[a++] = NULL;
		argv[a++] = "-f";
		argv[a++] = "-C";
		argv[a++] = "-e";
		argv[a++] = (char *)obj;
		for (u32 i = 0; i < k; i++) {
			snprintf(abuf[i], sizeof abuf[i], "0x%llx", (unsigned long long)addr[base + i]);
			argv[a++] = abuf[i];
		}
		argv[a] = NULL;
		char *outb = NULL;
		size_t olen = 0;
		for (int t = which >= 0 ? which : 0; t < 3 && !outb; t++) {
			argv[0] = (char *)tools[t];
			outb = run_capture(argv, &olen, NULL);
			if (outb && which < 0)
				which = t;
		}
		if (!outb)
			return;
		/* two lines per address: function, file:line [(discriminator N)] */
		u32 i = 0, half = 0;
		u32 fname = 0;
		FOR_LINES(outb, olen, l, len)
		{
			if (i >= k)
				break;
			const char *s = l;
			size_t m = len;
			trim(&s, &m);
			if (!half) {
				fname = m && !(m == 2 && !memcmp(s, "??", 2)) ? str_add(p, s, m) : 0;
				half = 1;
				continue;
			}
			half = 0;
			ProfLoc *o = &out[base + i++];
			o->alt = fname;
			const char *sp = memchr(s, ' ', m);
			if (sp)
				m = (size_t)(sp - s);
			const char *c = NULL;
			for (size_t j = m; j-- > 0;)
				if (s[j] == ':') {
					c = s + j;
					break;
				}
			if (!c)
				continue;
			size_t fl = (size_t)(c - s);
			if (!fl || (fl == 2 && !memcmp(s, "??", 2)))
				continue;
			o->file = str_add(p, s, fl);
			o->line = (u32)num_at(c + 1, m - fl - 1, NULL);
		}
		free(outb);
	}
}

/* ---- massif ------------------------------------------------------------------------------- */

static u64 kv_num(const char *l, size_t n, const char *key)
{
	size_t k = strlen(key);
	return num_at(l + k, n - k, NULL);
}

static u32 massif_frame(Prof *p, const char *s, size_t n)
{
	/* "0x201A34: grow (hot.c:15)", "0x4C2A: ??? (in /lib/libc.so.7)",
	 * "in 3 places, all below massif's threshold (1.00%)" */
	if (starts(s, n, "in ") && mfind(s, n, "threshold", 9))
		return frame_of(p, fn_ofz(p, "(below massif's threshold)"), NONE, 0, 0, NONE);
	if (!starts(s, n, "0x"))
		return NONE;
	const char *c = memchr(s, ':', n);
	if (!c)
		return NONE;
	u64 addr = strtoull(s, NULL, 16);
	const char *r = c + 1;
	size_t rn = n - (size_t)(r - s);
	trim(&r, &rn);
	/* the location is the last " (...)" */
	const char *lp = NULL;
	if (rn && r[rn - 1] == ')')
		for (size_t j = rn; j-- > 1;)
			if (r[j] == '(' && r[j - 1] == ' ') {
				lp = r + j;
				break;
			}
	size_t name_n = lp ? (size_t)(lp - 1 - r) : rn;
	u32 file = NONE, line = 0;
	char nm[512];
	if (lp) {
		const char *loc = lp + 1;
		size_t ln = (size_t)(r + rn - 1 - loc);
		if (starts(loc, ln, "in ")) {
			if (name_n == 3 && !memcmp(r, "???", 3)) {
				/* unknown function: name it after its object */
				char ob[400];
				snprintf(ob, sizeof ob, "%.*s", (int)(ln - 3), loc + 3);
				snprintf(nm, sizeof nm, "??? [%s]", base_name(ob));
				return frame_of(p, fn_ofz(p, nm), NONE, 0, 0, NONE);
			}
		} else {
			const char *col = NULL;
			for (size_t j = ln; j-- > 0;)
				if (loc[j] == ':') {
					col = loc + j;
					break;
				}
			size_t used = 0;
			if (col)
				line = (u32)num_at(col + 1, (size_t)(loc + ln - col - 1), &used);
			if (col && used) {
				file = file_of(p, loc, (size_t)(col - loc));
			} else
				line = 0;
		}
	}
	(void)addr;
	return frame_of(p, fn_of(p, r, name_n), file, line, 0, NONE);
}

int prof_parse_massif(Prof *p, const char *s, size_t n)
{
	prof_begin(p, PK_MASSIF);
	set_met(p, 0, "Heap bytes", PU_BYTES);
	u32 snap = NONE;
	/* open tree nodes by depth */
	u32 *tfr = NULL, tcap = 0, no = 0;
	u64 *tby = NULL, *tch = NULL;
	u32 *path = NULL, pcap = 0;
	double v[4] = {0};
#define POP_NODE()                                                                                                     \
	do {                                                                                                               \
		u32 j = --no;                                                                                                  \
		u64 self = tby[j] > tch[j] ? tby[j] - tch[j] : 0;                                                              \
		if (j > 0 && self > 0) {                                                                                       \
			u32 k = 0;                                                                                                 \
			VX_GROW(path, pcap, j);                                                                                    \
			for (u32 q = j; q >= 1; q--)                                                                               \
				if (tfr[q] != NONE)                                                                                    \
					path[k++] = tfr[q];                                                                                \
			v[0] = (double)self;                                                                                       \
			stack_add(p, path, k, snap, v);                                                                            \
		}                                                                                                              \
		if (j > 0)                                                                                                     \
			tch[j - 1] += tby[j];                                                                                      \
	} while (0)
	FOR_LINES(s, n, l, len)
	{
		const char *t = l;
		size_t tn = len;
		while (tn && t[tn - 1] == '\r')
			tn--;
		if (starts(t, tn, "desc:")) {
			const char *d = t + 5;
			size_t dn = tn - 5;
			trim(&d, &dn);
			p->title = str_add(p, d, dn);
		} else if (starts(t, tn, "cmd:")) {
			const char *d = t + 4;
			size_t dn = tn - 4;
			trim(&d, &dn);
			p->cmd = str_add(p, d, dn);
		} else if (starts(t, tn, "time_unit:")) {
			const char *d = t + 10;
			size_t dn = tn - 10;
			trim(&d, &dn);
			p->time_unit = str_add(p, d, dn);
		} else if (starts(t, tn, "snapshot=")) {
			while (no)
				POP_NODE();
			SoaCol c[] = {SN_COLS(p)};
			soa_grow(c, 5, &p->snap_cap, p->nsnap + 1, p->nsnap);
			snap = p->nsnap++;
			p->sn_time[snap] = p->sn_heap[snap] = p->sn_extra[snap] = p->sn_stacks[snap] = 0;
			p->sn_tree[snap] = PSN_EMPTY;
		} else if (snap != NONE && starts(t, tn, "time=")) {
			p->sn_time[snap] = kv_num(t, tn, "time=");
		} else if (snap != NONE && starts(t, tn, "mem_heap_B=")) {
			p->sn_heap[snap] = kv_num(t, tn, "mem_heap_B=");
		} else if (snap != NONE && starts(t, tn, "mem_heap_extra_B=")) {
			p->sn_extra[snap] = kv_num(t, tn, "mem_heap_extra_B=");
		} else if (snap != NONE && starts(t, tn, "mem_stacks_B=")) {
			p->sn_stacks[snap] = kv_num(t, tn, "mem_stacks_B=");
		} else if (snap != NONE && starts(t, tn, "heap_tree=")) {
			if (starts(t, tn, "heap_tree=peak")) {
				p->sn_tree[snap] = PSN_PEAK;
				p->peak_snap = snap;
			} else if (starts(t, tn, "heap_tree=detailed"))
				p->sn_tree[snap] = PSN_DETAILED;
		} else if (snap != NONE && tn) {
			/* tree node: depth = leading spaces, "nK: BYTES desc" */
			u32 depth = 0;
			while (depth < tn && t[depth] == ' ')
				depth++;
			if (depth >= tn || t[depth] != 'n')
				continue;
			size_t used;
			num_at(t + depth + 1, tn - depth - 1, &used);
			const char *q = t + depth + 1 + used;
			size_t qn = tn - depth - 1 - used;
			if (!used || !qn || *q != ':')
				continue;
			q++, qn--;
			trim(&q, &qn);
			u64 bytes = num_at(q, qn, &used);
			q += used, qn -= used;
			trim(&q, &qn);
			if (depth > no)
				depth = no; /* malformed indentation: attach to the deepest open node */
			while (no > depth)
				POP_NODE();
			if (no + 1 > tcap) {
				u32 oc = tcap;
				VX_GROW(tfr, tcap, no + 1);
				tby = vx_realloc(tby, tcap * sizeof(u64));
				tch = vx_realloc(tch, tcap * sizeof(u64));
				(void)oc;
			}
			tfr[no] = depth == 0 ? NONE : massif_frame(p, q, qn);
			tby[no] = bytes;
			tch[no] = 0;
			no++;
		}
	}
	while (no)
		POP_NODE();
#undef POP_NODE
	free(tfr);
	free(tby);
	free(tch);
	free(path);
	if (p->peak_snap == NONE) {
		u64 best = 0;
		for (u32 i = 0; i < p->nsnap; i++)
			if (p->sn_tree[i] != PSN_EMPTY && p->sn_heap[i] + p->sn_extra[i] >= best) {
				best = p->sn_heap[i] + p->sn_extra[i];
				p->peak_snap = i;
			}
	}
	if (!p->nsnap) {
		snprintf(p->err, sizeof p->err, "No snapshots in this massif file");
		return -1;
	}
	return 0;
}

/* ---- jemalloc heap profiles ------------------------------------------------------------ */

/* jemalloc samples one allocation per sample_period bytes on average;
 * scale a (count, bytes) pair back up the way jeprof does. */
static void unbias(u64 period, double *cnt, double *bytes)
{
	if (period <= 1 || *cnt <= 0)
		return;
	double ratio = (*bytes / *cnt) / (double)period;
	double scale = 1.0 / (1.0 - exp(-ratio));
	*cnt *= scale;
	*bytes *= scale;
}

/* "a: b [c: d]" */
static bool four_nums(const char *s, size_t n, double out[4])
{
	u32 k = 0;
	size_t i = 0;
	while (i < n && k < 4) {
		while (i < n && (s[i] < '0' || s[i] > '9'))
			if (s[i++] == '@')
				return k == 4;
		if (i >= n)
			break;
		size_t used;
		out[k++] = (double)num_at(s + i, n - i, &used);
		i += used;
	}
	return k == 4;
}

static void jeprof_stack(Prof *p, const char *s, size_t n, const double raw[4], u32 **fr, u32 *frcap)
{
	/* "@ 0x1 0x2 ..." leaf first */
	u32 k = 0;
	size_t i = 0;
	while (i < n) {
		while (i < n && (s[i] == ' ' || s[i] == '@' || s[i] == '\t'))
			i++;
		if (i >= n)
			break;
		char *end;
		u64 a = strtoull(s + i, &end, 16);
		if (end == s + i)
			break;
		i = (size_t)(end - s);
		if (k)
			a -= 1; /* return addresses: the call instruction is before them */
		char nm[24];
		snprintf(nm, sizeof nm, "0x%llx", (unsigned long long)a);
		VX_GROW(*fr, *frcap, k + 1);
		(*fr)[k++] = frame_of(p, fn_ofz(p, nm), NONE, 0, a, NONE);
	}
	for (u32 a = 0, b = k ? k - 1 : 0; a < b; a++, b--) {
		u32 t = (*fr)[a];
		(*fr)[a] = (*fr)[b];
		(*fr)[b] = t;
	}
	double v[4] = {raw[1], raw[0], raw[3], raw[2]};
	unbias(p->sample_period, &v[1], &v[0]);
	unbias(p->sample_period, &v[3], &v[2]);
	if (v[0] > 0 || v[2] > 0)
		stack_add(p, *fr, k, NONE, v);
}

int prof_parse_jeprof(Prof *p, const char *s, size_t n)
{
	prof_begin(p, PK_JEPROF);
	set_met(p, 0, "In-use bytes", PU_BYTES);
	set_met(p, 1, "In-use objects", PU_COUNT);
	set_met(p, 2, "Allocated bytes", PU_BYTES);
	set_met(p, 3, "Allocated objects", PU_COUNT);
	bool v2 = false, maps = false, any_accum = false;
	u32 *fr = NULL, frcap = 0;
	const char *pend = NULL; /* heap_v2: the "@ ..." line waiting for its t*: line */
	size_t pend_n = 0;
	FOR_LINES(s, n, l, len)
	{
		const char *t = l;
		size_t tn = len;
		while (tn && (t[tn - 1] == '\r'))
			tn--;
		if (maps) {
			/* start-end perms offset dev inode path */
			char *e1;
			u64 lo = strtoull(t, &e1, 16);
			if (*e1 != '-')
				continue;
			u64 hi = strtoull(e1 + 1, &e1, 16);
			while (*e1 == ' ')
				e1++;
			const char *perm = e1;
			if ((size_t)(perm - t) + 4 > tn || perm[2] != 'x')
				continue;
			char *e2;
			u64 off = strtoull(perm + 5, &e2, 16);
			const char *path = memchr(e2, '/', tn - (size_t)(e2 - t));
			if (!path)
				continue;
			SoaCol c[] = {MOD_COLS(p)};
			soa_grow(c, 4, &p->mod_cap, p->nmod + 1, p->nmod);
			u32 m = p->nmod++;
			p->mod_lo[m] = lo;
			p->mod_hi[m] = hi;
			p->mod_off[m] = off;
			p->mod_path[m] = str_add(p, path, tn - (size_t)(path - t));
			if (!p->cmd && !strstr(prof_str(p, p->mod_path[m]), ".so"))
				p->cmd = p->mod_path[m];
			continue;
		}
		if (starts(t, tn, "MAPPED_LIBRARIES:")) {
			maps = true;
			continue;
		}
		if (starts(t, tn, "heap_v2/")) {
			v2 = true;
			p->sample_period = num_at(t + 8, tn - 8, NULL);
			p->title = str_add(p, t, tn);
			continue;
		}
		if (starts(t, tn, "heap profile:")) {
			const char *at = mfind(t, tn, "heap_v2/", 8);
			if (at)
				p->sample_period = num_at(at + 8, tn - (size_t)(at + 8 - t), NULL);
			p->title = str_add(p, t, tn);
			continue;
		}
		const char *u = t;
		size_t un = tn;
		trim(&u, &un);
		if (v2) {
			if (starts(u, un, "@")) {
				pend = u;
				pend_n = un;
			} else if (pend && starts(u, un, "t*:")) {
				double raw[4];
				if (four_nums(u + 3, un - 3, raw)) {
					any_accum |= raw[2] > 0 || raw[3] > 0;
					jeprof_stack(p, pend, pend_n, raw, &fr, &frcap);
				}
				pend = NULL;
			}
		} else {
			/* "1: 512 [1: 512] @ 0x... 0x..." */
			const char *at = memchr(u, '@', un);
			double raw[4];
			if (at && un && u[0] >= '0' && u[0] <= '9' && four_nums(u, (size_t)(at - u), raw)) {
				any_accum |= raw[2] > 0 || raw[3] > 0;
				jeprof_stack(p, at, un - (size_t)(at - u), raw, &fr, &frcap);
			}
		}
	}
	free(fr);
	if (!any_accum)
		p->nmet = 2;
	if (!p->nst && !p->title) {
		snprintf(p->err, sizeof p->err, "Not a jemalloc heap profile");
		return -1;
	}
	return 0;
}

static bool alloc_internal(const char *n)
{
	static const char *const exact[] = {"malloc", "calloc", "realloc", "free", "valloc", "pvalloc", "memalign",
	                                    "posix_memalign", "aligned_alloc", "reallocf", "reallocarray", "mallocx",
	                                    "rallocx", "xallocx", "nallocx", "strdup", "strndup", "operator new(unsigned long)",
	                                    "operator new[](unsigned long)"};
	static const char *const pre[] = {"je_", "prof_", "imalloc", "ialloc", "iralloc", "arena_", "tcache_", "__malloc",
	                                  "__libc_", "_malloc_", "operator new", "_Znw", "_Zna", "tc_", "malloc_", "isalloc",
	                                  "sdallocx", "emap_", "pa_", "pai_", "large_"};
	for (size_t i = 0; i < VX_COUNT(exact); i++)
		if (!strcmp(n, exact[i]))
			return true;
	for (size_t i = 0; i < VX_COUNT(pre); i++)
		if (!strncmp(n, pre[i], strlen(pre[i])))
			return true;
	return false;
}

/* file offset -> the object's virtual address (its section headers) */
static u64 off_to_vaddr(const BinImage *img, u64 off)
{
	for (u32 s = 0; s < img->ns; s++)
		if ((img->s_flags[s] & BS_CODE) && off >= img->s_off[s] && off < img->s_off[s] + img->s_size[s])
			return off - img->s_off[s] + img->s_addr[s];
	return off;
}

void prof_jeprof_symbolize(Prof *p)
{
	ProfLoc *loc = vx_alloc((p->nfr + 1) * sizeof *loc);
	u64 *va = vx_alloc((p->nfr + 1) * sizeof(u64));
	u32 *ids = vx_alloc((p->nfr + 1) * sizeof(u32));
	for (u32 m = 0; m < p->nmod; m++) {
		u32 k = 0;
		for (u32 f = 0; f < p->nfr; f++)
			if (p->fr_addr[f] >= p->mod_lo[m] && p->fr_addr[f] < p->mod_hi[m] && p->fr_mod[f] == NONE)
				ids[k++] = f;
		if (!k)
			continue;
		const char *path = prof_str(p, p->mod_path[m]);
		size_t dn;
		char *data = vx_read_file(path, &dn);
		BinImage img = {0};
		bool ok = data && bin_parse((const u8 *)data, dn, &img) == 0;
		for (u32 i = 0; i < k; i++) {
			u64 fo = p->fr_addr[ids[i]] - p->mod_lo[m] + p->mod_off[m];
			va[i] = ok ? off_to_vaddr(&img, fo) : fo;
		}
		addr2line_batch(p, path, va, k, loc);
		for (u32 i = 0; i < k; i++) {
			u32 f = ids[i];
			char nm[600];
			const char *name = NULL;
			if (loc[i].alt)
				name = prof_str(p, loc[i].alt);
			else if (ok) {
				u64 d;
				u32 y = bin_symbol_at(&img, va[i], &d);
				if (y != UINT32_MAX && (d < img.y_size[y] || !img.y_size[y]))
					name = bin_str(&img, img.y_name[y]);
			}
			if (!name) {
				/* unreadable object: keep the run-time address */
				snprintf(nm, sizeof nm, "0x%llx [%s]", (unsigned long long)(ok ? va[i] : p->fr_addr[f]),
				         base_name(path));
				name = nm;
				p->nunresolved++;
			}
			u32 fn = fn_ofz(p, name);
			u32 file = loc[i].file ? file_of(p, prof_str(p, loc[i].file), strlen(prof_str(p, loc[i].file))) : NONE;
			/* rewrite the frame in place (addresses are unique frames) */
			p->fr_fn[f] = fn;
			p->fr_file[f] = file;
			p->fr_line[f] = loc[i].line;
			p->fr_mod[f] = m;
			if (file != NONE && loc[i].line) {
				if (p->fn_file[fn] == NONE)
					p->fn_file[fn] = file;
				if (p->fn_file[fn] == file && (!p->fn_line[fn] || loc[i].line < p->fn_line[fn]))
					p->fn_line[fn] = loc[i].line;
			}
		}
		if (ok)
			bin_free(&img);
		free(data);
	}
	for (u32 f = 0; f < p->nfr; f++)
		if (p->fr_mod[f] == NONE)
			p->nunresolved++;
	/* drop the allocator's own frames at the leaf end */
	for (u32 s = 0; s < p->nst; s++)
		while (p->st_len[s] > 1 &&
		       alloc_internal(prof_str(p, p->fn_name[p->fr_fn[p->sfr[p->st_off[s] + p->st_len[s] - 1]]])))
			p->st_len[s]--;
	/* the frames' hash keys changed: rebuild the index */
	ix_free(&p->fr_ix);
	for (u32 f = 0; f < p->nfr; f++) {
		FrKey k = {p->fr_fn[f], p->fr_file[f], p->fr_line[f], p->fr_mod[f], p->fr_addr[f]};
		ix_add(&p->fr_ix, hbytes(&k, sizeof k, 0), f);
	}
	free(loc);
	free(va);
	free(ids);
}

/* ---- gcov ---------------------------------------------------------------------------- */

static u64 gcov_count(const char *s, size_t n, u8 *flags)
{
	/* "400200", "400200*", "1.2k", "#####", "=====", "$$$$$", "-" */
	*flags = 0;
	if (n && s[n - 1] == '*') {
		*flags |= PL_PARTIAL;
		n--;
	}
	if (!n)
		return 0;
	if (s[0] == '-') {
		*flags = 0;
		return 0;
	}
	*flags |= PL_CODE;
	if (s[0] == '#' || s[0] == '=' || s[0] == '$') {
		*flags |= PL_UNCOVERED;
		return 0;
	}
	char b[48];
	snprintf(b, sizeof b, "%.*s", (int)VX_MIN(n, sizeof b - 1), s);
	char *e;
	double v = strtod(b, &e);
	switch (*e) {
	case 'k': v *= 1e3; break;
	case 'M': v *= 1e6; break;
	case 'G': v *= 1e9; break;
	case 'T': v *= 1e12; break;
	case 'P': v *= 1e15; break;
	}
	return (u64)(v + 0.5);
}

int prof_parse_gcov(Prof *p, const char *s, size_t n)
{
	prof_begin(p, PK_GCOV);
	set_met(p, 0, "Executions", PU_COUNT);
	u32 file = NONE, last_line = 0;
	u32 pend_fn = NONE;
	FOR_LINES(s, n, l, len)
	{
		const char *t = l;
		size_t tn = len;
		while (tn && t[tn - 1] == '\r')
			tn--;
		if (starts(t, tn, "function ")) {
			/* function NAME called N returned X% blocks executed Y% */
			const char *nm = t + 9;
			const char *sp = mfind(nm, tn - 9, " called ", 8);
			size_t nn = sp ? (size_t)(sp - nm) : tn - 9;
			pend_fn = fn_of(p, nm, nn);
			if (sp)
				p->fn_calls[pend_fn] += num_at(sp + 8, tn - (size_t)(sp + 8 - t), NULL);
			const char *bl = mfind(t, tn, "blocks executed ", 16);
			if (bl)
				p->fn_blocks[pend_fn] = (u8)VX_MIN(num_at(bl + 16, tn - (size_t)(bl + 16 - t), NULL), 100);
			continue;
		}
		if (starts(t, tn, "branch ") && p->ngl && p->gl_file[p->ngl - 1] == file) {
			u32 g = p->ngl - 1;
			if (p->gl_br_tot[g] < 0xFFFF)
				p->gl_br_tot[g]++;
			const char *tk = mfind(t, tn, "taken ", 6);
			if (tk) {
				size_t used;
				u64 c = num_at(tk + 6, tn - (size_t)(tk + 6 - t), &used);
				if (c > 0 && p->gl_br_hit[g] < 0xFFFF)
					p->gl_br_hit[g]++;
			}
			continue;
		}
		/* "  count:  line:text" */
		const char *c1 = memchr(t, ':', tn);
		if (!c1)
			continue;
		const char *cnt = t;
		size_t cn = (size_t)(c1 - t);
		trim(&cnt, &cn);
		const char *ln = c1 + 1;
		size_t rest = tn - (size_t)(ln - t);
		while (rest && *ln == ' ')
			ln++, rest--;
		size_t used;
		u32 line = (u32)num_at(ln, rest, &used);
		if (!used || used >= rest || ln[used] != ':' || !cn)
			continue;
		const char *text = ln + used + 1;
		size_t textn = rest - used - 1;
		if (line == 0) {
			if (starts(text, textn, "Source:")) {
				file = file_of(p, text + 7, textn - 7);
				if (!p->title)
					p->title = str_add(p, text + 7, textn - 7);
				last_line = 0;
			}
			continue;
		}
		if (file == NONE || line <= last_line)
			continue; /* template instantiation blocks repeat lines */
		last_line = line;
		u8 fl;
		u64 c = gcov_count(cnt, cn, &fl);
		if (pend_fn != NONE) {
			if (p->fn_file[pend_fn] == NONE) {
				p->fn_file[pend_fn] = file;
				p->fn_line[pend_fn] = line;
			}
			pend_fn = NONE;
		}
		if (!(fl & PL_CODE))
			continue;
		SoaCol gc[] = {GL_COLS(p)};
		soa_grow(gc, 6, &p->gl_cap, p->ngl + 1, p->ngl);
		u32 g = p->ngl++;
		p->gl_file[g] = file;
		p->gl_line[g] = line;
		p->gl_count[g] = c;
		p->gl_flags[g] = fl;
		p->gl_br_tot[g] = p->gl_br_hit[g] = 0;
	}
	if (!p->nfile) {
		snprintf(p->err, sizeof p->err, "No \"Source:\" header: not gcov output");
		return -1;
	}
	return 0;
}

/* gcov -t on a .gcda: the annotated sources as text (NULL on failure) */
static char *gcov_text(const char *path, size_t *n)
{
	char dir[4096];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	else
		snprintf(dir, sizeof dir, ".");
	const char *env = getenv("VXIDE_GCOV");
	/* gcov from the compiler that wrote the file is best; llvm-cov reads both */
	const char *tools[][2] = {{env && *env ? env : "gcov", NULL}, {"gcov", NULL},   {"llvm-cov", "gcov"},
	                          {"gcov14", NULL},                    {"gcov13", NULL}, {"gcov15", NULL}};
	for (size_t t = 0; t < VX_COUNT(tools); t++) {
		char *argv[12];
		int a = 0;
		argv[a++] = (char *)tools[t][0];
		if (tools[t][1])
			argv[a++] = (char *)tools[t][1];
		argv[a++] = "-b";
		argv[a++] = "-c";
		argv[a++] = "-t";
		argv[a++] = "-o";
		argv[a++] = dir;
		argv[a++] = (char *)path;
		argv[a] = NULL;
		char *out = run_capture_in(dir, argv, n, NULL);
		if (out && mfind(out, *n, ":Source:", 8))
			return out;
		free(out);
	}
	return NULL;
}

static int load_gcda(Prof *p, const char *path)
{
	size_t n;
	char *out = gcov_text(path, &n);
	if (out) {
		int rc = prof_parse_gcov(p, out, n);
		free(out);
		return rc;
	}
	prof_begin(p, PK_GCOV);
	snprintf(p->err, sizeof p->err, "gcov could not read %s (is its .gcno next to it?)", base_name(path));
	return -1;
}

/* ---- gmon ------------------------------------------------------------------------------ */

static u64 rd(const u8 *d, int sz)
{
	if (sz == 8) {
		u64 v;
		memcpy(&v, d, 8);
		return v;
	}
	if (sz == 4) {
		u32 v;
		memcpy(&v, d, 4);
		return v;
	}
	if (sz == 2) {
		u16 v;
		memcpy(&v, d, 2);
		return v;
	}
	return d[0];
}

void gmon_free(Gmon *g)
{
	free(g->bins);
	free(g->arc_from);
	free(g->arc_self);
	free(g->arc_count);
	memset(g, 0, sizeof *g);
}

static void gmon_arc(Gmon *g, u32 *cap, u64 from, u64 self, u64 count)
{
	if (g->narc + 1 > *cap) {
		*cap = *cap ? *cap * 2 : 256;
		g->arc_from = vx_realloc(g->arc_from, *cap * sizeof(u64));
		g->arc_self = vx_realloc(g->arc_self, *cap * sizeof(u64));
		g->arc_count = vx_realloc(g->arc_count, *cap * sizeof(u64));
	}
	g->arc_from[g->narc] = from;
	g->arc_self[g->narc] = self;
	g->arc_count[g->narc] = count;
	g->narc++;
}

int gmon_parse(const u8 *d, size_t n, int ps, Gmon *g)
{
	memset(g, 0, sizeof *g);
	u32 acap = 0;
	if (n >= 20 && !memcmp(d, "gmon", 4)) {
		/* GNU: header, then tagged records */
		g->gnu = true;
		size_t pos = 20;
		while (pos < n) {
			u8 tag = d[pos++];
			if (tag == 0) { /* GMON_TAG_TIME_HIST */
				size_t hs = (size_t)(2 * ps + 4 + 4 + 15 + 1);
				if (pos + hs > n)
					break;
				u64 lo = rd(d + pos, ps), hi = rd(d + pos + ps, ps);
				u32 nb = (u32)rd(d + pos + 2 * ps, 4), rate = (u32)rd(d + pos + 2 * ps + 4, 4);
				pos += hs;
				if (pos + (size_t)nb * 2 > n)
					break;
				if (!g->bins) {
					g->lowpc = lo;
					g->highpc = hi;
					g->nbins = nb;
					g->rate = rate;
					g->bins = vx_alloc(((size_t)nb + 1) * sizeof(u64));
				}
				if (lo == g->lowpc && hi == g->highpc && nb == g->nbins)
					for (u32 i = 0; i < nb; i++)
						g->bins[i] += rd(d + pos + (size_t)i * 2, 2);
				pos += (size_t)nb * 2;
			} else if (tag == 1) { /* GMON_TAG_CG_ARC */
				if (pos + (size_t)(2 * ps + 4) > n)
					break;
				gmon_arc(g, &acap, rd(d + pos, ps), rd(d + pos + ps, ps), rd(d + pos + 2 * ps, 4));
				pos += (size_t)(2 * ps + 4);
			} else
				break; /* basic-block counts: not used */
		}
	} else {
		/* BSD: struct gmonhdr { u_long lpc, hpc; int ncnt, version, profrate,
		 * histcounter_type, spare[2]; } then the histogram, then rawarcs */
		size_t hdr = (size_t)(2 * ps + 24);
		if (n < hdr) {
			snprintf(g->err, sizeof g->err, "gmon file too short");
			return -1;
		}
		u64 lo = rd(d, ps), hi = rd(d + ps, ps);
		u32 ncnt = (u32)rd(d + 2 * ps, 4), ver = (u32)rd(d + 2 * ps + 4, 4);
		u32 rate = (u32)rd(d + 2 * ps + 8, 4);
		i32 ht = (i32)rd(d + 2 * ps + 12, 4);
		if (ver != 0x00051879u && (ncnt > n || hi <= lo)) {
			snprintf(g->err, sizeof g->err, "Not a gmon file (no gmon header)");
			return -1;
		}
		int csz = ht == 0 ? 2 : abs(ht) / 8;
		if (csz != 1 && csz != 2 && csz != 4 && csz != 8)
			csz = 2;
		if (ncnt < hdr || ncnt > n)
			ncnt = (u32)VX_MIN((size_t)ncnt, n);
		g->lowpc = lo;
		g->highpc = hi;
		g->rate = rate;
		g->nbins = ncnt > hdr ? (u32)((ncnt - hdr) / (u32)csz) : 0;
		g->bins = vx_alloc(((size_t)g->nbins + 1) * sizeof(u64));
		for (u32 i = 0; i < g->nbins; i++)
			g->bins[i] = rd(d + hdr + (size_t)i * (size_t)csz, csz);
		size_t asz = (size_t)ps * 3;
		for (size_t pos = ncnt; pos + asz <= n; pos += asz)
			gmon_arc(g, &acap, rd(d + pos, ps), rd(d + pos + ps, ps), rd(d + pos + 2 * ps, ps));
	}
	if (!g->rate)
		g->rate = 100;
	return 0;
}

/* gprof's propagation, as stacks: a function's self samples are emitted in
 * every calling context, weighted by the share of its calls coming from
 * that context. */
typedef struct {
	Prof *p;
	double *fs;    /* samples per frame */
	u32 *fn_first; /* self frames per function (linked through fr_nextself) */
	u32 *fr_next;
	u32 *out_first, *out_next; /* outgoing arcs per function */
	double *ncalls;            /* incoming calls (no self-recursion) */
	double *T;                 /* propagated time, for pruning */
	double *emitted;
	u8 *onpath;
	u32 *path;
	u32 depth;
	double eps;
} Synth;

static double synth_T(Synth *S, u32 f, u8 *state)
{
	if (state[f] == 2)
		return S->T[f];
	if (state[f] == 1)
		return 0; /* a cycle */
	state[f] = 1;
	Prof *p = S->p;
	double t = 0;
	for (u32 fr = S->fn_first[f]; fr != NONE; fr = S->fr_next[fr])
		t += S->fs[fr];
	for (u32 a = S->out_first[f]; a != NONE; a = S->out_next[a]) {
		u32 g = p->arc_to[a];
		if (g != f && S->ncalls[g] > 0)
			t += synth_T(S, g, state) * (double)p->arc_count[a] / S->ncalls[g];
	}
	state[f] = 2;
	S->T[f] = t;
	return t;
}

static void synth_emit(Synth *S, u32 f, double q)
{
	Prof *p = S->p;
	double v[4] = {0};
	for (u32 fr = S->fn_first[f]; fr != NONE; fr = S->fr_next[fr]) {
		v[0] = q * S->fs[fr];
		if (v[0] <= 0)
			continue;
		S->path[S->depth] = fr;
		stack_add(p, S->path, S->depth + 1, NONE, v);
		S->emitted[f] += v[0];
	}
	if (S->depth >= 62 || p->nst > 400000)
		return;
	S->onpath[f] = 1;
	for (u32 a = S->out_first[f]; a != NONE; a = S->out_next[a]) {
		u32 g = p->arc_to[a];
		if (S->onpath[g] || S->ncalls[g] <= 0)
			continue;
		double qq = q * (double)p->arc_count[a] / S->ncalls[g];
		if (qq * S->T[g] < S->eps)
			continue;
		S->path[S->depth++] = p->arc_from[a];
		synth_emit(S, g, qq);
		S->depth--;
	}
	S->onpath[f] = 0;
}

static void gprof_synthesize(Prof *p, double *fs)
{
	Synth S = {0};
	S.p = p;
	S.fs = fs;
	S.fn_first = vx_alloc((p->nfn + 1) * sizeof(u32));
	S.out_first = vx_alloc((p->nfn + 1) * sizeof(u32));
	S.fr_next = vx_alloc((p->nfr + 1) * sizeof(u32));
	S.out_next = vx_alloc((p->narc + 1) * sizeof(u32));
	S.ncalls = vx_alloc((p->nfn + 1) * sizeof(double));
	S.T = vx_alloc((p->nfn + 1) * sizeof(double));
	S.emitted = vx_alloc((p->nfn + 1) * sizeof(double));
	S.onpath = vx_alloc(p->nfn + 1);
	S.path = vx_alloc(64 * sizeof(u32));
	for (u32 f = 0; f < p->nfn; f++)
		S.fn_first[f] = S.out_first[f] = NONE;
	for (u32 fr = p->nfr; fr-- > 0;)
		if (fs[fr] > 0) {
			S.fr_next[fr] = S.fn_first[p->fr_fn[fr]];
			S.fn_first[p->fr_fn[fr]] = fr;
		}
	for (u32 a = p->narc; a-- > 0;) {
		u32 from = p->fr_fn[p->arc_from[a]];
		S.out_next[a] = S.out_first[from];
		S.out_first[from] = a;
		if (from != p->arc_to[a])
			S.ncalls[p->arc_to[a]] += (double)p->arc_count[a];
	}
	u8 *state = vx_alloc(p->nfn + 1);
	double total = 0;
	for (u32 f = 0; f < p->nfn; f++)
		synth_T(&S, f, state);
	for (u32 fr = 0; fr < p->nfr; fr++)
		total += fs[fr];
	S.eps = VX_MAX(total * 1e-7, 1e-9);
	u32 nfn0 = p->nfn;
	for (u32 f = 0; f < nfn0; f++)
		if (S.ncalls[f] <= 0 && S.T[f] > 0)
			synth_emit(&S, f, 1.0);
	/* what pruning, cycles or missing arcs left out goes in at the root */
	double v[4] = {0};
	for (u32 f = 0; f < nfn0; f++) {
		double self = 0;
		for (u32 fr = S.fn_first[f]; fr != NONE; fr = S.fr_next[fr])
			self += fs[fr];
		double rem = self - S.emitted[f];
		if (self <= 0 || rem <= self * 1e-6)
			continue;
		for (u32 fr = S.fn_first[f]; fr != NONE; fr = S.fr_next[fr]) {
			v[0] = fs[fr] * rem / self;
			if (v[0] > 0)
				stack_add(p, &fr, 1, NONE, v);
		}
	}
	free(state);
	free(S.fn_first);
	free(S.out_first);
	free(S.fr_next);
	free(S.out_next);
	free(S.ncalls);
	free(S.T);
	free(S.emitted);
	free(S.onpath);
	free(S.path);
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

static u32 addr_index(const u64 *u, u32 n, u64 a)
{
	u32 lo = 0, hi = n;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (u[mid] < a)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

void prof_from_gmon(Prof *p, const Gmon *g, ProfResolveFn resolve, void *ctx)
{
	set_met(p, 0, "Time", PU_SAMPLES);
	p->sec_per_sample = 1.0 / (double)(g->rate ? g->rate : 100);
	/* every address we need: histogram bins, call sites, callees */
	u32 na = 0;
	u64 *ad = vx_alloc(((size_t)g->nbins + 2 * (size_t)g->narc + 1) * sizeof(u64));
	double span = (double)(g->highpc - g->lowpc);
	for (u32 i = 0; i < g->nbins; i++)
		if (g->bins[i]) {
			ad[na++] = g->lowpc + (u64)((double)i * span / g->nbins);
			p->hist_samples += g->bins[i];
		}
	for (u32 a = 0; a < g->narc; a++) {
		ad[na++] = g->arc_from[a] ? g->arc_from[a] - 1 : 0;
		ad[na++] = g->arc_self[a];
	}
	qsort(ad, na, sizeof(u64), cmp_u64);
	u32 nu = 0;
	for (u32 i = 0; i < na; i++)
		if (!nu || ad[nu - 1] != ad[i])
			ad[nu++] = ad[i];
	ProfLoc *loc = vx_alloc((nu + 1) * sizeof *loc);
	resolve(ctx, p, ad, nu, loc);
	/* the functions' own first lines and demangled names */
	u64 *fst = vx_alloc((nu + 1) * sizeof(u64));
	u32 nf = 0;
	for (u32 i = 0; i < nu; i++)
		if (loc[i].fstart != UINT64_MAX)
			fst[nf++] = loc[i].fstart;
	qsort(fst, nf, sizeof(u64), cmp_u64);
	u32 nfu = 0;
	for (u32 i = 0; i < nf; i++)
		if (!nfu || fst[nfu - 1] != fst[i])
			fst[nfu++] = fst[i];
	ProfLoc *floc = vx_alloc((nfu + 1) * sizeof *floc);
	if (nfu)
		resolve(ctx, p, fst, nfu, floc);
	u32 *afn = vx_alloc((nu + 1) * sizeof(u32));
	for (u32 i = 0; i < nu; i++) {
		const char *name = prof_str(p, loc[i].name);
		u32 fi = loc[i].fstart != UINT64_MAX ? addr_index(fst, nfu, loc[i].fstart) : NONE;
		if (fi != NONE && fi < nfu && floc[fi].alt && !strncmp(name, "_Z", 2))
			name = prof_str(p, floc[fi].alt);
		if (!*name)
			name = "(unknown)";
		u32 fn = fn_ofz(p, name);
		afn[i] = fn;
		if (fi != NONE && fi < nfu && floc[fi].file && floc[fi].line) {
			const char *fp = prof_str(p, floc[fi].file);
			p->fn_file[fn] = file_of(p, fp, strlen(fp));
			p->fn_line[fn] = floc[fi].line;
		}
	}
	/* frames for the samples */
	u32 cap = 0;
	double *fs = NULL;
	for (u32 i = 0; i < g->nbins; i++) {
		if (!g->bins[i])
			continue;
		u64 a = g->lowpc + (u64)((double)i * span / g->nbins);
		u32 k = addr_index(ad, nu, a);
		const char *fp = loc[k].file ? prof_str(p, loc[k].file) : NULL;
		u32 file = fp ? file_of(p, fp, strlen(fp)) : NONE;
		u32 fr = frame_of(p, afn[k], file, file != NONE ? loc[k].line : 0, 0, NONE);
		if (fr >= cap) {
			u32 oc = cap;
			cap = VX_MAX(fr + 1, cap * 2);
			fs = vx_realloc(fs, cap * sizeof(double));
			memset(fs + oc, 0, (cap - oc) * sizeof(double));
		}
		fs[fr] += (double)g->bins[i];
	}
	/* call arcs: from the call site's frame to the callee */
	for (u32 a = 0; a < g->narc; a++) {
		u32 kf = addr_index(ad, nu, g->arc_from[a] ? g->arc_from[a] - 1 : 0);
		u32 ks = addr_index(ad, nu, g->arc_self[a]);
		const char *fp = loc[kf].file ? prof_str(p, loc[kf].file) : NULL;
		u32 file = fp ? file_of(p, fp, strlen(fp)) : NONE;
		u32 fr = frame_of(p, afn[kf], file, file != NONE ? loc[kf].line : 0, 0, NONE);
		SoaCol c[] = {ARC_COLS(p)};
		soa_grow(c, 3, &p->arc_cap, p->narc + 1, p->narc);
		u32 r = p->narc++;
		p->arc_from[r] = fr;
		p->arc_to[r] = afn[ks];
		p->arc_count[r] = g->arc_count[a];
		p->fn_calls[afn[ks]] += g->arc_count[a];
		p->total_calls += g->arc_count[a];
	}
	if (p->nfr > cap) {
		fs = vx_realloc(fs, (p->nfr + 1) * sizeof(double));
		memset(fs + cap, 0, (p->nfr + 1 - cap) * sizeof(double));
	}
	gprof_synthesize(p, fs ? fs : (double[1]){0});
	free(fs);
	free(afn);
	free(floc);
	free(fst);
	free(loc);
	free(ad);
}

/* The executable that wrote a gmon file. */
static char *find_gmon_exe(const char *gmon)
{
	char dir[4096];
	snprintf(dir, sizeof dir, "%s", gmon);
	char *sl = strrchr(dir, '/');
	const char *base = sl ? sl + 1 : gmon;
	char stem[1024];
	snprintf(stem, sizeof stem, "%s", base);
	if (sl)
		*sl = 0;
	else
		snprintf(dir, sizeof dir, ".");
	size_t sn = strlen(stem);
	bool bsd = sn > 5 && !strcmp(stem + sn - 5, ".gmon");
	if (bsd)
		stem[sn - 5] = 0;
	DIR *dp = opendir(dir);
	if (!dp)
		return NULL;
	char best[4096] = "";
	int best_score = -1;
	time_t best_t = 0;
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.')
			continue;
		char p[4096 + 256];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
			continue;
		int fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		char mag[4];
		ssize_t r = read(fd, mag, 4);
		close(fd);
		if (r != 4 || memcmp(mag, "\x7f" "ELF", 4))
			continue;
		int score = !strcmp(e->d_name, stem) ? 2 : 0;
		if (score > best_score || (score == best_score && st.st_mtime > best_t)) {
			best_score = score;
			best_t = st.st_mtime;
			snprintf(best, sizeof best, "%s", p);
		}
	}
	closedir(dp);
	return best[0] ? vx_strdup(best) : NULL;
}

typedef struct {
	const BinImage *img;
	const char *exe;
	u64 bias;
} ExeRes;

static void exe_resolve(void *ctx, Prof *p, const u64 *addr, u32 n, ProfLoc *out)
{
	ExeRes *r = ctx;
	u64 *va = vx_alloc((n + 1) * sizeof(u64));
	for (u32 i = 0; i < n; i++)
		va[i] = addr[i] - r->bias;
	addr2line_batch(p, r->exe, va, n, out);
	for (u32 i = 0; i < n; i++) {
		u64 d;
		u32 y = bin_symbol_at(r->img, va[i], &d);
		/* skip labels (eprol, etext) to the function they sit in */
		while (y != UINT32_MAX && y > 0 && !r->img->y_func[y] && r->img->y_addr[y - 1] <= va[i])
			y--;
		if (y != UINT32_MAX && (r->img->y_func[y] || !r->img->y_size[y] || d < r->img->y_size[y])) {
			const char *nm = bin_str(r->img, r->img->y_name[y]);
			out[i].name = str_add(p, nm, strlen(nm));
			out[i].fstart = r->img->y_addr[y] + r->bias;
		} else {
			out[i].name = out[i].alt;
			out[i].fstart = UINT64_MAX;
		}
	}
	free(va);
}

static bool sym_addr(const BinImage *img, const char *name, u64 *a)
{
	for (u32 i = 0; i < img->nsym; i++)
		if (!strcmp(bin_str(img, img->y_name[i]), name)) {
			*a = img->y_addr[i];
			return true;
		}
	return false;
}

static int load_gmon(Prof *p, const char *path, const u8 *data, size_t n, const char *exe_in)
{
	prof_begin(p, PK_GPROF);
	char *exe = exe_in && *exe_in ? vx_strdup(exe_in) : find_gmon_exe(path);
	if (!exe) {
		snprintf(p->err, sizeof p->err,
		         "No executable next to %s: use \"Profile: Set Executable for gprof...\"", base_name(path));
		return -1;
	}
	p->cmd = str_add(p, exe, strlen(exe));
	size_t en;
	char *ed = vx_read_file(exe, &en);
	BinImage img = {0};
	if (!ed || bin_parse((const u8 *)ed, en, &img) != 0) {
		snprintf(p->err, sizeof p->err, "Cannot read the executable %s", exe);
		free(ed);
		free(exe);
		return -1;
	}
	Gmon g;
	if (gmon_parse(data, n, img.bits == 64 ? 8 : 4, &g) != 0) {
		snprintf(p->err, sizeof p->err, "%s", g.err);
		gmon_free(&g);
		bin_free(&img);
		free(ed);
		free(exe);
		return -1;
	}
	/* position-independent executables: the histogram holds run-time
	 * addresses; etext (highpc) or eprol (lowpc) gives the load bias */
	u64 bias = 0, sa;
	if (sym_addr(&img, "etext", &sa) || sym_addr(&img, "_etext", &sa) || sym_addr(&img, "__etext", &sa))
		bias = g.highpc - sa;
	else if (sym_addr(&img, "eprol", &sa))
		bias = g.lowpc - sa;
	else {
		bool inside = false;
		u64 lowest = UINT64_MAX;
		for (u32 s = 0; s < img.ns; s++)
			if (img.s_flags[s] & BS_CODE) {
				inside |= g.lowpc >= img.s_addr[s] && g.lowpc < img.s_addr[s] + img.s_size[s];
				lowest = VX_MIN(lowest, img.s_addr[s]);
			}
		if (!inside && lowest != UINT64_MAX && g.lowpc > lowest)
			bias = g.lowpc - lowest;
	}
	ExeRes r = {&img, exe, bias};
	p->title = str_add(p, base_name(exe), strlen(base_name(exe)));
	prof_from_gmon(p, &g, exe_resolve, &r);
	gmon_free(&g);
	bin_free(&img);
	free(ed);
	free(exe);
	if (!p->hist_samples && !p->narc) {
		snprintf(p->err, sizeof p->err, "The profile is empty (no samples, no calls)");
		return -1;
	}
	return 0;
}

/* ---- loading, freeing -------------------------------------------------------------------- */

int prof_load(Prof *p, int kind, const char *path, const u8 *data, size_t n, const char *exe)
{
	switch (kind) {
	case PK_MASSIF: return prof_parse_massif(p, (const char *)data, n);
	case PK_JEPROF: {
		int rc = prof_parse_jeprof(p, (const char *)data, n);
		prof_jeprof_symbolize(p);
		return rc;
	}
	case PK_GPROF: return load_gmon(p, path, data, n, exe);
	case PK_GCOV:
		if (path && ends_with(path, ".gcda"))
			return load_gcda(p, path);
		return prof_parse_gcov(p, (const char *)data, n);
	default:
		prof_begin(p, PK_NONE);
		snprintf(p->err, sizeof p->err, "Unknown profile format");
		return -1;
	}
}

char *prof_raw_report(int kind, const char *path, const char *exe, size_t *n)
{
	*n = 0;
	if (kind == PK_GPROF) {
		if (!exe)
			return NULL;
		char *argv[] = {"gprof", "-b", (char *)exe, (char *)path, NULL};
		char *r = run_capture(argv, n, NULL);
		if (r && *n)
			return r;
		free(r);
		return NULL;
	}
	if (kind == PK_GCOV && ends_with(path, ".gcda"))
		return gcov_text(path, n);
	return vx_read_file(path, n);
}

void prof_free(Prof *p)
{
	SoaCol a[] = {FILE_COLS(p)}, b[] = {FN_COLS(p)}, c[] = {FR_COLS(p)}, d[] = {ST_COLS(p)}, e[] = {SN_COLS(p)},
	       f[] = {ARC_COLS(p)}, g[] = {MOD_COLS(p)}, h[] = {GL_COLS(p)};
	soa_free(a, 1);
	soa_free(b, 5);
	soa_free(c, 5);
	soa_free(d, 7);
	soa_free(e, 5);
	soa_free(f, 3);
	soa_free(g, 4);
	soa_free(h, 6);
	free(p->sfr);
	ix_free(&p->file_ix);
	ix_free(&p->fn_ix);
	ix_free(&p->fr_ix);
	pool_free(&p->pool);
	memset(p, 0, sizeof *p);
}

/* ---- aggregation ---------------------------------------------------------------------------- */

static bool st_in(const Prof *p, u32 s, u32 snap) { return snap == NONE || p->st_snap[s] == snap; }

double prof_total(const Prof *p, u32 m, u32 snap)
{
	double t = 0;
	if (p->kind == PK_GCOV) {
		for (u32 g = 0; g < p->ngl; g++)
			t += (double)p->gl_count[g];
		return t;
	}
	for (u32 s = 0; s < p->nst; s++)
		if (st_in(p, s, snap))
			t += prof_sv(p, m, s);
	return t;
}

void prof_fn_sums(const Prof *p, u32 m, u32 snap, double *self, double *total)
{
	memset(self, 0, p->nfn * sizeof(double));
	memset(total, 0, p->nfn * sizeof(double));
	if (p->kind == PK_GCOV) {
		for (u32 f = 0; f < p->nfn; f++)
			self[f] = total[f] = (double)p->fn_calls[f];
		return;
	}
	u32 *stamp = vx_alloc((p->nfn + 1) * sizeof(u32));
	for (u32 s = 0; s < p->nst; s++) {
		if (!st_in(p, s, snap))
			continue;
		double v = prof_sv(p, m, s);
		const u32 *fr = p->sfr + p->st_off[s];
		u32 n = p->st_len[s];
		self[p->fr_fn[fr[n - 1]]] += v;
		for (u32 i = 0; i < n; i++) {
			u32 f = p->fr_fn[fr[i]];
			if (stamp[f] != s + 1) {
				stamp[f] = s + 1;
				total[f] += v;
			}
		}
	}
	free(stamp);
}

#define TREE_COLS(t)                                                                                                   \
	SOA((t)->fn), SOA((t)->parent), SOA((t)->child), SOA((t)->next), SOA((t)->depth), SOA((t)->val), SOA((t)->self)

void prof_tree_free(ProfTree *t)
{
	SoaCol c[] = {TREE_COLS(t)};
	soa_free(c, 7);
	memset(t, 0, sizeof *t);
}

static const ProfTree *sort_tree;
static int cmp_tree(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	const ProfTree *t = sort_tree;
	if (t->parent[x] != t->parent[y])
		return t->parent[x] < t->parent[y] ? -1 : 1;
	if (t->val[x] != t->val[y])
		return t->val[x] > t->val[y] ? -1 : 1;
	return x < y ? -1 : x > y;
}

static bool eq_node(const Prof *p, u32 id, const void *k)
{
	(void)p;
	const u32 *kk = k;
	const ProfTree *t = sort_tree;
	return t->parent[id] == kk[0] && t->fn[id] == kk[1];
}

void prof_tree(const Prof *p, u32 m, u32 snap, bool bottom_up, ProfTree *t)
{
	prof_tree_free(t);
	SoaCol c[] = {TREE_COLS(t)};
	soa_grow(c, 7, &t->cap, 64, 0);
	t->n = 1;
	t->fn[0] = NONE;
	t->parent[0] = NONE;
	t->child[0] = t->next[0] = NONE;
	t->depth[0] = 0;
	t->val[0] = t->self[0] = 0;
	ProfIx ix = {0};
	sort_tree = t;
	for (u32 s = 0; s < p->nst; s++) {
		if (!st_in(p, s, snap))
			continue;
		double v = prof_sv(p, m, s);
		if (v == 0)
			continue;
		const u32 *fr = p->sfr + p->st_off[s];
		u32 n = p->st_len[s], cur = 0;
		t->val[0] += v;
		for (u32 i = 0; i < n; i++) {
			u32 f = p->fr_fn[fr[bottom_up ? n - 1 - i : i]];
			u32 key[2] = {cur, f};
			u64 h = hbytes(key, sizeof key, 0);
			sort_tree = t;
			u32 nd = ix_find(&ix, h, p, eq_node, key);
			if (nd == NONE) {
				soa_grow(c, 7, &t->cap, t->n + 1, t->n);
				sort_tree = t;
				nd = t->n++;
				t->fn[nd] = f;
				t->parent[nd] = cur;
				t->child[nd] = t->next[nd] = NONE;
				t->depth[nd] = t->depth[cur] + 1;
				t->val[nd] = t->self[nd] = 0;
				if (t->depth[nd] > t->maxdepth)
					t->maxdepth = t->depth[nd];
				ix_add(&ix, h, nd);
			}
			t->val[nd] += v;
			cur = nd;
		}
		t->self[cur] += v;
	}
	ix_free(&ix);
	/* children lists, largest first */
	u32 *ord = vx_alloc(t->n * sizeof(u32));
	for (u32 i = 0; i < t->n; i++)
		ord[i] = i;
	sort_tree = t;
	qsort(ord + 1, t->n - 1, sizeof(u32), cmp_tree);
	for (u32 i = t->n; i-- > 1;) {
		u32 nd = ord[i], par = t->parent[nd];
		t->next[nd] = t->child[par];
		t->child[par] = nd;
	}
	free(ord);
	sort_tree = NULL;
}

#define LN_COLS(l)                                                                                                     \
	SOA((l)->file), SOA((l)->line), SOA((l)->fn), SOA((l)->self), SOA((l)->total), SOA((l)->flags), SOA((l)->br_tot),  \
	    SOA((l)->br_hit)

void prof_lines_free(ProfLines *l)
{
	SoaCol c[] = {LN_COLS(l)};
	soa_free(c, 8);
	memset(l, 0, sizeof *l);
}

static const ProfLines *ln_tab;
static bool eq_ln(const Prof *p, u32 id, const void *k)
{
	(void)p;
	const u32 *kk = k;
	return ln_tab->file[id] == kk[0] && ln_tab->line[id] == kk[1];
}

void prof_lines(const Prof *p, u32 m, u32 snap, ProfLines *out)
{
	prof_lines_free(out);
	SoaCol c[] = {LN_COLS(out)};
	if (p->kind == PK_GCOV) {
		soa_grow(c, 8, &out->cap, p->ngl + 1, 0);
		/* the function a line belongs to: the last one starting at or before it */
		for (u32 g = 0; g < p->ngl; g++) {
			u32 i = out->n++, best = NONE, bl = 0;
			for (u32 f = 0; f < p->nfn; f++)
				if (p->fn_file[f] == p->gl_file[g] && p->fn_line[f] <= p->gl_line[g] && p->fn_line[f] >= bl) {
					best = f;
					bl = p->fn_line[f];
				}
			out->file[i] = p->gl_file[g];
			out->line[i] = p->gl_line[g];
			out->fn[i] = best;
			out->self[i] = out->total[i] = (double)p->gl_count[g];
			out->flags[i] = p->gl_flags[g];
			out->br_tot[i] = p->gl_br_tot[g];
			out->br_hit[i] = p->gl_br_hit[g];
		}
		return;
	}
	ProfIx ix = {0};
	u32 *stamp = NULL, scap = 0;
	ln_tab = out;
	for (u32 s = 0; s < p->nst; s++) {
		if (!st_in(p, s, snap))
			continue;
		double v = prof_sv(p, m, s);
		if (v == 0)
			continue;
		const u32 *fr = p->sfr + p->st_off[s];
		u32 n = p->st_len[s];
		for (u32 i = 0; i < n; i++) {
			u32 f = fr[i];
			if (p->fr_file[f] == NONE || !p->fr_line[f])
				continue;
			u32 key[2] = {p->fr_file[f], p->fr_line[f]};
			u64 h = hbytes(key, sizeof key, 0);
			ln_tab = out;
			u32 id = ix_find(&ix, h, p, eq_ln, key);
			if (id == NONE) {
				soa_grow(c, 8, &out->cap, out->n + 1, out->n);
				ln_tab = out;
				id = out->n++;
				out->file[id] = key[0];
				out->line[id] = key[1];
				out->fn[id] = p->fr_fn[f];
				out->self[id] = out->total[id] = 0;
				out->flags[id] = PL_CODE;
				out->br_tot[id] = out->br_hit[id] = 0;
				ix_add(&ix, h, id);
				VX_GROW(stamp, scap, out->n);
				stamp[id] = 0;
			}
			if (i == n - 1)
				out->self[id] += v;
			if (stamp[id] != s + 1) {
				stamp[id] = s + 1;
				out->total[id] += v;
			}
		}
	}
	free(stamp);
	ix_free(&ix);
	ln_tab = NULL;
}

static double *nb_val;
static int cmp_nb(const void *a, const void *b)
{
	double x = nb_val[*(const u32 *)a], y = nb_val[*(const u32 *)b];
	return x > y ? -1 : x < y;
}

u32 prof_neighbors(const Prof *p, u32 m, u32 snap, u32 f, bool callers, u32 **fn_out, double **val_out)
{
	*fn_out = NULL;
	*val_out = NULL;
	if (f >= p->nfn)
		return 0;
	double *acc = vx_alloc((p->nfn + 1) * sizeof(double));
	for (u32 s = 0; s < p->nst; s++) {
		if (!st_in(p, s, snap))
			continue;
		const u32 *fr = p->sfr + p->st_off[s];
		u32 n = p->st_len[s];
		u32 i = 0;
		while (i < n && p->fr_fn[fr[i]] != f)
			i++;
		if (i == n)
			continue;
		if (callers && i > 0)
			acc[p->fr_fn[fr[i - 1]]] += prof_sv(p, m, s);
		if (!callers && i + 1 < n)
			acc[p->fr_fn[fr[i + 1]]] += prof_sv(p, m, s);
	}
	/* gprof: neighbours by call arcs even when no time flowed through them */
	if (p->kind == PK_GPROF)
		for (u32 a = 0; a < p->narc; a++) {
			u32 from = p->fr_fn[p->arc_from[a]], to = p->arc_to[a];
			if (callers && to == f && from != f && acc[from] == 0)
				acc[from] = 1e-300;
			if (!callers && from == f && to != f && acc[to] == 0)
				acc[to] = 1e-300;
		}
	u32 k = 0;
	for (u32 g = 0; g < p->nfn; g++)
		if (acc[g] != 0)
			k++;
	u32 *ids = vx_alloc((k + 1) * sizeof(u32));
	k = 0;
	for (u32 g = 0; g < p->nfn; g++)
		if (acc[g] != 0)
			ids[k++] = g;
	nb_val = acc;
	qsort(ids, k, sizeof(u32), cmp_nb);
	double *vals = vx_alloc((k + 1) * sizeof(double));
	for (u32 i = 0; i < k; i++)
		vals[i] = acc[ids[i]] < 1e-200 ? 0 : acc[ids[i]];
	free(acc);
	*fn_out = ids;
	*val_out = vals;
	return k;
}

u64 prof_arc_calls(const Prof *p, u32 from_fn, u32 to_fn)
{
	u64 c = 0;
	for (u32 a = 0; a < p->narc; a++)
		if (p->arc_to[a] == to_fn && p->fr_fn[p->arc_from[a]] == from_fn)
			c += p->arc_count[a];
	return c;
}
