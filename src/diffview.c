/* Diff editor (side by side, like VSCodium's), range staging, and merge
 * conflict decorations.
 *
 * A diff view is an ordinary View whose `doc` is the right side (the real
 * working file for working-tree diffs, so it stays editable) and whose
 * `diff_left` is a read-only document with the original (index, HEAD or a
 * parent commit). Rows align the two sides: unchanged lines face each
 * other, changed lines are paired, and filler rows pad the shorter side.
 *
 * Conflicts are not hidden behind "Accept" buttons: the markers stay as
 * plain, editable text; the regions are only coloured and labelled. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "githunk.h"
#include "syntax.h"

#include <git2.h>
#include <stdio.h>
#include <SDL3/SDL.h>

enum { ROW_SAME, ROW_CHANGED, ROW_INSERT, ROW_DELETE };
#define NONE UINT32_MAX

/* Row cache per diff view (few are open at once). */
typedef struct {
	View *v;
	u32 lver, rver;
	u32 n, cap;
	u32 *rl, *rr; /* left / right line per row, NONE for filler */
	u8 *kind;
	u32 *hunk; /* hunk index per row, NONE outside hunks */
	GitHunks h;
	u32 last_head; /* to scroll to the cursor only when it moves */
} Rows;

static Rows cache[16];

static Rows *rows_for(View *v)
{
	for (int i = 0; i < 16; i++)
		if (cache[i].v == v)
			return &cache[i];
	for (int i = 0; i < 16; i++)
		if (!cache[i].v) {
			memset(&cache[i], 0, sizeof cache[i]);
			cache[i].v = v;
			cache[i].lver = cache[i].rver = NONE;
			cache[i].last_head = NONE;
			return &cache[i];
		}
	/* evict the first */
	githunk_free(&cache[0].h);
	free(cache[0].rl);
	free(cache[0].rr);
	free(cache[0].kind);
	free(cache[0].hunk);
	memset(&cache[0], 0, sizeof cache[0]);
	cache[0].v = v;
	cache[0].lver = cache[0].rver = NONE;
	cache[0].last_head = NONE;
	return &cache[0];
}

void diff_view_closed(View *v)
{
	for (int i = 0; i < 16; i++)
		if (cache[i].v == v) {
			githunk_free(&cache[i].h);
			free(cache[i].rl);
			free(cache[i].rr);
			free(cache[i].kind);
			free(cache[i].hunk);
			memset(&cache[i], 0, sizeof cache[i]);
		}
	if (v->diff_left != NONE)
		doc_release(v->diff_left);
	v->diff_left = NONE;
}

static void row_add(Rows *r, u32 l, u32 rr, u8 kind, u32 hunk)
{
	if (r->n == r->cap) {
		r->cap = r->cap ? r->cap * 2 : 1024;
		r->rl = vx_realloc(r->rl, r->cap * sizeof(u32));
		r->rr = vx_realloc(r->rr, r->cap * sizeof(u32));
		r->kind = vx_realloc(r->kind, r->cap);
		r->hunk = vx_realloc(r->hunk, r->cap * sizeof(u32));
	}
	r->rl[r->n] = l;
	r->rr[r->n] = rr;
	r->kind[r->n] = kind;
	r->hunk[r->n] = hunk;
	r->n++;
}

static void rows_build(View *v, Rows *r)
{
	Text *L = &app.docs.text[v->diff_left], *R = &app.docs.text[v->doc];
	if (r->lver == L->version && r->rver == R->version)
		return;
	r->lver = L->version;
	r->rver = R->version;
	githunk_free(&r->h);
	const char *lt = text_flat(L);
	u32 ln = L->len;
	char *lcopy = vx_strndup(lt, ln); /* text_flat buffers are per document; keep both stable */
	githunk_diff(lcopy, ln, text_flat(R), R->len, &r->h);
	free(lcopy);
	r->n = 0;
	u32 nl = text_lines(L), nr = text_lines(R);
	/* a trailing newline yields an empty last "line": keep the counts aligned */
	u32 ol = 0, rl = 0;
	for (u32 k = 0; k < r->h.n; k++) {
		u32 os = r->h.old_start[k], oc = r->h.old_lines[k];
		u32 ns = r->h.new_start[k], nc = r->h.new_lines[k];
		while (ol < os && rl < ns)
			row_add(r, ol++, rl++, ROW_SAME, NONE);
		u32 m = VX_MIN(oc, nc);
		for (u32 i = 0; i < m; i++)
			row_add(r, ol++, rl++, ROW_CHANGED, k);
		for (u32 i = m; i < oc; i++)
			row_add(r, ol++, NONE, ROW_DELETE, k);
		for (u32 i = m; i < nc; i++)
			row_add(r, NONE, rl++, ROW_INSERT, k);
	}
	while (ol < nl || rl < nr) {
		row_add(r, ol < nl ? ol : NONE, rl < nr ? rl : NONE, ROW_SAME, NONE);
		ol++;
		rl++;
	}
}

