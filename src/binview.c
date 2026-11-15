/* Binary files, read-only: a hex editor in the style of HxD (offset / hex /
 * text columns, selection mirrored in both, a data inspector for the bytes
 * at the cursor, named coloured tags), and executables split into subtabs:
 * the whole file, then one per section with bytes in the file - code
 * sections disassembled (capstone, on a background thread), data sections
 * in the hex editor. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "binfile.h"
#include "syntax.h"
#include "search.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#define NONE UINT32_MAX
#define BYTES_PER_ROW 16

/* ---- per-document state -------------------------------------------------------- */

typedef struct BinDoc BinDoc;

typedef struct {
	BinDoc *bd;
	u32 sec;
	BinDisasm dis;
	_Atomic int state; /* 0 idle, 1 running, 2 done */
	pthread_t th;
} DisJob;

struct BinDoc {
	bool used;
	char *path;
	int prc; /* bin_parse result */
	BinImage img;
	u32 nparts;
	u32 *part_sec; /* section per part (NONE = whole file) */
	DisJob *jobs;  /* per section */
	/* tags (SoA) */
	u32 nt, ct;
	u64 *t_off, *t_len;
	u32 *t_name;
	u8 *t_col;
	Pool tpool;
	const u8 *data;
	size_t n;
};

static BinDoc bdoc[MAX_DOCS];
static u32 bdoc_index(const BinDoc *b) { return (u32)(b - bdoc); }

/* find (below) */
static bool hex_found(u32 doc, u32 part, u64 o);
static bool row_found(u32 doc, u32 part, u32 r);
static void bf_forget_view(View *v);
static void bf_forget_doc(u32 d);
static bool find_busy(void);

static const u8 *doc_bytes(u32 d, size_t *n)
{
	Text *t = &app.docs.text[d];
	if (t->mapped || (t->file && t->np <= 1)) {
		*n = t->file_len;
		return (const u8 *)t->file;
	}
	*n = t->len;
	return (const u8 *)text_flat(t);
}

static void *dis_main(void *arg)
{
	DisJob *j = arg;
	bin_disasm(&j->bd->img, j->sec, j->bd->data, j->bd->n, &j->dis);
	atomic_store(&j->state, 2);
	return NULL;
}

#define TAG_COLS(b) SOA((b)->t_off), SOA((b)->t_len), SOA((b)->t_name), SOA((b)->t_col)

static void bd_free(BinDoc *b)
{
	if (!b->used)
		return;
	for (u32 s = 0; b->jobs && s < b->img.ns; s++) {
		if (atomic_load(&b->jobs[s].state) != 0)
			pthread_join(b->jobs[s].th, NULL);
		bin_disasm_free(&b->jobs[s].dis);
	}
	free(b->jobs);
	bin_free(&b->img);
	free(b->part_sec);
	SoaCol tc[] = {TAG_COLS(b)};
	soa_free(tc, 4);
	pool_free(&b->tpool);
	free(b->path);
	memset(b, 0, sizeof *b);
}

static BinDoc *bd_get(u32 d)
{
	BinDoc *b = &bdoc[d];
	const char *p = app.docs.path[d] ? app.docs.path[d] : "";
	if (b->used && b->path && !strcmp(b->path, p))
		return b;
	bd_free(b);
	b->used = true;
	b->path = vx_strdup(p);
	b->data = doc_bytes(d, &b->n);
	b->prc = bin_parse(b->data, b->n, &b->img);
	u32 ns = b->prc >= 0 && b->prc != 1 ? b->img.ns : 0;
	b->part_sec = vx_alloc((ns + 1) * sizeof(u32));
	b->part_sec[0] = NONE;
	b->nparts = 1;
	for (u32 s = 0; s < ns; s++)
		if ((b->img.s_flags[s] & BS_HAS_FILE_DATA) && b->img.s_size[s])
			b->part_sec[b->nparts++] = s;
	b->jobs = vx_alloc((ns + 1) * sizeof(DisJob));
	for (u32 s = 0; s < ns; s++) {
		b->jobs[s].bd = b;
		b->jobs[s].sec = s;
	}
	return b;
}

/* ---- per-view state ------------------------------------------------------------------- */

typedef struct {
	View *v;
	u32 nparts;
	u64 *cur, *anchor; /* hex: byte offsets (absolute); disasm: row index */
	i64 *scroll;       /* rows */
	bool ascii;        /* focus in the text column */
	bool big_endian;
} BinView;

static BinView bview[32];

static BinView *bv_get(View *v, BinDoc *b)
{
	BinView *f = NULL;
	for (int i = 0; i < 32; i++)
		if (bview[i].v == v)
			return &bview[i];
	for (int i = 0; i < 32 && !f; i++)
		if (!bview[i].v)
			f = &bview[i];
	if (!f) {
		f = &bview[0];
		free(f->cur);
		free(f->anchor);
		free(f->scroll);
	}
	memset(f, 0, sizeof *f);
	f->v = v;
	f->nparts = b->nparts;
	f->cur = vx_alloc((b->nparts + 1) * sizeof(u64));
	f->anchor = vx_alloc((b->nparts + 1) * sizeof(u64));
	f->scroll = vx_alloc((b->nparts + 1) * sizeof(i64));
	f->big_endian = b->img.big_endian;
	for (u32 p = 1; p < b->nparts; p++)
		f->cur[p] = f->anchor[p] = b->img.s_off[b->part_sec[p]];
	return f;
}

void binview_closed(View *v)
{
	bf_forget_view(v);
	for (int i = 0; i < 32; i++)
		if (bview[i].v == v) {
			free(bview[i].cur);
			free(bview[i].anchor);
			free(bview[i].scroll);
			memset(&bview[i], 0, sizeof bview[i]);
		}
}

void binview_doc_released(u32 d)
{
	if (app.docs.prof[d])
		profview_doc_released(d);
	bf_forget_doc(d);
	bd_free(&bdoc[d]);
}

#define PROF_VIEW(v) (app.docs.prof[(v)->doc])

