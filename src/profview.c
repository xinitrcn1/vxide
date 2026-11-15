/* Profile viewer: valgrind massif, jemalloc heap profiles, gprof (gmon) and
 * gcov open as read-only tabs with subtabs:
 *   Memory Graph   massif: heap / extra heap / stacks over time, the peak,
 *                  click a detailed snapshot to look at it
 *   Top            functions by self and total cost, with the selected
 *                  function's callers and callees underneath
 *   Allocation Sites / Callers   the bottom-up call tree
 *   Flame Graph    click to zoom, Backspace to zoom out, Ctrl+F highlights
 *   Hot Lines      source lines by cost, with the line's text
 *   Coverage       gcov: per-file line / function / branch coverage
 * A profile also annotates source files: each line gets a heat colour (the
 * flame graph's yellow -> red) and its cost in a gutter column, the hottest
 * lines ranked, marks in the overview ruler. Loading (parsing, addr2line,
 * gcov, finding the sources) runs on a thread. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "prof.h"
#include "syntax.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#define NONE PROF_NONE

enum { PART_GRAPH, PART_TOP, PART_TREE, PART_FLAME, PART_LINES, PART_FILES, PART_COUNT };
enum { PR_PROF_EXE = 250, PR_PROF_FIND };
enum { PICK_PROFILE = 401 };

typedef struct {
	u32 nfile;
	char **abs;   /* resolved path per profile file (NULL: not found) */
	char **text;  /* its contents (for the Hot Lines snippets) */
	u32 **lstart; /* line starts */
	u32 *nl;
} Sources;

typedef struct ProfDoc {
	bool used;
	char *path;
	int kind;
	char *exe; /* gprof: the executable chosen by the user */
	Prof p;
	int rc;
	_Atomic int state; /* 0 idle, 1 loading, 2 loaded */
	bool thread;
	pthread_t th;
	Sources src;
	u32 gen; /* bumps when the aggregates are rebuilt */

	/* what is looked at */
	u32 met, snap;
	u8 parts[PART_COUNT];
	u32 nparts;

	/* aggregates */
	bool agg_ok;
	double total;
	double *fself, *ftot;
	u32 *top, ntop;
	int top_sort; /* 0 name, 1 self, 2 total, 3 calls */
	ProfTree td, bu;
	double *foff; /* flame: each top-down node's offset in value space */
	ProfLines ln;
	u32 *lord, nlord;
	int ln_sort; /* 0 location, 1 self, 2 total */
	bool uncovered_only;

	/* UI state per part */
	i64 scroll[PART_COUNT];
	u32 sel[PART_COUNT];
	u32 sel_fn;
	u32 nb_for, nb_gen;
	u32 nb_n[2];
	u32 *nb_fn[2];
	double *nb_val[2];
	i64 nb_scroll[2];
	u8 *open; /* bottom-up tree: expanded nodes */
	u32 *rows, nrows, rows_cap;
	bool rows_ok;
	u32 zoom; /* flame: zoomed node */
	bool icicle;
	char find[128];
	double find_val;
	i64 fscroll; /* flame: pixels */
	u32 graph_hover;
} ProfDoc;

static ProfDoc pdoc[MAX_DOCS];

/* source annotation (below) */
static u32 ann_pd_of(void);
static void ann_drop(void);
static void ann_auto(ProfDoc *b);

/* ---- sources ------------------------------------------------------------------------------ */

static bool is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *canon(const char *p)
{
	char buf[4096];
	if (realpath(p, buf))
		return vx_strdup(buf);
	return vx_strdup(p);
}

/* Walk the project once for files still missing (by path suffix). */
static void find_under(const char *dir, int depth, const Prof *p, Sources *s, u32 *left, u32 *budget)
{
	if (!*left || !*budget || depth > 12)
		return;
	DIR *dp = opendir(dir);
	if (!dp)
		return;
	struct dirent *e;
	while ((e = readdir(dp)) && *left && *budget) {
		(*budget)--;
		const char *n = e->d_name;
		if (n[0] == '.' || !strcmp(n, "node_modules"))
			continue;
		char path[4096];
		snprintf(path, sizeof path, "%s/%s", dir, n);
#ifdef DT_DIR
		bool dirp = e->d_type == DT_DIR;
		bool reg = e->d_type == DT_REG;
		if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
			struct stat st;
			if (stat(path, &st) != 0)
				continue;
			dirp = S_ISDIR(st.st_mode);
			reg = S_ISREG(st.st_mode);
		}
#else
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		bool dirp = S_ISDIR(st.st_mode), reg = S_ISREG(st.st_mode);
#endif
		if (dirp) {
			find_under(path, depth + 1, p, s, left, budget);
			continue;
		}
		if (!reg)
			continue;
		for (u32 f = 0; f < s->nfile; f++) {
			if (s->abs[f])
				continue;
			const char *want = prof_str(p, p->file_path[f]);
			while (want[0] == '.' && want[1] == '/')
				want += 2;
			const char *wb = strrchr(want, '/');
			if (strcmp(wb ? wb + 1 : want, n))
				continue;
			size_t pl = strlen(path), wl = strlen(want);
			if (pl >= wl && !strcmp(path + pl - wl, want) && (pl == wl || path[pl - wl - 1] == '/')) {
				s->abs[f] = canon(path);
				(*left)--;
			}
		}
	}
	closedir(dp);
}

static void sources_resolve(const char *profile, const Prof *p, Sources *s)
{
	s->nfile = p->nfile;
	s->abs = vx_alloc((p->nfile + 1) * sizeof(char *));
	s->text = vx_alloc((p->nfile + 1) * sizeof(char *));
	s->lstart = vx_alloc((p->nfile + 1) * sizeof(u32 *));
	s->nl = vx_alloc((p->nfile + 1) * sizeof(u32));
	char pdir[4096];
	snprintf(pdir, sizeof pdir, "%s", profile);
	char *sl = strrchr(pdir, '/');
	if (sl)
		*sl = 0;
	u32 left = 0;
	for (u32 f = 0; f < p->nfile; f++) {
		const char *rel = prof_str(p, p->file_path[f]);
		char c[8192];
		if (rel[0] == '/') {
			if (is_file(rel))
				s->abs[f] = canon(rel);
		} else {
			/* the profile's folder and its parents (up to the project), then the project */
			char d[4096];
			snprintf(d, sizeof d, "%s", pdir);
			for (int up = 0; up < 8 && !s->abs[f]; up++) {
				snprintf(c, sizeof c, "%s/%s", d, rel);
				if (is_file(c))
					s->abs[f] = canon(c);
				if (app.root && !strcmp(d, app.root))
					break;
				char *q = strrchr(d, '/');
				if (!q || q == d)
					break;
				*q = 0;
			}
			if (!s->abs[f] && app.root) {
				snprintf(c, sizeof c, "%s/%s", app.root, rel);
				if (is_file(c))
					s->abs[f] = canon(c);
			}
		}
		if (!s->abs[f])
			left++;
	}
	if (left && app.root) {
		u32 budget = 60000;
		find_under(app.root, 0, p, s, &left, &budget);
	}
	for (u32 f = 0; f < p->nfile; f++) {
		if (!s->abs[f])
			continue;
		size_t n;
		char *t = vx_read_file(s->abs[f], &n);
		if (!t || n > (32u << 20)) {
			free(t);
			continue;
		}
		u32 cap = 0, k = 0;
		u32 *ls = NULL;
		VX_GROW(ls, cap, 1);
		ls[k++] = 0;
		for (size_t i = 0; i < n; i++)
			if (t[i] == '\n') {
				VX_GROW(ls, cap, k + 1);
				ls[k++] = (u32)i + 1;
			}
		s->text[f] = t;
		s->lstart[f] = ls;
		s->nl[f] = k;
	}
}

static void sources_free(Sources *s)
{
	for (u32 f = 0; f < s->nfile; f++) {
		free(s->abs[f]);
		free(s->text[f]);
		free(s->lstart[f]);
	}
	free(s->abs);
	free(s->text);
	free(s->lstart);
	free(s->nl);
	memset(s, 0, sizeof *s);
}

/* line l (1-based) of file f, trimmed; "" when unknown */
static const char *source_line(ProfDoc *b, u32 f, u32 l, u32 *n)
{
	*n = 0;
	if (f >= b->src.nfile || !b->src.text[f] || !l || l > b->src.nl[f])
		return "";
	const char *t = b->src.text[f];
	u32 s = b->src.lstart[f][l - 1];
	u32 e = l < b->src.nl[f] ? b->src.lstart[f][l] - 1 : (u32)strlen(t);
	while (s < e && (t[s] == ' ' || t[s] == '\t'))
		s++;
	while (e > s && (t[e - 1] == '\r' || t[e - 1] == ' '))
		e--;
	*n = e - s;
	return t + s;
}

/* ---- loading ----------------------------------------------------------------------------- */

static void *load_main(void *arg)
{
	ProfDoc *b = arg;
	size_t n = 0;
	size_t pl = strlen(b->path);
	bool gcda = pl > 5 && !strcmp(b->path + pl - 5, ".gcda");
	char *data = gcda ? NULL : vx_read_file(b->path, &n);
	b->rc = prof_load(&b->p, b->kind, b->path, (const u8 *)(data ? data : ""), data ? n : 0, b->exe);
	free(data);
	sources_resolve(b->path, &b->p, &b->src);
	atomic_store(&b->state, 2);
	return NULL;
}

static void pd_start(ProfDoc *b)
{
	atomic_store(&b->state, 1);
	b->thread = pthread_create(&b->th, NULL, load_main, b) == 0;
	if (!b->thread)
		load_main(b);
}

static void pd_clear_agg(ProfDoc *b)
{
	free(b->fself);
	free(b->ftot);
	free(b->top);
	free(b->foff);
	free(b->lord);
	free(b->open);
	free(b->rows);
	for (int i = 0; i < 2; i++) {
		free(b->nb_fn[i]);
		free(b->nb_val[i]);
		b->nb_fn[i] = NULL;
		b->nb_val[i] = NULL;
		b->nb_n[i] = 0;
	}
	prof_tree_free(&b->td);
	prof_tree_free(&b->bu);
	prof_lines_free(&b->ln);
	b->fself = b->ftot = b->foff = NULL;
	b->top = b->lord = b->rows = NULL;
	b->open = NULL;
	b->ntop = b->nlord = b->nrows = b->rows_cap = 0;
	b->rows_ok = false;
	b->agg_ok = false;
	b->nb_for = NONE;
}

static void pd_free(ProfDoc *b)
{
	if (!b->used)
		return;
	if (b->thread)
		pthread_join(b->th, NULL);
	pd_clear_agg(b);
	prof_free(&b->p);
	sources_free(&b->src);
	free(b->path);
	free(b->exe);
	memset(b, 0, sizeof *b);
}

static const u8 parts_massif[] = {PART_GRAPH, PART_TOP, PART_TREE, PART_FLAME, PART_LINES};
static const u8 parts_stack[] = {PART_TOP, PART_TREE, PART_FLAME, PART_LINES};
static const u8 parts_gcov[] = {PART_FILES, PART_TOP, PART_LINES};

static ProfDoc *pd_get(u32 d)
{
	ProfDoc *b = &pdoc[d];
	const char *p = app.docs.path[d] ? app.docs.path[d] : "";
	if (b->used && b->path && !strcmp(b->path, p))
		return b;
	pd_free(b);
	b->used = true;
	b->path = vx_strdup(p);
	b->kind = app.docs.prof[d];
	b->snap = NONE;
	b->sel_fn = NONE;
	b->nb_for = NONE;
	b->top_sort = b->kind == PK_GCOV ? 3 : 1;
	b->ln_sort = 1;
	const u8 *ps = b->kind == PK_MASSIF ? parts_massif : b->kind == PK_GCOV ? parts_gcov : parts_stack;
	b->nparts = b->kind == PK_MASSIF ? VX_COUNT(parts_massif) : b->kind == PK_GCOV ? VX_COUNT(parts_gcov) : VX_COUNT(parts_stack);
	memcpy(b->parts, ps, b->nparts);
	pd_start(b);
	return b;
}

static void pd_reload(ProfDoc *b)
{
	if (atomic_load(&b->state) == 1) {
		ui_status("The profile is still loading");
		return;
	}
	if (b->thread)
		pthread_join(b->th, NULL);
	b->thread = false;
	pd_clear_agg(b);
	prof_free(&b->p);
	sources_free(&b->src);
	b->snap = NONE;
	b->sel_fn = NONE;
	b->zoom = 0;
	pd_start(b);
	app.dirty = true;
}

/* ---- aggregates ------------------------------------------------------------------------- */

static ProfDoc *sort_b;

static int cmp_top(const void *a, const void *c)
{
	const ProfDoc *b = sort_b;
	u32 x = *(const u32 *)a, y = *(const u32 *)c;
	double vx = 0, vy = 0;
	switch (b->top_sort) {
	case 0: return strcmp(prof_str(&b->p, b->p.fn_name[x]), prof_str(&b->p, b->p.fn_name[y]));
	case 1: vx = b->fself[x], vy = b->fself[y]; break;
	case 2: vx = b->ftot[x], vy = b->ftot[y]; break;
	case 3: vx = (double)b->p.fn_calls[x], vy = (double)b->p.fn_calls[y]; break;
	}
	if (vx != vy)
		return vx > vy ? -1 : 1;
	if (b->ftot[x] != b->ftot[y])
		return b->ftot[x] > b->ftot[y] ? -1 : 1;
	return x < y ? -1 : x > y;
}

static int cmp_ln(const void *a, const void *c)
{
	const ProfDoc *b = sort_b;
	u32 x = *(const u32 *)a, y = *(const u32 *)c;
	const ProfLines *l = &b->ln;
	if (b->ln_sort == 0) {
		int r = strcmp(prof_str(&b->p, b->p.file_path[l->file[x]]), prof_str(&b->p, b->p.file_path[l->file[y]]));
		if (r)
			return r;
		return l->line[x] < l->line[y] ? -1 : l->line[x] > l->line[y];
	}
	double vx = b->ln_sort == 1 ? l->self[x] : l->total[x], vy = b->ln_sort == 1 ? l->self[y] : l->total[y];
	if (vx != vy)
		return vx > vy ? -1 : 1;
	if (l->total[x] != l->total[y])
		return l->total[x] > l->total[y] ? -1 : 1;
	return x < y ? -1 : x > y;
}

static void sort_top(ProfDoc *b)
{
	sort_b = b;
	qsort(b->top, b->ntop, sizeof(u32), cmp_top);
}

static void sort_lines(ProfDoc *b)
{
	b->nlord = 0;
	for (u32 i = 0; i < b->ln.n; i++)
		if (!b->uncovered_only || (b->ln.flags[i] & PL_UNCOVERED) ||
		    (b->ln.br_tot[i] && b->ln.br_hit[i] < b->ln.br_tot[i]))
			b->lord[b->nlord++] = i;
	sort_b = b;
	qsort(b->lord, b->nlord, sizeof(u32), cmp_ln);
}