/* Row showing right-hand line l (or the nearest filler before it). */
static u32 row_of_right(Rows *r, u32 l)
{
	for (u32 i = 0; i < r->n; i++)
		if (r->rr[i] != NONE && r->rr[i] >= l)
			return i;
	return r->n ? r->n - 1 : 0;
}

/* ---- opening diffs ---------------------------------------------------------------- */

extern View *show_view_diff(u32 doc, u32 left, u8 kind, const char *rel);

static char *read_rev(const char *workdir, const char *spec, size_t *n)
{
	git_repository *repo = NULL;
	char *out = NULL;
	*n = 0;
	if (git_repository_open(&repo, workdir) != 0)
		return NULL;
	git_object *obj = NULL;
	if (git_revparse_single(&obj, repo, spec) == 0 && git_object_type(obj) == GIT_OBJECT_BLOB) {
		git_blob *b = (git_blob *)obj;
		*n = (size_t)git_blob_rawsize(b);
		out = vx_strndup(git_blob_rawcontent(b), *n);
	}
	git_object_free(obj);
	git_repository_free(repo);
	return out;
}

static int lang_of(const char *rel, const char *text, size_t n)
{
	return syn_lang_for_path(rel, text, VX_MIN(n, (size_t)512));
}

static const char *base_of(const char *rel)
{
	const char *s = strrchr(rel, '/');
	return s ? s + 1 : rel;
}

void diff_open_worktree(const char *rel)
{
	if (!app.git)
		return;
	char abs[8192];
	snprintf(abs, sizeof abs, "%s%s", git_workdir(app.git), rel);
	char *idx = NULL;
	size_t ni = 0;
	int rc = githunk_read_index(git_workdir(app.git), rel, &idx, &ni);
	if (rc < 0) {
		ui_status("Git: %s", githunk_error());
		return;
	}
	u32 d = doc_open(abs);
	if (d == NONE) {
		free(idx);
		return;
	}
	char name[600];
	snprintf(name, sizeof name, "%s (Index)", base_of(rel));
	u32 left = doc_new_virtual(name, idx ? idx : "", ni, lang_of(rel, idx, ni));
	free(idx);
	show_view_diff(d, left, DIFF_WORKTREE, rel);
}

void diff_open_staged(const char *rel)
{
	if (!app.git)
		return;
	char *head = NULL, *idx = NULL;
	size_t nh = 0, ni = 0;
	githunk_read_head(git_workdir(app.git), rel, &head, &nh);
	if (githunk_read_index(git_workdir(app.git), rel, &idx, &ni) < 0) {
		free(head);
		ui_status("Git: %s", githunk_error());
		return;
	}
	char name[600];
	snprintf(name, sizeof name, "%s (HEAD)", base_of(rel));
	u32 left = doc_new_virtual(name, head ? head : "", nh, lang_of(rel, head, nh));
	snprintf(name, sizeof name, "%s (Index)", base_of(rel));
	u32 right = doc_new_virtual(name, idx ? idx : "", ni, lang_of(rel, idx, ni));
	free(head);
	free(idx);
	show_view_diff(right, left, DIFF_STAGED, rel);
}

void diff_open_commit(const char *hash, const char *rel)
{
	if (!app.git)
		return;
	char spec[1200];
	size_t no = 0, nn = 0;
	snprintf(spec, sizeof spec, "%s^:%s", hash, rel);
	char *old = read_rev(git_workdir(app.git), spec, &no);
	snprintf(spec, sizeof spec, "%s:%s", hash, rel);
	char *new_ = read_rev(git_workdir(app.git), spec, &nn);
	char name[600];
	snprintf(name, sizeof name, "%s (%.7s^)", base_of(rel), hash);
	u32 left = doc_new_virtual(name, old ? old : "", no, lang_of(rel, new_ ? new_ : old, new_ ? nn : no));
	snprintf(name, sizeof name, "%s (%.7s)", base_of(rel), hash);
	u32 right = doc_new_virtual(name, new_ ? new_ : "", nn, lang_of(rel, new_, nn));
	free(old);
	free(new_);
	show_view_diff(right, left, DIFF_COMMIT, rel);
}