/* Set up a view of a binary document (parts = subtabs). */
void binview_init(View *v)
{
	if (PROF_VIEW(v)) {
		profview_init(v);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	v->nparts = b->nparts;
	v->part = 0;
	/* executables open on their entry section's code, like a disassembler */
	if (b->nparts > 1)
		for (u32 p = 1; p < b->nparts; p++) {
			u32 s = b->part_sec[p];
			if ((b->img.s_flags[s] & BS_CODE) && b->img.entry >= b->img.s_addr[s] &&
			    b->img.entry < b->img.s_addr[s] + b->img.s_size[s]) {
				v->part = p;
				break;
			}
		}
}

static bool part_is_code(BinDoc *b, u32 part)
{
	u32 s = b->part_sec[part];
	return s != NONE && (b->img.s_flags[s] & BS_CODE);
}

/* byte range a hex part shows */
static void part_range(BinDoc *b, u32 part, u64 *lo, u64 *hi)
{
	u32 s = b->part_sec[part];
	if (s == NONE) {
		*lo = 0;
		*hi = b->n;
	} else {
		*lo = VX_MIN(b->img.s_off[s], (u64)b->n);
		*hi = VX_MIN(b->img.s_off[s] + b->img.s_size[s], (u64)b->n);
	}
}

static const char *part_name(BinDoc *b, u32 part, char *buf, size_t cap)
{
	u32 s = b->part_sec[part];
	if (s == NONE)
		return b->nparts > 1 ? "File" : "Hex";
	snprintf(buf, cap, "%s", bin_str(&b->img, b->img.s_name[s]));
	return buf;
}

/* ---- subtabs ---------------------------------------------------------------------------- */

static int subtab_w(BinDoc *b, u32 p)
{
	char nb[128];
	const char *nm = part_name(b, p, nb, sizeof nb);
	return font_width(FONT_SMALL, nm, strlen(nm)) + PX(20);
}

/* Subtabs wrap into rows like the file tabs. */
static int subtab_layout(BinDoc *b, int w, u32 want, int *ox, int *oy)
{
	int x = 0, row = 0;
	for (u32 p = 0; p < b->nparts; p++) {
		int tw = subtab_w(b, p);
		if (x + tw > w && x > 0) {
			x = 0;
			row++;
		}
		if (p == want) {
			*ox = x;
			*oy = row * app.subtabs_h;
		}
		x += tw;
	}
	return row + 1;
}

int view_subtabs_height(View *v, int w)
{
	if (v->nparts > 1 && PROF_VIEW(v))
		return profview_subtabs_height(v, w);
	if (v->nparts > 1 && !app.docs.bin[v->doc])
		return mdview_subtabs_height(v, w); /* Markdown: Edit / Preview */
	if (v->nparts <= 1 || !app.docs.bin[v->doc])
		return 0;
	BinDoc *b = bd_get(v->doc);
	int ox, oy;
	return subtab_layout(b, w, NONE, &ox, &oy) * app.subtabs_h;
}

void view_subtabs_draw(View *v, int x, int y, int w, int h)
{
	(void)h;
	if (!app.docs.bin[v->doc]) {
		if (v->nparts > 1)
			mdview_subtabs_draw(v, x, y, w);
		return;
	}
	if (PROF_VIEW(v)) {
		profview_subtabs_draw(v, x, y, w);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	int hh = view_subtabs_height(v, w);
	draw_rect(x, y, w, hh, COL(C_EDITOR_BG));
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	for (u32 p = 0; p < b->nparts; p++) {
		int ox, oy;
		subtab_layout(b, w, p, &ox, &oy);
		int tw = subtab_w(b, p);
		bool active = p == v->part;
		int tx = x + ox, ty = y + oy;
		if (active) {
			draw_rect(tx, ty, tw, app.subtabs_h, COL(C_TAB_ACTIVE_BG));
			draw_rect(tx, ty + app.subtabs_h - PX(2), tw, PX(2), COL(C_TAB_ACTIVE_BORDER_TOP));
		}
		char nb[128];
		const char *nm = part_name(b, p, nb, sizeof nb);
		u32 c = active ? COL(C_TAB_ACTIVE_FG) : part_is_code(b, p) ? COL(C_FG) : COL(C_TAB_INACTIVE_FG);
		draw_text(FONT_SMALL, tx + PX(10), ty + (app.subtabs_h - font_m[FONT_SMALL].line) / 2, nm, strlen(nm), c);
	}
}

void view_subtab_click(View *v, int rel_x, int rel_y, int w)
{
	if (PROF_VIEW(v)) {
		profview_subtab_click(v, rel_x, rel_y, w);
		return;
	}
	if (!app.docs.bin[v->doc]) {
		mdview_subtab_click(v, rel_x, rel_y, w);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	for (u32 p = 0; p < b->nparts; p++) {
		int ox, oy;
		subtab_layout(b, w, p, &ox, &oy);
		if (rel_x >= ox && rel_x < ox + subtab_w(b, p) && rel_y >= oy && rel_y < oy + app.subtabs_h) {
			v->part = p;
			app.dirty = true;
			return;
		}
	}
}

/* ---- disassembly jobs ------------------------------------------------------------------- */

static DisJob *dis_for(BinDoc *b, u32 part)
{
	u32 s = b->part_sec[part];
	DisJob *j = &b->jobs[s];
	if (atomic_load(&j->state) == 0) {
		atomic_store(&j->state, 1);
		if (pthread_create(&j->th, NULL, dis_main, j) != 0) {
			bin_disasm(&b->img, s, b->data, b->n, &j->dis);
			atomic_store(&j->state, 2);
		}
	}
	return j;
}

bool binview_busy(void)
{
	if (profview_busy() || find_busy())
		return true;
	for (u32 d = 0; d < MAX_DOCS; d++)
		if (bdoc[d].used)
			for (u32 s = 0; s < bdoc[d].img.ns; s++)
				if (bdoc[d].jobs && atomic_load(&bdoc[d].jobs[s].state) == 1)
					return true;
	return false;
}

/* ---- drawing ---------------------------------------------------------------------------- */

static const u32 tag_cols[5] = {0xFFB00070u, 0xDC267F70u, 0x40B0A670u, 0xB66DFF70u, 0x59A4F970u};

static int tag_at(BinDoc *b, u64 off)
{
	for (u32 i = b->nt; i-- > 0;)
		if (off >= b->t_off[i] && off < b->t_off[i] + b->t_len[i])
			return (int)i;
	return -1;
}

#define INSPECT_W PX(330)

/* where draw_inspector put the tag list (for clicks) */
static int tags_y0, tags_lh;

static int hex_digits(BinDoc *b) { return b->n > 0xFFFFFFFFull ? 16 : 8; }

typedef struct {
	int off_x, hex_x, asc_x, cw, row_h, head_h, main_w;
} HexGeo;

static HexGeo hex_geo(BinDoc *b, int x, int w)
{
	HexGeo g;
	g.cw = app.char_w;
	g.row_h = app.line_h;
	g.head_h = PX(22);
	g.main_w = VX_MAX(w - INSPECT_W, PX(200));
	g.off_x = x + PX(10);
	g.hex_x = g.off_x + (hex_digits(b) + 2) * g.cw;
	g.asc_x = g.hex_x + (BYTES_PER_ROW * 3 + 1) * g.cw + g.cw;
	return g;
}

static int hex_col_x(const HexGeo *g, int c) { return g->hex_x + (c * 3 + (c >= 8 ? 1 : 0)) * g->cw; }

static void draw_inspector(BinDoc *b, BinView *s, u32 part, int x, int y, int w, int h)
{
	(void)part;
	draw_rect(x, y, w, h, COL(C_SIDEBAR_BG));
	draw_rect(x, y, PX(1), h, COL(C_SIDEBAR_BORDER));
	int cy = y + PX(6);
	const char *title = "DATA INSPECTOR";
	draw_text(FONT_SMALL_BOLD, x + PX(12), cy, title, strlen(title), COL(C_SECTION_HEADER_FG));
	const char *bo = s->big_endian ? "Big endian" : "Little endian";
	int bw = font_width(FONT_SMALL, bo, strlen(bo));
	draw_text(FONT_SMALL, x + w - bw - PX(12), cy, bo, strlen(bo), COL(C_LINK));
	cy += PX(22);
	u64 cur = s->cur[part];
	BinInspect in;
	bin_inspect(b->data + VX_MIN(cur, (u64)b->n), cur < b->n ? b->n - cur : 0, s->big_endian, &in);
	int lh = font_m[FONT_SMALL].line + PX(3);
	for (u32 i = 0; i < in.n && cy + lh < y + h - PX(120); i++) {
		draw_text_fit(FONT_SMALL, x + PX(12), cy, in.name[i], strlen(in.name[i]), PX(100), COL(C_DESCRIPTION_FG));
		draw_text_fit(FONT_MONO, x + PX(116), cy - PX(1), in.value[i], strlen(in.value[i]), w - PX(126),
		              COL(C_FG));
		cy += lh;
	}
	/* selection summary */
	u64 a = VX_MIN(s->cur[part], s->anchor[part]), e = VX_MAX(s->cur[part], s->anchor[part]);
	char sel[160];
	if (e > a)
		snprintf(sel, sizeof sel, "Selection: 0x%llX-0x%llX (%llu bytes)", (unsigned long long)a,
		         (unsigned long long)e, (unsigned long long)(e - a + 1));
	else
		snprintf(sel, sizeof sel, "Offset: 0x%llX (%llu)", (unsigned long long)cur, (unsigned long long)cur);
	cy += PX(6);
	draw_text_fit(FONT_SMALL, x + PX(12), cy, sel, strlen(sel), w - PX(24), COL(C_FG));
	cy += lh + PX(10);
	/* tags */
	const char *tt = "TAGS";
	draw_text(FONT_SMALL_BOLD, x + PX(12), cy, tt, strlen(tt), COL(C_SECTION_HEADER_FG));
	cy += PX(20);
	if (!b->nt) {
		const char *m = "Select bytes, then right click > Tag Selection...";
		draw_text_fit(FONT_SMALL, x + PX(12), cy, m, strlen(m), w - PX(24), COL(C_DESCRIPTION_FG));
	}
	tags_y0 = cy;
	tags_lh = lh;
	for (u32 i = 0; i < b->nt && cy + lh < y + h; i++) {
		draw_rect(x + PX(12), cy + PX(2), PX(10), PX(10), tag_cols[b->t_col[i] % 5] | 0xFFu);
		char line[200];
		snprintf(line, sizeof line, "%s  0x%llX +%llu", b->tpool.b + b->t_name[i], (unsigned long long)b->t_off[i],
		         (unsigned long long)b->t_len[i]);
		draw_text_fit(FONT_SMALL, x + PX(28), cy, line, strlen(line), w - PX(40), COL(C_FG));
		cy += lh;
	}
}

static int last_vis = 20; /* rows the last binary view showed (find centres its result) */

static void draw_hex(BinDoc *b, BinView *s, u32 part, int x, int y, int w, int h, bool focused)
{
	HexGeo g = hex_geo(b, x, w);
	u64 lo, hi;
	part_range(b, part, &lo, &hi);
	u64 base = lo & ~(u64)(BYTES_PER_ROW - 1);
	u64 nrows = (hi - base + BYTES_PER_ROW - 1) / BYTES_PER_ROW;
	int vis = (h - g.head_h) / g.row_h;
	last_vis = vis;
	/* keep the cursor visible */
	i64 crow = (i64)((s->cur[part] - base) / BYTES_PER_ROW);
	if (crow < s->scroll[part])
		s->scroll[part] = crow;
	if (crow >= s->scroll[part] + vis)
		s->scroll[part] = crow - vis + 1;
	s->scroll[part] = VX_CLAMP(s->scroll[part], 0, VX_MAX((i64)nrows - 1, 0));
	draw_rect(x, y, g.main_w, h, COL(C_EDITOR_BG));
	draw_clip(x, y, g.main_w, h);
	/* header */
	char hb[16];
	draw_text(FONT_MONO, g.off_x, y + PX(2), "Offset(h)", 9, COL(C_LINE_NUMBER));
	for (int c = 0; c < BYTES_PER_ROW; c++) {
		snprintf(hb, sizeof hb, "%02X", c);
		draw_text(FONT_MONO, hex_col_x(&g, c), y + PX(2), hb, 2, COL(C_LINE_NUMBER));
	}
	draw_text(FONT_MONO, g.asc_x, y + PX(2), "Decoded text", 12, COL(C_LINE_NUMBER));
	draw_rect(x, y + g.head_h - PX(1), g.main_w, PX(1), COL(C_TABS_BORDER));
	u64 a = VX_MIN(s->cur[part], s->anchor[part]), e = VX_MAX(s->cur[part], s->anchor[part]);
	int digits = hex_digits(b);
	for (int r = 0; r <= vis && (u64)(s->scroll[part] + r) < nrows; r++) {
		u64 row_off = base + (u64)(s->scroll[part] + r) * BYTES_PER_ROW;
		int ry = y + g.head_h + r * g.row_h;
		char ob[24];
		snprintf(ob, sizeof ob, "%0*llX", digits, (unsigned long long)row_off);
		draw_text(FONT_MONO, g.off_x, ry, ob, (size_t)digits, COL(C_LINE_NUMBER));
		for (int c = 0; c < BYTES_PER_ROW; c++) {
			u64 o = row_off + (u64)c;
			if (o < lo || o >= hi)
				continue;
			u8 byte = b->data[o];
			int hx = hex_col_x(&g, c), ax = g.asc_x + c * g.cw;
			int t = tag_at(b, o);
			bool in_sel = o >= a && o <= e && e > a;
			if (t >= 0) {
				draw_rect(hx - g.cw / 2, ry, 3 * g.cw, g.row_h, tag_cols[b->t_col[t] % 5]);
				draw_rect(ax, ry, g.cw, g.row_h, tag_cols[b->t_col[t] % 5]);
			}
			if (!in_sel && hex_found(bdoc_index(b), part, o)) {
				draw_rect(hx - g.cw / 2, ry, 3 * g.cw, g.row_h, COL(C_FIND_MATCH_OTHER));
				draw_rect(ax, ry, g.cw, g.row_h, COL(C_FIND_MATCH_OTHER));
			}
			if (in_sel) {
				draw_rect(hx - g.cw / 2, ry, 3 * g.cw, g.row_h, COL(C_SELECTION));
				draw_rect(ax, ry, g.cw, g.row_h, COL(C_SELECTION));
			}
			if (o == s->cur[part]) {
				/* the cursor: a box in the focused column, a fill in the other */
				u32 cc = COL(C_FOCUS_BORDER);
				if (s->ascii) {
					draw_frame(ax, ry, g.cw, g.row_h, cc);
					draw_rect(hx, ry, 2 * g.cw, g.row_h, (cc & 0xFFFFFF00u) | 0x50u);
				} else {
					draw_frame(hx - PX(1), ry, 2 * g.cw + PX(2), g.row_h, cc);
					draw_rect(ax, ry, g.cw, g.row_h, (cc & 0xFFFFFF00u) | 0x50u);
				}
				(void)focused;
			}
			snprintf(hb, sizeof hb, "%02X", byte);
			u32 col = byte == 0 ? COL(C_LINE_NUMBER) : COL(C_EDITOR_FG);
			draw_text(FONT_MONO, hx, ry, hb, 2, col);
			char ch = byte >= 0x20 && byte < 0x7F ? (char)byte : '.';
			draw_text(FONT_MONO, ax, ry, &ch, 1, byte >= 0x20 && byte < 0x7F ? COL(C_EDITOR_FG) : COL(C_LINE_NUMBER));
		}
	}
	draw_noclip();
	draw_inspector(b, s, part, x + g.main_w, y, w - g.main_w, h);
}

static u32 dis_row_of_addr(const BinImage *img, const BinDisasm *d, u64 addr)
{
	u32 lo = 0, hi = d->n;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (bin_row_addr(img, d, mid) < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	while (lo < d->n && bin_row_kind(d, lo) == BR_LABEL && lo + 1 < d->n && bin_row_addr(img, d, lo + 1) == addr)
		lo++; /* the instruction, not its label */
	return lo;
}

static void draw_disasm(BinDoc *b, BinView *s, u32 part, int x, int y, int w, int h)
{
	DisJob *j = dis_for(b, part);
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	if (atomic_load(&j->state) != 2) {
		const char *m = "Disassembling...";
		draw_text(FONT_UI, x + PX(20), y + PX(10), m, strlen(m), COL(C_DESCRIPTION_FG));
		return;
	}
	BinDisasm *d = &j->dis;
	if (!d->n) {
		draw_text_fit(FONT_UI, x + PX(20), y + PX(10), d->err[0] ? d->err : "No instructions", strlen(d->err[0] ? d->err : "No instructions"),
		              w - PX(40), COL(C_DESCRIPTION_FG));
		return;
	}
	int lh = app.line_h, cw = app.char_w;
	int vis = h / lh;
	last_vis = vis;
	i64 cur = (i64)VX_MIN(s->cur[part], (u64)d->n - 1);
	s->cur[part] = (u64)cur;
	if (cur < s->scroll[part])
		s->scroll[part] = cur;
	if (cur >= s->scroll[part] + vis)
		s->scroll[part] = cur - vis + 1;
	s->scroll[part] = VX_CLAMP(s->scroll[part], 0, (i64)d->n - 1);
	int adigits = b->img.bits > 32 ? 16 : 8;
	int ax = x + PX(10), bx = ax + (adigits + 2) * cw, tx = bx + 21 * cw;
	draw_clip(x, y, w, h);
	u32 sa = (u32)VX_MIN(s->cur[part], s->anchor[part]), se = (u32)VX_MAX(s->cur[part], s->anchor[part]);
	static u32 st[256], sl[256];
	static u8 sc[256];
	for (int r = 0; r <= vis; r++) {
		u64 row = (u64)s->scroll[part] + (u64)r;
		if (row >= d->n)
			break;
		int ry = y + r * lh;
		if (row >= sa && row <= se)
			draw_rect(x, ry, w, lh, row == (u64)cur ? COL(C_LIST_ACTIVE_BG) : COL(C_SELECTION));
		else if (row_found(bdoc_index(b), part, (u32)row))
			draw_rect(x, ry, w, lh, COL(C_FIND_MATCH_OTHER));
		/* decoded for the rows on screen only (the table keeps 5 bytes a row) */
		char tbuf[256];
		u64 target;
		bin_row_text(&b->img, d, b->data, b->n, (u32)row, tbuf, sizeof tbuf, &target);
		const char *t = tbuf;
		u64 raddr = bin_row_addr(&b->img, d, (u32)row);
		char buf[64];
		if (bin_row_kind(d, (u32)row) == BR_LABEL) {
			int n2 = snprintf(buf, sizeof buf, "%0*llX", adigits, (unsigned long long)raddr);
			draw_text(FONT_MONO, ax, ry, buf, (size_t)n2, COL(C_LINE_NUMBER));
			int ex = draw_text(FONT_MONO, bx, ry, "<", 1, COL(C_FG));
			ex = draw_text(FONT_MONO, ex, ry, t, strlen(t), TOK(TK_FUNCTION));
			draw_text(FONT_MONO, ex, ry, ">:", 2, COL(C_FG));
			continue;
		}
		int n2 = snprintf(buf, sizeof buf, "%0*llX", adigits, (unsigned long long)raddr);
		draw_text(FONT_MONO, ax, ry, buf, (size_t)n2, COL(C_LINE_NUMBER));
		/* instruction bytes (up to 6 shown) */
		char hx[64];
		u32 nb = bin_row_len(d, (u32)row), k = 0;
		u64 roff = bin_row_off(d, (u32)row);
		for (u32 i = 0; i < nb && i < 6 && roff + i < b->n; i++)
			k += (u32)snprintf(hx + k, sizeof hx - k, "%02x ", b->data[roff + i]);
		if (nb > 6)
			k += (u32)snprintf(hx + k, sizeof hx - k, "..");
		draw_text(FONT_MONO, bx, ry, hx, k, COL(C_DESCRIPTION_FG));
		/* the instruction, coloured like Intel-syntax assembly */
		u32 n = (u32)strlen(t);
		SynSpans sp = {st, sl, sc, 0, 256};
		syn_line(LANG_ASM_INTEL, 0, t, n, &sp);
		int px = tx;
		u32 pos = 0;
		for (u32 q = 0; q <= sp.count; q++) {
			u32 s0 = q < sp.count ? sp.start[q] : n;
			if (s0 > pos)
				px = draw_text(FONT_MONO, px, ry, t + pos, s0 - pos, TOK(TK_TEXT));
			if (q < sp.count) {
				px = draw_text(FONT_MONO, px, ry, t + sp.start[q], sp.len[q], TOK(sp.cls[q]));
				pos = sp.start[q] + sp.len[q];
			}
		}
		if (target != UINT64_MAX) {
			u64 delta;
			u32 sym = bin_symbol_at(&b->img, target, &delta);
			/* only inside a symbol: the nearest one before may be far away */
			if (sym != NONE && (delta == 0 || delta < b->img.y_size[sym])) {
				char an[300];
				if (delta)
					snprintf(an, sizeof an, "  ; <%s+0x%llx>", bin_str(&b->img, b->img.y_name[sym]),
					         (unsigned long long)delta);
				else
					snprintf(an, sizeof an, "  ; <%s>", bin_str(&b->img, b->img.y_name[sym]));
				draw_text(FONT_MONO, px, ry, an, strlen(an), COL(C_CODELENS));
			}
		}
	}
	draw_noclip();
}

void binview_draw(View *v, int x, int y, int w, int h, bool focused)
{
	if (PROF_VIEW(v)) {
		profview_draw(v, x, y, w, h, focused);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	if (v->nparts != b->nparts)
		binview_init(v);
	BinView *s = bv_get(v, b);
	if (v->part >= b->nparts)
		v->part = 0;
	if (part_is_code(b, v->part))
		draw_disasm(b, s, v->part, x, y, w, h);
	else
		draw_hex(b, s, v->part, x, y, w, h, focused);
	/* the find widget over the main column (left of the inspector) */
	ed_find_widget(v, x, y, part_is_code(b, v->part) ? w : hex_geo(b, x, w).main_w);
}

/* ---- input --------------------------------------------------------------------------- */

static int last_x, last_y, last_w, last_h;

void binview_geometry(int x, int y, int w, int h)
{
	last_x = x;
	last_y = y;
	last_w = w;
	last_h = h;
}

static void hex_move(BinDoc *b, BinView *s, u32 part, i64 delta, bool extend)
{
	u64 lo, hi;
	part_range(b, part, &lo, &hi);
	if (hi == lo)
		return;
	i64 c = (i64)s->cur[part] + delta;
	c = VX_CLAMP(c, (i64)lo, (i64)hi - 1);
	s->cur[part] = (u64)c;
	if (!extend)
		s->anchor[part] = s->cur[part];
}

static void jump_to_target(View *v, BinDoc *b, BinView *s)
{
	DisJob *j = dis_for(b, v->part);
	if (atomic_load(&j->state) != 2 || s->cur[v->part] >= j->dis.n)
		return;
	char tb[256];
	u64 t;
	bin_row_text(&b->img, &j->dis, b->data, b->n, (u32)s->cur[v->part], tb, sizeof tb, &t);
	if (t == UINT64_MAX)
		return;
	/* the section holding the target (may be another subtab) */
	for (u32 p = 1; p < b->nparts; p++) {
		u32 sec = b->part_sec[p];
		if (part_is_code(b, p) && t >= b->img.s_addr[sec] && t < b->img.s_addr[sec] + b->img.s_size[sec]) {
			DisJob *k = dis_for(b, p);
			while (atomic_load(&k->state) != 2) {
				struct timespec ts = {0, 2000000};
				nanosleep(&ts, NULL);
			}
			v->part = p;
			s->cur[p] = s->anchor[p] = dis_row_of_addr(&b->img, &k->dis, t);
			s->scroll[p] = (i64)s->cur[p] - 5;
			return;
		}
	}
	ui_status("Target 0x%llx is outside the code sections", (unsigned long long)t);
}

bool binview_key(View *v, const Event *e)
{
	if (PROF_VIEW(v))
		return profview_key(v, e);
	BinDoc *b = bd_get(v->doc);
	BinView *s = bv_get(v, b);
	u32 p = v->part;
	bool ext = e->mods & MOD_SHIFT;
	bool ctrl = e->mods & MOD_CTRL;
	int vis = VX_MAX(last_h / app.line_h - 2, 1);
	app.dirty = true;
	if (part_is_code(b, p)) {
		DisJob *j = dis_for(b, p);
		if (atomic_load(&j->state) != 2)
			return true;
		i64 n = j->dis.n, c = (i64)s->cur[p];
		switch (e->key) {
		case SDLK_UP: c--; break;
		case SDLK_DOWN: c++; break;
		case SDLK_PAGEUP: c -= vis; break;
		case SDLK_PAGEDOWN: c += vis; break;
		case SDLK_HOME: if (ctrl) c = 0; break;
		case SDLK_END: if (ctrl) c = n - 1; break;
		case SDLK_RETURN: jump_to_target(v, b, s); return true;
		default: return false;
		}
		s->cur[p] = (u64)VX_CLAMP(c, 0, n - 1);
		if (!ext)
			s->anchor[p] = s->cur[p];
		return true;
	}
	switch (e->key) {
	case SDLK_LEFT: hex_move(b, s, p, -1, ext); return true;
	case SDLK_RIGHT: hex_move(b, s, p, 1, ext); return true;
	case SDLK_UP: hex_move(b, s, p, -BYTES_PER_ROW, ext); return true;
	case SDLK_DOWN: hex_move(b, s, p, BYTES_PER_ROW, ext); return true;
	case SDLK_PAGEUP: hex_move(b, s, p, -(i64)vis * BYTES_PER_ROW, ext); return true;
	case SDLK_PAGEDOWN: hex_move(b, s, p, (i64)vis * BYTES_PER_ROW, ext); return true;
	case SDLK_HOME:
		hex_move(b, s, p, ctrl ? -(i64)s->cur[p] : -(i64)(s->cur[p] % BYTES_PER_ROW), ext);
		return true;
	case SDLK_END:
		hex_move(b, s, p, ctrl ? (i64)b->n : (i64)(BYTES_PER_ROW - 1 - s->cur[p] % BYTES_PER_ROW), ext);
		return true;
	case SDLK_TAB:
		s->ascii = !s->ascii; /* hex <-> text column, like HxD */
		return true;
	default:
		return false;
	}
}

void binview_mouse(View *v, const Event *e, int x, int y, int w, int h)
{
	if (PROF_VIEW(v)) {
		profview_mouse(v, e, x, y, w, h);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	BinView *s = bv_get(v, b);
	u32 p = v->part;
	binview_geometry(x, y, w, h);
	app.dirty = true;
	if (e->type == EV_WHEEL) {
		s->scroll[p] -= (i64)(e->wy * 3);
		if (s->scroll[p] < 0)
			s->scroll[p] = 0;
		/* scrolling must not drag the cursor back into view */
		if (part_is_code(b, p)) {
			i64 vis = h / app.line_h;
			i64 c = (i64)s->cur[p];
			if (c < s->scroll[p] || c >= s->scroll[p] + vis)
				s->cur[p] = s->anchor[p] = (u64)VX_MAX(s->scroll[p], 0);
		} else {
			HexGeo g = hex_geo(b, x, w);
			u64 lo, hi;
			part_range(b, p, &lo, &hi);
			u64 base = lo & ~(u64)(BYTES_PER_ROW - 1);
			i64 vis = (h - g.head_h) / g.row_h;
			i64 crow = (i64)((s->cur[p] - base) / BYTES_PER_ROW);
			if (crow < s->scroll[p] || crow >= s->scroll[p] + vis) {
				u64 o = base + (u64)s->scroll[p] * BYTES_PER_ROW + s->cur[p] % BYTES_PER_ROW;
				s->cur[p] = s->anchor[p] = VX_CLAMP(o, lo, hi ? hi - 1 : 0);
			}
		}
		return;
	}
	if (!(e->type == EV_MDOWN || (e->type == EV_MMOVE && app.mouse_down)))
		return;
	if (part_is_code(b, p)) {
		DisJob *j = dis_for(b, p);
		if (atomic_load(&j->state) != 2 || !j->dis.n)
			return;
		i64 row = s->scroll[p] + (e->y - y) / app.line_h;
		row = VX_CLAMP(row, 0, (i64)j->dis.n - 1);
		s->cur[p] = (u64)row;
		if (e->type == EV_MDOWN && !(e->mods & MOD_SHIFT))
			s->anchor[p] = s->cur[p];
		if (e->type == EV_MDOWN && e->clicks == 2)
			jump_to_target(v, b, s);
		return;
	}
	HexGeo g = hex_geo(b, x, w);
	if (e->x >= x + g.main_w) {
		/* inspector: the byte-order label toggles */
		if (e->type == EV_MDOWN && e->y < y + PX(24))
			s->big_endian = !s->big_endian;
		/* click a tag to jump to it (selects its bytes) */
		if (e->type == EV_MDOWN && e->y >= tags_y0 && tags_lh > 0) {
			u32 i = (u32)((e->y - tags_y0) / tags_lh);
			if (i < b->nt) {
				u64 lo, hi;
				part_range(b, p, &lo, &hi);
				if (b->t_off[i] < lo || b->t_off[i] >= hi)
					v->part = p = 0; /* outside this section: the whole-file view */
				s->anchor[p] = b->t_off[i];
				s->cur[p] = VX_MIN(b->t_off[i] + b->t_len[i] - 1, (u64)b->n - 1);
			}
		}
		return;
	}
	if (e->y < y + g.head_h)
		return;
	u64 lo, hi;
	part_range(b, p, &lo, &hi);
	u64 base = lo & ~(u64)(BYTES_PER_ROW - 1);
	i64 row = s->scroll[p] + (e->y - y - g.head_h) / g.row_h;
	int col;
	bool ascii = e->x >= g.asc_x - g.cw / 2;
	if (ascii) {
		col = (e->x - g.asc_x) / g.cw;
	} else {
		col = 0;
		for (int c = 0; c < BYTES_PER_ROW; c++)
			if (e->x >= hex_col_x(&g, c) - g.cw / 2)
				col = c;
	}
	col = VX_CLAMP(col, 0, BYTES_PER_ROW - 1);
	u64 o = base + (u64)row * BYTES_PER_ROW + (u64)col;
	if (hi == lo)
		return;
	o = VX_CLAMP(o, lo, hi - 1);
	s->cur[p] = o;
	if (e->type == EV_MDOWN) {
		s->ascii = ascii;
		if (!(e->mods & MOD_SHIFT))
			s->anchor[p] = o;
	}
}

/* ---- find: hex bytes or text in hex parts, row text in disassembly ------------------- */

/* One search at a time (the find widget is global). Hex parts are scanned
 * at once (memchr fast); disassembly rows are decoded on demand, so they
 * are scanned in time-boxed steps from binview_tick and the count grows. */
static struct {
	bool used;
	u32 doc, part, key;
	bool text;  /* hex parts: the query as text; else as bytes */
	bool disasm;
	u8 pat[256];
	u32 plen;
	u64 *off; /* hex matches, sorted (each plen bytes) */
	u32 n, cap;
	Matcher m; /* text: regex / literal for disassembly rows and regex in hex */
	bool m_ok;
	u32 *rows, nrows, rcap; /* disassembly matches */
	u32 scan;
	bool done;
	int want;  /* pending move: 1 first at/after `from`, 2 next after, -2 previous before */
	u64 from;
	View *v;
	bool said_text;
} bf;

static void bf_reset(void)
{
	free(bf.off);
	free(bf.rows);
	if (bf.m_ok)
		matcher_free(&bf.m);
	memset(&bf, 0, sizeof bf);
}

/* "7f 45 4c 46", "7F454C46", "0x7f 0x45": hex bytes; false for anything else */
static bool parse_hex(const char *q, u32 n, u8 *out, u32 *len)
{
	u32 k = 0, digits = 0;
	int hi = -1;
	for (u32 i = 0; i < n; i++) {
		char c = q[i];
		if (c == ' ' || c == ',' || c == '\t') {
			if (hi >= 0)
				return false; /* an odd digit inside a token */
			continue;
		}
		if (c == '0' && i + 1 < n && (q[i + 1] == 'x' || q[i + 1] == 'X') && hi < 0) {
			i++;
			continue;
		}
		int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (v < 0)
			return false;
		digits++;
		if (hi < 0)
			hi = v;
		else {
			if (k == 256)
				return false;
			out[k++] = (u8)(hi << 4 | v);
			hi = -1;
		}
	}
	*len = k;
	return hi < 0 && digits > 0;
}

static u8 lower(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }
static bool wordb(u8 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static void hex_scan(BinDoc *b, u32 part, int flags)
{
	u64 lo, hi;
	part_range(b, part, &lo, &hi);
	bf.n = 0;
	const u8 *d = b->data;
	if (bf.text && (flags & SEARCH_REGEX)) {
		if (!bf.m_ok || hi - lo > 0xFFFFFFFFull)
			return;
		u32 pos = 0, a, e;
		u32 n = (u32)(hi - lo);
		while (bf.n < 100000 && pos < n && matcher_next(&bf.m, (const char *)d + lo, n, pos, &a, &e)) {
			VX_GROW(bf.off, bf.cap, bf.n + 1);
			bf.off[bf.n++] = lo + a;
			pos = e > a ? e : a + 1;
		}
		bf.plen = 1; /* regex matches vary; the selection shows each one's start */
		return;
	}
	u32 m = bf.plen;
	if (!m || hi - lo < m)
		return;
	bool icase = bf.text && !(flags & SEARCH_CASE);
	bool word = bf.text && (flags & SEARCH_WORD);
	u8 f0 = bf.pat[0], f1 = icase ? (u8)(f0 >= 'a' && f0 <= 'z' ? f0 - 32 : f0) : f0;
	for (u64 o = lo; o + m <= hi && bf.n < 100000;) {
		/* jump to the next possible first byte */
		const u8 *p = memchr(d + o, f0, (size_t)(hi - m + 1 - o));
		if (icase && f1 != f0) {
			const u8 *q = memchr(d + o, f1, (size_t)(hi - m + 1 - o));
			if (!p || (q && q < p))
				p = q;
		}
		if (!p)
			break;
		o = (u64)(p - d);
		bool eq = true;
		for (u32 i = 1; i < m && eq; i++)
			eq = icase ? lower(d[o + i]) == bf.pat[i] : d[o + i] == bf.pat[i];
		if (eq && word && ((o > lo && wordb(d[o - 1])) || (o + m < hi && wordb(d[o + m]))))
			eq = false;
		if (eq) {
			VX_GROW(bf.off, bf.cap, bf.n + 1);
			bf.off[bf.n++] = o;
			o += m;
		} else
			o++;
	}
}

/* first index with off >= x */
static u32 off_lb(u64 x)
{
	u32 lo = 0, hi = bf.n;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (bf.off[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static u32 row_lb(u32 x)
{
	u32 lo = 0, hi = bf.nrows;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (bf.rows[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* is byte o inside a match (hex highlight) */
static bool hex_found(u32 doc, u32 part, u64 o)
{
	if (!app.find_open || !bf.used || bf.disasm || bf.doc != doc || bf.part != part || !bf.n)
		return false;
	u32 k = off_lb(o + 1);
	return k > 0 && o < bf.off[k - 1] + bf.plen;
}

static bool row_found(u32 doc, u32 part, u32 r)
{
	if (!app.find_open || !bf.used || !bf.disasm || bf.doc != doc || bf.part != part || !bf.nrows)
		return false;
	u32 k = row_lb(r);
	return k < bf.nrows && bf.rows[k] == r;
}

static void dis_step(BinDoc *b, u64 budget_ns)
{
	DisJob *j = dis_for(b, bf.part);
	if (atomic_load(&j->state) != 2 || bf.done)
		return;
	BinDisasm *d = &j->dis;
	u64 t0 = vx_now_ns();
	char buf[256];
	while (bf.scan < d->n) {
		u32 r = bf.scan++;
		bin_row_text(&b->img, d, b->data, b->n, r, buf, sizeof buf, NULL);
		u32 a, e;
		if (bf.m_ok && matcher_next(&bf.m, buf, (u32)strlen(buf), 0, &a, &e)) {
			VX_GROW(bf.rows, bf.rcap, bf.nrows + 1);
			bf.rows[bf.nrows++] = r;
		}
		if ((bf.scan & 255) == 0 && vx_now_ns() - t0 > budget_ns)
			break;
	}
	if (bf.scan >= d->n)
		bf.done = true;
}

/* Move to the pending target when it is known. */
static void dis_resolve(BinView *s)
{
	if (!bf.want)
		return;
	u32 p = bf.part;
	i64 pick = -1;
	if (bf.want == 1 || bf.want == 2) {
		u32 from = (u32)bf.from + (bf.want == 2 ? 1 : 0);
		u32 k = row_lb(from);
		if (k < bf.nrows)
			pick = k;
		else if (bf.done)
			pick = bf.nrows ? 0 : -1; /* wrap */
		else
			return;
	} else {
		u32 k = row_lb((u32)bf.from);
		if (k > 0 && (bf.scan > bf.from || bf.done))
			pick = k - 1;
		else if (bf.done)
			pick = bf.nrows ? (i64)bf.nrows - 1 : -1;
		else
			return;
	}
	bf.want = 0;
	if (pick >= 0) {
		s->cur[p] = s->anchor[p] = bf.rows[pick];
		if ((i64)s->cur[p] < s->scroll[p] + 2 || (i64)s->cur[p] >= s->scroll[p] + last_vis - 1)
			s->scroll[p] = (i64)s->cur[p] - last_vis / 3; /* with context, below the widget */
		app.find_index = (u32)pick + 1;
	} else
		app.find_index = 0;
	app.dirty = true;
}

void binview_find(View *v, int dir)
{
	if (PROF_VIEW(v))
		return;
	BinDoc *b = bd_get(v->doc);
	BinView *s = bv_get(v, b);
	u32 p = v->part;
	bool dis = part_is_code(b, p);
	int flags = app.find_flags & (SEARCH_CASE | SEARCH_WORD | SEARCH_REGEX);
	const char *q = app.find_in.b ? app.find_in.b : "";
	u32 qn = app.find_in.n;
	/* the key: query, flags, view, part, and for hex which column has the focus */
	u32 key = 2166136261u ^ (u32)flags ^ (s->ascii ? 0x9E3779B9u : 0) ^ (u32)dis << 30;
	for (u32 i = 0; i < qn; i++)
		key = (key ^ (u8)q[i]) * 16777619u;
	if (!bf.used || bf.doc != v->doc || bf.part != p || bf.key != key || bf.v != v) {
		bf_reset();
		bf.used = true;
		bf.doc = v->doc;
		bf.part = p;
		bf.key = key;
		bf.v = v;
		bf.disasm = dis;
		if (!qn) {
			bf.done = true; /* nothing to look for */
			app.find_count = app.find_index = 0;
			return;
		}
		if (dis) {
			bf.m_ok = matcher_init(&bf.m, q, qn, flags);
		} else {
			bool hexq = !s->ascii && !(flags & SEARCH_REGEX) && parse_hex(q, qn, bf.pat, &bf.plen);
			bf.text = !hexq;
			if (bf.text) {
				bf.plen = VX_MIN(qn, 256u);
				for (u32 i = 0; i < bf.plen; i++)
					bf.pat[i] = (flags & SEARCH_CASE) ? (u8)q[i] : lower((u8)q[i]);
				if (flags & SEARCH_REGEX)
					bf.m_ok = matcher_init(&bf.m, q, qn, flags);
			}
			hex_scan(b, p, flags);
			ui_status(bf.text ? "Find: text%s" : "Find: bytes%s",
			          bf.text && !s->ascii ? " (not hex digits; click the text column to always search text)" : "");
		}
	}
	if (!qn)
		return;
	if (dis) {
		u64 cur = VX_MIN(s->cur[p], s->anchor[p]);
		bf.want = dir == 0 ? 1 : dir > 0 ? 2 : -2;
		bf.from = cur;
		dis_step(b, 20000000ull);
		app.find_count = bf.nrows;
		dis_resolve(s);
		app.dirty = true;
		return;
	}
	app.find_count = bf.n;
	if (!bf.n) {
		app.find_index = 0;
		return;
	}
	u64 at = VX_MIN(s->cur[p], s->anchor[p]);
	u32 k;
	if (dir == 0)
		k = off_lb(at);
	else if (dir > 0)
		k = off_lb(at + 1);
	else {
		k = off_lb(at);
		k = k == 0 ? bf.n - 1 : k - 1;
	}
	if (k >= bf.n)
		k = 0; /* wrap */
	s->anchor[p] = bf.off[k];
	s->cur[p] = VX_MIN(bf.off[k] + bf.plen - 1, (u64)b->n - 1);
	{
		u64 lo, hi;
		part_range(b, p, &lo, &hi);
		i64 row = (i64)((bf.off[k] - (lo & ~(u64)(BYTES_PER_ROW - 1))) / BYTES_PER_ROW);
		if (row < s->scroll[p] + 2 || row >= s->scroll[p] + last_vis - 1)
			s->scroll[p] = row - last_vis / 3;
	}
	app.find_index = k + 1;
	app.dirty = true;
}

void binview_tick(void)
{
	if (!bf.used || !bf.disasm || bf.done || !bf.m_ok || !app.find_open || !bf.v)
		return;
	if (bf.doc >= MAX_DOCS || !bdoc[bf.doc].used || !app.docs.bin[bf.doc]) {
		bf_reset();
		return;
	}
	BinDoc *b = &bdoc[bf.doc];
	dis_step(b, 25000000ull);
	app.find_count = bf.nrows;
	dis_resolve(bv_get(bf.v, b));
	DisJob *j = dis_for(b, bf.part);
	if (!bf.done && atomic_load(&j->state) == 2 && j->dis.n)
		ui_status("Searching the disassembly... %u%%", (unsigned)((u64)bf.scan * 100 / j->dis.n));
	else if (bf.done)
		ui_status("%u matching rows", bf.nrows);
	app.dirty = true;
}

static void bf_forget_doc(u32 d)
{
	if (bf.used && bf.doc == d)
		bf_reset();
}

static void bf_forget_view(View *v)
{
	if (bf.used && bf.v == v)
		bf_reset();
}

static bool find_busy(void) { return bf.used && bf.disasm && !bf.done && bf.m_ok && app.find_open && bf.v; }

/* Ctrl+F: the find widget, seeded from the selected bytes */
static void find_open(View *v, BinDoc *b, BinView *s)
{
	u32 p = v->part;
	app.find_open = true;
	app.find_replace = false;
	app.find_flags &= ~0x100;
	app.focus = FOCUS_FIND;
	if (!part_is_code(b, p) && s->cur[p] != s->anchor[p]) {
		u64 a = VX_MIN(s->cur[p], s->anchor[p]), e = VX_MAX(s->cur[p], s->anchor[p]);
		if (e - a < 64) {
			char q[256];
			u32 k = 0;
			bool printable = true;
			for (u64 o = a; o <= e; o++)
				printable &= b->data[o] >= 0x20 && b->data[o] < 0x7F;
			if (s->ascii && printable)
				for (u64 o = a; o <= e; o++)
					q[k++] = (char)b->data[o];
			else
				for (u64 o = a; o <= e; o++)
					k += (u32)snprintf(q + k, sizeof q - k, o < e ? "%02X " : "%02X", b->data[o]);
			input_set(&app.find_in, q, k);
		}
	}
	app.find_in.anchor = 0;
	app.find_in.cur = app.find_in.n;
	bf.used = false; /* search again from here */
	binview_find(v, 0);
}

/* ---- commands: tags, go to offset, copy ------------------------------------------------ */

enum { PR_HEX_TAG = 200, PR_HEX_GOTO };

static View *bin_active(BinDoc **bp, BinView **sp)
{
	View *v = active_view();
	if (!v || !app.docs.bin[v->doc])
		return NULL;
	*bp = bd_get(v->doc);
	*sp = bv_get(v, *bp);
	return v;
}

void binview_prompt_accept(int kind, const char *text)
{
	if (kind >= 270) {
		workflow_prompt_accept(kind, text);
		return;
	}
	if (kind >= 250) {
		profview_prompt_accept(kind, text);
		return;
	}
	BinDoc *b;
	BinView *s;
	View *v = bin_active(&b, &s);
	if (!v)
		return;
	u32 p = v->part;
	if (kind == PR_HEX_TAG && *text) {
		u64 a = VX_MIN(s->cur[p], s->anchor[p]), e = VX_MAX(s->cur[p], s->anchor[p]);
		SoaCol c[] = {TAG_COLS(b)};
		soa_grow(c, 4, &b->ct, b->nt + 1, b->nt);
		u32 i = b->nt++;
		b->t_off[i] = a;
		b->t_len[i] = e - a + 1;
		b->t_name[i] = pool_add(&b->tpool, text, (u32)strlen(text));
		b->t_col[i] = (u8)(i % 5);
	} else if (kind == PR_HEX_GOTO && *text) {
		char *end;
		const char *t = text;
		bool hexn = !strncmp(t, "0x", 2) || !strncmp(t, "0X", 2) || strpbrk(t, "abcdefABCDEF");
		unsigned long long o = strtoull(t, &end, hexn ? 16 : 10);
		if (part_is_code(b, p)) {
			DisJob *j = dis_for(b, p);
			if (atomic_load(&j->state) == 2)
				s->cur[p] = s->anchor[p] = dis_row_of_addr(&b->img, &j->dis, o);
		} else {
			u64 lo, hi;
			part_range(b, p, &lo, &hi);
			if (hi > lo)
				s->cur[p] = s->anchor[p] = VX_CLAMP((u64)o, lo, hi - 1);
		}
	}
	app.dirty = true;
}

static void prompt(int kind, const char *title)
{
	app.quick = QUICK_PROMPT;
	app.quick_prompt_kind = kind;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, "", 0);
	app.focus = FOCUS_QUICK;
}

bool binview_command(int cmd)
{
	BinDoc *b;
	BinView *s;
	View *av = active_view();
	if (av && PROF_VIEW(av))
		return profview_view_command(av, cmd);
	View *v = bin_active(&b, &s);
	if (!v)
		return false;
	u32 p = v->part;
	switch (cmd) {
	case CMD_FIND:
	case CMD_REPLACE:
		find_open(v, b, s);
		return true;
	case CMD_FIND_NEXT:
	case CMD_FIND_PREV:
		if (app.find_in.n)
			binview_find(v, cmd == CMD_FIND_NEXT ? 1 : -1);
		return true;
	case CMD_HEX_ADD_TAG:
		if (part_is_code(b, p)) {
			ui_status("Tags are for the hex view");
			return true;
		}
		prompt(PR_HEX_TAG, "Tag name for the selected bytes");
		return true;
	case CMD_HEX_REMOVE_TAG: {
		int t = tag_at(b, s->cur[p]);
		if (t >= 0) {
			u32 last = --b->nt;
			b->t_off[t] = b->t_off[last];
			b->t_len[t] = b->t_len[last];
			b->t_name[t] = b->t_name[last];
			b->t_col[t] = b->t_col[last];
		}
		return true;
	}
	case CMD_HEX_GOTO:
	case CMD_GOTO_LINE:
		prompt(PR_HEX_GOTO, part_is_code(b, p) ? "Go to address (hex)" : "Go to offset (0x... or decimal)");
		return true;
	case CMD_HEX_ENDIAN:
		s->big_endian = !s->big_endian;
		return true;
	case CMD_COPY: {
		/* hex: the selected bytes as hex text; disassembly: the rows */
		Pool out = {0};
		if (part_is_code(b, p)) {
			DisJob *j = dis_for(b, p);
			if (atomic_load(&j->state) != 2)
				return true;
			u32 a = (u32)VX_MIN(s->cur[p], s->anchor[p]), e = (u32)VX_MAX(s->cur[p], s->anchor[p]);
			for (u32 r = a; r <= e && r < j->dis.n; r++) {
				char line[400];
				char tb[256];
				bin_row_text(&b->img, &j->dis, b->data, b->n, r, tb, sizeof tb, NULL);
				int k = snprintf(line, sizeof line, "%llx:\t%s\n",
				                 (unsigned long long)bin_row_addr(&b->img, &j->dis, r), tb);
				VX_GROW(out.b, out.cap, out.n + (u32)k + 1);
				memcpy(out.b + out.n, line, (size_t)k);
				out.n += (u32)k;
			}
		} else {
			u64 a = VX_MIN(s->cur[p], s->anchor[p]), e = VX_MAX(s->cur[p], s->anchor[p]);
			for (u64 o = a; o <= e && o < b->n && out.n < (16u << 20); o++) {
				char hb[4];
				snprintf(hb, sizeof hb, "%02X%s", b->data[o], o < e ? " " : "");
				u32 k = (u32)strlen(hb);
				VX_GROW(out.b, out.cap, out.n + k + 1);
				memcpy(out.b + out.n, hb, k);
				out.n += k;
			}
		}
		ui_clipboard_set(out.b ? out.b : "", out.n);
		pool_free(&out);
		return true;
	}
	case CMD_SELECT_ALL: {
		u64 lo, hi;
		part_range(b, p, &lo, &hi);
		if (!part_is_code(b, p) && hi > lo) {
			s->anchor[p] = lo;
			s->cur[p] = hi - 1;
		}
		return true;
	}
	default:
		return false;
	}
}

void binview_context(View *v, int x, int y)
{
	if (PROF_VIEW(v)) {
		profview_context(v, x, y);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	bool code = part_is_code(b, v->part);
	app.ctx = CTX_EDITOR;
	menu_open(x, y);
	if (!code) {
		menu_item("Tag Selection...", CMD_HEX_ADD_TAG, true);
		menu_item("Remove Tag", CMD_HEX_REMOVE_TAG, true);
		menu_sep();
	}
	menu_item(code ? "Copy Rows" : "Copy as Hex", CMD_COPY, true);
	menu_item(code ? "Go to Address..." : "Go to Offset...", CMD_HEX_GOTO, true);
	if (!code)
		menu_item("Toggle Inspector Byte Order", CMD_HEX_ENDIAN, true);
}

/* Status bar text for binary views. */
void binview_status(View *v, char *buf, size_t cap)
{
	if (PROF_VIEW(v)) {
		profview_status(v, buf, cap);
		return;
	}
	BinDoc *b = bd_get(v->doc);
	BinView *s = bv_get(v, b);
	u32 p = v->part;
	const char *fmt = b->prc == 0 || b->prc == -1 ? b->img.format_name : "Binary";
	const char *arch = b->prc == 0 || b->prc == -1 ? b->img.arch_name : "";
	if (part_is_code(b, p)) {
		DisJob *j = dis_for(b, p);
		if (atomic_load(&j->state) == 2 && s->cur[p] < j->dis.n)
			snprintf(buf, cap, "%s %s  0x%llx", fmt ? fmt : "", arch ? arch : "", (unsigned long long)bin_row_addr(&b->img, &j->dis, (u32)s->cur[p]));
		else
			snprintf(buf, cap, "%s %s", fmt ? fmt : "", arch ? arch : "");
	} else {
		snprintf(buf, cap, "%s %s  Offset 0x%llX", fmt ? fmt : "", arch ? arch : "", (unsigned long long)s->cur[p]);
	}
}