static void flame_offsets(ProfDoc *b)
{
	ProfTree *t = &b->td;
	b->foff = vx_alloc((t->n + 1) * sizeof(double));
	/* parents come before children in node order */
	for (u32 n = 0; n < t->n; n++) {
		double o = b->foff[n];
		for (u32 c = t->child[n]; c != NONE; c = t->next[c]) {
			b->foff[c] = o;
			o += t->val[c];
		}
	}
}

static void agg_build(ProfDoc *b)
{
	pd_clear_agg(b);
	Prof *p = &b->p;
	if (b->met >= VX_MAX(p->nmet, 1u))
		b->met = 0;
	if (b->kind == PK_MASSIF && b->snap == NONE)
		b->snap = p->peak_snap;
	u32 snap = b->kind == PK_MASSIF ? b->snap : NONE;
	b->total = prof_total(p, b->met, snap);
	b->fself = vx_alloc((p->nfn + 1) * sizeof(double));
	b->ftot = vx_alloc((p->nfn + 1) * sizeof(double));
	prof_fn_sums(p, b->met, snap, b->fself, b->ftot);
	b->top = vx_alloc((p->nfn + 1) * sizeof(u32));
	for (u32 f = 0; f < p->nfn; f++)
		if (b->ftot[f] != 0 || b->kind == PK_GCOV || p->fn_calls[f])
			b->top[b->ntop++] = f;
	sort_top(b);
	prof_tree(p, b->met, snap, false, &b->td);
	prof_tree(p, b->met, snap, true, &b->bu);
	flame_offsets(b);
	prof_lines(p, b->met, snap, &b->ln);
	b->lord = vx_alloc((b->ln.n + 1) * sizeof(u32));
	sort_lines(b);
	/* the bottom-up tree opens on its biggest site */
	b->open = vx_alloc(b->bu.n + 1);
	b->open[0] = 1;
	if (b->bu.n > 1 && b->bu.child[0] != NONE)
		b->open[b->bu.child[0]] = 1;
	b->zoom = 0;
	b->find_val = 0;
	b->agg_ok = true;
	b->gen++;
	for (int i = 0; i < PART_COUNT; i++)
		if (b->sel[i] > 1000000000u)
			b->sel[i] = 0;
	if (b->sel_fn != NONE && b->sel_fn >= p->nfn)
		b->sel_fn = NONE;
	if (b->sel_fn == NONE && b->ntop && b->kind != PK_GCOV)
		b->sel_fn = b->top[VX_MIN(b->sel[PART_TOP], b->ntop - 1)]; /* callers / callees show at once */
}

static bool pd_ready(ProfDoc *b)
{
	if (atomic_load(&b->state) != 2)
		return false;
	if (b->thread) {
		pthread_join(b->th, NULL);
		b->thread = false;
	}
	if (!b->agg_ok) {
		agg_build(b);
		/* the first profile that loads colours the sources until another is chosen */
		if (ann_pd_of() == NONE && (b->p.nst || b->p.ngl))
			ann_auto(b);
	}
	return true;
}

bool profview_busy(void)
{
	for (u32 d = 0; d < MAX_DOCS; d++)
		if (pdoc[d].used && atomic_load(&pdoc[d].state) == 1)
			return true;
	return false;
}

void profview_doc_released(u32 d)
{
	if (ann_pd_of() == d)
		ann_drop(); /* the annotating profile is gone */
	pd_free(&pdoc[d]);
}

void profview_init(View *v)
{
	ProfDoc *b = pd_get(v->doc);
	v->nparts = b->nparts;
	if (v->part >= b->nparts)
		v->part = 0;
}

/* ---- colours, text, hit rectangles ------------------------------------------------------------- */

static u64 hstr(const char *s)
{
	u64 h = 1469598103934665603ull;
	while (*s) {
		h ^= (u8)*s++;
		h *= 1099511628211ull;
	}
	return h;
}

/* Flame graph colours (Brendan Gregg's palettes): "hot" for time, "mem"
 * (greens) for memory; stable per function name. */
static u32 flame_col(int kind, const char *name)
{
	u64 h = hstr(name);
	double a = (double)(h & 0xFF) / 255.0, c = (double)((h >> 8) & 0xFF) / 255.0, e = (double)((h >> 16) & 0xFF) / 255.0;
	u32 r, g, bl;
	if (kind == PK_MASSIF || kind == PK_JEPROF) {
		r = (u32)(40 + 60 * a);
		g = (u32)(170 + 70 * c);
		bl = (u32)(60 + 80 * e);
	} else {
		r = (u32)(205 + 50 * a);
		g = (u32)(60 + 170 * c);
		bl = (u32)(20 + 40 * e);
	}
	return r << 24 | g << 16 | bl << 8 | 0xFFu;
}

/* heat: 0 = pale yellow, 1 = deep red (flame graph style) */
static u32 heat_rgb(double f)
{
	f = VX_CLAMP(f, 0.0, 1.0);
	double r, g, bl;
	if (f < 0.5) {
		double t = f / 0.5;
		r = 255;
		g = 226 - 76 * t;
		bl = 110 - 80 * t;
	} else {
		double t = (f - 0.5) / 0.5;
		r = 255 - 35 * t;
		g = 150 - 115 * t;
		bl = 30;
	}
	return (u32)r << 24 | (u32)g << 16 | (u32)bl << 8;
}

static bool in_r(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static int ty(int y, int h, int font) { return y + (h - font_m[font].line) / 2; }

static int text_w(int font, const char *s) { return font_width(font, s, strlen(s)); }

static void text_r(int font, int xr, int y, const char *s, u32 c)
{
	draw_text(font, xr - text_w(font, s), y, s, strlen(s), c);
}

static void text_fit(int font, int x, int y, const char *s, int maxw, u32 c)
{
	if (maxw > 0)
		draw_text_fit(font, x, y, s, strlen(s), maxw, c);
}

enum {
	H_NONE,
	H_METRIC,
	H_SORT_TOP,
	H_SORT_LN,
	H_ICICLE,
	H_UNZOOM,
	H_UNCOV,
	H_PEAK,
	H_SETEXE,
	H_ANNOTATE,
	H_SITE,  /* graph side panel: a top allocation site */
	H_RELOAD,
};

typedef struct {
	int x, y, w, h, act, arg;
} Hit;
static Hit hits[64];
static int nhit;

static void hit_add(int x, int y, int w, int h, int act, int arg)
{
	if (nhit < (int)VX_COUNT(hits))
		hits[nhit++] = (Hit){x, y, w, h, act, arg};
}

static int hit_at(int mx, int my, int *arg)
{
	for (int i = nhit; i-- > 0;)
		if (mx >= hits[i].x && mx < hits[i].x + hits[i].w && my >= hits[i].y && my < hits[i].y + hits[i].h) {
			*arg = hits[i].arg;
			return hits[i].act;
		}
	return H_NONE;
}

/* A small button; returns its width. */
static int chip(int x, int y, int h, const char *label, int act, int arg, bool on, const char *tip)
{
	int w = text_w(FONT_SMALL, label) + PX(16);
	bool hov = in_r(app.hover_x, app.hover_y, x, y, w, h);
	draw_rect(x, y, w, h, on ? COL(C_BUTTON_BG) : hov ? COL(C_LIST_HOVER_BG) : COL(C_INPUT_BG));
	draw_frame(x, y, w, h, on ? COL(C_BUTTON_BG) : COL(C_INPUT_BORDER));
	draw_text(FONT_SMALL, x + PX(8), ty(y, h, FONT_SMALL), label, strlen(label), on ? COL(C_BUTTON_FG) : COL(C_FG));
	hit_add(x, y, w, h, act, arg);
	if (tip)
		ui_tip(x, y, w, h, tip);
	return w;
}

static void fmtv(ProfDoc *b, double v, char *buf, size_t cap) { prof_fmt(&b->p, b->met, v, buf, cap); }

static void fmt_pct(double v, double total, char *buf, size_t cap)
{
	double pc = total > 0 ? 100.0 * v / total : 0;
	if (pc >= 10 || pc == 0)
		snprintf(buf, cap, "%.1f%%", pc);
	else
		snprintf(buf, cap, "%.2f%%", pc);
}

/* value + percentage, right-aligned in [x, x+w), with a heat bar behind */
static void value_cell(ProfDoc *b, int x, int y, int w, int h, double v, double total, bool bar)
{
	if (bar && total > 0 && v > 0) {
		double f = VX_MIN(v / total, 1.0);
		int bw = (int)(f * (w - PX(8)));
		draw_rect(x + PX(4), y + PX(3), VX_MAX(bw, PX(1)), h - PX(6), heat_rgb(f) | 0x60u);
	}
	char vb[48], pb[24], s[80];
	fmtv(b, v, vb, sizeof vb);
	fmt_pct(v, total, pb, sizeof pb);
	snprintf(s, sizeof s, "%s  %s", vb, pb);
	text_r(FONT_UI, x + w - PX(8), ty(y, h, FONT_UI), v == 0 ? "-" : s, v == 0 ? COL(C_DESCRIPTION_FG) : COL(C_FG));
}

static const char *fn_name(ProfDoc *b, u32 f) { return f == NONE ? "(all)" : prof_str(&b->p, b->p.fn_name[f]); }

static const char *file_name(ProfDoc *b, u32 file)
{
	if (file == NONE)
		return "";
	const char *s = prof_str(&b->p, b->p.file_path[file]);
	if (app.root && file < b->src.nfile && b->src.abs[file]) {
		size_t rl = strlen(app.root);
		if (!strncmp(b->src.abs[file], app.root, rl) && b->src.abs[file][rl] == '/')
			return b->src.abs[file] + rl + 1;
	}
	return s;
}

static void fn_location(ProfDoc *b, u32 f, char *buf, size_t cap)
{
	buf[0] = 0;
	if (f == NONE || b->p.fn_file[f] == NONE)
		return;
	if (b->p.fn_line[f])
		snprintf(buf, cap, "%s:%u", file_name(b, b->p.fn_file[f]), b->p.fn_line[f]);
	else
		snprintf(buf, cap, "%s", file_name(b, b->p.fn_file[f]));
}

/* ---- subtabs ----------------------------------------------------------------------------------- */

static const char *part_title(ProfDoc *b, u32 i)
{
	bool mem = b->kind == PK_MASSIF || b->kind == PK_JEPROF;
	switch (b->parts[i]) {
	case PART_GRAPH: return "Memory Graph";
	case PART_TOP: return b->kind == PK_GCOV ? "Functions" : mem ? "Top Allocators" : "Flat Profile";
	case PART_TREE: return mem ? "Allocation Sites" : "Callers";
	case PART_FLAME: return "Flame Graph";
	case PART_LINES: return b->kind == PK_GCOV ? "Lines" : "Hot Lines";
	case PART_FILES: return "Coverage";
	default: return "";
	}
}

static int sub_w(ProfDoc *b, u32 i) { return text_w(FONT_SMALL, part_title(b, i)) + PX(20); }

static int sub_layout(ProfDoc *b, int w, u32 want, int *ox, int *oy)
{
	int x = 0, row = 0;
	for (u32 i = 0; i < b->nparts; i++) {
		int tw = sub_w(b, i);
		if (x + tw > w && x > 0) {
			x = 0;
			row++;
		}
		if (i == want) {
			*ox = x;
			*oy = row * app.subtabs_h;
		}
		x += tw;
	}
	return row + 1;
}

int profview_subtabs_height(View *v, int w)
{
	ProfDoc *b = pd_get(v->doc);
	int ox, oy;
	return sub_layout(b, w, NONE, &ox, &oy) * app.subtabs_h;
}

void profview_subtabs_draw(View *v, int x, int y, int w)
{
	ProfDoc *b = pd_get(v->doc);
	int hh = profview_subtabs_height(v, w);
	draw_rect(x, y, w, hh, COL(C_EDITOR_BG));
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	for (u32 i = 0; i < b->nparts; i++) {
		int ox = 0, oy = 0;
		sub_layout(b, w, i, &ox, &oy);
		int tw = sub_w(b, i), tx = x + ox, tyy = y + oy;
		bool active = i == v->part;
		if (active) {
			draw_rect(tx, tyy, tw, app.subtabs_h, COL(C_TAB_ACTIVE_BG));
			draw_rect(tx, tyy + app.subtabs_h - PX(2), tw, PX(2), COL(C_TAB_ACTIVE_BORDER_TOP));
		}
		const char *nm = part_title(b, i);
		draw_text(FONT_SMALL, tx + PX(10), ty(tyy, app.subtabs_h, FONT_SMALL), nm, strlen(nm),
		          active ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG));
	}
}

void profview_subtab_click(View *v, int rx, int ry, int w)
{
	ProfDoc *b = pd_get(v->doc);
	for (u32 i = 0; i < b->nparts; i++) {
		int ox = 0, oy = 0;
		sub_layout(b, w, i, &ox, &oy);
		if (rx >= ox && rx < ox + sub_w(b, i) && ry >= oy && ry < oy + app.subtabs_h) {
			v->part = i;
			app.dirty = true;
			return;
		}
	}
}

/* ---- header ----------------------------------------------------------------------------------- */

#define HDR_H PX(34)

static int draw_header(ProfDoc *b, int x, int y, int w, const char *title)
{
	draw_rect(x, y, w, HDR_H, COL(C_EDITOR_BG));
	draw_rect(x, y + HDR_H - PX(1), w, PX(1), COL(C_TABS_BORDER));
	int ch = PX(22), cy = y + (HDR_H - ch) / 2;
	/* right side: metric, snapshot */
	int rx = x + w - PX(10);
	Prof *p = &b->p;
	if (p->nmet > 1) {
		char m[96];
		snprintf(m, sizeof m, "%s  \xE2\x96\xBE", prof_str(p, p->met_name[b->met]));
		int cw = text_w(FONT_SMALL, m) + PX(16);
		rx -= cw;
		chip(rx, cy, ch, m, H_METRIC, 0, false, "Switch metric (Profile: Switch Metric)");
		rx -= PX(6);
	}
	if (b->kind == PK_MASSIF && p->nsnap) {
		char m[96];
		bool pk = b->snap == p->peak_snap;
		snprintf(m, sizeof m, "Snapshot %u%s", b->snap, pk ? " (peak)" : "");
		int cw = text_w(FONT_SMALL, m) + PX(16);
		rx -= cw;
		chip(rx, cy, ch, m, H_PEAK, 0, pk, pk ? "The peak snapshot (pick another in Memory Graph)" : "Back to the peak");
		rx -= PX(6);
	}
	int tx = x + PX(12);
	int tw = draw_text(FONT_UI_BOLD, tx, ty(y, HDR_H, FONT_UI_BOLD), title, strlen(title), COL(C_FG));
	char tot[160], vb[48];
	if (b->kind == PK_GCOV) {
		u32 code = 0, hit = 0;
		for (u32 g = 0; g < p->ngl; g++) {
			code++;
			hit += p->gl_count[g] > 0;
		}
		snprintf(tot, sizeof tot, "%u of %u lines executed (%.1f%%)", hit, code, code ? 100.0 * hit / code : 0.0);
	} else {
		fmtv(b, b->total, vb, sizeof vb);
		snprintf(tot, sizeof tot, "Total %s", vb);
	}
	text_fit(FONT_SMALL, tw + PX(14), ty(y, HDR_H, FONT_SMALL), tot, rx - tw - PX(20), COL(C_DESCRIPTION_FG));
	return HDR_H;
}