/* Reload the index-side documents of open diffs after staging. */
static void refresh_index_sides(const char *rel)
{
	for (u32 gi = 0; gi < app.ngroups; gi++)
		for (u32 t = 0; t < app.groups[gi].nt; t++) {
			View *v = gview(&app.groups[gi], t);
			if (v->diff_left == NONE || strcmp(view_rel(v), rel))
				continue;
			u32 d = v->diff_kind == DIFF_WORKTREE ? v->diff_left : v->diff_kind == DIFF_STAGED ? v->doc : NONE;
			if (d == NONE)
				continue;
			char *idx = NULL;
			size_t ni = 0;
			if (githunk_read_index(git_workdir(app.git), rel, &idx, &ni) < 0)
				continue;
			Text *tx = &app.docs.text[d];
			text_replace(tx, 0, tx->len, idx ? idx : "", (u32)ni, 0);
			app.docs.lstate_ok[d] = 0;
			free(idx);
		}
	git_request_refresh(0);
	for (u32 d = 0; d < app.docs.n; d++)
		app.docs.gm_for[d] = UINT32_MAX;
	app.dirty = true;
}

/* ---- drawing ------------------------------------------------------------------------ */

static int pane_gutter(void) { return PX(10) + 5 * app.char_w + PX(10); }

static void draw_words(Rows *r, u32 row, bool left, Text *t, u32 line, int tx, int ly, int lh, int x0, int x1)
{
	u32 k = r->hunk[row];
	if (k == NONE || r->kind[row] != ROW_CHANGED)
		return;
	u32 ls = text_line_start(t, line), le = ls + text_line_len(t, line);
	for (u32 c = 0; c < r->h.chg_count[k]; c++) {
		u32 ci = r->h.chg_first[k] + c;
		u32 off = left ? r->h.c_old_off[ci] : r->h.c_new_off[ci];
		u32 len = left ? r->h.c_old_len[ci] : r->h.c_new_len[ci];
		if (off + len <= ls || off >= le || !len)
			continue;
		u32 a = VX_MAX(off, ls), b = VX_MIN(off + len, le);
		int xa = tx + ed_x_of(t, line, a), xb = tx + ed_x_of(t, line, b);
		xa = VX_MAX(xa, x0);
		xb = VX_MIN(xb, x1);
		if (xb > xa)
			draw_rect(xa, ly, xb - xa, lh, left ? (COL(C_DIFF_REMOVED) | 0x30u) : (COL(C_DIFF_INSERTED) | 0x30u));
	}
}

static int g_pane_h;

void diff_draw(Group *g, View *v, int x, int y, int w, int h, bool focused)
{
	(void)g;
	Rows *r = rows_for(v);
	rows_build(v, r);
	g_pane_h = h;
	int lh = app.line_h;
	int half = w / 2;
	int gut = pane_gutter();
	Text *L = &app.docs.text[v->diff_left], *R = &app.docs.text[v->doc];
	/* keep the cursor's row visible when it moved */
	if (v->head[0] != r->last_head) {
		r->last_head = v->head[0];
		u32 row = row_of_right(r, text_line_of(R, v->head[0]));
		i64 ry = (i64)row * lh;
		if (ry < v->scroll_y)
			v->scroll_y = ry;
		else if (ry + lh > v->scroll_y + h)
			v->scroll_y = ry + lh - h;
	}
	i64 maxs = VX_MAX((i64)r->n * lh - h / 2, 0);
	v->scroll_y = VX_CLAMP(v->scroll_y, 0, maxs);
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	u32 first = (u32)(v->scroll_y / lh);
	int y0 = y - (int)(v->scroll_y % lh);
	u32 last = VX_MIN(r->n, first + (u32)(h / lh) + 2);
	u32 filler = (COL(C_FG) & 0xFFFFFF00u) | 0x0Cu;
	for (int side = 0; side < 2; side++) {
		int px = x + side * half, pw = side ? w - half : half;
		int tx = px + gut - v->scroll_x;
		Text *t = side ? R : L;
		u32 d = side ? v->doc : v->diff_left;
		draw_clip(px, y, pw, h);
		for (u32 row = first; row < last; row++) {
			int ly = y0 + (int)(row - first) * lh;
			u32 line = side ? r->rr[row] : r->rl[row];
			u8 k = r->kind[row];
			if (line == NONE) {
				draw_rect(px, ly, pw, lh, filler);
				continue;
			}
			if (k == ROW_CHANGED || (side == 0 && k == ROW_DELETE) || (side == 1 && k == ROW_INSERT))
				draw_rect(px, ly, pw, lh, side ? COL(C_DIFF_INSERTED) : COL(C_DIFF_REMOVED));
			draw_words(r, row, side == 0, t, line, tx, ly, lh, px + gut, px + pw);
			/* selections (right side) */
			if (side == 1) {
				u32 ls = text_line_start(t, line), le = ls + text_line_len(t, line);
				for (u32 c = 0; c < v->nc; c++) {
					u32 a = VX_MIN(v->head[c], v->anchor[c]), b = VX_MAX(v->head[c], v->anchor[c]);
					if (a == b || b < ls || a > le)
						continue;
					int xa = tx + ed_x_of(t, line, VX_MAX(a, ls)), xb = tx + ed_x_of(t, line, VX_MIN(b, le));
					if (b > le)
						xb += app.char_w / 2;
					if (xb > xa)
						draw_rect(xa, ly, xb - xa, lh, COL(C_SELECTION));
				}
			}
			char num[16];
			int kk = snprintf(num, sizeof num, "%u", line + 1);
			draw_text(FONT_MONO, px + gut - PX(10) - kk * app.char_w, ly, num, (size_t)kk, COL(C_LINE_NUMBER));
			draw_clip(px + gut, y, pw - gut, h);
			ed_draw_line_text(d, line, tx, ly, px + gut, px + pw);
			draw_clip(px, y, pw, h);
			/* cursor */
			if (side == 1 && focused)
				for (u32 c = 0; c < v->nc; c++)
					if (text_line_of(t, v->head[c]) == line && v->head[c] >= text_line_start(t, line)) {
						int cx = tx + ed_x_of(t, line, v->head[c]);
						if (cx >= px + gut)
							draw_rect(cx, ly, PX(2), lh, COL(C_CURSOR));
					}
		}
		/* hunk actions at the first row of each change (right gutter):
		 * stage + revert for working-tree diffs, unstage for staged ones */
		if (side == 1 && v->diff_kind != DIFF_COMMIT)
			for (u32 row = first; row < last; row++) {
				if (r->hunk[row] == NONE || (row > 0 && r->hunk[row - 1] == r->hunk[row]))
					continue;
				int ly = y0 + (int)(row - first) * lh;
				int ix = px + PX(2);
				int iy = ly + (lh - PX(16)) / 2;
				if (v->diff_kind == DIFF_WORKTREE) {
					draw_icon(ICON_PLUS, ix, iy, COL(C_FG));
					draw_icon(ICON_DISCARD, ix + PX(16), iy, COL(C_FG));
				} else {
					draw_icon(ICON_MINUS, ix, iy, COL(C_FG));
				}
			}
		draw_noclip();
		/* split line */
		if (side == 1)
			draw_rect(px - PX(1), y, PX(1), h, COL(C_TABS_BORDER));
	}
	/* overview: changes in the scrollbar lane at the far right */
	if (r->n)
		for (u32 row = 0; row < r->n; row++) {
			if (r->kind[row] == ROW_SAME)
				continue;
			int my = y + (int)((i64)row * h / (i64)r->n);
			u32 c = r->kind[row] == ROW_DELETE ? COL(C_GUTTER_DELETED) : r->kind[row] == ROW_INSERT ? COL(C_GUTTER_ADDED) : COL(C_GUTTER_MODIFIED);
			draw_rect(x + w - PX(6), my, PX(5), PX(2), c);
		}
}

/* ---- range staging ---------------------------------------------------------------- */

/* Line ranges covered by the view's cursors/selections (0-based, inclusive). */
static u32 sel_ranges(View *v, u32 *lo, u32 *hi, u32 max)
{
	Text *t = &app.docs.text[v->doc];
	u32 n = 0;
	for (u32 c = 0; c < v->nc && n < max; c++) {
		u32 a = VX_MIN(v->head[c], v->anchor[c]), b = VX_MAX(v->head[c], v->anchor[c]);
		u32 la = text_line_of(t, a), lb = text_line_of(t, b);
		if (lb > la && b == text_line_start(t, lb))
			lb--;
		lo[n] = la;
		hi[n] = lb;
		n++;
	}
	return n;
}