/* the loading / error states; true when there is nothing else to draw */
static bool draw_state(ProfDoc *b, int x, int y, int w, int h)
{
	if (atomic_load(&b->state) != 2) {
		draw_rect(x, y, w, h, COL(C_EDITOR_BG));
		const char *m = b->kind == PK_GPROF   ? "Reading the profile and symbolising (addr2line)..."
		                : b->kind == PK_GCOV  ? "Running gcov..."
		                : b->kind == PK_JEPROF ? "Reading the heap profile and symbolising..."
		                                      : "Reading the profile...";
		draw_text(FONT_UI, x + PX(20), y + PX(14), m, strlen(m), COL(C_DESCRIPTION_FG));
		return true;
	}
	if (b->rc != 0 && !b->p.nst && !b->p.ngl && !b->p.nsnap) {
		draw_rect(x, y, w, h, COL(C_EDITOR_BG));
		const char *m = b->p.err[0] ? b->p.err : "Could not read this profile";
		draw_icon(ICON_ERROR, x + PX(20), y + PX(16), COL(C_ERROR));
		text_fit(FONT_UI, x + PX(44), y + PX(14), m, w - PX(64), COL(C_FG));
		int bx = x + PX(44), by = y + PX(44);
		if (b->kind == PK_GPROF)
			bx += chip(bx, by, PX(24), "Set Executable...", H_SETEXE, 0, true, NULL) + PX(8);
		chip(bx, by, PX(24), "Reload", H_RELOAD, 0, false, NULL);
		return true;
	}
	return false;
}

/* ---- lists ------------------------------------------------------------------------------------- */

typedef struct {
	const char *title;
	int w; /* 0: takes the rest */
	bool right;
	int sort; /* -1: not sortable */
} Col;

static void col_layout(const Col *c, int n, int x, int w, int *cx, int *cw)
{
	int fixed = 0, flex = 0;
	for (int i = 0; i < n; i++) {
		fixed += c[i].w;
		flex += !c[i].w;
	}
	int rest = VX_MAX(w - fixed, PX(120));
	int px = x;
	for (int i = 0; i < n; i++) {
		cx[i] = px;
		cw[i] = c[i].w ? c[i].w : rest / VX_MAX(flex, 1);
		px += cw[i];
	}
}

static void list_header(const Col *c, int n, const int *cx, const int *cw, int y, int x, int w, int act, int cur_sort)
{
	draw_rect(x, y, w, app.row_h, COL(C_SIDEBAR_BG));
	draw_rect(x, y + app.row_h - PX(1), w, PX(1), COL(C_TABS_BORDER));
	for (int i = 0; i < n; i++) {
		char t[96];
		snprintf(t, sizeof t, "%s%s", c[i].title, c[i].sort >= 0 && c[i].sort == cur_sort ? " \xE2\x96\xBE" : "");
		int yy = ty(y, app.row_h, FONT_SMALL_BOLD);
		if (c[i].right)
			text_r(FONT_SMALL_BOLD, cx[i] + cw[i] - PX(8), yy, t, COL(C_SECTION_HEADER_FG));
		else
			text_fit(FONT_SMALL_BOLD, cx[i] + PX(8), yy, t, cw[i] - PX(12), COL(C_SECTION_HEADER_FG));
		if (c[i].sort >= 0) {
			hit_add(cx[i], y, cw[i], app.row_h, act, c[i].sort);
			ui_tip(cx[i], y, cw[i], app.row_h, "Sort by this column");
		}
	}
}

/* geometry of the last drawn lists (for the mouse) */
static struct {
	int x, y, w, h; /* the part */
	int list_y, list_h, row_h;
	int nb_x[2], nb_y, nb_w, nb_h; /* Top: callers / callees */
	int chart_x0, chart_x1, chart_y0, chart_y1;
	int side_x;
	int flame_y0, flame_h;
} geo;

static void keep_visible(i64 *scroll, u32 sel, int vis)
{
	if ((i64)sel < *scroll)
		*scroll = sel;
	if ((i64)sel >= *scroll + vis)
		*scroll = (i64)sel - vis + 1;
	if (*scroll < 0)
		*scroll = 0;
}

static void clamp_scroll(i64 *scroll, u32 n, int vis)
{
	i64 mx = VX_MAX((i64)n - vis, 0);
	*scroll = VX_CLAMP(*scroll, 0, mx);
}

/* ---- Top ------------------------------------------------------------------------------------------ */

static void neighbors_update(ProfDoc *b)
{
	if (b->nb_for == b->sel_fn && b->nb_gen == b->gen)
		return;
	for (int i = 0; i < 2; i++) {
		free(b->nb_fn[i]);
		free(b->nb_val[i]);
		b->nb_fn[i] = NULL;
		b->nb_val[i] = NULL;
		b->nb_n[i] = 0;
		b->nb_scroll[i] = 0;
	}
	b->nb_for = b->sel_fn;
	b->nb_gen = b->gen;
	if (b->sel_fn == NONE || b->kind == PK_GCOV)
		return;
	u32 snap = b->kind == PK_MASSIF ? b->snap : NONE;
	for (int i = 0; i < 2; i++)
		b->nb_n[i] = prof_neighbors(&b->p, b->met, snap, b->sel_fn, i == 0, &b->nb_fn[i], &b->nb_val[i]);
}

static void draw_neighbors(ProfDoc *b, int side, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_SIDEBAR_BG));
	char t[300];
	snprintf(t, sizeof t, "%s %s", side == 0 ? "CALLERS OF" : "CALLEES OF", fn_name(b, b->sel_fn));
	text_fit(FONT_SMALL_BOLD, x + PX(10), y + PX(5), t, w - PX(20), COL(C_SECTION_HEADER_FG));
	int ly = y + PX(24), rh = app.row_h;
	int vis = VX_MAX((h - PX(24)) / rh, 1);
	clamp_scroll(&b->nb_scroll[side], b->nb_n[side], vis);
	bool calls = b->kind == PK_GPROF;
	int vw = PX(170), cw = calls ? PX(90) : 0;
	draw_clip(x, ly, w, h - PX(24));
	if (!b->nb_n[side]) {
		const char *m = side == 0 ? (b->kind == PK_MASSIF || b->kind == PK_JEPROF ? "Allocates directly (no caller recorded)" : "No callers (a root)")
		                          : "Calls nothing that was sampled";
		text_fit(FONT_SMALL, x + PX(10), ly + PX(4), m, w - PX(20), COL(C_DESCRIPTION_FG));
	}
	for (int r = 0; r < vis && (u32)(b->nb_scroll[side] + r) < b->nb_n[side]; r++) {
		u32 i = (u32)b->nb_scroll[side] + (u32)r;
		int ry = ly + r * rh;
		bool hov = in_r(app.hover_x, app.hover_y, x, ry, w, rh);
		if (hov)
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		u32 f = b->nb_fn[side][i];
		text_fit(FONT_UI, x + PX(10), ty(ry, rh, FONT_UI), fn_name(b, f), w - vw - cw - PX(20), COL(C_FG));
		value_cell(b, x + w - vw - cw, ry, vw, rh, b->nb_val[side][i], b->total, true);
		if (calls) {
			u64 c = side == 0 ? prof_arc_calls(&b->p, f, b->sel_fn) : prof_arc_calls(&b->p, b->sel_fn, f);
			char cb[32];
			prof_fmt_count((double)c, cb, sizeof cb);
			text_r(FONT_UI, x + w - PX(10), ty(ry, rh, FONT_UI), c ? cb : "-", COL(C_DESCRIPTION_FG));
		}
	}
	draw_noclip();
	geo.nb_x[side] = x;
	geo.nb_y = ly;
	geo.nb_w = w;
	geo.nb_h = h - PX(24);
}

static void draw_top(ProfDoc *b, u32 part, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	bool gcov = b->kind == PK_GCOV, gprof = b->kind == PK_GPROF;
	const char *title = gcov ? "Functions" : gprof ? "Flat profile" : "Top allocators";
	y += draw_header(b, x, y, w, title);
	h -= HDR_H;
	int nb_h = gcov || b->sel_fn == NONE ? 0 : VX_MIN(h * 2 / 5, PX(240));
	int lh = h - nb_h;
	Col cc[5];
	int nc;
	if (gcov) {
		Col c[] = {{"Function", 0, false, 0}, {"Calls", PX(110), true, 3}, {"Blocks", PX(90), true, -1},
		           {"Location", PX(260), false, -1}};
		memcpy(cc, c, sizeof c);
		nc = 4;
	} else {
		Col c[] = {{"Function", 0, false, 0},
		           {gprof ? "Self time" : "Self (allocated here)", PX(200), true, 1},
		           {"Total (with callees)", PX(200), true, 2},
		           {"Calls", PX(90), true, 3},
		           {"Location", PX(240), false, -1}};
		memcpy(cc, c, sizeof c);
		nc = 5;
		if (!gprof) {
			cc[3] = cc[4]; /* no call counts in heap profiles */
			nc = 4;
		}
	}
	int cx[5], cw[5];
	col_layout(cc, nc, x, w, cx, cw);
	list_header(cc, nc, cx, cw, y, x, w, H_SORT_TOP, b->top_sort);
	int ly = y + app.row_h, rh = app.row_h;
	int vis = VX_MAX((lh - app.row_h) / rh, 1);
	u32 n = b->ntop;
	if (b->sel[part] >= n && n)
		b->sel[part] = n - 1;
	clamp_scroll(&b->scroll[part], n, vis);
	geo.list_y = ly;
	geo.list_h = lh - app.row_h;
	geo.row_h = rh;
	draw_clip(x, ly, w, lh - app.row_h);
	if (!n) {
		const char *m = "No functions in this profile";
		draw_text(FONT_UI, x + PX(12), ly + PX(6), m, strlen(m), COL(C_DESCRIPTION_FG));
	}
	for (int r = 0; r <= vis && (u32)(b->scroll[part] + r) < n; r++) {
		u32 i = (u32)b->scroll[part] + (u32)r, f = b->top[i];
		int ry = ly + r * rh;
		bool sel = i == b->sel[part];
		bool hov = in_r(app.hover_x, app.hover_y, x, ry, w, rh);
		if (sel)
			draw_rect(x, ry, w, rh, app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
		else if (hov)
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		u32 fg = sel && app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_FG) : COL(C_FG);
		char loc[600];
		fn_location(b, f, loc, sizeof loc);
		if (gcov) {
			u64 calls = b->p.fn_calls[f];
			text_fit(FONT_UI, cx[0] + PX(10), ty(ry, rh, FONT_UI), fn_name(b, f), cw[0] - PX(14),
			         calls ? fg : COL(C_ERROR));
			char cb[32];
			prof_fmt_count((double)calls, cb, sizeof cb);
			text_r(FONT_UI, cx[1] + cw[1] - PX(8), ty(ry, rh, FONT_UI), cb, calls ? fg : COL(C_ERROR));
			if (b->p.fn_blocks[f] != 255) {
				snprintf(cb, sizeof cb, "%u%%", b->p.fn_blocks[f]);
				int bw = (cw[2] - PX(16)) * b->p.fn_blocks[f] / 100;
				draw_rect(cx[2] + PX(8), ry + rh - PX(5), bw, PX(2), COL(C_GUTTER_ADDED));
				text_r(FONT_UI, cx[2] + cw[2] - PX(8), ty(ry, rh, FONT_UI), cb, fg);
			}
			text_fit(FONT_UI, cx[3] + PX(8), ty(ry, rh, FONT_UI), loc, cw[3] - PX(12), COL(C_DESCRIPTION_FG));
		} else {
			/* a flame-coloured swatch, then the name */
			draw_rect(cx[0] + PX(8), ry + PX(6), PX(4), rh - PX(12), flame_col(b->kind, fn_name(b, f)));
			text_fit(FONT_UI, cx[0] + PX(18), ty(ry, rh, FONT_UI), fn_name(b, f), cw[0] - PX(22), fg);
			value_cell(b, cx[1], ry, cw[1], rh, b->fself[f], b->total, true);
			value_cell(b, cx[2], ry, cw[2], rh, b->ftot[f], b->total, true);
			int li = 3;
			if (gprof) {
				char cb[32];
				prof_fmt_count((double)b->p.fn_calls[f], cb, sizeof cb);
				text_r(FONT_UI, cx[3] + cw[3] - PX(8), ty(ry, rh, FONT_UI), b->p.fn_calls[f] ? cb : "-", fg);
				li = 4;
			}
			text_fit(FONT_UI, cx[li] + PX(8), ty(ry, rh, FONT_UI), loc, cw[li] - PX(12), COL(C_DESCRIPTION_FG));
		}
	}
	draw_noclip();
	if (nb_h) {
		neighbors_update(b);
		int ny = y + lh;
		draw_rect(x, ny, w, PX(1), COL(C_PANEL_BORDER));
		draw_neighbors(b, 0, x, ny + PX(1), w / 2, nb_h - PX(1));
		draw_rect(x + w / 2, ny + PX(1), PX(1), nb_h - PX(1), COL(C_PANEL_BORDER));
		draw_neighbors(b, 1, x + w / 2 + PX(1), ny + PX(1), w - w / 2 - PX(1), nb_h - PX(1));
	} else
		geo.nb_h = 0;
}

/* ---- bottom-up tree ---------------------------------------------------------------------------------- */

static void rows_add(ProfDoc *b, u32 nd)
{
	for (u32 c = b->bu.child[nd]; c != NONE; c = b->bu.next[c]) {
		VX_GROW(b->rows, b->rows_cap, b->nrows + 1);
		b->rows[b->nrows++] = c;
		if (b->open[c])
			rows_add(b, c);
	}
}

static void rows_update(ProfDoc *b)
{
	if (b->rows_ok)
		return;
	b->nrows = 0;
	if (b->bu.n)
		rows_add(b, 0);
	b->rows_ok = true;
}

static void open_all(ProfDoc *b, u32 nd, int depth)
{
	b->open[nd] = 1;
	if (depth > 40)
		return;
	for (u32 c = b->bu.child[nd]; c != NONE; c = b->bu.next[c])
		open_all(b, c, depth + 1);
}