/* Replace doc text with new text as one minimal, undoable edit. */
static void doc_set_text_minimal(u32 d, const char *s, size_t n)
{
	Text *t = &app.docs.text[d];
	const char *o = text_flat(t);
	u32 on = t->len;
	u32 p = 0;
	while (p < on && p < n && o[p] == s[p])
		p++;
	u32 q = 0;
	while (q < on - p && q < n - p && o[on - 1 - q] == s[n - 1 - q])
		q++;
	doc_replace(d, p, on - p - q, s + p, (u32)(n - p - q), ed_new_group());
}

/* the repository path a view stands for: the diffed path, else its file's */
static const char *repo_rel_of(View *v)
{
	if (v->diff_left != NONE)
		return view_rel(v);
	return doc_repo_rel(v->doc);
}

static void stage_lines(View *v, u32 *lo, u32 *hi, u32 n, int what)
{
	const char *rel = repo_rel_of(v);
	if (!rel || !app.git || !n)
		return;
	char relc[1024];
	snprintf(relc, sizeof relc, "%s", rel);
	Text *t = &app.docs.text[v->doc];
	int rc = 0;
	if (what == 0) { /* stage: worktree lines */
		if (v->diff_left != NONE && v->diff_kind != DIFF_WORKTREE)
			return;
		rc = githunk_stage_ranges(git_workdir(app.git), relc, text_flat(t), t->len, lo, hi, n);
	} else if (what == 1) { /* unstage: index lines (staged diff) */
		if (v->diff_left == NONE || v->diff_kind != DIFF_STAGED) {
			ui_status("Unstage Selected Ranges works in a staged-changes diff (Open Staged Changes)");
			return;
		}
		rc = githunk_unstage_ranges(git_workdir(app.git), relc, lo, hi, n);
	} else { /* revert: worktree lines back to the index */
		if (v->diff_left != NONE && v->diff_kind != DIFF_WORKTREE)
			return;
		size_t nn = 0;
		char *s = githunk_revert_ranges(git_workdir(app.git), relc, text_flat(t), t->len, lo, hi, n, &nn);
		if (!s) {
			rc = -1;
		} else {
			doc_set_text_minimal(v->doc, s, nn);
			free(s);
		}
	}
	if (rc != 0)
		ui_status("Git: %s", githunk_error());
	refresh_index_sides(relc);
}

/* ---- conflicts ----------------------------------------------------------------------- */

typedef struct {
	u32 doc, ver;
	GitMarkers m;
} ConflictCache;
static ConflictCache cc[MAX_DOCS];
static bool cc_init;

u32 conflict_regions(u32 d, u32 **start, u32 **sep, u32 **end, u32 **base)
{
	if (!cc_init) {
		for (u32 i = 0; i < MAX_DOCS; i++)
			cc[i].ver = NONE;
		cc_init = true;
	}
	Text *t = &app.docs.text[d];
	ConflictCache *c = &cc[d];
	if (c->ver != t->version || c->doc != d) {
		githunk_markers_free(&c->m);
		c->doc = d;
		c->ver = t->version;
		/* cheap pre-check before scanning big files */
		if (t->len <= (16u << 20)) {
			const char *s = text_flat(t);
			if (strstr(s, "<<<<<<<"))
				githunk_find_markers(s, t->len, &c->m);
		}
	}
	*start = c->m.start;
	*sep = c->m.sep;
	*end = c->m.end;
	*base = c->m.base;
	return c->m.n;
}

/* Colour conflict regions (merge.* colours) and label the marker lines;
 * the markers themselves stay ordinary text. */
void conflict_draw(View *v, u32 first, u32 last, int x, int text_x, int text_w, int y0, int lh, int tx)
{
	(void)x;
	u32 *st, *sp, *en, *bs;
	u32 n = conflict_regions(v->doc, &st, &sp, &en, &bs);
	if (!n)
		return;
	Text *t = &app.docs.text[v->doc];
	const u32 cur_h = 0x40A6FF80u, cur_c = 0x40A6FF33u, inc_h = 0x40C8AE80u, inc_c = 0x40C8AE33u;
	const u32 com_h = 0x60606080u, com_c = 0x60606066u;
	for (u32 k = 0; k < n; k++) {
		if (en[k] < first || st[k] >= last)
			continue;
		for (u32 l = VX_MAX(st[k], first); l <= en[k] && l < last; l++) {
			u32 col;
			const char *label = NULL;
			bool has_base = bs[k] != NONE;
			if (l == st[k]) {
				col = cur_h;
				label = "(Current Change)";
			} else if (has_base && l == bs[k]) {
				col = com_h;
				label = "(Common Ancestors)";
			} else if (l == sp[k]) {
				col = inc_c;
			} else if (l == en[k]) {
				col = inc_h;
				label = "(Incoming Change)";
			} else if (l < (has_base ? bs[k] : sp[k])) {
				col = cur_c;
			} else if (has_base && l < sp[k]) {
				col = com_c;
			} else {
				col = inc_c;
			}
			int ly = y0 + (int)(l - first) * lh;
			draw_rect(text_x, ly, text_w, lh, col);
			if (label) {
				int ex = tx + ed_x_of(t, l, text_line_start(t, l) + text_line_len(t, l)) + app.char_w * 2;
				draw_text(FONT_MONO_ITALIC, ex, ly, label, strlen(label), COL(C_CODELENS));
			}
		}
	}
}

static void goto_conflict(View *v, int dir)
{
	u32 *st, *sp, *en, *bs;
	u32 n = conflict_regions(v->doc, &st, &sp, &en, &bs);
	if (!n) {
		ui_status("No merge conflicts found in this file");
		return;
	}
	Text *t = &app.docs.text[v->doc];
	u32 cur = text_line_of(t, v->head[0]);
	u32 pick = dir > 0 ? st[0] : st[n - 1];
	for (u32 k = 0; k < n; k++) {
		u32 kk = dir > 0 ? k : n - 1 - k;
		if ((dir > 0 && st[kk] > cur) || (dir < 0 && st[kk] < cur)) {
			pick = st[kk];
			break;
		}
	}
	ed_set_cursor(v, text_line_start(t, pick), false);
	ed_reveal(v, v->head[0], true);
}

/* ---- events ---------------------------------------------------------------------- */

void diff_mouse(View *v, const Event *e, int x, int y, int w, int h)
{
	Rows *r = rows_for(v);
	rows_build(v, r);
	int half = w / 2, gut = pane_gutter();
	int lh = app.line_h;
	if (e->type == EV_WHEEL) {
		v->scroll_y -= (i64)(e->wy * 3 * lh);
		v->scroll_x -= (int)(e->wx * 3 * app.char_w * 4);
		if (v->scroll_x < 0)
			v->scroll_x = 0;
		app.dirty = true;
		return;
	}
	if (!(e->type == EV_MDOWN || (e->type == EV_MMOVE && app.mouse_down)))
		return;
	i64 py = (i64)(e->y - y) + v->scroll_y;
	u32 row = py < 0 ? 0 : (u32)(py / lh);
	if (row >= r->n)
		row = r->n ? r->n - 1 : 0;
	if (!r->n)
		return;
	(void)h;
	/* hunk buttons in the right gutter */
	if (e->type == EV_MDOWN && e->x >= x + half && e->x < x + half + PX(34) && r->hunk[row] != NONE &&
	    (row == 0 || r->hunk[row - 1] != r->hunk[row]) && v->diff_kind != DIFF_COMMIT) {
		u32 k = r->hunk[row];
		bool second = e->x >= x + half + PX(18);
		if (v->diff_kind == DIFF_WORKTREE) {
			u32 lo = r->h.new_start[k], hi = lo + (r->h.new_lines[k] ? r->h.new_lines[k] - 1 : 0);
			stage_lines(v, &lo, &hi, 1, second ? 2 : 0);
		} else if (!second) {
			u32 lo = r->h.new_start[k], hi = lo + (r->h.new_lines[k] ? r->h.new_lines[k] - 1 : 0);
			stage_lines(v, &lo, &hi, 1, 1);
		}
		return;
	}
	if (e->x < x + half)
		return; /* the original side is read-only */
	/* nearest real right-hand line at or below this row */
	u32 rr = NONE;
	for (u32 i = row; i < r->n && rr == NONE; i++)
		rr = r->rr[i];
	for (u32 i = row + 1; i-- > 0 && rr == NONE;)
		rr = r->rr[i];
	if (rr == NONE)
		return;
	Text *t = &app.docs.text[v->doc];
	u32 off = ed_off_at_x(t, rr, e->x - (x + half + gut) + v->scroll_x);
	if (e->type == EV_MDOWN) {
		bool ext = e->mods & MOD_SHIFT;
		ed_set_cursor(v, off, ext);
		if (e->clicks == 2) {
			u32 a = off, b = off;
			while (a > 0 && is_word_byte(text_byte(t, a - 1)))
				a--;
			while (b < t->len && is_word_byte(text_byte(t, b)))
				b++;
			v->anchor[0] = a;
			v->head[0] = b;
		}
	} else {
		v->head[0] = off;
		v->nc = 1;
	}
	app.dirty = true;
}