static void draw_tree(ProfDoc *b, u32 part, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	bool mem = b->kind == PK_MASSIF || b->kind == PK_JEPROF;
	y += draw_header(b, x, y, w, mem ? "Allocation sites, then who called them" : "Where the time went, then the callers");
	h -= HDR_H;
	rows_update(b);
	Col c[] = {{mem ? "Site / caller" : "Function / caller", 0, false, -1},
	           {"Total", PX(210), true, -1},
	           {mem ? "Self (stack ends here)" : "Self", PX(190), true, -1}};
	int cx[3], cw[3];
	col_layout(c, 3, x, w, cx, cw);
	list_header(c, 3, cx, cw, y, x, w, H_NONE, -1);
	int ly = y + app.row_h, rh = app.row_h;
	int vis = VX_MAX((h - app.row_h) / rh, 1);
	if (b->nrows && b->sel[part] >= b->nrows)
		b->sel[part] = b->nrows - 1;
	clamp_scroll(&b->scroll[part], b->nrows, vis);
	geo.list_y = ly;
	geo.list_h = h - app.row_h;
	geo.row_h = rh;
	draw_clip(x, ly, w, h - app.row_h);
	for (int r = 0; r <= vis && (u32)(b->scroll[part] + r) < b->nrows; r++) {
		u32 i = (u32)b->scroll[part] + (u32)r, nd = b->rows[i];
		int ry = ly + r * rh;
		bool sel = i == b->sel[part];
		if (sel)
			draw_rect(x, ry, w, rh, app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
		else if (in_r(app.hover_x, app.hover_y, x, ry, w, rh))
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		int ind = (int)(b->bu.depth[nd] - 1) * PX(16);
		int ix = cx[0] + PX(6) + ind;
		for (u32 dd = 1; dd < b->bu.depth[nd]; dd++)
			draw_rect(cx[0] + PX(6) + (int)(dd - 1) * PX(16) + PX(7), ry, PX(1), rh, COL(C_TREE_INDENT_GUIDE));
		if (b->bu.child[nd] != NONE)
			draw_icon(b->open[nd] ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ix, ry + (rh - PX(16)) / 2, COL(C_FG));
		const char *nm = fn_name(b, b->bu.fn[nd]);
		draw_rect(ix + PX(18), ry + PX(6), PX(4), rh - PX(12), flame_col(b->kind, nm));
		u32 fg = sel && app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_FG) : COL(C_FG);
		int nx = ix + PX(26);
		int ex = draw_text_fit(FONT_UI, nx, ty(ry, rh, FONT_UI), nm, strlen(nm), cx[1] - nx - PX(8), fg);
		char loc[600];
		fn_location(b, b->bu.fn[nd], loc, sizeof loc);
		if (*loc && ex + PX(20) < cx[1])
			text_fit(FONT_SMALL, ex + PX(10), ty(ry, rh, FONT_SMALL), loc, cx[1] - ex - PX(18), COL(C_DESCRIPTION_FG));
		value_cell(b, cx[1], ry, cw[1], rh, b->bu.val[nd], b->total, true);
		value_cell(b, cx[2], ry, cw[2], rh, b->bu.self[nd], b->total, false);
	}
	draw_noclip();
}

/* ---- flame graph --------------------------------------------------------------------------------------- */

#define FRAME_H PX(18)

static bool find_match(ProfDoc *b, u32 fn)
{
	return b->find[0] && fn != NONE && strstr(fn_name(b, fn), b->find);
}

static u32 flame_hover;

static void flame_node(ProfDoc *b, u32 nd, int x, int w, int y_base, double zoff, double zval, int ytop, int ybot)
{
	ProfTree *t = &b->td;
	double scale = (double)w / zval;
	double x0 = (b->foff[nd] - zoff) * scale, x1 = x0 + t->val[nd] * scale;
	if (x1 <= 0 || x0 >= w || x1 - x0 < 0.6)
		return;
	int d = (int)t->depth[nd];
	int fy = b->icicle ? y_base + d * FRAME_H : y_base - (d + 1) * FRAME_H;
	if (fy + FRAME_H >= ytop && fy < ybot) {
		int a = x + (int)VX_MAX(x0, 0.0), e = x + (int)VX_MIN(x1, (double)w);
		int fw = VX_MAX(e - a - PX(1), 1);
		const char *nm = nd ? fn_name(b, t->fn[nd]) : "all";
		u32 col = nd ? flame_col(b->kind, nm) : 0xB0B0B0FFu;
		bool dim = b->zoom && b->foff[nd] <= b->foff[b->zoom] && t->depth[nd] < t->depth[b->zoom];
		if (find_match(b, t->fn[nd]))
			col = 0xE03CE0FFu;
		if (dim)
			col = (col & 0xFFFFFF00u) | 0x80u;
		bool hov = in_r(app.hover_x, app.hover_y, a, fy, fw, FRAME_H - PX(1));
		if (hov)
			flame_hover = nd;
		draw_rect(a, fy, fw, FRAME_H - PX(1), col);
		if (hov)
			draw_frame(a, fy, fw, FRAME_H - PX(1), 0x202020FFu);
		if (fw > PX(28))
			text_fit(FONT_SMALL, a + PX(3), ty(fy, FRAME_H - PX(1), FONT_SMALL), nm, fw - PX(6), 0x101010FFu);
	}
	for (u32 c = t->child[nd]; c != NONE; c = t->next[c])
		flame_node(b, c, x, w, y_base, zoff, zval, ytop, ybot);
}

static double find_total(ProfDoc *b, u32 nd)
{
	/* the value of the outermost matching frames */
	if (find_match(b, b->td.fn[nd]))
		return b->td.val[nd];
	double s = 0;
	for (u32 c = b->td.child[nd]; c != NONE; c = b->td.next[c])
		s += find_total(b, c);
	return s;
}

static void tooltip_box(int mx, int my, const char *const *lines, int n, int bound_x1, int bound_y1)
{
	int w = 0, lh = font_m[FONT_SMALL].line + PX(3);
	for (int i = 0; i < n; i++)
		w = VX_MAX(w, text_w(i ? FONT_SMALL : FONT_SMALL_BOLD, lines[i]));
	w += PX(20);
	int h = n * lh + PX(12);
	int x = mx + PX(14), y = my + PX(18);
	if (x + w > bound_x1)
		x = VX_MAX(mx - w - PX(8), 0);
	if (y + h > bound_y1)
		y = VX_MAX(my - h - PX(8), 0);
	draw_rect(x + PX(2), y + PX(2), w, h, (COL(C_WIDGET_SHADOW) & 0xFFFFFF00u) | 0x60u);
	draw_rect(x, y, w, h, COL(C_HOVER_BG));
	draw_frame(x, y, w, h, COL(C_WIDGET_BORDER));
	for (int i = 0; i < n; i++)
		draw_text(i ? FONT_SMALL : FONT_SMALL_BOLD, x + PX(10), y + PX(6) + i * lh, lines[i], strlen(lines[i]),
		          i ? COL(C_FG) : COL(C_LINK));
}

static void draw_flame(ProfDoc *b, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	ProfTree *t = &b->td;
	/* header: title, then buttons on the right */
	draw_header(b, x, y, w, "Flame graph");
	int ch = PX(22), cy = y + HDR_H + PX(6);
	int bx = x + PX(10);
	bx += chip(bx, cy, ch, b->icicle ? "Icicle (root on top)" : "Flame (root at the bottom)", H_ICICLE, 0, false,
	           "Flip the graph") + PX(6);
	if (b->zoom)
		bx += chip(bx, cy, ch, "Reset Zoom", H_UNZOOM, 0, true, "Show everything (Backspace zooms out one level)") + PX(6);
	char hint[200];
	if (b->find[0]) {
		if (b->find_val == 0)
			b->find_val = find_total(b, 0);
		char vb[48], pb[24];
		fmtv(b, b->find_val, vb, sizeof vb);
		fmt_pct(b->find_val, b->total, pb, sizeof pb);
		snprintf(hint, sizeof hint, "\"%s\" matches %s (%s)  -  Ctrl+F to change, Escape clears", b->find, vb, pb);
	} else
		snprintf(hint, sizeof hint, "Click a frame to zoom, double-click opens its source, Ctrl+F highlights");
	text_fit(FONT_SMALL, bx + PX(6), ty(cy, ch, FONT_SMALL), hint, x + w - bx - PX(16), COL(C_DESCRIPTION_FG));
	int top = y + HDR_H + ch + PX(12);
	int ah = y + h - top;
	geo.flame_y0 = top;
	geo.flame_h = ah;
	if (!t->n || t->val[0] <= 0) {
		const char *m = "Nothing to draw (no stacks with this metric)";
		draw_text(FONT_UI, x + PX(12), top + PX(6), m, strlen(m), COL(C_DESCRIPTION_FG));
		return;
	}
	int content = (int)(t->maxdepth + 1) * FRAME_H + PX(8);
	i64 maxs = VX_MAX((i64)content - ah, 0);
	b->fscroll = VX_CLAMP(b->fscroll, 0, maxs);
	int gx = x + PX(8), gw = w - PX(16);
	/* flame: root at the bottom of the content; scrolling moves up */
	int base = b->icicle ? top + PX(4) - (int)b->fscroll : top + ah - PX(4) + (int)b->fscroll;
	double zoff = b->foff[b->zoom], zval = VX_MAX(t->val[b->zoom], 1e-300);
	flame_hover = NONE;
	draw_clip(x, top, w, ah);
	flame_node(b, 0, gx, gw, base, zoff, zval, top, top + ah);
	draw_noclip();
	if (flame_hover != NONE) {
		u32 nd = flame_hover;
		char l1[400], l2[160], l3[160], l4[160], l5[600], vb[48], pb[24], pz[24];
		snprintf(l1, sizeof l1, "%s", nd ? fn_name(b, t->fn[nd]) : "all");
		fmtv(b, t->val[nd], vb, sizeof vb);
		fmt_pct(t->val[nd], b->total, pb, sizeof pb);
		snprintf(l2, sizeof l2, "Total: %s (%s of everything)", vb, pb);
		fmt_pct(t->val[nd], t->val[t->parent[nd] == NONE ? 0 : t->parent[nd]], pz, sizeof pz);
		snprintf(l3, sizeof l3, "%s of its caller", pz);
		fmtv(b, t->self[nd], vb, sizeof vb);
		fmt_pct(t->self[nd], b->total, pb, sizeof pb);
		snprintf(l4, sizeof l4, "Self: %s (%s)", vb, pb);
		fn_location(b, nd ? t->fn[nd] : NONE, l5, sizeof l5);
		const char *ls[5] = {l1, l2, l3, l4, l5};
		tooltip_box(app.hover_x, app.hover_y, ls, l5[0] ? 5 : 4, x + w, y + h);
	}
}

/* ---- hot lines ----------------------------------------------------------------------------------------- */

static void draw_lines(ProfDoc *b, u32 part, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	bool gcov = b->kind == PK_GCOV;
	y += draw_header(b, x, y, w, gcov ? "Lines" : "Hot lines");
	h -= HDR_H;
	/* a row of options */
	int ch = PX(22), cy = y + PX(6), bx = x + PX(10);
	if (gcov)
		bx += chip(bx, cy, ch, b->uncovered_only ? "Showing: not executed / partial" : "Showing: all lines", H_UNCOV, 0,
		           b->uncovered_only, "Only lines that never ran, or with branches never taken") + PX(8);
	bx += chip(bx, cy, ch, "Annotate Sources", H_ANNOTATE, 0, false,
	           "Colour source files by this profile (Profile: Annotate Source with Profile...)") + PX(8);
	const char *hint = "Double-click or Enter opens the line, annotated";
	text_fit(FONT_SMALL, bx + PX(4), ty(cy, ch, FONT_SMALL), hint, x + w - bx - PX(14), COL(C_DESCRIPTION_FG));
	y += ch + PX(12);
	h -= ch + PX(12);
	Col sc[] = {{"Location", PX(240), false, 0},
	            {"Function", PX(200), false, -1},
	            {gcov ? "Executed" : "Self", gcov ? PX(140) : PX(190), true, 1},
	            {gcov ? "Branches" : "Total (with callees)", gcov ? PX(110) : PX(190), true, gcov ? -1 : 2},
	            {"Source", 0, false, -1}};
	int cx[5], cw[5];
	col_layout(sc, 5, x, w, cx, cw);
	list_header(sc, 5, cx, cw, y, x, w, H_SORT_LN, b->ln_sort);
	int ly = y + app.row_h, rh = app.row_h;
	int vis = VX_MAX((h - app.row_h) / rh, 1);
	if (b->nlord && b->sel[part] >= b->nlord)
		b->sel[part] = b->nlord - 1;
	clamp_scroll(&b->scroll[part], b->nlord, vis);
	geo.list_y = ly;
	geo.list_h = h - app.row_h;
	geo.row_h = rh;
	double maxv = 0;
	for (u32 i = 0; i < b->ln.n; i++)
		maxv = VX_MAX(maxv, b->ln.self[i]);
	draw_clip(x, ly, w, h - app.row_h);
	if (!b->nlord) {
		const char *m = gcov ? "Every line ran" : "No line information (built without -g?)";
		draw_text(FONT_UI, x + PX(12), ly + PX(6), m, strlen(m), COL(C_DESCRIPTION_FG));
	}
	for (int r = 0; r <= vis && (u32)(b->scroll[part] + r) < b->nlord; r++) {
		u32 k = (u32)b->scroll[part] + (u32)r, i = b->lord[k];
		int ry = ly + r * rh;
		bool sel = k == b->sel[part];
		if (sel)
			draw_rect(x, ry, w, rh, app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
		else if (in_r(app.hover_x, app.hover_y, x, ry, w, rh))
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		u32 fg = sel && app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_FG) : COL(C_FG);
		/* heat swatch */
		double hf = gcov ? (maxv > 0 && b->ln.self[i] > 0 ? log1p(b->ln.self[i]) / log1p(maxv) : 0)
		                 : (b->total > 0 ? VX_MIN(b->ln.self[i] / VX_MAX(maxv, 1e-300), 1.0) : 0);
		u32 sw = gcov && (b->ln.flags[i] & PL_UNCOVERED) ? COL(C_ERROR) : heat_rgb(hf) | 0xFFu;
		if (b->ln.self[i] > 0 || gcov)
			draw_rect(cx[0] + PX(6), ry + PX(5), PX(4), rh - PX(10), sw);
		char loc[700];
		snprintf(loc, sizeof loc, "%s:%u", file_name(b, b->ln.file[i]), b->ln.line[i]);
		text_fit(FONT_UI, cx[0] + PX(16), ty(ry, rh, FONT_UI), loc, cw[0] - PX(20), fg);
		text_fit(FONT_UI, cx[1] + PX(8), ty(ry, rh, FONT_UI), fn_name(b, b->ln.fn[i]), cw[1] - PX(12),
		         COL(C_DESCRIPTION_FG));
		if (gcov) {
			char cb[40];
			if (b->ln.flags[i] & PL_UNCOVERED)
				snprintf(cb, sizeof cb, "never");
			else
				prof_fmt_count(b->ln.self[i], cb, sizeof cb);
			text_r(FONT_UI, cx[2] + cw[2] - PX(8), ty(ry, rh, FONT_UI), cb,
			       (b->ln.flags[i] & PL_UNCOVERED) ? COL(C_ERROR) : fg);
			if (b->ln.br_tot[i]) {
				snprintf(cb, sizeof cb, "%u/%u", b->ln.br_hit[i], b->ln.br_tot[i]);
				text_r(FONT_UI, cx[3] + cw[3] - PX(8), ty(ry, rh, FONT_UI), cb,
				       b->ln.br_hit[i] < b->ln.br_tot[i] ? COL(C_WARNING) : fg);
			}
		} else {
			value_cell(b, cx[2], ry, cw[2], rh, b->ln.self[i], b->total, true);
			value_cell(b, cx[3], ry, cw[3], rh, b->ln.total[i], b->total, false);
		}
		u32 sn;
		const char *s = source_line(b, b->ln.file[i], b->ln.line[i], &sn);
		if (sn)
			draw_text_fit(FONT_MONO, cx[4] + PX(8), ty(ry, rh, FONT_MONO), s, sn, cw[4] - PX(12), COL(C_EDITOR_FG));
	}
	draw_noclip();
}

/* ---- gcov: coverage per file ----------------------------------------------------------------------------- */

static void draw_files(ProfDoc *b, u32 part, int x, int y, int w, int h)
{
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	Prof *p = &b->p;
	y += draw_header(b, x, y, w, "Coverage");
	h -= HDR_H;
	Col c[] = {{"File", 0, false, -1}, {"Lines", PX(240), true, -1}, {"Functions", PX(170), true, -1},
	           {"Branches", PX(170), true, -1}};
	int cx[4], cw[4];
	col_layout(c, 4, x, w, cx, cw);
	list_header(c, 4, cx, cw, y, x, w, H_NONE, -1);
	int ly = y + app.row_h, rh = app.row_h + PX(6);
	int vis = VX_MAX((h - app.row_h) / rh, 1);
	u32 n = p->nfile;
	if (n && b->sel[part] >= n)
		b->sel[part] = n - 1;
	clamp_scroll(&b->scroll[part], n, vis);
	geo.list_y = ly;
	geo.list_h = h - app.row_h;
	geo.row_h = rh;
	draw_clip(x, ly, w, h - app.row_h);
	for (int r = 0; r <= vis && (u32)(b->scroll[part] + r) < n; r++) {
		u32 f = (u32)b->scroll[part] + (u32)r;
		int ry = ly + r * rh;
		bool sel = f == b->sel[part];
		if (sel)
			draw_rect(x, ry, w, rh, app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
		else if (in_r(app.hover_x, app.hover_y, x, ry, w, rh))
			draw_rect(x, ry, w, rh, COL(C_LIST_HOVER_BG));
		u32 lines = 0, hit = 0, bt = 0, bh = 0, fns = 0, fcalled = 0;
		for (u32 g = 0; g < p->ngl; g++)
			if (p->gl_file[g] == f) {
				lines++;
				hit += p->gl_count[g] > 0;
				bt += p->gl_br_tot[g];
				bh += p->gl_br_hit[g];
			}
		for (u32 k = 0; k < p->nfn; k++)
			if (p->fn_file[k] == f) {
				fns++;
				fcalled += p->fn_calls[k] > 0;
			}
		u32 fg = sel && app.focus == FOCUS_EDITOR ? COL(C_LIST_ACTIVE_FG) : COL(C_FG);
		const char *fname = file_name(b, f);
		file_icon_draw(fname, cx[0] + PX(8), ry + (rh - PX(16)) / 2, PX(16));
		text_fit(FONT_UI, cx[0] + PX(30), ty(ry, rh, FONT_UI), fname, cw[0] - PX(34),
		         f < b->src.nfile && b->src.abs[f] ? fg : COL(C_DESCRIPTION_FG));
		char t[96];
		double pc = lines ? 100.0 * hit / lines : 0;
		snprintf(t, sizeof t, "%u / %u  %.1f%%", hit, lines, pc);
		int bw = cw[1] - PX(16);
		draw_rect(cx[1] + PX(8), ry + rh - PX(6), bw, PX(3), (COL(C_ERROR) & 0xFFFFFF00u) | 0x90u);
		draw_rect(cx[1] + PX(8), ry + rh - PX(6), (int)(bw * pc / 100), PX(3), COL(C_GUTTER_ADDED));
		text_r(FONT_UI, cx[1] + cw[1] - PX(8), ty(ry, rh - PX(4), FONT_UI), t, fg);
		snprintf(t, sizeof t, "%u / %u", fcalled, fns);
		text_r(FONT_UI, cx[2] + cw[2] - PX(8), ty(ry, rh - PX(4), FONT_UI), fns ? t : "-", fg);
		snprintf(t, sizeof t, "%u / %u", bh, bt);
		text_r(FONT_UI, cx[3] + cw[3] - PX(8), ty(ry, rh - PX(4), FONT_UI), bt ? t : "-", fg);
	}
	draw_noclip();
}

/* ---- massif: memory over time ------------------------------------------------------------------------- */

#define C_AREA_HEAP 0x2E9E5BE0u
#define C_AREA_EXTRA 0xE0A030E0u
#define C_AREA_STACK 0x5B8DEFE0u

static void fmt_time(ProfDoc *b, double t, char *buf, size_t cap)
{
	const char *u = prof_str(&b->p, b->p.time_unit);
	if (!strcmp(u, "B"))
		prof_fmt_bytes(t, buf, cap);
	else if (!strcmp(u, "ms"))
		snprintf(buf, cap, t >= 1000 ? "%.2f s" : "%.0f ms", t >= 1000 ? t / 1000 : t);
	else if (t >= 1e9)
		snprintf(buf, cap, "%.2fG instr", t / 1e9);
	else if (t >= 1e6)
		snprintf(buf, cap, "%.1fM instr", t / 1e6);
	else if (t >= 1e3)
		snprintf(buf, cap, "%.1fk instr", t / 1e3);
	else
		snprintf(buf, cap, "%.0f instr", t);
}

static double nice_ceil(double v)
{
	if (v <= 0)
		return 1;
	double e = pow(10, floor(log10(v))), m = v / e;
	m = m <= 1 ? 1 : m <= 2 ? 2 : m <= 2.5 ? 2.5 : m <= 5 ? 5 : 10;
	return m * e;
}

static void select_snapshot(ProfDoc *b, u32 s)
{
	if (s == b->snap || s >= b->p.nsnap || b->p.sn_tree[s] == PSN_EMPTY)
		return;
	b->snap = s;
	agg_build(b);
	app.dirty = true;
}

static u32 snap_step(ProfDoc *b, int dir)
{
	Prof *p = &b->p;
	i64 s = b->snap == NONE ? 0 : (i64)b->snap;
	for (s += dir; s >= 0 && s < (i64)p->nsnap; s += dir)
		if (p->sn_tree[s] != PSN_EMPTY)
			return (u32)s;
	return b->snap;
}

static u32 nearest_snap(ProfDoc *b, double t, bool detailed)
{
	Prof *p = &b->p;
	u32 best = NONE;
	double bd = 0;
	for (u32 i = 0; i < p->nsnap; i++) {
		if (detailed && p->sn_tree[i] == PSN_EMPTY)
			continue;
		double d = fabs((double)p->sn_time[i] - t);
		if (best == NONE || d < bd) {
			best = i;
			bd = d;
		}
	}
	return best;
}

static void side_row(int x, int *y, int w, const char *k, const char *v)
{
	int lh = font_m[FONT_SMALL].line + PX(5);
	draw_text(FONT_SMALL, x + PX(14), *y, k, strlen(k), COL(C_DESCRIPTION_FG));
	text_r(FONT_SMALL, x + w - PX(14), *y, v, COL(C_FG));
	*y += lh;
}

static void draw_graph(ProfDoc *b, int x, int y, int w, int h)
{
	Prof *p = &b->p;
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	draw_header(b, x, y, w, "Memory over time");
	y += HDR_H;
	h -= HDR_H;
	int side = w < PX(760) ? 0 : VX_CLAMP(w / 3, PX(280), PX(400));
	int cw = w - side;
	/* legend */
	int lx = x + PX(16), lyy = y + PX(10);
	const char *lg[3] = {"Useful heap", "Extra heap (allocator overhead)", "Stacks"};
	u32 lc[3] = {C_AREA_HEAP, C_AREA_EXTRA, C_AREA_STACK};
	for (int i = 0; i < 3; i++) {
		draw_rect(lx, lyy + PX(3), PX(10), PX(10), lc[i] | 0xFFu);
		lx = draw_text(FONT_SMALL, lx + PX(16), lyy, lg[i], strlen(lg[i]), COL(C_FG)) + PX(18);
	}
	int x0 = x + PX(92), x1 = x + cw - PX(24), y0 = y + PX(40), y1 = y + h - PX(44);
	geo.chart_x0 = x0;
	geo.chart_x1 = x1;
	geo.chart_y0 = y0;
	geo.chart_y1 = y1;
	if (x1 - x0 > PX(60) && y1 - y0 > PX(60) && p->nsnap) {
		double vmax = 0, tmax = (double)p->sn_time[p->nsnap - 1];
		for (u32 i = 0; i < p->nsnap; i++)
			vmax = VX_MAX(vmax, (double)(p->sn_heap[i] + p->sn_extra[i] + p->sn_stacks[i]));
		/* round the axis in the unit it is labelled in (KiB, MiB, GiB) */
		double unit = 1;
		while (vmax / unit >= 1024 && unit < 1024.0 * 1024 * 1024)
			unit *= 1024;
		double vtop = nice_ceil(vmax / unit) * unit;
		if (tmax <= 0)
			tmax = 1;
		int H = y1 - y0;
		char lb[64];
		for (int k = 0; k <= 4; k++) {
			int gy = y1 - H * k / 4;
			draw_rect(x0, gy, x1 - x0, PX(1), COL(C_INDENT_GUIDE));
			prof_fmt_bytes(vtop * k / 4, lb, sizeof lb);
			text_r(FONT_SMALL, x0 - PX(8), gy - font_m[FONT_SMALL].line / 2, lb, COL(C_DESCRIPTION_FG));
		}
		/* stacked areas, one column per pixel */
		u32 si = 0;
		for (int px = x0; px < x1; px++) {
			double t = (double)(px - x0) / (double)(x1 - x0) * tmax;
			while (si + 1 < p->nsnap && (double)p->sn_time[si + 1] <= t)
				si++;
			double hp = (double)p->sn_heap[si], ex = (double)p->sn_extra[si], st = (double)p->sn_stacks[si];
			if (si + 1 < p->nsnap && p->sn_time[si + 1] > p->sn_time[si]) {
				double f = (t - (double)p->sn_time[si]) / (double)(p->sn_time[si + 1] - p->sn_time[si]);
				f = VX_CLAMP(f, 0.0, 1.0);
				hp += f * ((double)p->sn_heap[si + 1] - hp);
				ex += f * ((double)p->sn_extra[si + 1] - ex);
				st += f * ((double)p->sn_stacks[si + 1] - st);
			}
			int a = (int)(hp / vtop * H), c2 = (int)((hp + ex) / vtop * H), d2 = (int)((hp + ex + st) / vtop * H);
			if (a > 0)
				draw_rect(px, y1 - a, 1, a, C_AREA_HEAP);
			if (c2 > a)
				draw_rect(px, y1 - c2, 1, c2 - a, C_AREA_EXTRA);
			if (d2 > c2)
				draw_rect(px, y1 - d2, 1, d2 - c2, C_AREA_STACK);
		}
		draw_rect(x0, y0, PX(1), H, COL(C_DESCRIPTION_FG));
		draw_rect(x0, y1, x1 - x0, PX(1), COL(C_DESCRIPTION_FG));
		/* snapshot ticks: detailed ones taller; the peak; the selection */
		for (u32 i = 0; i < p->nsnap; i++) {
			int xs = x0 + (int)((double)p->sn_time[i] / tmax * (x1 - x0));
			bool det = p->sn_tree[i] != PSN_EMPTY;
			draw_rect(xs, y1 + PX(1), PX(1), det ? PX(7) : PX(3), det ? COL(C_FG) : COL(C_DESCRIPTION_FG));
		}
		if (p->peak_snap != NONE) {
			u32 s = p->peak_snap;
			int xs = x0 + (int)((double)p->sn_time[s] / tmax * (x1 - x0));
			draw_rect(xs, y0, PX(1), H, (COL(C_ERROR) & 0xFFFFFF00u) | 0xA0u);
			char pk[96], vb[48];
			prof_fmt_bytes((double)(p->sn_heap[s] + p->sn_extra[s] + p->sn_stacks[s]), vb, sizeof vb);
			snprintf(pk, sizeof pk, "peak %s", vb);
			int pw = text_w(FONT_SMALL_BOLD, pk);
			int px = xs + PX(4) + pw > x1 ? xs - PX(4) - pw : xs + PX(4);
			draw_text(FONT_SMALL_BOLD, px, y0 - font_m[FONT_SMALL].line - PX(2), pk, strlen(pk), COL(C_ERROR));
		}
		if (b->snap != NONE && b->snap != p->peak_snap) {
			int xs = x0 + (int)((double)p->sn_time[b->snap] / tmax * (x1 - x0));
			draw_rect(xs - PX(1), y0, PX(2), H, COL(C_FOCUS_BORDER));
			char sl[48];
			snprintf(sl, sizeof sl, "snapshot %u", b->snap);
			draw_text(FONT_SMALL_BOLD, xs + PX(4), y0 + PX(2), sl, strlen(sl), COL(C_FOCUS_BORDER));
		}
		for (int k = 0; k <= 4; k++) {
			int gx = x0 + (x1 - x0) * k / 4;
			fmt_time(b, tmax * k / 4, lb, sizeof lb);
			int tw = text_w(FONT_SMALL, lb);
			draw_text(FONT_SMALL, VX_CLAMP(gx - tw / 2, x0, x1 - tw), y1 + PX(10), lb, strlen(lb), COL(C_DESCRIPTION_FG));
		}
		const char *hint = "Click a detailed snapshot (tall tick) to look at it; Left / Right step between them";
		text_fit(FONT_SMALL, x0, y1 + PX(26), hint, x1 - x0, COL(C_DESCRIPTION_FG));
		/* hover: the nearest snapshot */
		if (in_r(app.hover_x, app.hover_y, x0, y0, x1 - x0, H + PX(10))) {
			double t = (double)(app.hover_x - x0) / (double)(x1 - x0) * tmax;
			u32 s = nearest_snap(b, t, false);
			if (s != NONE) {
				int xs = x0 + (int)((double)p->sn_time[s] / tmax * (x1 - x0));
				draw_rect(xs, y0, PX(1), H, (COL(C_FG) & 0xFFFFFF00u) | 0x70u);
				char l1[64], l2[96], l3[96], l4[96], l5[96], l6[96], vb[48];
				snprintf(l1, sizeof l1, "Snapshot %u%s", s,
				         p->sn_tree[s] == PSN_PEAK ? " (peak)" : p->sn_tree[s] == PSN_DETAILED ? " (detailed)" : "");
				fmt_time(b, (double)p->sn_time[s], vb, sizeof vb);
				snprintf(l2, sizeof l2, "Time: %s", vb);
				prof_fmt_bytes((double)p->sn_heap[s], vb, sizeof vb);
				snprintf(l3, sizeof l3, "Useful heap: %s", vb);
				prof_fmt_bytes((double)p->sn_extra[s], vb, sizeof vb);
				snprintf(l4, sizeof l4, "Extra heap: %s", vb);
				prof_fmt_bytes((double)p->sn_stacks[s], vb, sizeof vb);
				snprintf(l5, sizeof l5, "Stacks: %s", vb);
				prof_fmt_bytes((double)(p->sn_heap[s] + p->sn_extra[s] + p->sn_stacks[s]), vb, sizeof vb);
				snprintf(l6, sizeof l6, "Total: %s", vb);
				const char *ls[6] = {l1, l2, l3, l4, l5, l6};
				tooltip_box(app.hover_x, app.hover_y, ls, 6, x + cw, y + h);
			}
		}
	}
	/* side panel */
	geo.side_x = side ? x + cw : x + w;
	if (!side)
		return;
	int sx = x + cw;
	draw_rect(sx, y, side, h, COL(C_SIDEBAR_BG));
	draw_rect(sx, y, PX(1), h, COL(C_SIDEBAR_BORDER));
	int cy = y + PX(10);
	char t[160], vb[48];
	u32 s = b->snap;
	if (s != NONE && s < p->nsnap) {
		snprintf(t, sizeof t, "SNAPSHOT %u%s", s, s == p->peak_snap ? " - PEAK" : "");
		draw_text(FONT_SMALL_BOLD, sx + PX(14), cy, t, strlen(t), COL(C_SECTION_HEADER_FG));
		cy += PX(22);
		fmt_time(b, (double)p->sn_time[s], vb, sizeof vb);
		side_row(sx, &cy, side, "Time", vb);
		prof_fmt_bytes((double)p->sn_heap[s], vb, sizeof vb);
		side_row(sx, &cy, side, "Useful heap", vb);
		prof_fmt_bytes((double)p->sn_extra[s], vb, sizeof vb);
		side_row(sx, &cy, side, "Extra heap", vb);
		prof_fmt_bytes((double)p->sn_stacks[s], vb, sizeof vb);
		side_row(sx, &cy, side, "Stacks", vb);
		prof_fmt_bytes((double)(p->sn_heap[s] + p->sn_extra[s] + p->sn_stacks[s]), vb, sizeof vb);
		side_row(sx, &cy, side, "Total", vb);
		cy += PX(12);
	}
	draw_text(FONT_SMALL_BOLD, sx + PX(14), cy, "TOP ALLOCATION SITES", 20, COL(C_SECTION_HEADER_FG));
	cy += PX(22);
	/* the ten largest by self */
	u32 best[10], nb = 0;
	for (u32 f = 0; f < p->nfn; f++) {
		if (b->fself[f] <= 0)
			continue;
		u32 k = nb < 10 ? nb++ : 10;
		if (k == 10) {
			if (b->fself[f] <= b->fself[best[9]])
				continue;
			k = 9;
		}
		while (k > 0 && b->fself[best[k - 1]] < b->fself[f]) {
			best[k] = best[k - 1];
			k--;
		}
		best[k] = f;
	}
	int rh = app.row_h;
	for (u32 i = 0; i < nb && cy + rh < y + h - PX(90); i++) {
		u32 f = best[i];
		bool hov = in_r(app.hover_x, app.hover_y, sx, cy, side, rh);
		if (hov)
			draw_rect(sx + PX(1), cy, side - PX(1), rh, COL(C_LIST_HOVER_BG));
		double fr = b->total > 0 ? b->fself[f] / b->total : 0;
		draw_rect(sx + PX(14), cy + rh - PX(4), (int)((side - PX(28)) * fr), PX(2), flame_col(b->kind, fn_name(b, f)));
		char pb[24];
		prof_fmt_bytes(b->fself[f], vb, sizeof vb);
		fmt_pct(b->fself[f], b->total, pb, sizeof pb);
		snprintf(t, sizeof t, "%s  %s", vb, pb);
		int vw = text_w(FONT_SMALL, t);
		text_fit(FONT_SMALL, sx + PX(14), ty(cy, rh, FONT_SMALL), fn_name(b, f), side - vw - PX(36), COL(C_FG));
		text_r(FONT_SMALL, sx + side - PX(14), ty(cy, rh, FONT_SMALL), t, COL(C_DESCRIPTION_FG));
		hit_add(sx, cy, side, rh, H_SITE, (int)f);
		cy += rh;
	}
	if (!nb) {
		const char *m = s == NONE ? "No detailed snapshot" : "Nothing allocated in this snapshot";
		draw_text(FONT_SMALL, sx + PX(14), cy, m, strlen(m), COL(C_DESCRIPTION_FG));
		cy += rh;
	}
	cy += PX(12);
	draw_text(FONT_SMALL_BOLD, sx + PX(14), cy, "PROGRAM", 7, COL(C_SECTION_HEADER_FG));
	cy += PX(22);
	text_fit(FONT_MONO, sx + PX(14), cy, prof_str(p, p->cmd), side - PX(28), COL(C_FG));
	cy += font_m[FONT_MONO].line + PX(4);
	u32 det = 0;
	for (u32 i = 0; i < p->nsnap; i++)
		det += p->sn_tree[i] != PSN_EMPTY;
	snprintf(t, sizeof t, "%u snapshots, %u detailed", p->nsnap, det);
	draw_text(FONT_SMALL, sx + PX(14), cy, t, strlen(t), COL(C_DESCRIPTION_FG));
	cy += font_m[FONT_SMALL].line + PX(4);
	if (p->title && *prof_str(p, p->title)) {
		snprintf(t, sizeof t, "massif %s", prof_str(p, p->title));
		text_fit(FONT_SMALL, sx + PX(14), cy, t, side - PX(28), COL(C_DESCRIPTION_FG));
	}
}

/* ---- dispatch -------------------------------------------------------------------------------------------- */

void profview_draw(View *v, int x, int y, int w, int h, bool focused)
{
	(void)focused;
	ProfDoc *b = pd_get(v->doc);
	nhit = 0;
	geo.x = x;
	geo.y = y;
	geo.w = w;
	geo.h = h;
	geo.nb_h = 0;
	if (v->nparts != b->nparts)
		profview_init(v);
	if (draw_state(b, x, y, w, h) || !pd_ready(b))
		return;
	u32 part = v->part < b->nparts ? b->parts[v->part] : PART_TOP;
	switch (part) {
	case PART_GRAPH: draw_graph(b, x, y, w, h); break;
	case PART_TOP: draw_top(b, part, x, y, w, h); break;
	case PART_TREE: draw_tree(b, part, x, y, w, h); break;
	case PART_FLAME: draw_flame(b, x, y, w, h); break;
	case PART_LINES: draw_lines(b, part, x, y, w, h); break;
	case PART_FILES: draw_files(b, part, x, y, w, h); break;
	}
	if (b->rc != 0 && b->p.err[0]) {
		/* partial data: say what went wrong at the bottom */
		int eh = app.row_h;
		draw_rect(x, y + h - eh, w, eh, (COL(C_ERROR) & 0xFFFFFF00u) | 0x40u);
		text_fit(FONT_SMALL, x + PX(10), ty(y + h - eh, eh, FONT_SMALL), b->p.err, w - PX(20), COL(C_FG));
	}
}

/* ---- annotation of source files ------------------------------------------------------------------------- */

static u32 ann_pd = NONE; /* the profile annotating sources (a pdoc index) */
static u32 ann_serial = 1;

typedef struct {
	u32 doc, key, n;
	bool none;
	double *self, *tot;
	u8 *flags;
	u16 *bt, *bh;
	u8 *rank;
	u32 *hot, nhot; /* lines by cost, hottest first */
	double mself, mtot, total;
} Ann;

static Ann ann[8];
static u32 ann_next;

static void ann_free(Ann *a)
{
	free(a->self);
	free(a->tot);
	free(a->flags);
	free(a->bt);
	free(a->bh);
	free(a->rank);
	free(a->hot);
	memset(a, 0, sizeof *a);
	a->doc = NONE;
}

static void ann_reset(void)
{
	for (int i = 0; i < 8; i++)
		ann_free(&ann[i]);
	ann_serial++;
	app.dirty = true;
}

static const Ann *sort_ann;
static int cmp_hot(const void *a, const void *c)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)c;
	double vx = sort_ann->self[x] > 0 ? sort_ann->self[x] : sort_ann->tot[x] * 1e-9;
	double vy = sort_ann->self[y] > 0 ? sort_ann->self[y] : sort_ann->tot[y] * 1e-9;
	if (vx != vy)
		return vx > vy ? -1 : 1;
	return x < y ? -1 : x > y;
}