bool diff_key(View *v, const Event *e)
{
	/* page keys scroll by rows; everything else edits the right side */
	if (e->key == SDLK_PAGEDOWN || e->key == SDLK_PAGEUP) {
		int rows = VX_MAX(g_pane_h / app.line_h - 1, 1);
		v->scroll_y += (e->key == SDLK_PAGEDOWN ? 1 : -1) * (i64)rows * app.line_h;
		app.dirty = true;
		return true;
	}
	return false;
}

/* Range commands work in normal editors (working-tree lines) and in diff
 * views; conflict commands in normal editors. */
bool diff_command(View *v, int cmd)
{
	u32 lo[MAX_CURSORS], hi[MAX_CURSORS];
	switch (cmd) {
	case CMD_GIT_STAGE_RANGES:
	case CMD_GIT_UNSTAGE_RANGES:
	case CMD_GIT_REVERT_RANGES: {
		if (!v)
			return true;
		u32 n = sel_ranges(v, lo, hi, MAX_CURSORS);
		stage_lines(v, lo, hi, n, cmd == CMD_GIT_STAGE_RANGES ? 0 : cmd == CMD_GIT_UNSTAGE_RANGES ? 1 : 2);
		return true;
	}
	case CMD_GIT_STAGE_CHANGE:
	case CMD_GIT_REVERT_CHANGE: {
		if (!v)
			return true;
		u32 l = app.ctx_off;
		app.ctx = CTX_NONE;
		stage_lines(v, &l, &l, 1, cmd == CMD_GIT_STAGE_CHANGE ? 0 : 2);
		return true;
	}
	case CMD_GIT_MARK_RESOLVED: {
		const char *rel = app.ctx == CTX_SCM_FILE ? app.ctx_path : v ? repo_rel_of(v) : NULL;
		app.ctx = CTX_NONE;
		if (!rel || !app.git)
			return true;
		/* save first: the file on disk is what gets staged */
		if (v && !strcmp(view_rel(v) ? view_rel(v) : "", rel) && text_dirty(&app.docs.text[v->doc]))
			doc_save(v->doc);
		if (githunk_mark_resolved(git_workdir(app.git), rel, false) != 0)
			ui_status("Git: %s", githunk_error());
		else
			ui_status("Marked '%s' as resolved", rel);
		git_request_refresh(0);
		return true;
	}
	case CMD_CONFLICT_NEXT:
	case CMD_CONFLICT_PREV:
		if (v)
			goto_conflict(v, cmd == CMD_CONFLICT_NEXT ? 1 : -1);
		return true;
	case CMD_DIFF_NEXT_CHANGE:
	case CMD_DIFF_PREV_CHANGE: {
		if (!v || v->diff_left == NONE)
			return true;
		Rows *r = rows_for(v);
		rows_build(v, r);
		Text *t = &app.docs.text[v->doc];
		u32 cur = row_of_right(r, text_line_of(t, v->head[0]));
		u32 n = r->n;
		for (u32 s = 1; s <= n; s++) {
			u32 row = cmd == CMD_DIFF_NEXT_CHANGE ? (cur + s) % n : (cur + n - s) % n;
			if (r->hunk[row] != NONE && (row == 0 || r->hunk[row - 1] != r->hunk[row])) {
				u32 rr = NONE;
				for (u32 i = row; i < n && rr == NONE; i++)
					rr = r->rr[i];
				if (rr != NONE)
					ed_set_cursor(v, text_line_start(t, rr), false);
				v->scroll_y = (i64)row * app.line_h - g_pane_h / 3;
				r->last_head = v->head[0];
				break;
			}
		}
		return true;
	}
	default:
		return false;
	}
}