static const Ann *ann_for(u32 d)
{
	if (ann_pd == NONE || d >= MAX_DOCS || !app.docs.path[d] || app.docs.bin[d])
		return NULL;
	ProfDoc *b = &pdoc[ann_pd];
	if (!b->used || !pd_ready(b))
		return NULL;
	u32 key = ann_serial * 2654435761u ^ b->gen * 40503u ^ (u32)ann_pd;
	for (int i = 0; i < 8; i++)
		if (ann[i].doc == d && ann[i].key == key)
			return ann[i].none ? NULL : &ann[i];
	Ann *a = &ann[ann_next++ % 8];
	ann_free(a);
	a->doc = d;
	a->key = key;
	a->none = true;
	char *cp = canon(app.docs.path[d]);
	u32 file = NONE;
	for (u32 f = 0; f < b->src.nfile; f++)
		if (b->src.abs[f] && !strcmp(b->src.abs[f], cp))
			file = f;
	free(cp);
	if (file == NONE)
		return NULL;
	u32 n = text_lines(&app.docs.text[d]);
	a->n = n;
	a->self = vx_alloc((n + 1) * sizeof(double));
	a->tot = vx_alloc((n + 1) * sizeof(double));
	a->flags = vx_alloc(n + 1);
	a->bt = vx_alloc((n + 1) * sizeof(u16));
	a->bh = vx_alloc((n + 1) * sizeof(u16));
	a->rank = vx_alloc(n + 1);
	a->hot = vx_alloc((n + 1) * sizeof(u32));
	a->total = b->total;
	bool any = false;
	for (u32 i = 0; i < b->ln.n; i++) {
		if (b->ln.file[i] != file || !b->ln.line[i] || b->ln.line[i] > n)
			continue;
		u32 l = b->ln.line[i] - 1;
		a->self[l] += b->ln.self[i];
		a->tot[l] = VX_MAX(a->tot[l], b->ln.total[i]);
		a->flags[l] |= b->ln.flags[i];
		a->bt[l] = b->ln.br_tot[i];
		a->bh[l] = b->ln.br_hit[i];
		any = true;
	}
	if (!any)
		return NULL;
	for (u32 l = 0; l < n; l++) {
		a->mself = VX_MAX(a->mself, a->self[l]);
		a->mtot = VX_MAX(a->mtot, a->tot[l]);
		if (a->self[l] > 0 || a->tot[l] > 0)
			a->hot[a->nhot++] = l;
	}
	sort_ann = a;
	qsort(a->hot, a->nhot, sizeof(u32), cmp_hot);
	for (u32 r = 0; r < a->nhot && r < 5; r++)
		if (a->self[a->hot[r]] > 0)
			a->rank[a->hot[r]] = (u8)(r + 1);
	a->none = false;
	return a;
}

static u32 ann_pd_of(void) { return ann_pd; }
static void ann_set(ProfDoc *b);
static void ann_auto(ProfDoc *b) { ann_set(b); }
static void ann_drop(void)
{
	ann_pd = NONE;
	ann_reset();
}

static ProfDoc *ann_doc(void) { return ann_pd == NONE ? NULL : &pdoc[ann_pd]; }

/* heat of a line, 0..1 (gcov: log of the count) */
static double ann_heat(const Ann *a, u32 l, bool *callsite)
{
	ProfDoc *b = ann_doc();
	*callsite = false;
	if (b && b->kind == PK_GCOV)
		return a->mself > 0 && a->self[l] > 0 ? log1p(a->self[l]) / log1p(a->mself) : 0;
	if (a->self[l] > 0)
		return a->mself > 0 ? sqrt(a->self[l] / a->mself) : 0;
	if (a->tot[l] > 0) {
		*callsite = true;
		return a->mtot > 0 ? sqrt(a->tot[l] / a->mtot) : 0;
	}
	return 0;
}

int profann_gutter_w(u32 d) { return ann_for(d) ? PX(78) : 0; }

void profann_lines(u32 d, u32 first, u32 last, int text_x, int text_w, int y0, int lh)
{
	const Ann *a = ann_for(d);
	if (!a)
		return;
	ProfDoc *b = ann_doc();
	bool gcov = b->kind == PK_GCOV;
	for (u32 l = first; l < last && l < a->n; l++) {
		int ly = y0 + (int)(l - first) * lh;
		bool cs;
		double f = ann_heat(a, l, &cs);
		if (gcov && (a->flags[l] & PL_UNCOVERED)) {
			draw_rect(text_x, ly, text_w, lh, (COL(C_ERROR) & 0xFFFFFF00u) | 0x26u);
			continue;
		}
		if (f <= 0)
			continue;
		if (cs) /* a call site: the cost is in what it calls */
			draw_rect(text_x, ly, PX(3), lh, heat_rgb(f) | 0xC0u);
		else
			draw_rect(text_x, ly, text_w, lh, heat_rgb(f) | (u32)(gcov ? 0x18 + f * 0x40 : 0x22 + f * 0x66));
	}
}

static void short_count(double v, char *b, size_t cap)
{
	if (v >= 1e9)
		snprintf(b, cap, "%.1fG", v / 1e9);
	else if (v >= 1e6)
		snprintf(b, cap, "%.1fM", v / 1e6);
	else if (v >= 1e4)
		snprintf(b, cap, "%.0fk", v / 1e3);
	else
		snprintf(b, cap, "%.0f", v);
}

void profann_gutter(u32 d, u32 first, u32 last, int x, int y0, int lh)
{
	const Ann *a = ann_for(d);
	if (!a)
		return;
	ProfDoc *b = ann_doc();
	bool gcov = b->kind == PK_GCOV;
	int w = PX(78);
	draw_rect(x + w - PX(1), y0, PX(1), (int)(last - first + 1) * lh, COL(C_INDENT_GUIDE));
	for (u32 l = first; l < last && l < a->n; l++) {
		int ly = y0 + (int)(l - first) * lh;
		bool cs;
		double f = ann_heat(a, l, &cs);
		char t[32];
		u32 fg = COL(C_LINE_NUMBER);
		int font = FONT_SMALL;
		if (gcov) {
			if (a->flags[l] & PL_UNCOVERED) {
				snprintf(t, sizeof t, "never");
				fg = COL(C_ERROR);
				draw_rect(x + w - PX(4), ly, PX(3), lh, COL(C_ERROR));
			} else if (a->flags[l] & PL_CODE) {
				short_count(a->self[l], t, sizeof t);
				draw_rect(x + PX(2), ly + PX(1), w - PX(6), lh - PX(2), heat_rgb(f) | (u32)(0x30 + f * 0x90));
				fg = COL(C_FG);
			} else
				continue;
			if (a->bt[l] && a->bh[l] < a->bt[l])
				draw_rect(x + PX(2), ly, PX(3), lh, COL(C_WARNING));
		} else {
			if (f <= 0)
				continue;
			double v = cs ? a->tot[l] : a->self[l];
			double pc = a->total > 0 ? 100.0 * v / a->total : 0;
			snprintf(t, sizeof t, cs ? "(%.*f%%)" : "%.*f%%", pc >= 10 ? 1 : 2, pc);
			if (!cs) {
				draw_rect(x + PX(2), ly + PX(1), w - PX(6), lh - PX(2), heat_rgb(f) | (u32)(0x50 + f * 0xA0));
				fg = f > 0.55 ? 0xFFFFFFFFu : 0x1A1A1AFFu;
			}
			if (a->rank[l]) {
				font = FONT_SMALL_BOLD;
				char r[8];
				snprintf(r, sizeof r, "%u", a->rank[l]);
				draw_text(FONT_SMALL_BOLD, x + PX(5), ty(ly, lh, FONT_SMALL_BOLD), r, strlen(r), fg);
			}
		}
		text_r(font, x + w - PX(7), ty(ly, lh, font), t, fg);
		if (in_r(app.hover_x, app.hover_y, x, ly, w, lh)) {
			char tip[400], vb[48], tb[48], pb[24], pt[24];
			if (gcov) {
				if (a->flags[l] & PL_UNCOVERED)
					snprintf(tip, sizeof tip, "Never executed");
				else
					snprintf(tip, sizeof tip, "Executed %.0f times%s", a->self[l], "");
				if (a->bt[l]) {
					size_t k = strlen(tip);
					snprintf(tip + k, sizeof tip - k, "  -  branches taken: %u of %u", a->bh[l], a->bt[l]);
				}
			} else {
				fmtv(b, a->self[l], vb, sizeof vb);
				fmtv(b, a->tot[l], tb, sizeof tb);
				fmt_pct(a->self[l], a->total, pb, sizeof pb);
				fmt_pct(a->tot[l], a->total, pt, sizeof pt);
				char rk[40] = "";
				if (a->rank[l])
					snprintf(rk, sizeof rk, "Hottest line #%u  -  ", a->rank[l]);
				snprintf(tip, sizeof tip, "%sSelf: %s (%s)  -  with callees: %s (%s)", rk, vb, pb, tb, pt);
			}
			ui_tip(x, ly, w, lh, tip);
		}
	}
}

void profann_ruler(u32 d, int x, int y, int w, int h, u32 nl)
{
	const Ann *a = ann_for(d);
	if (!a || !nl)
		return;
	ProfDoc *b = ann_doc();
	for (u32 l = 0; l < a->n; l++) {
		bool cs;
		double f = ann_heat(a, l, &cs);
		bool unc = b->kind == PK_GCOV && (a->flags[l] & PL_UNCOVERED);
		if ((f < 0.15 || cs) && !unc)
			continue;
		int my = y + (int)((i64)l * h / (i64)nl);
		draw_rect(x, my, w, PX(3), unc ? COL(C_ERROR) : heat_rgb(f) | 0xFFu);
	}
}

static void ann_set(ProfDoc *b)
{
	u32 i = (u32)(b - pdoc);
	if (ann_pd != i) {
		ann_pd = i;
		ann_reset();
	}
}

void profann_status(u32 d, char *buf, size_t cap)
{
	buf[0] = 0;
	const Ann *a = ann_for(d);
	if (!a)
		return;
	ProfDoc *b = ann_doc();
	snprintf(buf, cap, "%s: %s", b->kind == PK_GCOV ? "Coverage" : "Profile", app.docs.rel[ann_pd] ? app.docs.rel[ann_pd] : "");
}

/* ---- opening sources ----------------------------------------------------------------------------------- */

static void open_src(ProfDoc *b, u32 file, u32 line)
{
	if (file == NONE) {
		ui_status("No source location for this entry");
		return;
	}
	if (file >= b->src.nfile || !b->src.abs[file]) {
		ui_status("Source not found: %s", prof_str(&b->p, b->p.file_path[file]));
		return;
	}
	ann_set(b);
	open_file(b->src.abs[file], false, line ? line - 1 : UINT32_MAX, 0);
}

static void open_fn(ProfDoc *b, u32 f)
{
	if (f == NONE)
		return;
	/* the function's hottest line when it has one, else where it starts */
	u32 best = NONE;
	for (u32 i = 0; i < b->ln.n; i++)
		if (b->ln.fn[i] == f && b->ln.self[i] > 0 && (best == NONE || b->ln.self[i] > b->ln.self[best]))
			best = i;
	if (best != NONE && b->kind != PK_GCOV)
		open_src(b, b->ln.file[best], b->ln.line[best]);
	else
		open_src(b, b->p.fn_file[f], b->p.fn_line[f]);
}

static void select_fn(ProfDoc *b, View *v, u32 f)
{
	for (u32 i = 0; i < b->nparts; i++)
		if (b->parts[i] == PART_TOP)
			v->part = i;
	b->sel_fn = f;
	for (u32 i = 0; i < b->ntop; i++)
		if (b->top[i] == f) {
			b->sel[PART_TOP] = i;
			i64 vis = VX_MAX((geo.list_h ? geo.list_h : PX(300)) / app.row_h - 1, 1);
			if ((i64)i < b->scroll[PART_TOP] || (i64)i >= b->scroll[PART_TOP] + vis)
				b->scroll[PART_TOP] = (i64)i - vis / 3;
		}
}

/* the function the current part points at */
static u32 current_fn(ProfDoc *b, View *v)
{
	u32 part = b->parts[v->part];
	switch (part) {
	case PART_TOP: return b->sel[part] < b->ntop ? b->top[b->sel[part]] : NONE;
	case PART_TREE: rows_update(b); return b->sel[part] < b->nrows ? b->bu.fn[b->rows[b->sel[part]]] : NONE;
	case PART_FLAME: return b->zoom ? b->td.fn[b->zoom] : NONE;
	case PART_LINES: return b->sel[part] < b->nlord ? b->ln.fn[b->lord[b->sel[part]]] : NONE;
	default: return NONE;
	}
}

static void activate(ProfDoc *b, View *v)
{
	u32 part = b->parts[v->part];
	if (part == PART_LINES && b->sel[part] < b->nlord) {
		u32 i = b->lord[b->sel[part]];
		open_src(b, b->ln.file[i], b->ln.line[i]);
	} else if (part == PART_FILES && b->sel[part] < b->p.nfile) {
		/* the first line that never ran, else the top */
		u32 f = b->sel[part], line = 0;
		for (u32 g = 0; g < b->p.ngl; g++)
			if (b->p.gl_file[g] == f && (b->p.gl_flags[g] & PL_UNCOVERED)) {
				line = b->p.gl_line[g];
				break;
			}
		open_src(b, f, line);
	} else if (part == PART_GRAPH) {
		for (u32 i = 0; i < b->nparts; i++)
			if (b->parts[i] == PART_TOP)
				v->part = i;
	} else
		open_fn(b, current_fn(b, v));
}

/* ---- keys ------------------------------------------------------------------------------------------------ */

static u32 list_len(ProfDoc *b, u32 part)
{
	switch (part) {
	case PART_TOP: return b->ntop;
	case PART_TREE: rows_update(b); return b->nrows;
	case PART_LINES: return b->nlord;
	case PART_FILES: return b->p.nfile;
	default: return 0;
	}
}

static void prompt(int kind, const char *title, const char *init)
{
	app.quick = QUICK_PROMPT;
	app.quick_prompt_kind = kind;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, init ? init : "", init ? strlen(init) : 0);
	app.focus = FOCUS_QUICK;
}

bool profview_key(View *v, const Event *e)
{
	ProfDoc *b = pd_get(v->doc);
	app.dirty = true;
	if (!pd_ready(b))
		return true;
	bool ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
	if (e->key == SDLK_TAB && !ctrl) {
		v->part = (v->part + (shift ? b->nparts - 1 : 1)) % b->nparts;
		return true;
	}
	u32 part = b->parts[v->part];
	if (part == PART_GRAPH) {
		switch (e->key) {
		case SDLK_LEFT: select_snapshot(b, snap_step(b, -1)); return true;
		case SDLK_RIGHT: select_snapshot(b, snap_step(b, 1)); return true;
		case SDLK_HOME:
		case SDLK_END:
			for (u32 i = 0; i < b->p.nsnap; i++) {
				u32 k = e->key == SDLK_HOME ? i : b->p.nsnap - 1 - i;
				if (b->p.sn_tree[k] != PSN_EMPTY) {
					select_snapshot(b, k);
					break;
				}
			}
			return true;
		case SDLK_P: select_snapshot(b, b->p.peak_snap); return true;
		case SDLK_RETURN: case SDLK_KP_ENTER: activate(b, v); return true;
		default: return false;
		}
	}
	if (part == PART_FLAME) {
		switch (e->key) {
		case SDLK_BACKSPACE:
			if (b->zoom)
				b->zoom = b->td.parent[b->zoom] == NONE ? 0 : b->td.parent[b->zoom];
			return true;
		case SDLK_ESCAPE:
			if (b->find[0]) {
				b->find[0] = 0;
				b->find_val = 0;
			} else
				b->zoom = 0;
			return true;
		case SDLK_UP: b->fscroll += b->icicle ? -FRAME_H : FRAME_H; return true;
		case SDLK_DOWN: b->fscroll += b->icicle ? FRAME_H : -FRAME_H; return true;
		case SDLK_RETURN: case SDLK_KP_ENTER: activate(b, v); return true;
		default: return false;
		}
	}
	u32 n = list_len(b, part);
	i64 vis = VX_MAX(geo.list_h / VX_MAX(geo.row_h, 1) - 1, 1);
	i64 s = b->sel[part];
	switch (e->key) {
	case SDLK_UP: s--; break;
	case SDLK_DOWN: s++; break;
	case SDLK_PAGEUP: s -= vis; break;
	case SDLK_PAGEDOWN: s += vis; break;
	case SDLK_HOME: s = 0; break;
	case SDLK_END: s = (i64)n - 1; break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if (part == PART_TREE && b->sel[part] < b->nrows && !shift) {
			u32 nd = b->rows[b->sel[part]];
			if (b->bu.child[nd] != NONE && !ctrl) {
				b->open[nd] ^= 1;
				b->rows_ok = false;
				return true;
			}
		}
		if (part == PART_TREE && shift && b->sel[part] < b->nrows) {
			open_all(b, b->rows[b->sel[part]], 0);
			b->rows_ok = false;
			return true;
		}
		activate(b, v);
		return true;
	case SDLK_RIGHT:
	case SDLK_LEFT:
		if (part != PART_TREE || b->sel[part] >= b->nrows)
			return false;
		{
			u32 nd = b->rows[b->sel[part]];
			if (e->key == SDLK_RIGHT) {
				if (b->bu.child[nd] == NONE)
					return true;
				if (!b->open[nd]) {
					b->open[nd] = 1;
					b->rows_ok = false;
				} else
					s++;
			} else if (b->open[nd] && b->bu.child[nd] != NONE) {
				b->open[nd] = 0;
				b->rows_ok = false;
			} else {
				u32 par = b->bu.parent[nd];
				for (u32 i = 0; i < b->nrows; i++)
					if (b->rows[i] == par)
						s = i;
			}
		}
		break;
	default: return false;
	}
	if (!n)
		return true;
	b->sel[part] = (u32)VX_CLAMP(s, 0, (i64)n - 1);
	keep_visible(&b->scroll[part], b->sel[part], (int)vis);
	if (part == PART_TOP)
		b->sel_fn = b->top[b->sel[part]];
	return true;
}

/* ---- mouse ----------------------------------------------------------------------------------------------- */

static void hit_run(ProfDoc *b, View *v, int act, int arg)
{
	switch (act) {
	case H_METRIC:
		b->met = (b->met + 1) % VX_MAX(b->p.nmet, 1u);
		agg_build(b);
		ann_reset();
		ui_status("Metric: %s", prof_str(&b->p, b->p.met_name[b->met]));
		break;
	case H_SORT_TOP: {
		u32 f = b->sel[PART_TOP] < b->ntop ? b->top[b->sel[PART_TOP]] : NONE;
		b->top_sort = arg;
		sort_top(b);
		if (f != NONE)
			select_fn(b, v, f);
		break;
	}
	case H_SORT_LN:
		b->ln_sort = arg;
		sort_lines(b);
		b->sel[PART_LINES] = 0;
		b->scroll[PART_LINES] = 0;
		break;
	case H_ICICLE:
		b->icicle = !b->icicle;
		b->fscroll = 0;
		break;
	case H_UNZOOM: b->zoom = 0; break;
	case H_UNCOV:
		b->uncovered_only = !b->uncovered_only;
		sort_lines(b);
		b->sel[PART_LINES] = 0;
		b->scroll[PART_LINES] = 0;
		break;
	case H_PEAK: select_snapshot(b, b->p.peak_snap); break;
	case H_SETEXE: run_command(CMD_PROF_SET_EXE); break;
	case H_ANNOTATE:
		ann_set(b);
		ui_status("Source files are annotated with %s", app.docs.rel[v->doc] ? app.docs.rel[v->doc] : "this profile");
		break;
	case H_SITE: select_fn(b, v, (u32)arg); break;
	case H_RELOAD: pd_reload(b); break;
	}
}

void profview_mouse(View *v, const Event *e, int x, int y, int w, int h)
{
	ProfDoc *b = pd_get(v->doc);
	app.dirty = true;
	if (atomic_load(&b->state) != 2) /* hits from the loading / error screen still work */
		return;
	int arg;
	if (e->type == EV_MDOWN && e->button == 1) {
		int act = hit_at(e->x, e->y, &arg);
		if (act != H_NONE) {
			hit_run(b, v, act, arg);
			return;
		}
	}
	if (!pd_ready(b))
		return;
	u32 part = b->parts[v->part];
	if (e->type == EV_WHEEL) {
		int dy = (int)(e->wy * 3);
		if (part == PART_FLAME)
			b->fscroll += b->icicle ? -dy * FRAME_H : dy * FRAME_H;
		else if (part == PART_TOP && geo.nb_h && e->y >= geo.nb_y)
			b->nb_scroll[e->x >= geo.nb_x[1] ? 1 : 0] -= dy;
		else
			b->scroll[part] -= dy;
		return;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return;
	(void)w;
	(void)h;
	(void)x;
	(void)y;
	bool dbl = e->clicks >= 2;
	if (part == PART_GRAPH) {
		if (e->x >= geo.chart_x0 && e->x <= geo.chart_x1 && e->y >= geo.chart_y0 && e->y <= geo.chart_y1 + PX(10) &&
		    b->p.nsnap) {
			double tmax = (double)VX_MAX(b->p.sn_time[b->p.nsnap - 1], 1u);
			double t = (double)(e->x - geo.chart_x0) / (double)VX_MAX(geo.chart_x1 - geo.chart_x0, 1) * tmax;
			u32 s = nearest_snap(b, t, true);
			if (s != NONE)
				select_snapshot(b, s);
			if (dbl)
				activate(b, v);
		}
		return;
	}
	if (part == PART_FLAME) {
		if (flame_hover != NONE && e->y >= geo.flame_y0) {
			u32 nd = flame_hover;
			if (dbl) {
				open_fn(b, nd ? b->td.fn[nd] : NONE);
				return;
			}
			b->zoom = nd == b->zoom ? (b->td.parent[nd] == NONE ? 0 : b->td.parent[nd]) : nd;
		}
		return;
	}
	/* Top: the callers / callees lists */
	if (part == PART_TOP && geo.nb_h && e->y >= geo.nb_y && e->y < geo.nb_y + geo.nb_h) {
		int side = e->x >= geo.nb_x[1] ? 1 : 0;
		i64 r = b->nb_scroll[side] + (e->y - geo.nb_y) / app.row_h;
		if (r >= 0 && (u32)r < b->nb_n[side]) {
			u32 f = b->nb_fn[side][r];
			if (dbl)
				open_fn(b, f);
			else
				select_fn(b, v, f);
		}
		return;
	}
	if (e->y < geo.list_y || e->y >= geo.list_y + geo.list_h)
		return;
	i64 r = b->scroll[part] + (e->y - geo.list_y) / VX_MAX(geo.row_h, 1);
	u32 n = list_len(b, part);
	if (r < 0 || (u32)r >= n)
		return;
	b->sel[part] = (u32)r;
	if (part == PART_TOP)
		b->sel_fn = b->top[r];
	if (part == PART_TREE) {
		u32 nd = b->rows[r];
		int ix = geo.x + PX(6) + (int)(b->bu.depth[nd] - 1) * PX(16);
		if (b->bu.child[nd] != NONE && e->x < ix + PX(18)) {
			b->open[nd] ^= 1; /* the chevron */
			b->rows_ok = false;
			return;
		}
	}
	if (dbl)
		activate(b, v);
}

/* ---- commands ------------------------------------------------------------------------------------------- */

static ProfDoc *active_prof(View **vp)
{
	View *v = active_view();
	if (vp)
		*vp = v;
	if (v && app.docs.prof[v->doc] && v->diff_left == UINT32_MAX)
		return pd_get(v->doc);
	return NULL;
}

/* raw report: the file itself, or gprof's / gcov's own text */
static struct {
	_Atomic int state;
	pthread_t th;
	char *path, *exe;
	int kind;
	char *out;
	size_t n;
	char name[320];
} raw;

static void *raw_main(void *a)
{
	(void)a;
	raw.out = prof_raw_report(raw.kind, raw.path, raw.exe, &raw.n);
	atomic_store(&raw.state, 2);
	return NULL;
}

static void raw_start(ProfDoc *b, u32 d)
{
	if (atomic_load(&raw.state) != 0) {
		ui_status("Still preparing the previous report");
		return;
	}
	free(raw.path);
	free(raw.exe);
	raw.path = vx_strdup(b->path);
	raw.exe = b->p.cmd && b->kind == PK_GPROF ? vx_strdup(prof_str(&b->p, b->p.cmd)) : NULL;
	raw.kind = b->kind;
	snprintf(raw.name, sizeof raw.name, "%s (report)", app.docs.rel[d] ? app.docs.rel[d] : "profile");
	atomic_store(&raw.state, 1);
	if (pthread_create(&raw.th, NULL, raw_main, NULL) != 0) {
		raw_main(NULL);
		raw.th = pthread_self();
	}
	ui_status("Preparing the report...");
}

void profview_tick(void)
{
	if (atomic_load(&raw.state) != 2)
		return;
	if (!pthread_equal(raw.th, pthread_self()))
		pthread_join(raw.th, NULL);
	atomic_store(&raw.state, 0);
	if (!raw.out) {
		ui_status("No report: the tool failed or is not installed");
		return;
	}
	extern void show_doc_public(u32 d);
	u32 d = doc_new_virtual_owned(raw.name, raw.out, (u32)raw.n, LANG_NONE);
	raw.out = NULL;
	if (d != UINT32_MAX)
		show_doc_public(d);
	app.dirty = true;
}

static void hot_step(int dir)
{
	View *v = active_view();
	if (!v || app.docs.bin[v->doc]) {
		ui_status("Open an annotated source file first");
		return;
	}
	const Ann *a = ann_for(v->doc);
	if (!a || !a->nhot) {
		ui_status(ann_pd == NONE ? "No profile annotates sources (Profile: Annotate Source with Profile...)"
		                         : "This file has no cost in the profile");
		return;
	}
	Text *t = &app.docs.text[v->doc];
	u32 cur = text_line_of(t, v->head[0]);
	i64 pos = -1;
	for (u32 i = 0; i < a->nhot; i++)
		if (a->hot[i] == cur)
			pos = i;
	i64 nx = pos < 0 ? (dir > 0 ? 0 : (i64)a->nhot - 1) : (pos + dir + (i64)a->nhot) % (i64)a->nhot;
	u32 l = a->hot[nx];
	u32 off = ed_line_col_to_off(t, l, 0);
	ed_set_cursor(v, off, false);
	ed_reveal(v, off, true);
	ProfDoc *b = ann_doc();
	char vb[48], pb[24];
	double val = a->self[l] > 0 ? a->self[l] : a->tot[l];
	if (b->kind == PK_GCOV)
		prof_fmt_count(val, vb, sizeof vb);
	else
		fmtv(b, val, vb, sizeof vb);
	fmt_pct(val, a->total, pb, sizeof pb);
	ui_status("Hot line %lld of %u: %s%s (%s)", (long long)nx + 1, a->nhot, a->self[l] > 0 ? "" : "with callees ", vb,
	          pb);
}

static void annotate_pick(void)
{
	u32 n = 0, last = NONE;
	for (u32 d = 0; d < MAX_DOCS; d++)
		if (pdoc[d].used && app.docs.prof[d] && atomic_load(&pdoc[d].state) == 2) {
			n++;
			last = d;
		}
	if (!n) {
		ui_status("Open a profile first (massif.out.*, *.heap, gmon.out / *.gmon, *.gcov, *.gcda)");
		return;
	}
	if (n == 1) {
		ann_set(&pdoc[last]);
		ui_status("Source files are annotated with %s", app.docs.rel[last]);
		return;
	}
	pick_begin(PICK_PROFILE, "Annotate source files with which profile?", NULL);
	for (u32 d = 0; d < MAX_DOCS; d++)
		if (pdoc[d].used && app.docs.prof[d] && atomic_load(&pdoc[d].state) == 2)
			pick_add(app.docs.rel[d] ? app.docs.rel[d] : "profile", prof_kind_name(pdoc[d].kind), d);
	pick_show();
}

void profview_pick_accept(int kind, u32 val)
{
	(void)kind;
	if (val < MAX_DOCS && pdoc[val].used) {
		ann_set(&pdoc[val]);
		ui_status("Source files are annotated with %s", app.docs.rel[val] ? app.docs.rel[val] : "the profile");
	}
}

void profview_prompt_accept(int kind, const char *text)
{
	View *v;
	ProfDoc *b = active_prof(&v);
	if (!b)
		return;
	if (kind == PR_PROF_EXE && *text) {
		char p[4096];
		if (text[0] == '/' || !app.root)
			snprintf(p, sizeof p, "%s", text);
		else
			snprintf(p, sizeof p, "%s/%s", app.root, text);
		free(b->exe);
		b->exe = vx_strdup(p);
		pd_reload(b);
	} else if (kind == PR_PROF_FIND) {
		snprintf(b->find, sizeof b->find, "%s", text);
		b->find_val = 0;
		for (u32 i = 0; i < b->nparts; i++)
			if (b->parts[i] == PART_FLAME)
				v->part = i;
	}
	app.dirty = true;
}

static void copy_row(ProfDoc *b, View *v)
{
	u32 part = b->parts[v->part];
	char line[1400], vb[48], tb[48];
	line[0] = 0;
	if (part == PART_LINES && b->sel[part] < b->nlord) {
		u32 i = b->lord[b->sel[part]];
		fmtv(b, b->ln.self[i], vb, sizeof vb);
		fmtv(b, b->ln.total[i], tb, sizeof tb);
		u32 sn;
		const char *s = source_line(b, b->ln.file[i], b->ln.line[i], &sn);
		snprintf(line, sizeof line, "%s:%u\t%s\tself %s\ttotal %s\t%.*s", file_name(b, b->ln.file[i]), b->ln.line[i],
		         fn_name(b, b->ln.fn[i]), vb, tb, (int)VX_MIN(sn, 600u), s);
	} else {
		u32 f = current_fn(b, v);
		if (f == NONE)
			return;
		char loc[600];
		fn_location(b, f, loc, sizeof loc);
		fmtv(b, b->fself[f], vb, sizeof vb);
		fmtv(b, b->ftot[f], tb, sizeof tb);
		snprintf(line, sizeof line, "%s\tself %s\ttotal %s\t%s", fn_name(b, f), vb, tb, loc);
	}
	ui_clipboard_set(line, strlen(line));
}

/* Commands that work from anywhere (palette, menus). */
bool profview_command(int cmd)
{
	View *v;
	ProfDoc *b = active_prof(&v);
	switch (cmd) {
	case CMD_PROF_ANNOTATE:
		if (b && pd_ready(b)) {
			ann_set(b);
			ui_status("Source files are annotated with %s", app.docs.rel[v->doc]);
		} else
			annotate_pick();
		return true;
	case CMD_PROF_CLEAR:
		ann_pd = NONE;
		ann_reset();
		ui_status("Source annotation cleared");
		return true;
	case CMD_PROF_NEXT_HOT: hot_step(1); return true;
	case CMD_PROF_PREV_HOT: hot_step(-1); return true;
	default: break;
	}
	if (!b)
		b = ann_doc();
	if (!b)
		switch (cmd) {
		case CMD_PROF_METRIC:
		case CMD_PROF_SET_EXE:
		case CMD_PROF_RELOAD:
		case CMD_PROF_RAW:
		case CMD_PROF_OPEN_SOURCE:
		case CMD_PROF_RESET_ZOOM:
		case CMD_PROF_FLIP:
		case CMD_PROF_FIND:
			ui_status("Open a profile first");
			return true;
		default: return false;
		}
	u32 d = (u32)(b - pdoc);
	switch (cmd) {
	case CMD_PROF_METRIC:
		if (pd_ready(b))
			hit_run(b, v, H_METRIC, 0);
		return true;
	case CMD_PROF_SET_EXE:
		if (b->kind != PK_GPROF) {
			ui_status("Only gprof profiles need an executable");
			return true;
		}
		if (!active_prof(NULL)) {
			ui_status("Switch to the gprof tab first");
			return true;
		}
		prompt(PR_PROF_EXE, "Executable that wrote this gmon file (absolute, or relative to the project)",
		       b->exe ? b->exe : b->p.cmd ? prof_str(&b->p, b->p.cmd) : "");
		return true;
	case CMD_PROF_RELOAD: pd_reload(b); return true;
	case CMD_PROF_RAW: raw_start(b, d); return true;
	case CMD_PROF_OPEN_SOURCE:
		if (v && active_prof(NULL) && pd_ready(b))
			activate(b, v);
		return true;
	case CMD_PROF_RESET_ZOOM: b->zoom = 0; return true;
	case CMD_PROF_FLIP:
		b->icicle = !b->icicle;
		b->fscroll = 0;
		return true;
	case CMD_PROF_FIND:
		if (active_prof(NULL))
			prompt(PR_PROF_FIND, "Highlight functions whose name contains", b->find);
		return true;
	default: return false;
	}
}

/* Commands while a profile tab is active (from binview_command). */
bool profview_view_command(View *v, int cmd)
{
	ProfDoc *b = pd_get(v->doc);
	switch (cmd) {
	case CMD_COPY:
		if (pd_ready(b))
			copy_row(b, v);
		return true;
	case CMD_FIND:
	case CMD_REPLACE: return profview_command(CMD_PROF_FIND);
	case CMD_GOTO_LINE: return true;
	case CMD_SELECT_ALL: return true;
	default: return profview_command(cmd);
	}
}

void profview_context(View *v, int x, int y)
{
	ProfDoc *b = pd_get(v->doc);
	app.ctx = CTX_EDITOR;
	menu_open(x, y);
	bool ready = atomic_load(&b->state) == 2;
	u32 part = b->parts[v->part];
	menu_item(part == PART_LINES || part == PART_FILES ? "Open Source Line" : "Open Function Source", CMD_PROF_OPEN_SOURCE,
	          ready && part != PART_GRAPH);
	menu_item("Annotate Source with This Profile", CMD_PROF_ANNOTATE, ready);
	menu_item("Copy", CMD_COPY, ready);
	menu_sep();
	if (part == PART_FLAME) {
		menu_item("Highlight Functions...", CMD_PROF_FIND, ready);
		menu_item("Reset Zoom", CMD_PROF_RESET_ZOOM, ready && b->zoom);
		menu_item("Flip Flame Graph", CMD_PROF_FLIP, ready);
		menu_sep();
	}
	if (b->p.nmet > 1)
		menu_item("Switch Metric", CMD_PROF_METRIC, ready);
	if (b->kind == PK_GPROF)
		menu_item("Set Executable...", CMD_PROF_SET_EXE, ready);
	menu_item(b->kind == PK_GPROF ? "Open gprof Report" : b->kind == PK_GCOV ? "Open gcov Report" : "Open Raw Profile",
	          CMD_PROF_RAW, ready);
	menu_item("Reload Profile", CMD_PROF_RELOAD, ready);
}

void profview_status(View *v, char *buf, size_t cap)
{
	ProfDoc *b = pd_get(v->doc);
	if (atomic_load(&b->state) != 2) {
		snprintf(buf, cap, "%s (loading)", prof_kind_name(b->kind));
		return;
	}
	Prof *p = &b->p;
	char vb[48];
	switch (b->kind) {
	case PK_MASSIF:
		if (p->peak_snap != NONE) {
			prof_fmt_bytes((double)(p->sn_heap[p->peak_snap] + p->sn_extra[p->peak_snap] + p->sn_stacks[p->peak_snap]), vb,
			               sizeof vb);
			snprintf(buf, cap, "Massif  peak %s  snapshot %u", vb, b->snap);
		} else
			snprintf(buf, cap, "Massif");
		break;
	case PK_JEPROF:
		fmtv(b, b->total, vb, sizeof vb);
		snprintf(buf, cap, "jemalloc  %s %s%s", vb, prof_str(p, p->met_name[b->met]),
		         p->nunresolved ? "  (some frames unresolved)" : "");
		break;
	case PK_GPROF: {
		fmtv(b, b->total, vb, sizeof vb);
		char cb[32];
		prof_fmt_count((double)p->total_calls, cb, sizeof cb);
		snprintf(buf, cap, "gprof  %s  %s calls  %s", vb, cb, prof_str(p, p->title));
		break;
	}
	case PK_GCOV: {
		u32 hit = 0;
		for (u32 g = 0; g < p->ngl; g++)
			hit += p->gl_count[g] > 0;
		snprintf(buf, cap, "gcov  %u/%u lines  %.1f%%", hit, p->ngl, p->ngl ? 100.0 * hit / p->ngl : 0.0);
		break;
	}
	default: snprintf(buf, cap, "Profile");
	}
}
