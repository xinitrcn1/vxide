/* Editor widget: drawing, multi-cursor editing, find, completion, hover. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <ctype.h>
#include <wctype.h>
#include <stdio.h>
#include <time.h>
#include <SDL3/SDL.h>

#define TAB 4
#define MAX_LINE_BYTES 8192
#define FM_COLS(a) SOA((a)->fm_a), SOA((a)->fm_b) /* find matches */

static u32 next_group = 1;

/* where the inline blame text was drawn (for its hover card) */
static int blame_rect_x0, blame_rect_x1 = -1, blame_rect_y;
static u32 blame_rect_doc = UINT32_MAX;
static char blame_rect_hash[16];

enum { GK_NONE, GK_TYPE, GK_DELETE, GK_OTHER };

static Text *T(View *v) { return &app.docs.text[v->doc]; }
static int tab_px(void) { return TAB * app.char_w; }

/* ---- geometry ----------------------------------------------------------- */

static int digits(u32 n)
{
	int d = 1;
	while (n >= 10) {
		n /= 10;
		d++;
	}
	return d;
}

int ed_gutter_w(View *v)
{
	int nd = VX_MAX(digits(text_lines(T(v))), 5);
	return PX(18) + nd * app.char_w + PX(26);
}

static int minimap_w(int w) { return app.minimap_on && w > PX(400) ? VX_MIN(PX(100), w / 8) : 0; }
#define SCROLL_W PX(14)

static int glyph_adv(u32 cp)
{
	if (cp < 128)
		return app.char_w;
	return font_glyphs.adv[font_glyph(FONT_MONO, cp)];
}

/* x (pixels from the text start, unscrolled) of byte off in line l */
static int x_of(Text *t, u32 l, u32 off)
{
	u32 n;
	const char *s = text_line(t, l, &n);
	u32 end = off - text_line_start(t, l);
	if (end > n)
		end = n;
	int x = 0;
	for (u32 i = 0; i < end;) {
		u8 c = (u8)s[i];
		if (c == '\t') {
			x += tab_px() - x % tab_px();
			i++;
		} else if (c < 0x80) {
			x += app.char_w;
			i++;
		} else {
			u32 cp;
			i += utf8_decode(s + i, n - i, &cp);
			x += glyph_adv(cp);
		}
	}
	return x;
}

/* nearest byte offset in line l for pixel x */
static u32 off_at_x(Text *t, u32 l, int px)
{
	u32 n;
	const char *s = text_line(t, l, &n);
	int x = 0;
	u32 i = 0;
	while (i < n) {
		u8 c = (u8)s[i];
		int a;
		u32 k = 1;
		if (c == '\t')
			a = tab_px() - x % tab_px();
		else if (c < 0x80)
			a = app.char_w;
		else {
			u32 cp;
			k = utf8_decode(s + i, n - i, &cp);
			a = glyph_adv(cp);
		}
		if (px < x + a / 2)
			break;
		x += a;
		i += k;
	}
	return text_line_start(t, l) + i;
}

u32 ed_line_col_to_off(Text *t, u32 line, u32 col)
{
	if (line >= text_lines(t))
		line = text_lines(t) - 1;
	u32 n = text_line_len(t, line);
	return text_line_start(t, line) + VX_MIN(col, n);
}

/* ---- highlighting ---------------------------------------------------------- */

void ed_lex_until(u32 d, u32 line)
{
	Docs *D = &app.docs;
	Text *t = &D->text[d];
	if (line >= text_lines(t))
		line = text_lines(t) - 1;
	VX_GROW(D->lstate[d], D->lstate_cap[d], text_lines(t) + 1);
	u32 l = D->lstate_ok[d];
	if (l > text_lines(t) - 1)
		l = text_lines(t) - 1;
	u32 st = l == 0 ? 0 : D->lstate[d][l];
	if (l == 0)
		D->lstate[d][0] = 0;
	while (l < line) {
		u32 n;
		const char *s = text_line(t, l, &n);
		st = syn_line(D->lang[d], st, s, n, NULL);
		D->lstate[d][++l] = st;
	}
	if (line > D->lstate_ok[d])
		D->lstate_ok[d] = line;
}

/* Per-byte token classes of one line, syntax then semantic overlay. */
static u32 sp_start[2048], sp_len[2048];
static u8 sp_cls[2048];
static u8 line_cls[MAX_LINE_BYTES];

static void classify_line(u32 d, u32 l, const char *s, u32 n)
{
	Docs *D = &app.docs;
	n = VX_MIN(n, (u32)MAX_LINE_BYTES);
	memset(line_cls, TK_TEXT, n);
	if (D->lang[d] != LANG_NONE) {
		SynSpans sp = {sp_start, sp_len, sp_cls, 0, 2048};
		ed_lex_until(d, l);
		syn_line(D->lang[d], D->lstate[d][l], s, n, &sp);
		for (u32 i = 0; i < sp.count; i++) {
			u32 a = sp.start[i], b = VX_MIN(a + sp.len[i], n);
			if (a < b)
				memset(line_cls + a, sp.cls[i], b - a);
		}
	}
	/* semantic tokens: binary search the first token on this line */
	u32 lo = 0, hi = D->sn[d];
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (D->s_line[d][mid] < l)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (u32 i = lo; i < D->sn[d] && D->s_line[d][i] == l; i++) {
		u8 c = D->s_cls[d][i];
		if (c == TK_TEXT || c == TK_COMMENT || c == TK_STRING || c == TK_NUMBER || c == TK_STORAGE ||
		    c == TK_OPERATOR)
			continue; /* the grammar already does these (and better) */
		u32 a = D->s_col[d][i], b = VX_MIN(a + D->s_len[d][i], n);
		if (a < b && line_cls[a] != TK_COMMENT && line_cls[a] != TK_STRING)
			memset(line_cls + a, c, b - a);
	}
}

/* Draw one document line's text (syntax + semantic colours) with its
 * origin at tx; glyphs left of clip_x0 or right of clip_x1 are skipped.
 * Used by the diff editor's two panes. */
void ed_draw_line_text(u32 d, u32 l, int tx, int ly, int clip_x0, int clip_x1)
{
	Text *t = &app.docs.text[d];
	if (l >= text_lines(t))
		return;
	u32 n;
	const char *s = text_line(t, l, &n);
	u32 nn = VX_MIN(n, (u32)MAX_LINE_BYTES);
	static char buf[MAX_LINE_BYTES];
	memcpy(buf, s, nn);
	classify_line(d, l, buf, nn);
	int px = 0;
	for (u32 i = 0; i < nn;) {
		u8 c = (u8)buf[i];
		if (c == '\t') {
			px += TAB * app.char_w - px % (TAB * app.char_w);
			i++;
			continue;
		}
		u32 cp = c, k = 1;
		if (c >= 0x80)
			k = utf8_decode(buf + i, nn - i, &cp);
		u8 st = tk_style[line_cls[i]];
		int font = (st & TS_BOLD) ? FONT_MONO_BOLD : (st & TS_ITALIC) ? FONT_MONO_ITALIC : FONT_MONO;
		u32 gid = font_glyph(font, cp);
		int gx = tx + px;
		if (gx > clip_x1)
			break;
		int adv = cp < 0x80 ? app.char_w : font_glyphs.adv[font_glyph(FONT_MONO, cp)];
		if (c != ' ' && gx + adv > clip_x0)
			draw_glyph(gid, gx, ly, TOK(line_cls[i]));
		px += adv;
		i += k;
	}
}

/* x of a byte offset within line l, and the offset nearest to x. */
int ed_x_of(Text *t, u32 l, u32 off);
u32 ed_off_at_x(Text *t, u32 l, int px);

/* ---- cursors ---------------------------------------------------------------- */

static void sort_cursors(View *v)
{
	/* insertion sort by head, then merge overlapping selections */
	for (u32 i = 1; i < v->nc; i++) {
		u32 h = v->head[i], a = v->anchor[i];
		i32 w = v->want_x[i];
		u32 j = i;
		while (j > 0 && VX_MIN(v->head[j - 1], v->anchor[j - 1]) > VX_MIN(h, a)) {
			v->head[j] = v->head[j - 1];
			v->anchor[j] = v->anchor[j - 1];
			v->want_x[j] = v->want_x[j - 1];
			j--;
		}
		v->head[j] = h;
		v->anchor[j] = a;
		v->want_x[j] = w;
	}
	u32 k = 0;
	for (u32 i = 0; i < v->nc; i++) {
		if (k > 0) {
			u32 pmax = VX_MAX(v->head[k - 1], v->anchor[k - 1]);
			u32 cmin = VX_MIN(v->head[i], v->anchor[i]);
			bool empty_both = v->head[k - 1] == v->anchor[k - 1] && v->head[i] == v->anchor[i];
			if (cmin < pmax || (cmin == pmax && empty_both)) {
				/* merge into k-1 */
				u32 lo = VX_MIN(VX_MIN(v->head[k - 1], v->anchor[k - 1]), cmin);
				u32 hi = VX_MAX(pmax, VX_MAX(v->head[i], v->anchor[i]));
				bool fwd = v->head[i] >= v->anchor[i];
				v->anchor[k - 1] = fwd ? lo : hi;
				v->head[k - 1] = fwd ? hi : lo;
				continue;
			}
		}
		v->head[k] = v->head[i];
		v->anchor[k] = v->anchor[i];
		v->want_x[k] = v->want_x[i];
		k++;
	}
	v->nc = k ? k : 1;
}

void ed_set_cursor(View *v, u32 off, bool extend)
{
	u32 len = T(v)->len;
	off = VX_MIN(off, len);
	v->nc = 1;
	v->head[0] = off;
	if (!extend)
		v->anchor[0] = off;
	v->want_x[0] = -1;
	v->group_kind = GK_NONE;
}

static int g_view_h = 600; /* editor height of the last draw, for paging */

void ed_reveal(View *v, u32 off, bool center)
{
	if (v->diff_left != UINT32_MAX) {
		app.dirty = true; /* the diff view scrolls in rows when it draws */
		return;
	}
	Text *t = T(v);
	u32 l = text_line_of(t, VX_MIN(off, t->len));
	i64 y = (i64)l * app.line_h;
	int h = g_view_h;
	if (center) {
		if (y < v->scroll_y || y + app.line_h > v->scroll_y + h)
			v->scroll_y = y - h / 2 + app.line_h / 2;
	} else {
		if (y < v->scroll_y)
			v->scroll_y = y;
		else if (y + app.line_h > v->scroll_y + h)
			v->scroll_y = y + app.line_h - h;
	}
	if (v->scroll_y < 0)
		v->scroll_y = 0;
	app.dirty = true;
}

static void clamp_scroll(View *v, int h)
{
	i64 max = (i64)(text_lines(T(v)) - 1) * app.line_h; /* scrollBeyondLastLine */
	if (v->scroll_y > max)
		v->scroll_y = max;
	if (v->scroll_y < 0)
		v->scroll_y = 0;
	if (v->scroll_x < 0)
		v->scroll_x = 0;
}

/* ---- editing primitives --------------------------------------------------- */

static u32 group_for(View *v, u8 kind)
{
	u64 now = vx_now_ns();
	if (kind == GK_OTHER || v->group_kind != kind || now - v->group_ns > 1500000000ull) {
		v->group = next_group++;
		v->group_kind = kind;
	}
	v->group_ns = now;
	return v->group;
}

static bool readonly_doc(View *v)
{
	if (!app.docs.readonly[v->doc])
		return false;
	ui_status("Cannot edit in read-only editor");
	return true;
}

/* ---- box (column) selection ---------------------------------------------------- */

static struct {
	bool on;
	u32 doc;
	u32 al, hl; /* anchor / head lines */
	int ax, hx; /* anchor / head x in pixels */
} box;

static u32 off_at_x(Text *t, u32 l, int px);
static int x_of(Text *t, u32 l, u32 off);

static void box_apply(View *v)
{
	Text *t = T(v);
	u32 l0 = VX_MIN(box.al, box.hl), l1 = VX_MAX(box.al, box.hl);
	v->nc = 0;
	for (u32 l = l0; l <= l1 && v->nc < MAX_CURSORS; l++) {
		/* VSCodium skips lines too short to reach the box (unless the box is empty) */
		int end = x_of(t, l, text_line_start(t, l) + text_line_len(t, l));
		if (end < VX_MIN(box.ax, box.hx) && box.ax != box.hx && l != box.hl)
			continue;
		view_reserve(v, v->nc + 1);
		u32 k = v->nc++;
		v->anchor[k] = off_at_x(t, l, box.ax);
		v->head[k] = off_at_x(t, l, box.hx);
		v->want_x[k] = box.hx;
	}
	if (!v->nc) {
		v->nc = 1;
		v->anchor[0] = v->head[0] = off_at_x(t, box.hl, box.hx);
	}
	ed_reveal(v, v->head[v->nc - 1], false);
	app.dirty = true;
}

static void box_start(View *v)
{
	Text *t = T(v);
	u32 c = v->nc - 1;
	box.on = true;
	box.doc = v->doc;
	box.al = text_line_of(t, v->anchor[c]);
	box.ax = x_of(t, box.al, v->anchor[c]);
	box.hl = text_line_of(t, v->head[c]);
	box.hx = x_of(t, box.hl, v->head[c]);
}

static void edit(View *v, u32 pos, u32 del, const char *ins, u32 n, u32 group)
{
	doc_replace(v->doc, pos, del, ins, n, group);
}

static u32 sel_lo(View *v, u32 c) { return VX_MIN(v->head[c], v->anchor[c]); }
static u32 sel_hi(View *v, u32 c) { return VX_MAX(v->head[c], v->anchor[c]); }

/* Insert at every cursor, replacing selections. */
void ed_insert_text(View *v, const char *s, size_t n, u8 kind)
{
	u32 g = group_for(v, kind);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		edit(v, a, b - a, s, (u32)n, g);
		v->head[c] = v->anchor[c] = a + (u32)n;
		v->want_x[c] = -1;
	}
	sort_cursors(v);
	ed_reveal(v, v->head[0], false);
}

static u32 prev_char(Text *t, u32 off)
{
	if (off == 0)
		return 0;
	off--;
	while (off > 0 && utf8_cont(text_byte(t, off)))
		off--;
	/* treat \r\n as one */
	if (off > 0 && text_byte(t, off) == '\n' && text_byte(t, off - 1) == '\r')
		off--;
	return off;
}

static u32 next_char(Text *t, u32 off)
{
	if (off >= t->len)
		return t->len;
	if (text_byte(t, off) == '\r' && text_byte(t, off + 1) == '\n')
		return off + 2;
	off++;
	while (off < t->len && utf8_cont(text_byte(t, off)))
		off++;
	return off;
}

static int char_kind(u8 c)
{
	if (c == ' ' || c == '\t')
		return 0;
	if (c == '\n' || c == '\r')
		return 3;
	if (strchr("`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?", c) && c)
		return 1;
	return 2;
}

static u32 word_right(Text *t, u32 off)
{
	u32 n = t->len;
	if (off < n && char_kind(text_byte(t, off)) == 3)
		return next_char(t, off);
	while (off < n && char_kind(text_byte(t, off)) == 0)
		off++;
	if (off < n && char_kind(text_byte(t, off)) == 3)
		return off;
	int k = off < n ? char_kind(text_byte(t, off)) : 0;
	while (off < n && char_kind(text_byte(t, off)) == k && k != 3)
		off++;
	return off;
}

static u32 word_left(Text *t, u32 off)
{
	if (off > 0 && char_kind(text_byte(t, off - 1)) == 3)
		return prev_char(t, off);
	while (off > 0 && char_kind(text_byte(t, off - 1)) == 0)
		off--;
	if (off > 0 && char_kind(text_byte(t, off - 1)) == 3)
		return off;
	int k = off > 0 ? char_kind(text_byte(t, off - 1)) : 0;
	while (off > 0 && char_kind(text_byte(t, off - 1)) == k && k != 3)
		off--;
	return off;
}

static void word_at(Text *t, u32 off, u32 *a, u32 *b)
{
	*a = *b = off;
	while (*a > 0 && is_word_byte(text_byte(t, *a - 1)))
		(*a)--;
	while (*b < t->len && is_word_byte(text_byte(t, *b)))
		(*b)++;
}

static u32 line_indent_end(Text *t, u32 l)
{
	u32 o = text_line_start(t, l), e = o + text_line_len(t, l);
	while (o < e && (text_byte(t, o) == ' ' || text_byte(t, o) == '\t'))
		o++;
	return o;
}

static void indent_unit(View *v, char *buf, u32 *n)
{
	if (app.docs.tabs[v->doc]) {
		buf[0] = '\t';
		*n = 1;
	} else {
		memset(buf, ' ', TAB);
		*n = TAB;
	}
}

/* ---- typed text ------------------------------------------------------------- */

static bool word_before(Text *t, u32 off) { return off > 0 && is_word_byte(text_byte(t, off - 1)); }

void ed_text(View *v, const char *s, size_t n)
{
	if (app.docs.bin[v->doc] && v->diff_left == UINT32_MAX)
		return;
	if (app.vim_on && vim_text(v, s, n))
		return;
	Text *t = T(v);
	box.on = false;
	if (readonly_doc(v))
		return;
	if (app.overtype && !memchr(s, '\n', n)) {
		/* overtype: each typed character replaces the next one on the line */
		u32 cps = 0;
		for (size_t i = 0; i < n; i++)
			cps += !utf8_cont((u8)s[i]);
		u32 g = group_for(v, GK_TYPE);
		for (u32 c = 0; c < v->nc; c++) {
			u32 a = sel_lo(v, c), b = sel_hi(v, c);
			if (a == b) {
				u32 l = text_line_of(t, a);
				u32 le = text_line_start(t, l) + text_line_len(t, l);
				for (u32 k = 0; k < cps && b < le; k++)
					b = next_char(t, b);
			}
			edit(v, a, b - a, s, (u32)n, g);
			v->head[c] = v->anchor[c] = a + (u32)n;
			v->want_x[c] = -1;
		}
		sort_cursors(v);
		ed_reveal(v, v->head[0], false);
		return;
	}
	app.hover_open = false;
	if (n == 1) {
		char c = s[0];
		static const char open_[] = "([{\"'", close_[] = ")]}\"'";
		const char *op = strchr(open_, c);
		const char *cl = strchr(close_, c);
		/* type over an auto-closed character */
		if (cl && c && v->nc == 1 && v->head[0] == v->anchor[0] && text_byte(t, v->head[0]) == (u8)c &&
		    (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'')) {
			u8 nb = text_byte(t, v->head[0]);
			if (nb == (u8)c && (c != '"' && c != '\'' ? true : !word_before(t, v->head[0]) || true)) {
				ed_set_cursor(v, v->head[0] + 1, false);
				return;
			}
		}
		if (op && c) {
			u8 after = text_byte(t, sel_hi(v, 0));
			bool quote = c == '"' || c == '\'';
			bool ok = (after == 0 || after == '\n' || after == '\r' || after == ' ' || after == '\t' ||
			           strchr(")]};,", after));
			if (quote && word_before(t, sel_lo(v, 0)))
				ok = false;
			/* surround selections */
			bool has_sel = v->head[0] != v->anchor[0];
			if (has_sel && v->nc == 1) {
				u32 a = sel_lo(v, 0), b = sel_hi(v, 0);
				u32 g = group_for(v, GK_OTHER);
				char pair[2] = {c, close_[op - open_]};
				edit(v, b, 0, pair + 1, 1, g);
				edit(v, a, 0, pair, 1, g);
				v->anchor[0] = a + 1;
				v->head[0] = b + 1;
				return;
			}
			if (ok) {
				char pair[2] = {c, close_[op - open_]};
				u32 g = group_for(v, GK_TYPE);
				for (u32 k = 0; k < v->nc; k++) {
					u32 a = sel_lo(v, k), b = sel_hi(v, k);
					edit(v, a, b - a, pair, 2, g);
					v->head[k] = v->anchor[k] = a + 1;
				}
				ed_reveal(v, v->head[0], false);
				ed_completion_trigger(v, false);
				return;
			}
		}
		if (c == '}' && v->nc == 1 && v->head[0] == v->anchor[0]) {
			/* outdent a closing brace typed on a blank line */
			u32 l = text_line_of(t, v->head[0]);
			u32 ls = text_line_start(t, l);
			if (line_indent_end(t, l) == v->head[0] && v->head[0] > ls) {
				u32 g = group_for(v, GK_OTHER);
				u32 cut = v->head[0] - ls;
				u32 rm = text_byte(t, v->head[0] - 1) == '\t' ? 1 : VX_MIN(cut, (u32)TAB);
				edit(v, v->head[0] - rm, rm, "}", 1, g);
				return;
			}
		}
	}
	/* consecutive typing (spaces included) is one undo step, as in VSCodium */
	ed_insert_text(v, s, n, GK_TYPE);
	ed_completion_trigger(v, false);
}

static void newline(View *v)
{
	Text *t = T(v);
	u32 g = group_for(v, GK_OTHER);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		u32 l = text_line_of(t, a);
		u32 ls = text_line_start(t, l);
		u32 ie = line_indent_end(t, l);
		u32 ind = VX_MIN(ie, a) - ls;
		char buf[512];
		const char *eol = text_eol(t);
		u32 el = (u32)strlen(eol);
		u32 k = 0;
		memcpy(buf, eol, el);
		k = el;
		ind = VX_MIN(ind, 400u);
		text_copy(t, ls, ind, buf + k);
		k += ind;
		/* skip whitespace after the cursor (VSCodium trims it into the new line) */
		u8 before = a > 0 ? text_byte(t, a - 1) : 0;
		u8 after = text_byte(t, b);
		u32 caret = k;
		if (before == '{' || before == '(' || before == '[' ||
		    (before == ':' && app.docs.lang[v->doc] == LANG_NONE)) {
			char unit[8];
			u32 un;
			indent_unit(v, unit, &un);
			memcpy(buf + k, unit, un);
			k += un;
			caret = k;
			if ((before == '{' && after == '}') || (before == '(' && after == ')') ||
			    (before == '[' && after == ']')) {
				memcpy(buf + k, eol, el);
				k += el;
				text_copy(t, ls, ind, buf + k);
				k += ind;
			}
		}
		/* remove trailing whitespace of an all-whitespace line (trimAutoWhitespace) */
		u32 del_from = a;
		if (ie >= a && a > ls && ie == a && text_line_len(t, l) == a - ls)
			del_from = ls;
		edit(v, del_from, b - del_from, buf, k, g);
		v->head[c] = v->anchor[c] = del_from + caret;
		v->want_x[c] = -1;
	}
	sort_cursors(v);
	ed_reveal(v, v->head[0], false);
}

static void backspace(View *v, bool word)
{
	Text *t = T(v);
	u32 g = group_for(v, GK_DELETE);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		if (a == b) {
			if (word)
				a = word_left(t, b);
			else {
				u32 l = text_line_of(t, b);
				u32 ls = text_line_start(t, l);
				/* in leading spaces: back to the previous tab stop */
				if (b > ls && line_indent_end(t, l) >= b && text_byte(t, b - 1) == ' ') {
					u32 col = b - ls;
					u32 to = (col - 1) / TAB * TAB;
					a = ls + to;
					for (u32 o = a; o < b; o++)
						if (text_byte(t, o) != ' ')
							a = o + 1;
				} else {
					a = prev_char(t, b);
				}
				/* delete an empty auto-closed pair */
				u8 p = a < b ? text_byte(t, a) : 0, q = text_byte(t, b);
				if (b - a == 1 && ((p == '(' && q == ')') || (p == '[' && q == ']') || (p == '{' && q == '}') ||
				                   (p == '"' && q == '"') || (p == '\'' && q == '\'')))
					b++;
			}
		}
		if (a < b)
			edit(v, a, b - a, "", 0, g);
		v->head[c] = v->anchor[c] = a;
		v->want_x[c] = -1;
	}
	sort_cursors(v);
	ed_reveal(v, v->head[0], false);
}

static void delete_fwd(View *v, bool word)
{
	Text *t = T(v);
	u32 g = group_for(v, GK_DELETE);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		if (a == b)
			b = word ? word_right(t, a) : next_char(t, a);
		if (a < b)
			edit(v, a, b - a, "", 0, g);
		v->head[c] = v->anchor[c] = a;
	}
	sort_cursors(v);
}

/* Lines touched by the cursors, as sorted unique ranges [l0, l1]. */
static u32 touched_lines(View *v, u32 *l0, u32 *l1)
{
	Text *t = T(v);
	u32 n = 0;
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = text_line_of(t, sel_lo(v, c));
		u32 hi = sel_hi(v, c);
		u32 b = text_line_of(t, hi);
		if (b > a && hi == text_line_start(t, b))
			b--; /* selection ending at column 0 excludes that line */
		if (n && a <= l1[n - 1] + 0) {
			if (b > l1[n - 1])
				l1[n - 1] = b;
			continue;
		}
		l0[n] = a;
		l1[n] = b;
		n++;
	}
	return n;
}

static void indent_lines(View *v, bool out)
{
	Text *t = T(v);
	static u32 l0[MAX_CURSORS], l1[MAX_CURSORS];
	u32 n = touched_lines(v, l0, l1);
	u32 g = group_for(v, GK_OTHER);
	char unit[8];
	u32 un;
	indent_unit(v, unit, &un);
	for (u32 r = n; r-- > 0;)
		for (u32 l = l1[r] + 1; l-- > l0[r];) {
			u32 ls = text_line_start(t, l);
			if (!out) {
				if (text_line_len(t, l) == 0)
					continue;
				edit(v, ls, 0, unit, un, g);
			} else {
				u32 k = 0;
				if (text_byte(t, ls) == '\t')
					k = 1;
				else
					while (k < TAB && text_byte(t, ls + k) == ' ')
						k++;
				if (k)
					edit(v, ls, k, "", 0, g);
			}
		}
	/* keep whole-line selections covering the lines */
	for (u32 c = 0; c < v->nc; c++)
		if (v->head[c] != v->anchor[c]) {
			u32 la = text_line_of(t, sel_lo(v, c));
			if (v->head[c] > v->anchor[c])
				v->anchor[c] = VX_MIN(v->anchor[c], text_line_start(t, la));
			else
				v->head[c] = VX_MIN(v->head[c], text_line_start(t, la));
		}
}

/* Wrap each touched line in open/close (Markdown's <!-- -->), or unwrap
 * when every non-blank line already is. */
static void toggle_block_comment(View *v, const char *open, const char *close)
{
	Text *t = T(v);
	static u32 l0[MAX_CURSORS], l1[MAX_CURSORS];
	u32 n = touched_lines(v, l0, l1);
	u32 ol = (u32)strlen(open), cl = (u32)strlen(close);
	bool all = true;
	for (u32 r = 0; r < n; r++)
		for (u32 l = l0[r]; l <= l1[r]; l++) {
			u32 ie = line_indent_end(t, l), le = text_line_start(t, l) + text_line_len(t, l);
			if (ie == le)
				continue;
			char a[16], b[16];
			if (le - ie < ol + cl) {
				all = false;
				continue;
			}
			text_copy(t, ie, ol, a);
			text_copy(t, le - cl, cl, b);
			if (memcmp(a, open, ol) || memcmp(b, close, cl))
				all = false;
		}
	u32 g = group_for(v, GK_OTHER);
	for (u32 r = n; r-- > 0;)
		for (u32 l = l1[r] + 1; l-- > l0[r];) {
			u32 ie = line_indent_end(t, l), le = text_line_start(t, l) + text_line_len(t, l);
			if (ie == le)
				continue;
			if (all) {
				edit(v, le - cl, cl, "", 0, g);
				edit(v, ie, ol, "", 0, g);
			} else {
				edit(v, le, 0, close, cl, g);
				edit(v, ie, 0, open, ol, g);
			}
		}
}

static void toggle_comment(View *v)
{
	Text *t = T(v);
	const char *pre = syn_line_comment(app.docs.lang[v->doc]);
	if (!pre && app.docs.lang[v->doc] != LANG_NONE) {
		toggle_block_comment(v, "<!-- ", " -->");
		return;
	}
	if (!pre)
		pre = "#";
	u32 pl = (u32)strlen(pre);
	static u32 l0[MAX_CURSORS], l1[MAX_CURSORS];
	u32 n = touched_lines(v, l0, l1);
	/* comment unless every non-blank line is commented */
	bool all = true;
	u32 minind = UINT32_MAX;
	for (u32 r = 0; r < n; r++)
		for (u32 l = l0[r]; l <= l1[r]; l++) {
			u32 ie = line_indent_end(t, l);
			u32 ls = text_line_start(t, l);
			if (ie == ls + text_line_len(t, l))
				continue;
			minind = VX_MIN(minind, ie - ls);
			char buf[16];
			text_copy(t, ie, VX_MIN(pl, t->len - ie), buf);
			if (t->len - ie < pl || memcmp(buf, pre, pl) != 0)
				all = false;
		}
	if (minind == UINT32_MAX)
		minind = 0;
	u32 g = group_for(v, GK_OTHER);
	for (u32 r = n; r-- > 0;)
		for (u32 l = l1[r] + 1; l-- > l0[r];) {
			u32 ls = text_line_start(t, l);
			u32 ie = line_indent_end(t, l);
			if (ie == ls + text_line_len(t, l))
				continue;
			if (all) {
				u32 k = pl;
				if (text_byte(t, ie + pl) == ' ')
					k++;
				edit(v, ie, k, "", 0, g);
			} else {
				char buf[16];
				memcpy(buf, pre, pl);
				buf[pl] = ' ';
				edit(v, ls + minind, 0, buf, pl + 1, g);
			}
		}
}

static void move_lines(View *v, int dir, bool copy)
{
	Text *t = T(v);
	static u32 l0[MAX_CURSORS], l1[MAX_CURSORS];
	u32 n = touched_lines(v, l0, l1);
	if (n != 1)
		n = 1; /* operate on the block of the first cursor range */
	u32 a = l0[0], b = l1[0];
	u32 nl = text_lines(t);
	if (!copy && ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= nl)))
		return;
	u32 g = group_for(v, GK_OTHER);
	u32 s = text_line_start(t, a);
	u32 e = b + 1 < nl ? text_line_start(t, b + 1) : t->len;
	u32 len = e - s;
	char *blk = vx_alloc(len + 2);
	text_copy(t, s, len, blk);
	bool lastline = b + 1 >= nl;
	const char *eol = text_eol(t);
	if (lastline) {
		/* block has no trailing newline: add one for moving */
		memcpy(blk + len, eol, strlen(eol));
		len += (u32)strlen(eol);
	}
	i64 shift = 0;
	if (copy) {
		edit(v, s, 0, blk, len, g);
		if (lastline) {
			/* inserted "block\n" before the block: fine */
		}
		shift = dir > 0 ? len : 0;
	} else if (dir < 0) {
		u32 ps = text_line_start(t, a - 1);
		u32 plen = s - ps;
		char *prev = vx_alloc(plen + 2);
		text_copy(t, ps, plen, prev);
		if (lastline) {
			/* previous line + block without its eol */
			edit(v, ps, e - ps, "", 0, g);
			u32 bl = len - (u32)strlen(eol);
			edit(v, ps, 0, blk, bl, g);
			edit(v, ps + bl, 0, eol, (u32)strlen(eol), g);
			edit(v, ps + bl + (u32)strlen(eol), 0, prev, plen - (u32)strlen(eol), g);
		} else {
			edit(v, ps, e - ps, "", 0, g);
			edit(v, ps, 0, prev, plen, g);
			edit(v, ps, 0, blk, len, g);
		}
		shift = -(i64)plen;
		free(prev);
	} else {
		u32 ne = b + 2 < nl ? text_line_start(t, b + 2) : t->len;
		u32 nlen = ne - e;
		char *next = vx_alloc(nlen + 4);
		text_copy(t, e, nlen, next);
		bool nlast = b + 2 >= nl;
		edit(v, s, ne - s, "", 0, g);
		if (nlast) {
			memcpy(next + nlen, eol, strlen(eol));
			nlen += (u32)strlen(eol);
			edit(v, s, 0, next, nlen, g);
			edit(v, s + nlen, 0, blk, len - (u32)strlen(eol), g);
		} else {
			edit(v, s, 0, next, nlen, g);
			edit(v, s + nlen, 0, blk, len, g);
		}
		shift = nlen;
		free(next);
	}
	free(blk);
	/* place cursors: same column, moved lines */
	(void)shift;
	for (u32 c = 0; c < v->nc; c++) {
		(void)c;
	}
	u32 newa = (u32)((i64)a + (copy ? (dir > 0 ? (b - a + 1) : 0) : dir));
	u32 la = text_line_of(t, sel_lo(v, 0));
	(void)la;
	/* simple: put the cursor at the same column on the moved first line */
	u32 col = 0;
	col = 0;
	ed_set_cursor(v, text_line_start(t, VX_MIN(newa, text_lines(t) - 1)) + col, false);
	if (b > a) {
		u32 endl = VX_MIN(newa + (b - a), text_lines(t) - 1);
		v->anchor[0] = text_line_start(t, newa);
		v->head[0] = text_line_start(t, endl) + text_line_len(t, endl);
	}
	ed_reveal(v, v->head[0], false);
}

static void delete_lines(View *v)
{
	Text *t = T(v);
	static u32 l0[MAX_CURSORS], l1[MAX_CURSORS];
	u32 n = touched_lines(v, l0, l1);
	u32 g = group_for(v, GK_OTHER);
	u32 first = l0[0];
	for (u32 r = n; r-- > 0;) {
		u32 s = text_line_start(t, l0[r]);
		u32 e = l1[r] + 1 < text_lines(t) ? text_line_start(t, l1[r] + 1) : t->len;
		if (e == t->len && s > 0 && l1[r] + 1 >= text_lines(t))
			s = prev_char(t, s) == s - 1 || text_byte(t, s - 1) == '\n' ? s - (t->crlf ? 2 : 1) : s;
		edit(v, s, e - s, "", 0, g);
	}
	first = VX_MIN(first, text_lines(t) - 1);
	ed_set_cursor(v, line_indent_end(t, first), false);
}

static void insert_line(View *v, bool above)
{
	Text *t = T(v);
	u32 g = group_for(v, GK_OTHER);
	u32 l = text_line_of(t, v->head[0]);
	u32 ls = text_line_start(t, l);
	u32 ind = line_indent_end(t, l) - ls;
	char buf[512];
	const char *eol = text_eol(t);
	u32 el = (u32)strlen(eol);
	ind = VX_MIN(ind, 400u);
	if (above) {
		text_copy(t, ls, ind, buf);
		memcpy(buf + ind, eol, el);
		edit(v, ls, 0, buf, ind + el, g);
		ed_set_cursor(v, ls + ind, false);
	} else {
		u32 le = ls + text_line_len(t, l);
		memcpy(buf, eol, el);
		text_copy(t, ls, ind, buf + el);
		edit(v, le, 0, buf, el + ind, g);
		ed_set_cursor(v, le + el + ind, false);
	}
	ed_reveal(v, v->head[0], false);
}

static void add_cursor_vert(View *v, int dir)
{
	Text *t = T(v);
	u32 c = dir < 0 ? 0 : v->nc - 1;
	/* use the extreme cursor in that direction */
	for (u32 k = 0; k < v->nc; k++) {
		if (dir < 0 && v->head[k] < v->head[c])
			c = k;
		if (dir > 0 && v->head[k] > v->head[c])
			c = k;
	}
	u32 l = text_line_of(t, v->head[c]);
	if ((dir < 0 && l == 0) || (dir > 0 && l + 1 >= text_lines(t)) || v->nc >= MAX_CURSORS)
		return;
	int x = v->want_x[c] >= 0 ? v->want_x[c] : x_of(t, l, v->head[c]);
	u32 nl = dir < 0 ? l - 1 : l + 1;
	u32 off = off_at_x(t, nl, x);
	view_reserve(v, v->nc + 1);
	u32 k = v->nc++;
	v->head[k] = v->anchor[k] = off;
	v->want_x[k] = x;
	v->want_x[c] = x;
	sort_cursors(v);
	ed_reveal(v, off, false);
}

static char *selection_text(View *v, size_t *n, bool *whole_line)
{
	Text *t = T(v);
	bool empty = true;
	for (u32 c = 0; c < v->nc; c++)
		if (v->head[c] != v->anchor[c])
			empty = false;
	*whole_line = empty;
	Pool p = {0};
	const char *eol = text_eol(t);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a, b;
		if (empty) {
			u32 l = text_line_of(t, v->head[c]);
			a = text_line_start(t, l);
			b = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
		} else {
			a = sel_lo(v, c);
			b = sel_hi(v, c);
		}
		char *buf = vx_alloc(b - a + 4);
		text_copy(t, a, b - a, buf);
		u32 len = b - a;
		if (empty && (len == 0 || buf[len - 1] != '\n')) {
			memcpy(buf + len, eol, strlen(eol));
			len += (u32)strlen(eol);
		}
		if (c && !empty) {
			VX_GROW(p.b, p.cap, p.n + (u32)strlen(eol) + 1);
			memcpy(p.b + p.n, eol, strlen(eol));
			p.n += (u32)strlen(eol);
		}
		VX_GROW(p.b, p.cap, p.n + len + 1);
		memcpy(p.b + p.n, buf, len);
		p.n += len;
		free(buf);
	}
	VX_GROW(p.b, p.cap, p.n + 1);
	p.b[p.n] = 0;
	*n = p.n;
	return p.b;
}

static void copy_sel(View *v, bool cut)
{
	size_t n;
	bool whole;
	char *s = selection_text(v, &n, &whole);
	ui_clipboard_set(s, n);
	app.clip_whole_line = whole;
	free(s);
	if (!cut)
		return;
	Text *t = T(v);
	u32 g = group_for(v, GK_OTHER);
	if (whole) {
		for (u32 c = v->nc; c-- > 0;) {
			u32 l = text_line_of(t, v->head[c]);
			u32 a = text_line_start(t, l);
			u32 b = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
			if (b == t->len && a > 0 && l + 1 >= text_lines(t))
				a = a - (t->crlf ? 2 : 1);
			edit(v, a, b - a, "", 0, g);
		}
	} else {
		for (u32 c = 0; c < v->nc; c++) {
			u32 a = sel_lo(v, c), b = sel_hi(v, c);
			edit(v, a, b - a, "", 0, g);
			v->head[c] = v->anchor[c] = a;
		}
	}
	sort_cursors(v);
}

static void paste(View *v)
{
	char *s = ui_clipboard_get();
	size_t n = strlen(s);
	Text *t = T(v);
	/* normalise line ends to the document's */
	if (!t->crlf) {
		size_t k = 0;
		for (size_t i = 0; i < n; i++)
			if (!(s[i] == '\r' && i + 1 < n && s[i + 1] == '\n'))
				s[k++] = s[i];
		n = k;
		s[n] = 0;
	}
	u32 g = group_for(v, GK_OTHER);
	/* one line per cursor when the counts match (multiCursorPaste: spread) */
	u32 lines = 1;
	for (size_t i = 0; i < n; i++)
		lines += s[i] == '\n';
	if (s[n ? n - 1 : 0] == '\n' && n)
		lines--;
	bool whole = app.clip_whole_line && v->nc == 1 && v->head[0] == v->anchor[0];
	if (v->nc > 1 && lines == v->nc) {
		const char *p = s;
		for (u32 c = 0; c < v->nc; c++) {
			const char *e = strchr(p, '\n');
			size_t len = e ? (size_t)(e - p) : strlen(p);
			if (len && p[len - 1] == '\r')
				len--;
			u32 a = sel_lo(v, c), b = sel_hi(v, c);
			edit(v, a, b - a, p, (u32)len, g);
			v->head[c] = v->anchor[c] = a + (u32)len;
			p = e ? e + 1 : p + len;
		}
	} else if (whole) {
		/* a copied whole line goes above the current line */
		u32 l = text_line_of(t, v->head[0]);
		u32 ls = text_line_start(t, l);
		u32 col = v->head[0] - ls;
		edit(v, ls, 0, s, (u32)n, g);
		v->head[0] = v->anchor[0] = ls + (u32)n + col;
	} else {
		bool over = app.overtype && !memchr(s, '\n', n);
		for (u32 c = 0; c < v->nc; c++) {
			u32 a = sel_lo(v, c), b = sel_hi(v, c);
			if (over && a == b) {
				/* editor.overtypeOnPaste: the pasted text replaces as many characters */
				u32 cps = 0;
				for (size_t i = 0; i < n; i++)
					cps += !utf8_cont((u8)s[i]);
				u32 l = text_line_of(t, a);
				u32 le = text_line_start(t, l) + text_line_len(t, l);
				for (u32 k = 0; k < cps && b < le; k++)
					b = next_char(t, b);
			}
			edit(v, a, b - a, s, (u32)n, g);
			v->head[c] = v->anchor[c] = a + (u32)n;
		}
	}
	free(s);
	sort_cursors(v);
	ed_reveal(v, v->head[0], false);
}

static void select_word_or_next(View *v, bool all)
{
	Text *t = T(v);
	u32 c = v->nc - 1;
	if (v->head[c] == v->anchor[c]) {
		u32 a, b;
		word_at(t, v->head[c], &a, &b);
		if (a == b)
			return;
		v->anchor[c] = a;
		v->head[c] = b;
		if (!all) {
			app.find_flags |= 0x200; /* whole word, case sensitive search for Ctrl+D */
			return;
		}
	}
	u32 a = sel_lo(v, c), b = sel_hi(v, c);
	u32 len = b - a;
	if (!len || len > 1000)
		return;
	char *q = vx_alloc(len + 1);
	text_copy(t, a, len, q);
	const char *hay = text_flat(t);
	int flags = SEARCH_CASE | ((app.find_flags & 0x200) ? SEARCH_WORD : 0);
	if (all) {
		v->nc = 0;
		u32 pos = 0;
		while (pos <= t->len && v->nc < MAX_CURSORS) {
			u32 m = find_in(hay + pos, t->len - pos, q, len, flags, 0, false);
			if (m == UINT32_MAX)
				break;
			m += pos;
			view_reserve(v, v->nc + 1);
			u32 k = v->nc++;
			v->anchor[k] = m;
			v->head[k] = m + len;
			v->want_x[k] = -1;
			pos = m + len;
		}
		if (!v->nc) {
			v->nc = 1;
			v->anchor[0] = a;
			v->head[0] = b;
		}
	} else {
		u32 m = find_in(hay, t->len, q, len, flags, b, false);
		if (m != UINT32_MAX && v->nc < MAX_CURSORS) {
			bool dup = false;
			for (u32 k = 0; k < v->nc; k++)
				if (sel_lo(v, k) == m)
					dup = true;
			if (!dup) {
				view_reserve(v, v->nc + 1);
				u32 k = v->nc++;
				v->anchor[k] = m;
				v->head[k] = m + len;
				v->want_x[k] = -1;
				sort_cursors(v);
				ed_reveal(v, m, false);
			}
		}
	}
	free(q);
}

/* ---- movement ------------------------------------------------------------- */

static void move_cursors(View *v, u32 key, int mods, int page_lines)
{
	box.on = false;
	Text *t = T(v);
	bool ext = mods & MOD_SHIFT;
	bool ctrl = mods & MOD_CTRL;
	for (u32 c = 0; c < v->nc; c++) {
		u32 h = v->head[c];
		bool has_sel = v->head[c] != v->anchor[c];
		u32 l = text_line_of(t, h);
		u32 ls = text_line_start(t, l);
		u32 le = ls + text_line_len(t, l);
		int vert = 0;
		switch (key) {
		case SDLK_LEFT:
			if (has_sel && !ext)
				h = sel_lo(v, c);
			else
				h = ctrl ? word_left(t, h) : prev_char(t, h);
			break;
		case SDLK_RIGHT:
			if (has_sel && !ext)
				h = sel_hi(v, c);
			else
				h = ctrl ? word_right(t, h) : next_char(t, h);
			break;
		case SDLK_HOME: {
			if (ctrl) {
				h = 0;
				break;
			}
			u32 ie = line_indent_end(t, l);
			h = h == ie ? ls : ie;
			break;
		}
		case SDLK_END:
			h = ctrl ? t->len : le;
			break;
		case SDLK_UP:
			vert = -1;
			break;
		case SDLK_DOWN:
			vert = 1;
			break;
		case SDLK_PAGEUP:
			vert = -page_lines;
			break;
		case SDLK_PAGEDOWN:
			vert = page_lines;
			break;
		}
		if (vert) {
			int x = v->want_x[c] >= 0 ? v->want_x[c] : x_of(t, l, h);
			i64 nl = (i64)l + vert;
			if (has_sel && !ext && (vert == -1 || vert == 1)) {
				/* collapse to the edge first, like VSCodium */
				h = vert < 0 ? sel_lo(v, c) : sel_hi(v, c);
				l = text_line_of(t, h);
				x = x_of(t, l, h);
				nl = (i64)l + vert;
			}
			if (nl < 0)
				h = 0;
			else if (nl >= (i64)text_lines(t))
				h = t->len;
			else
				h = off_at_x(t, (u32)nl, x);
			v->want_x[c] = x;
		} else {
			v->want_x[c] = -1;
		}
		v->head[c] = h;
		if (!ext)
			v->anchor[c] = h;
	}
	sort_cursors(v);
	v->group_kind = GK_NONE;
	ed_reveal(v, v->head[0], false);
}

/* ---- commands ---------------------------------------------------------------- */

static bool undo_redo_at(View *v, bool redo, u32 *at)
{
	u32 a, b;
	bool ok = redo ? text_redo(T(v), &a, &b) : text_undo(T(v), &a, &b);
	if (!ok)
		return false;
	if (at)
		*at = a;
	/* text_undo bypassed doc_replace: resync the dependent state */
	Docs *D = &app.docs;
	D->lstate_ok[v->doc] = 0;
	D->sn[v->doc] = 0;
	D->lsp_changed_ns[v->doc] = vx_now_ns();
	D->gm_due_ns[v->doc] = vx_now_ns() + 300000000ull;
	for (u32 gi = 0; gi < app.ngroups; gi++)
		for (u32 k = 0; k < app.groups[gi].nt; k++) {
			View *o = gview(&app.groups[gi], k);
			if (o->doc != v->doc)
				continue;
			for (u32 c = 0; c < o->nc; c++) {
				o->head[c] = VX_MIN(o->head[c], T(v)->len);
				o->anchor[c] = VX_MIN(o->anchor[c], T(v)->len);
			}
		}
	ed_set_cursor(v, b, false);
	ed_reveal(v, b, false);
	return true;
}

static void undo_redo(View *v, bool redo) { undo_redo_at(v, redo, NULL); }

/* Undo/redo one group; pos gets the start of the change (vim puts the cursor there). */
bool ed_undo_redo(View *v, bool redo, u32 *pos) { return undo_redo_at(v, redo, pos); }

/* Whole lines the last drawn editor shows (H/M/L, Ctrl+D, zz). */
int ed_view_lines(void) { return VX_MAX(g_view_h / VX_MAX(app.line_h, 1), 1); }

/* ---- case transforms (Transform to Uppercase/Lowercase/Title/Snake/Kebab) ---- */

enum { TR_UPPER, TR_LOWER, TR_TITLE, TR_SNAKE, TR_KEBAB };

static void put_cp(char **o, u32 *n, u32 *cap, u32 cp)
{
	char b[4];
	u32 k = utf8_encode(cp, b);
	VX_GROW(*o, *cap, *n + k + 1);
	memcpy(*o + *n, b, k);
	*n += k;
}

/* Transform one piece of text; returns a malloc'd string. */
static char *transform(const char *s, u32 n, int mode, u32 *outn)
{
	char *o = NULL;
	u32 on = 0, cap = 0;
	u32 prev = 0; /* previous code point */
	for (u32 i = 0; i < n;) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		i += k ? k : 1;
		u32 r = cp;
		switch (mode) {
		case TR_UPPER:
			r = (u32)towupper((wint_t)cp);
			break;
		case TR_LOWER:
			r = (u32)towlower((wint_t)cp);
			break;
		case TR_TITLE: {
			/* VSCodium: lower-case, then capitalise the first letter of each word */
			bool start = !prev || !iswalnum((wint_t)prev);
			r = start ? (u32)towupper((wint_t)cp) : (u32)towlower((wint_t)cp);
			break;
		}
		case TR_SNAKE:
		case TR_KEBAB: {
			/* fooBar / FooBAR baz -> foo_bar / foo_bar baz; separators become _ or - */
			u32 sep = mode == TR_SNAKE ? '_' : '-';
			if (cp == '_' || cp == '-') {
				r = sep;
				break;
			}
			bool up = iswupper((wint_t)cp);
			u32 nextcp = 0;
			if (i < n)
				utf8_decode(s + i, n - i, &nextcp);
			bool boundary = prev && up &&
			                (iswlower((wint_t)prev) || iswdigit((wint_t)prev) ||
			                 (iswupper((wint_t)prev) && nextcp && iswlower((wint_t)nextcp)));
			if (boundary)
				put_cp(&o, &on, &cap, sep);
			r = (u32)towlower((wint_t)cp);
			break;
		}
		}
		put_cp(&o, &on, &cap, r);
		prev = cp;
	}
	VX_GROW(o, cap, on + 1);
	o[on] = 0;
	*outn = on;
	return o;
}

static void transform_selections(View *v, int mode)
{
	Text *t = T(v);
	u32 g = group_for(v, GK_OTHER);
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		bool empty = a == b;
		if (empty)
			word_at(t, a, &a, &b); /* the word under the cursor, like VSCodium */
		if (a == b)
			continue;
		char *src = vx_alloc(b - a + 1);
		text_copy(t, a, b - a, src);
		u32 nn;
		char *dst = transform(src, b - a, mode, &nn);
		bool fwd = v->head[c] >= v->anchor[c];
		u32 h = v->head[c];
		edit(v, a, b - a, dst, nn, g);
		if (empty) {
			/* a cursor at the end of the word stays at its (new) end */
			v->head[c] = v->anchor[c] = h == b ? a + nn : VX_MIN(h, a + nn);
		} else {
			v->anchor[c] = fwd ? a : a + nn;
			v->head[c] = fwd ? a + nn : a;
		}
		free(src);
		free(dst);
	}
}

static void move_sel_next_match(View *v)
{
	/* Ctrl+K Ctrl+D: like Ctrl+D, but the last selection moves instead */
	u32 before = v->nc;
	u32 last_a = v->anchor[v->nc - 1], last_h = v->head[v->nc - 1];
	select_word_or_next(v, false);
	if (v->nc > before) {
		/* drop the old last selection */
		for (u32 c = 0; c < v->nc; c++)
			if (v->anchor[c] == last_a && v->head[c] == last_h) {
				memmove(v->head + c, v->head + c + 1, (v->nc - c - 1) * sizeof(u32));
				memmove(v->anchor + c, v->anchor + c + 1, (v->nc - c - 1) * sizeof(u32));
				memmove(v->want_x + c, v->want_x + c + 1, (v->nc - c - 1) * sizeof(i32));
				v->nc--;
				break;
			}
	}
}

bool ed_command(View *v, int cmd)
{
	Text *t = T(v);
	switch (cmd) {
	case CMD_CUT:
	case CMD_PASTE:
	case CMD_TOGGLE_COMMENT:
	case CMD_INDENT:
	case CMD_OUTDENT:
	case CMD_MOVE_LINE_UP:
	case CMD_MOVE_LINE_DOWN:
	case CMD_COPY_LINE_UP:
	case CMD_COPY_LINE_DOWN:
	case CMD_DELETE_LINE:
	case CMD_INSERT_LINE_BELOW:
	case CMD_INSERT_LINE_ABOVE:
	case CMD_UNDO:
	case CMD_REDO:
	case CMD_TRANSFORM_UPPER:
	case CMD_TRANSFORM_LOWER:
	case CMD_TRANSFORM_TITLE:
	case CMD_TRANSFORM_SNAKE:
	case CMD_TRANSFORM_KEBAB:
		if (readonly_doc(v))
			return true;
		break;
	default:
		break;
	}
	switch (cmd) {
	case CMD_TRANSFORM_UPPER:
	case CMD_TRANSFORM_LOWER:
	case CMD_TRANSFORM_TITLE:
	case CMD_TRANSFORM_SNAKE:
	case CMD_TRANSFORM_KEBAB:
		transform_selections(v, cmd - CMD_TRANSFORM_UPPER);
		return true;
	case CMD_MOVE_SEL_NEXT_MATCH:
		cursor_history_push(v);
		move_sel_next_match(v);
		return true;
	case CMD_COLUMN_SELECT_LEFT:
	case CMD_COLUMN_SELECT_RIGHT:
	case CMD_COLUMN_SELECT_UP:
	case CMD_COLUMN_SELECT_DOWN:
		if (!box.on || box.doc != v->doc) {
			cursor_history_push(v);
			box_start(v);
		}
		if (cmd == CMD_COLUMN_SELECT_LEFT)
			box.hx = VX_MAX(box.hx - app.char_w, 0);
		else if (cmd == CMD_COLUMN_SELECT_RIGHT)
			box.hx += app.char_w;
		else {
			u32 page = (u32)VX_MAX(g_view_h / app.line_h - 1, 1);
			if (cmd == CMD_COLUMN_SELECT_UP)
				box.hl = box.hl > page ? box.hl - page : 0;
			else
				box.hl = VX_MIN(box.hl + page, text_lines(t) - 1);
		}
		box_apply(v);
		return true;
	case CMD_UNDO:
		undo_redo(v, false);
		return true;
	case CMD_REDO:
		undo_redo(v, true);
		return true;
	case CMD_CUT:
		copy_sel(v, true);
		return true;
	case CMD_COPY:
		copy_sel(v, false);
		return true;
	case CMD_PASTE:
		paste(v);
		return true;
	case CMD_SELECT_ALL:
		v->nc = 1;
		v->anchor[0] = 0;
		v->head[0] = t->len;
		return true;
	case CMD_SELECT_LINE:
		for (u32 c = 0; c < v->nc; c++) {
			u32 l = text_line_of(t, v->head[c]);
			if (v->head[c] == v->anchor[c] || v->head[c] < v->anchor[c])
				v->anchor[c] = text_line_start(t, text_line_of(t, sel_lo(v, c)));
			v->head[c] = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
		}
		return true;
	case CMD_ADD_NEXT_MATCH:
		cursor_history_push(v);
		if (v->nc == 1 && v->head[0] == v->anchor[0])
			app.find_flags &= ~0x200;
		select_word_or_next(v, false);
		return true;
	case CMD_SELECT_ALL_MATCHES:
		cursor_history_push(v);
		select_word_or_next(v, true);
		return true;
	case CMD_TOGGLE_COMMENT:
		toggle_comment(v);
		return true;
	case CMD_INDENT:
		indent_lines(v, false);
		return true;
	case CMD_OUTDENT:
		indent_lines(v, true);
		return true;
	case CMD_MOVE_LINE_UP:
		move_lines(v, -1, false);
		return true;
	case CMD_MOVE_LINE_DOWN:
		move_lines(v, 1, false);
		return true;
	case CMD_COPY_LINE_UP:
		move_lines(v, -1, true);
		return true;
	case CMD_COPY_LINE_DOWN:
		move_lines(v, 1, true);
		return true;
	case CMD_DELETE_LINE:
		delete_lines(v);
		return true;
	case CMD_INSERT_LINE_BELOW:
		insert_line(v, false);
		return true;
	case CMD_INSERT_LINE_ABOVE:
		insert_line(v, true);
		return true;
	case CMD_CURSOR_ABOVE:
		cursor_history_push(v);
		add_cursor_vert(v, -1);
		return true;
	case CMD_CURSOR_BELOW:
		cursor_history_push(v);
		add_cursor_vert(v, 1);
		return true;
	case CMD_FIND:
	case CMD_REPLACE: {
		app.find_open = true;
		app.find_replace = cmd == CMD_REPLACE;
		app.find_flags &= ~0x100;
		app.focus = FOCUS_FIND;
		/* seed from the selection or the word at the cursor */
		u32 a = sel_lo(v, 0), b = sel_hi(v, 0);
		if (a == b)
			word_at(t, a, &a, &b);
		if (b > a && b - a < 200) {
			char buf[200], esc[400];
			text_copy(t, a, b - a, buf);
			u32 en = 0;
			for (u32 i = 0; i < b - a; i++) {
				if ((app.find_flags & SEARCH_REGEX) && strchr("\\^$.|?*+()[]{}", buf[i]))
					esc[en++] = '\\';
				esc[en++] = buf[i];
			}
			if (!memchr(buf, '\n', b - a))
				input_set(&app.find_in, esc, en);
		}
		app.find_in.anchor = 0;
		app.find_in.cur = app.find_in.n;
		ed_find_update(v);
		return true;
	}
	case CMD_FIND_NEXT:
	case CMD_FIND_PREV:
		if (app.find_in.n)
			ed_find_next(v, cmd == CMD_FIND_PREV);
		return true;
	case CMD_GOTO_DEFINITION:
		ed_goto_definition(v);
		return true;
	case CMD_TRIGGER_SUGGEST:
		ed_completion_trigger(v, true);
		return true;
	case CMD_SHOW_HOVER:
		app.hover_mouse = false; /* stays until Escape or a key, wherever the pointer is */
		app.hover_pending_off = (i32)v->head[0];
		app.hover_doc = v->doc;
		ed_hover_request(v);
		return true;
	default:
		return false;
	}
}

bool ed_key(View *v, const Event *e)
{
	if (app.docs.bin[v->doc] && v->diff_left == UINT32_MAX)
		return binview_key(v, e);
	if (app.vim_on && vim_key(v, e))
		return true;
	int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
	u32 k = e->key;
	if ((k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) &&
	    readonly_doc(v))
		return true;
	if (v->diff_left != UINT32_MAX && diff_key(v, e))
		return true;
	if (app.compl_open) {
		if (k == SDLK_DOWN || k == SDLK_UP || k == SDLK_PAGEDOWN || k == SDLK_PAGEUP) {
			u32 n = app.compl_nf;
			if (n) {
				int step = (k == SDLK_DOWN) ? 1 : (k == SDLK_UP) ? -1 : (k == SDLK_PAGEDOWN) ? 10 : -10;
				i64 s = (i64)app.compl_sel + step;
				if (step == 1 || step == -1)
					s = (s + n) % n;
				else
					s = VX_CLAMP(s, 0, (i64)n - 1);
				app.compl_sel = (u32)s;
			}
			app.dirty = true;
			return true;
		}
		if ((k == SDLK_RETURN || k == SDLK_TAB || k == SDLK_KP_ENTER) && !mods) {
			ed_completion_accept(v);
			return true;
		}
		if (k == SDLK_ESCAPE) {
			app.compl_open = false;
			return true;
		}
	}
	app.hover_open = false;
	switch (k) {
	case SDLK_LEFT:
	case SDLK_RIGHT:
	case SDLK_UP:
	case SDLK_DOWN:
	case SDLK_HOME:
	case SDLK_END:
	case SDLK_PAGEUP:
	case SDLK_PAGEDOWN:
		if ((k == SDLK_UP || k == SDLK_DOWN) && mods == MOD_CTRL) {
			/* scroll line */
			v->scroll_y += (k == SDLK_UP ? -1 : 1) * app.line_h;
			app.dirty = true;
			return true;
		}
		if (mods & MOD_ALT)
			return false;
		app.compl_open = false;
		if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
			int lines = VX_MAX(g_view_h / app.line_h - 1, 1);
			v->scroll_y += (k == SDLK_PAGEUP ? -1 : 1) * (i64)lines * app.line_h;
			move_cursors(v, k, mods, lines);
		} else {
			move_cursors(v, k, mods, 1);
		}
		return true;
	case SDLK_BACKSPACE:
		backspace(v, mods & MOD_CTRL);
		if (app.compl_open)
			ed_completion_filter(v);
		return true;
	case SDLK_DELETE:
		delete_fwd(v, mods & MOD_CTRL);
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if (mods & (MOD_CTRL | MOD_ALT))
			return false;
		app.compl_open = false;
		newline(v);
		return true;
	case SDLK_TAB: {
		app.compl_open = false;
		bool multi = false;
		for (u32 c = 0; c < v->nc; c++)
			if (text_line_of(T(v), v->head[c]) != text_line_of(T(v), v->anchor[c]))
				multi = true;
		if (mods & MOD_SHIFT || multi) {
			indent_lines(v, mods & MOD_SHIFT);
			return true;
		}
		if (app.docs.tabs[v->doc]) {
			ed_insert_text(v, "\t", 1, GK_OTHER);
		} else {
			/* spaces to the next tab stop, per cursor */
			Text *t = T(v);
			u32 g = group_for(v, GK_OTHER);
			for (u32 c = 0; c < v->nc; c++) {
				u32 a = sel_lo(v, c), b = sel_hi(v, c);
				u32 l = text_line_of(t, a);
				int x = x_of(t, l, a) / app.char_w;
				u32 n = TAB - (u32)x % TAB;
				edit(v, a, b - a, "    ", n, g);
				v->head[c] = v->anchor[c] = a + n;
			}
		}
		return true;
	}
	case SDLK_ESCAPE:
		if (v->nc > 1) {
			v->nc = 1;
			v->anchor[0] = v->head[0];
			app.dirty = true;
			return true;
		}
		if (v->head[0] != v->anchor[0]) {
			v->anchor[0] = v->head[0];
			app.dirty = true;
			return true;
		}
		return false;
	default:
		return false;
	}
}

/* ---- find --------------------------------------------------------------- */

static int find_flags(void) { return app.find_flags & (SEARCH_CASE | SEARCH_WORD | SEARCH_REGEX); }

u32 ed_new_group(void) { return next_group++; }

static u32 query_hash(void)
{
	u32 h = 2166136261u ^ (u32)find_flags();
	for (u32 i = 0; i < app.find_in.n; i++)
		h = (h ^ (u8)app.find_in.b[i]) * 16777619u;
	return h;
}

/* Recompute every match of the query in the document (VSCodium keeps up
 * to 19999 find decorations). */
static void find_matches(View *v)
{
	Text *t = T(v);
	u32 h = query_hash();
	if (app.find_doc == v->doc && app.find_version == t->version && app.find_hash == h)
		return;
	app.find_doc = v->doc;
	app.find_version = t->version;
	app.find_hash = h;
	app.fm_n = 0;
	matcher_free(&app.find_m);
	if (!matcher_init(&app.find_m, app.find_in.b ? app.find_in.b : "", app.find_in.n, find_flags()))
		return;
	const char *hay = text_flat(t);
	u32 pos = 0, a, b;
	while (app.fm_n < 19999 && pos <= t->len && matcher_next(&app.find_m, hay, t->len, pos, &a, &b)) {
		SoaCol fc[] = {FM_COLS(&app)};
		soa_grow(fc, 2, &app.fm_cap, app.fm_n + 1, app.fm_n);
		app.fm_a[app.fm_n] = a;
		app.fm_b[app.fm_n] = b;
		app.fm_n++;
		pos = b > a ? b : next_char(t, a);
		if (b == a && a >= t->len)
			break;
	}
	app.find_count = app.fm_n;
}

/* First match with start >= off. */
static u32 match_at_or_after(u32 off)
{
	u32 lo = 0, hi = app.fm_n;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (app.fm_a[mid] < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void select_match(View *v, u32 k)
{
	v->nc = 1;
	v->anchor[0] = app.fm_a[k];
	v->head[0] = app.fm_b[k];
	app.find_index = k + 1;
	ed_reveal(v, app.fm_a[k], true);
}

void ed_find_update(View *v)
{
	app.find_index = 0;
	find_matches(v);
	if (!app.fm_n)
		return;
	/* incremental: the first match at or after the selection start */
	u32 k = match_at_or_after(sel_lo(v, 0));
	select_match(v, k < app.fm_n ? k : 0);
	app.dirty = true;
}

void ed_find_next(View *v, bool back)
{
	find_matches(v);
	if (!app.fm_n) {
		app.find_index = 0;
		return;
	}
	u32 lo = sel_lo(v, 0), hi = sel_hi(v, 0);
	u32 k;
	if (!back) {
		k = match_at_or_after(hi > lo ? hi : lo + 1);
		/* an empty match at the cursor itself counts once */
		if (k < app.fm_n && app.fm_a[k] == lo && hi == lo && app.fm_b[k] == lo)
			k++;
		if (k >= app.fm_n)
			k = 0;
	} else {
		k = match_at_or_after(lo);
		k = k == 0 ? app.fm_n - 1 : k - 1;
	}
	select_match(v, k);
}

/* Replacement for match k (regex: expanded with its captures). */
static char *replacement_for(View *v, u32 k, u32 *len)
{
	Text *t = T(v);
	char *out = NULL;
	u32 cap = 0;
	*len = 0;
	const char *repl = app.repl_in.b ? app.repl_in.b : "";
	u32 a, b;
	if ((find_flags() & SEARCH_REGEX) &&
	    matcher_next(&app.find_m, text_flat(t), t->len, app.fm_a[k], &a, &b) && a == app.fm_a[k]) {
		matcher_expand(&app.find_m, repl, app.repl_in.n, text_flat(t), &out, len, &cap);
	} else {
		out = vx_strndup(repl, app.repl_in.n);
		*len = app.repl_in.n;
	}
	return out;
}

void ed_replace_one(View *v)
{
	find_matches(v);
	u32 lo = sel_lo(v, 0), hi = sel_hi(v, 0);
	u32 k = match_at_or_after(lo);
	if (k < app.fm_n && app.fm_a[k] == lo && app.fm_b[k] == hi) {
		u32 len;
		char *r = replacement_for(v, k, &len);
		edit(v, lo, hi - lo, r, len, group_for(v, GK_OTHER));
		free(r);
		ed_set_cursor(v, lo + len, false);
	}
	ed_find_next(v, false);
}

void ed_replace_all(View *v)
{
	find_matches(v);
	u32 n = app.fm_n;
	if (!n)
		return;
	/* expand every replacement against the original text, then apply
	 * the edits from the end so earlier offsets stay valid */
	u32 *off = vx_alloc((n + 1) * sizeof(u32)), *ln = vx_alloc((n + 1) * sizeof(u32));
	u32 *ma = vx_alloc(n * sizeof(u32)), *mb = vx_alloc(n * sizeof(u32));
	memcpy(ma, app.fm_a, n * sizeof(u32));
	memcpy(mb, app.fm_b, n * sizeof(u32));
	Pool reps = {0};
	for (u32 k = 0; k < n; k++) {
		u32 len;
		char *r = replacement_for(v, k, &len);
		off[k] = pool_add(&reps, r, len);
		ln[k] = len;
		free(r);
	}
	u32 g = group_for(v, GK_OTHER);
	for (u32 k = n; k-- > 0;)
		edit(v, ma[k], mb[k] - ma[k], reps.b + off[k], ln[k], g);
	ui_status("Replaced %u occurrence%s", n, n == 1 ? "" : "s");
	pool_free(&reps);
	free(off);
	free(ln);
	free(ma);
	free(mb);
	ed_find_update(v);
}

/* ---- LSP features ----------------------------------------------------------- */

static bool lsp_doc(View *v) { return doc_lsp(v->doc) != NULL; }

static void flush_change(View *v)
{
	Docs *D = &app.docs;
	u32 d = v->doc;
	if (D->lsp_sent[d] != D->text[d].version) {
		D->lsp_ver[d]++;
		lsp_did_change(doc_lsp(d), D->path[d], D->lsp_ver[d], text_flat(&D->text[d]), D->text[d].len);
		D->lsp_sent[d] = D->text[d].version;
	}
}

static void pos_of(Text *t, u32 off, u32 *line, u32 *col)
{
	*line = text_line_of(t, off);
	*col = off - text_line_start(t, *line);
}

void ed_completion_trigger(View *v, bool explicit_)
{
	Text *t = T(v);
	u32 h = v->head[0];
	u8 prev = h ? text_byte(t, h - 1) : 0;
	bool word = is_word_byte(prev);
	bool trig = prev == '.' || (prev == '>' && h > 1 && text_byte(t, h - 2) == '-');
	if (app.compl_open && word) {
		ed_completion_filter(v);
		return;
	}
	if (!explicit_ && !trig && !(word && !app.compl_open))
		return;
	if (!explicit_ && word) {
		/* only start on the first character of a word */
		u32 a, b;
		word_at(t, h, &a, &b);
		if (h - a != 1)
			return;
		if (isdigit(text_byte(t, a)))
			return;
	}
	if (!lsp_doc(v))
		return;
	flush_change(v);
	u32 a, b;
	word_at(t, h, &a, &b);
	app.compl_start = a;
	app.compl_doc = v->doc;
	u32 line, col;
	pos_of(t, h, &line, &col);
	app.compl_req = lsp_completion(doc_lsp(v->doc), app.docs.path[v->doc], line, col);
}

#define COMPL_COLS(a) SOA((a)->compl_label), SOA((a)->compl_insert), SOA((a)->compl_detail), SOA((a)->compl_filtered), SOA((a)->compl_kind)

void ed_take_completion(const LspEvent *ev)
{
	if (ev->req != app.compl_req)
		return;
	app.compl_req = -1;
	View *v = active_view();
	if (!v || v->doc != app.compl_doc)
		return;
	const LspCompletion *c = ev->completion;
	pool_clear(&app.compl_pool);
	u32 n = c->count;
	SoaCol cc[] = {COMPL_COLS(&app)};
	soa_grow(cc, 5, &app.compl_cap, n, 0); /* all rows are rewritten */
	for (u32 i = 0; i < n; i++) {
		const char *l = pool_str(ev->pool, c->label[i]);
		const char *ins = pool_str(ev->pool, c->insert[i]);
		const char *det = pool_str(ev->pool, c->detail[i]);
		app.compl_label[i] = pool_add(&app.compl_pool, l, (u32)strlen(l));
		app.compl_insert[i] = pool_add(&app.compl_pool, ins, (u32)strlen(ins));
		app.compl_detail[i] = pool_add(&app.compl_pool, det, (u32)strlen(det));
		app.compl_kind[i] = c->kind[i];
	}
	app.compl_n = n;
	app.compl_open = n > 0;
	ed_completion_filter(v);
}

void ed_completion_filter(View *v)
{
	Text *t = T(v);
	u32 h = v->head[0];
	if (h < app.compl_start || text_line_of(t, h) != text_line_of(t, app.compl_start)) {
		app.compl_open = false;
		return;
	}
	char pre[128];
	u32 pn = VX_MIN(h - app.compl_start, (u32)sizeof pre - 1);
	text_copy(t, app.compl_start, pn, pre);
	/* rank: prefix matches first, then fuzzy */
	static int score[65536];
	u32 k = 0;
	for (u32 i = 0; i < app.compl_n && i < 65536; i++) {
		const char *l = app.compl_pool.b + app.compl_label[i];
		u64 m;
		int s = fuzzy_score(pre, pn, l, strlen(l), &m);
		if (s < 0)
			continue;
		if (pn && strncmp(l, pre, pn) == 0)
			s += 1000;
		score[i] = s;
		app.compl_filtered[k++] = i;
	}
	/* stable insertion sort by score (lists are small after filtering) */
	if (pn)
		for (u32 i = 1; i < k; i++) {
			u32 x = app.compl_filtered[i];
			u32 j = i;
			while (j > 0 && score[app.compl_filtered[j - 1]] < score[x]) {
				app.compl_filtered[j] = app.compl_filtered[j - 1];
				j--;
			}
			app.compl_filtered[j] = x;
		}
	app.compl_nf = k;
	app.compl_sel = 0;
	app.compl_top = 0;
	app.compl_open = k > 0;
	app.dirty = true;
}

void ed_completion_accept(View *v)
{
	if (!app.compl_nf) {
		app.compl_open = false;
		return;
	}
	u32 i = app.compl_filtered[app.compl_sel];
	const char *ins = app.compl_pool.b + app.compl_insert[i];
	Text *t = T(v);
	u32 h = v->head[0];
	u32 a = app.compl_start;
	/* replace the whole word under the cursor */
	u32 wa, wb;
	word_at(t, h, &wa, &wb);
	if (wb < h)
		wb = h;
	edit(v, a, wb - a, ins, (u32)strlen(ins), group_for(v, GK_OTHER));
	ed_set_cursor(v, a + (u32)strlen(ins), false);
	app.compl_open = false;
}

void ed_hover_request(View *v)
{
	if (app.hover_pending_off < 0 || v->doc != app.hover_doc)
		return;
	u32 off = (u32)app.hover_pending_off;
	app.hover_off = off;
	free(app.hover_text);
	app.hover_text = NULL;
	app.hover_open = true; /* diagnostics show even without a reply */
	if (!lsp_doc(v))
		return;
	flush_change(v);
	u32 line, col;
	pos_of(T(v), off, &line, &col);
	app.hover_req = lsp_hover(doc_lsp(v->doc), app.docs.path[v->doc], line, col);
}

void ed_take_hover(const LspEvent *ev)
{
	if (ev->req != app.hover_req)
		return;
	app.hover_req = -1;
	free(app.hover_text);
	app.hover_text = ev->text && *ev->text ? vx_strdup(ev->text) : NULL;
	app.hover_open = true;
}

void ed_goto_definition(View *v)
{
	if (!lsp_doc(v)) {
		ui_status("No definition provider (clangd not running for this file)");
		return;
	}
	flush_change(v);
	u32 line, col;
	pos_of(T(v), v->head[0], &line, &col);
	app.def_req = lsp_definition(doc_lsp(v->doc), app.docs.path[v->doc], line, col);
}

void ed_take_definition(const LspEvent *ev)
{
	if (ev->req != app.def_req)
		return;
	app.def_req = -1;
	const LspLocations *l = ev->locations;
	if (!l->count) {
		ui_status("No definition found");
		return;
	}
	const char *p = pool_str(ev->pool, l->path[0]);
	open_file(p, false, l->line[0], l->col[0]);
}

/* ---- mouse ------------------------------------------------------------------ */

static u32 hit_offset(View *v, int mx, int my, int x, int y)
{
	Text *t = T(v);
	i64 py = (i64)(my - y) + v->scroll_y;
	if (py < 0)
		return 0;
	u32 l = (u32)(py / app.line_h);
	if (l >= text_lines(t))
		return t->len;
	int tx = mx - x - ed_gutter_w(v) + v->scroll_x;
	return off_at_x(t, l, tx);
}

static bool find_widget_click(View *v, const Event *e);
static int fw_x, fw_y, fw_w, fw_h; /* find widget rect, set while drawing */

u32 ed_offset_at(View *v, int mx, int my, int x, int y);

int ed_cursor_shape(View *v, int mx, int my, int x, int y, int w, int h, int mods)
{
	int mm = minimap_w(w);
	if (mx >= x + w - SCROLL_W - mm || mx < x + ed_gutter_w(v))
		return CURSOR_ARROW;
	if (app.find_open && mx >= fw_x && mx < fw_x + fw_w && my >= fw_y && my < fw_y + fw_h)
		return CURSOR_ARROW;
	(void)h;
	if (mods & MOD_CTRL) {
		/* a go-to-definition link under the mouse */
		Text *t = T(v);
		u32 off = ed_offset_at(v, mx, my, x, y);
		if (off < t->len && is_word_byte(text_byte(t, off)))
			return CURSOR_HAND;
	}
	return CURSOR_IBEAM;
}

u32 ed_offset_at(View *v, int mx, int my, int x, int y) { return hit_offset(v, mx, my, x, y); }

static int drag_mode; /* 1 chars, 2 words, 3 lines, 4 scrollbar, 5 minimap */
static u32 drag_a, drag_b;
static int drag_grab;

void ed_mouse(View *v, const Event *e, int x, int y, int w, int h)
{
	if (v->diff_left != UINT32_MAX) {
		diff_mouse(v, e, x, y, w, h);
		return;
	}
	if (app.docs.bin[v->doc]) {
		if (e->type == EV_MDOWN && e->button == 1)
			app.drag = 3; /* DRAG_EDITOR in ui.c: drag-select */
		binview_mouse(v, e, x, y, w, h);
		return;
	}
	Text *t = T(v);
	int mm = minimap_w(w);
	int sb_x = x + w - SCROLL_W;
	int mm_x = sb_x - mm;
	i64 content = (i64)(text_lines(t) - 1) * app.line_h + h;
	if (e->type == EV_WHEEL) {
		if (e->mods & MOD_SHIFT)
			v->scroll_x -= (int)(e->wy * 3 * app.char_w * 4);
		else
			v->scroll_y -= (i64)(e->wy * 3 * app.line_h);
		v->scroll_x -= (int)(e->wx * 3 * app.char_w * 4);
		clamp_scroll(v, h);
		app.hover_open = false;
		app.dirty = true;
		return;
	}
	if (e->type == EV_MDOWN && e->button == 1 && find_widget_click(v, e))
		return;
	if (e->type == EV_MDOWN && e->button == 2) {
		/* middle-button drag: column selection (editor.columnSelection) */
		Text *t2 = T(v);
		u32 off = hit_offset(v, e->x, e->y, x, y);
		cursor_history_push(v);
		box.on = true;
		box.doc = v->doc;
		box.al = box.hl = text_line_of(t2, off);
		box.ax = box.hx = e->x - x - ed_gutter_w(v) + v->scroll_x;
		box_apply(v);
		drag_mode = 6;
		app.drag = 3; /* DRAG_EDITOR in ui.c */
		return;
	}
	if (e->type == EV_MDOWN && e->button == 1 && app.git) {
		/* a click on the git change bar in the gutter (VSCodium's quick diff peek) */
		int nd = VX_MAX(digits(text_lines(t)), 5);
		int gx = x + PX(18) + nd * app.char_w + PX(5);
		i64 py = (i64)(e->y - y) + v->scroll_y;
		u32 l = py < 0 ? 0 : (u32)(py / app.line_h);
		Docs *D = &app.docs;
		if (e->x >= gx - PX(3) && e->x < gx + PX(8) && l < D->gm_n[v->doc] && D->gm[v->doc][l] &&
		    D->gm_for[v->doc] != UINT32_MAX) {
			app.ctx = CTX_EDITOR;
			app.ctx_off = l;
			menu_open(e->x, e->y);
			menu_item("Stage Change", CMD_GIT_STAGE_CHANGE, true);
			menu_item("Revert Change", CMD_GIT_REVERT_CHANGE, true);
			menu_sep();
			menu_item("Open Changes", CMD_SCM_OPEN_CHANGES, true);
			return;
		}
	}
	if (e->type == EV_MDOWN && e->button == 1) {
		app.compl_open = false;
		app.hover_open = false;
		if (e->x >= sb_x || (mm && e->x >= mm_x)) {
			/* scrollbar or minimap: jump/drag */
			drag_mode = e->x >= sb_x ? 4 : 5;
			i64 slider_h = VX_MAX((i64)h * h / VX_MAX(content, 1), (i64)PX(20));
			i64 slider_y = (i64)(h - slider_h) * v->scroll_y / VX_MAX(content - h, 1);
			if (drag_mode == 4 && e->y - y >= slider_y && e->y - y < slider_y + slider_h)
				drag_grab = (int)(e->y - y - slider_y);
			else
				drag_grab = (int)(slider_h / 2);
			goto drag_scroll;
		}
		u32 off = hit_offset(v, e->x, e->y, x, y);
		if (e->mods & MOD_CTRL) {
			ed_set_cursor(v, off, false);
			ed_goto_definition(v);
			drag_mode = 0;
			return;
		}
		if ((e->mods & (MOD_ALT | MOD_SHIFT)) == (MOD_ALT | MOD_SHIFT)) {
			/* column (box) selection from the primary cursor to here */
			Text *t2 = T(v);
			cursor_history_push(v);
			box_start(v);
			box.hl = text_line_of(t2, off);
			box.hx = x_of(t2, box.hl, off);
			box_apply(v);
			drag_mode = 6;
			return;
		}
		if (e->mods & MOD_ALT) {
			cursor_history_push(v);
			if (v->nc < MAX_CURSORS) {
				view_reserve(v, v->nc + 1);
				u32 k = v->nc++;
				v->head[k] = v->anchor[k] = off;
				v->want_x[k] = -1;
				sort_cursors(v);
			}
			drag_mode = 0;
			return;
		}
		int clicks = e->clicks > 3 ? (e->clicks - 1) % 3 + 1 : e->clicks;
		if (e->mods & MOD_SHIFT) {
			v->nc = 1;
			v->head[0] = off;
			drag_mode = 1;
			drag_a = v->anchor[0];
			drag_b = v->anchor[0];
			return;
		}
		if (clicks == 2) {
			u32 a, b;
			word_at(t, off, &a, &b);
			v->nc = 1;
			v->anchor[0] = a;
			v->head[0] = b;
			drag_mode = 2;
			drag_a = a;
			drag_b = b;
		} else if (clicks == 3) {
			u32 l = text_line_of(t, off);
			u32 a = text_line_start(t, l);
			u32 b = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
			v->nc = 1;
			v->anchor[0] = a;
			v->head[0] = b;
			drag_mode = 3;
			drag_a = a;
			drag_b = b;
		} else {
			ed_set_cursor(v, off, false);
			drag_mode = 1;
			drag_a = drag_b = off;
		}
		v->group_kind = GK_NONE;
		return;
	}
	if (e->type == EV_MMOVE && app.mouse_down && drag_mode) {
		if (drag_mode == 6) {
			Text *t2 = T(v);
			i64 py = (i64)(e->y - y) + v->scroll_y;
			box.hl = py < 0 ? 0 : (u32)VX_MIN((u64)(py / app.line_h), (u64)text_lines(t2) - 1);
			box.hx = VX_MAX(e->x - x - ed_gutter_w(v) + v->scroll_x, 0);
			box_apply(v);
			return;
		}
		if (drag_mode >= 4)
			goto drag_scroll;
		u32 off = hit_offset(v, e->x, e->y, x, y);
		if (drag_mode == 2) {
			u32 a, b;
			word_at(t, off, &a, &b);
			if (off < drag_a) {
				v->anchor[0] = drag_b;
				v->head[0] = a;
			} else {
				v->anchor[0] = drag_a;
				v->head[0] = VX_MAX(b, drag_b);
			}
		} else if (drag_mode == 3) {
			u32 l = text_line_of(t, off);
			if (off < drag_a) {
				v->anchor[0] = drag_b;
				v->head[0] = text_line_start(t, l);
			} else {
				v->anchor[0] = drag_a;
				v->head[0] = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
			}
		} else {
			v->head[0] = off;
		}
		v->nc = 1;
		ed_reveal(v, v->head[0], false);
		return;
	}
	if (e->type == EV_MUP) {
		drag_mode = 0;
		return;
	}
	if (e->type == EV_MMOVE && !app.mouse_down) {
		/* the inline blame text shows the commit card */
		bool over = blame_rect_doc == v->doc && blame_rect_x1 > blame_rect_x0 && e->x >= blame_rect_x0 &&
		            e->x < blame_rect_x1 && e->y >= blame_rect_y && e->y < blame_rect_y + app.line_h;
		if (over != app.blame_hover || (over && strcmp(app.blame_hover_hash, blame_rect_hash))) {
			app.blame_hover = over;
			app.blame_hover_t = vx_now_ns();
			snprintf(app.blame_hover_hash, sizeof app.blame_hover_hash, "%s", blame_rect_hash);
			app.blame_hover_x = blame_rect_x0;
			app.blame_hover_y = blame_rect_y;
			app.blame_hover_x1 = blame_rect_x1;
		}
		if (over) {
			app.hover_due = 0;
			return;
		}
		/* hover after a rest of 300 ms over text */
		if (e->x < x + ed_gutter_w(v) || e->x >= mm_x) {
			app.hover_due = 0;
			return;
		}
		u32 off = hit_offset(v, e->x, e->y, x, y);
		u32 a, b;
		word_at(t, off, &a, &b);
		bool on_diag = false;
		u32 l = text_line_of(t, off);
		for (u32 i = 0; i < app.docs.dn[v->doc]; i++)
			if (app.docs.d_line0[v->doc][i] <= l && app.docs.d_line1[v->doc][i] >= l)
				on_diag = true;
		if (a == b && !on_diag) {
			if (app.hover_open && (off < app.hover_off - 1 || off > app.hover_off + 1))
				app.hover_open = false;
			app.hover_due = 0;
			return;
		}
		if (app.hover_open && a <= app.hover_off && app.hover_off <= b)
			return;
		app.hover_open = false;
		app.hover_pending_off = (i32)off;
		app.hover_doc = v->doc;
		app.hover_mouse = true;
		app.hover_due = vx_now_ns() + 150000000ull;
		return;
	}
	return;
drag_scroll : {
	i64 slider_h = VX_MAX((i64)h * h / VX_MAX(content, 1), (i64)PX(20));
	i64 track = h - slider_h;
	if (drag_mode == 5) {
		/* minimap: click centres that line */
		i64 mm_line_h = PX(2);
		i64 mm_content = (i64)text_lines(t) * mm_line_h;
		i64 mm_scroll = mm_content > h ? (i64)(mm_content - h) * v->scroll_y / VX_MAX(content - h, 1) : 0;
		i64 line = (e->y - y + mm_scroll) / mm_line_h;
		v->scroll_y = line * app.line_h - h / 2;
	} else if (track > 0) {
		i64 sy = e->y - y - drag_grab;
		v->scroll_y = sy * (content - h) / track;
	}
	clamp_scroll(v, h);
	app.dirty = true;
}
}

/* ---- drawing ------------------------------------------------------------------ */

static void draw_selection_line(Text *t, View *v, u32 l, int tx, int ly, int max_x)
{
	u32 ls = text_line_start(t, l);
	u32 le = ls + text_line_len(t, l);
	u32 next = l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len;
	for (u32 c = 0; c < v->nc; c++) {
		u32 a = sel_lo(v, c), b = sel_hi(v, c);
		if (a == b || b < ls || a > le || (a == next && a != ls))
			continue;
		if (a >= next)
			continue;
		u32 sa = VX_MAX(a, ls), sb = VX_MIN(b, le);
		int x0 = tx + x_of(t, l, sa);
		int x1 = tx + x_of(t, l, sb);
		if (b > le && b >= next && next > le)
			x1 += app.char_w / 2 + PX(3); /* the line break is selected */
		if (x1 > x0)
			draw_rect(x0, ly, VX_MIN(x1, max_x) - x0, app.line_h,
			          app.focus == FOCUS_EDITOR ? COL(C_SELECTION) : COL(C_SELECTION_INACTIVE));
	}
}

static const char *bracket_pairs = "()[]{}";

/* Matching bracket for the one at or before the cursor. */
static bool match_bracket(Text *t, u32 h, u32 *a, u32 *b)
{
	u32 cand[2] = {h, h ? h - 1 : UINT32_MAX};
	for (int k = 0; k < 2; k++) {
		u32 p = cand[k];
		if (p >= t->len)
			continue;
		u8 c = text_byte(t, p);
		const char *q = strchr(bracket_pairs, c);
		if (!q || !c)
			continue;
		int idx = (int)(q - bracket_pairs);
		bool open = idx % 2 == 0;
		u8 other = (u8)bracket_pairs[open ? idx + 1 : idx - 1];
		int depth = 0;
		u32 limit = 200000;
		if (open) {
			for (u32 o = p; o < t->len && limit--; o++) {
				u8 x = text_byte(t, o);
				if (x == c)
					depth++;
				else if (x == other && --depth == 0) {
					*a = p;
					*b = o;
					return true;
				}
			}
		} else {
			for (u32 o = p + 1; o-- > 0 && limit--;) {
				u8 x = text_byte(t, o);
				if (x == c)
					depth++;
				else if (x == other && --depth == 0) {
					*a = o;
					*b = p;
					return true;
				}
			}
		}
	}
	return false;
}

static int line_indent_cols(Text *t, u32 l, bool *blank)
{
	u32 n;
	const char *s = text_line(t, l, &n);
	int col = 0;
	u32 i = 0;
	for (; i < n; i++) {
		if (s[i] == ' ')
			col++;
		else if (s[i] == '\t')
			col += TAB - col % TAB;
		else
			break;
	}
	*blank = i == n;
	return col;
}

/* ---- find widget ------------------------------------------------------------ */


static int fw_row1 = 33, fw_row2; /* set by fw_layout */

static void fw_layout(int x, int y, int w, int *fx, int *fy, int *fwid, int *fh, int *ix, int *iw)
{
	*fwid = VX_MIN(PX(440), w - PX(40));
	/* rows grow with wrapped text (the Aa ab .* strip stays on the right) */
	int iw0 = *fwid - PX(24) - PX(140);
	fw_row1 = VX_MAX(PX(33), input_height(&app.find_in, iw0, PX(70)) + PX(8));
	fw_row2 = app.find_replace ? VX_MAX(PX(33), input_height(&app.repl_in, iw0, 0) + PX(8)) : 0;
	*fh = fw_row1 + fw_row2;
	*fx = x + w - *fwid - SCROLL_W - PX(14);
	*fy = y;
	*ix = *fx + PX(24);
	*iw = *fwid - PX(24) - PX(140);
}

static void toggle_box(int bx, int by, const char *label, bool on, bool underline)
{
	if (on) {
		draw_rect(bx, by, PX(20), PX(19), (COL(C_FOCUS_BORDER) & 0xFFFFFF00u) | 0x50u);
		draw_frame(bx, by, PX(20), PX(19), COL(C_FOCUS_BORDER));
	}
	int tw = font_width(FONT_SMALL_BOLD, label, strlen(label));
	draw_text(FONT_SMALL_BOLD, bx + (PX(20) - tw) / 2, by + (PX(19) - font_m[FONT_SMALL].line) / 2, label,
	          strlen(label), COL(C_FG));
	if (underline)
		draw_rect(bx + PX(3), by + PX(15), PX(14), PX(1), COL(C_FG));
}

static void find_widget_draw(int x, int y, int w)
{
	int fx, fy, fwid, fh, ix, iw;
	fw_layout(x, y, w, &fx, &fy, &fwid, &fh, &ix, &iw);
	fw_x = fx;
	fw_y = fy;
	fw_w = fwid;
	fw_h = fh;
	draw_rect(fx - PX(1), fy, fwid + PX(2), fh + PX(3), COL(C_WIDGET_SHADOW));
	draw_rect(fx, fy, fwid, fh, COL(C_WIDGET_BG));
	draw_rect(fx, fy, PX(3), fh, COL(C_WIDGET_BORDER));
	draw_icon(app.find_replace ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, fx + PX(5), fy + PX(8), COL(C_FG));
	bool find_focus = app.focus == FOCUS_FIND && !(app.find_flags & 0x100);
	bool bad = app.find_m.err[0] != 0;
	input_draw_ex(&app.find_in, ix, fy + PX(4), iw, PX(70), "Find", find_focus);
	if (bad)
		draw_frame(ix, fy + PX(4), iw, fw_row1 - PX(8), COL(C_ERROR));
	/* toggles inside the box: Aa, ab, .* */
	int bx = ix + iw - PX(68);
	toggle_box(bx, fy + PX(7), "Aa", app.find_flags & SEARCH_CASE, false);
	toggle_box(bx + PX(22), fy + PX(7), "ab", app.find_flags & SEARCH_WORD, true);
	toggle_box(bx + PX(44), fy + PX(7), ".*", app.find_flags & SEARCH_REGEX, false);
	char msg[64];
	if (!app.find_in.n || !app.find_count)
		snprintf(msg, sizeof msg, "No results");
	else if (app.find_index)
		snprintf(msg, sizeof msg, "%u of %u", app.find_index, app.find_count);
	else
		snprintf(msg, sizeof msg, "? of %u", app.find_count);
	u32 mc = (app.find_in.n && !app.find_count) ? COL(C_ERROR) : COL(C_FG);
	draw_text_fit(FONT_UI, ix + iw + PX(6), fy + (PX(33) - font_m[FONT_UI].line) / 2, msg, strlen(msg), PX(66), mc);
	draw_icon(ICON_ARROW_UP, fx + fwid - PX(66), fy + PX(8), COL(C_FG));
	draw_icon(ICON_ARROW_DOWN, fx + fwid - PX(46), fy + PX(8), COL(C_FG));
	draw_icon(ICON_CLOSE, fx + fwid - PX(24), fy + PX(8), COL(C_FG));
	ui_tip(bx, fy + PX(7), PX(20), PX(20), "Match Case (Alt+C)");
	ui_tip(bx + PX(22), fy + PX(7), PX(20), PX(20), "Match Whole Word (Alt+W)");
	ui_tip(bx + PX(44), fy + PX(7), PX(20), PX(20), "Use Regular Expression (Alt+R)");
	ui_tip(fx + fwid - PX(70), fy + PX(4), PX(20), PX(24), "Previous Match (Shift+Enter)");
	ui_tip(fx + fwid - PX(50), fy + PX(4), PX(20), PX(24), "Next Match (Enter)");
	ui_tip(fx + fwid - PX(28), fy + PX(4), PX(22), PX(24), "Close (Escape)");
	if (app.find_replace) {
		bool rf = app.focus == FOCUS_FIND && (app.find_flags & 0x100);
		input_draw_ex(&app.repl_in, ix, fy + fw_row1 + PX(3), iw, 0, "Replace", rf);
		/* replace / replace all buttons */
		const char *b1 = "\xE2\x86\xB5", *b2 = "All";
		int by = fy + fw_row1 + PX(6);
		draw_frame(ix + iw + PX(6), by, PX(24), PX(19), COL(C_WIDGET_BORDER));
		draw_text(FONT_UI, ix + iw + PX(12), by + (PX(19) - font_m[FONT_UI].line) / 2, b1, strlen(b1), COL(C_FG));
		draw_frame(ix + iw + PX(34), by, PX(30), PX(19), COL(C_WIDGET_BORDER));
		draw_text(FONT_SMALL, ix + iw + PX(39), by + (PX(19) - font_m[FONT_SMALL].line) / 2, b2, 3, COL(C_FG));
	}
	if (bad) {
		/* validation message under the input, like VSCodium */
		int ey = fy + fh + PX(2);
		int ew = font_width(FONT_UI, app.find_m.err, strlen(app.find_m.err)) + PX(12);
		ew = VX_MIN(ew, fwid);
		draw_rect(ix, ey, ew, PX(24), (COL(C_ERROR) & 0xFFFFFF00u) | 0x33u);
		draw_frame(ix, ey, ew, PX(24), COL(C_ERROR));
		draw_text_fit(FONT_UI, ix + PX(6), ey + (PX(24) - font_m[FONT_UI].line) / 2, app.find_m.err,
		              strlen(app.find_m.err), ew - PX(12), COL(C_FG));
	}
}

/* Click inside the find widget; true when handled. */
static bool find_widget_click(View *v, const Event *e)
{
	if (!app.find_open || e->x < fw_x || e->x >= fw_x + fw_w || e->y < fw_y || e->y >= fw_y + fw_h)
		return false;
	int ix = fw_x + PX(24), iw = fw_w - PX(24) - PX(140);
	int mx = e->x, my = e->y - fw_y;
	bool row2 = my >= fw_row1;
	if (mx < ix) {
		app.find_replace = !app.find_replace;
		return true;
	}
	if (!row2) {
		int bx = ix + iw - PX(68);
		if (mx >= bx && mx < bx + PX(66)) {
			int which = (mx - bx) / PX(22);
			app.find_flags ^= which == 0 ? SEARCH_CASE : which == 1 ? SEARCH_WORD : SEARCH_REGEX;
			ed_find_update(v);
			app.focus = FOCUS_FIND;
			return true;
		}
		if (mx < ix + iw) {
			app.focus = FOCUS_FIND;
			app.find_flags &= ~0x100;
			input_click_ex(&app.find_in, ix, fw_y + PX(4), iw, PX(70), mx, e->y, e->clicks);
			return true;
		}
		if (mx >= fw_x + fw_w - PX(70) && mx < fw_x + fw_w - PX(50))
			ed_find_next(v, true);
		else if (mx >= fw_x + fw_w - PX(50) && mx < fw_x + fw_w - PX(28))
			ed_find_next(v, false);
		else if (mx >= fw_x + fw_w - PX(28)) {
			app.find_open = false;
			app.focus = FOCUS_EDITOR;
		}
		return true;
	}
	if (mx < ix + iw) {
		app.focus = FOCUS_FIND;
		app.find_flags |= 0x100;
		input_click_ex(&app.repl_in, ix, fw_y + fw_row1 + PX(3), iw, 0, mx, e->y, e->clicks);
	} else if (mx < ix + iw + PX(32)) {
		ed_replace_one(v);
	} else if (mx < ix + iw + PX(66)) {
		ed_replace_all(v);
	}
	return true;
}

void ed_draw(Group *g, View *v, int x, int y, int w, int h, bool focused)
{
	if (v->diff_left != UINT32_MAX) {
		diff_draw(g, v, x, y, w, h, focused);
		return;
	}
	if (app.docs.bin[v->doc]) {
		binview_draw(v, x, y, w, h, focused);
		return;
	}
	if (v->doc == blame_rect_doc)
		blame_rect_x1 = -1; /* re-recorded below if still shown */
	(void)g;
	Docs *D = &app.docs;
	u32 d = v->doc;
	Text *t = &D->text[d];
	g_view_h = h;
	clamp_scroll(v, h);
	int gut = ed_gutter_w(v);
	int mm = minimap_w(w);
	int text_x = x + gut;
	int text_w = w - gut - mm - SCROLL_W;
	int lh = app.line_h;
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));

	u32 first = (u32)(v->scroll_y / lh);
	int y0 = y - (int)(v->scroll_y % lh);
	u32 nl = text_lines(t);
	u32 last = VX_MIN(nl, first + (u32)(h / lh) + 2);
	u32 cur_line = text_line_of(t, v->head[0]);
	bool any_sel = false;
	for (u32 c = 0; c < v->nc; c++)
		any_sel |= v->head[c] != v->anchor[c];
	int tx = text_x - v->scroll_x;

	/* keep the cursor horizontally visible */
	{
		int cx = x_of(t, cur_line, v->head[0]);
		if (cx - v->scroll_x > text_w - app.char_w * 2)
			v->scroll_x = cx - text_w + app.char_w * 4;
		if (cx < v->scroll_x)
			v->scroll_x = VX_MAX(cx - app.char_w * 4, 0);
		tx = text_x - v->scroll_x;
	}

	/* selection occurrence highlight: single-line selection text */
	char occ[256];
	u32 occ_n = 0;
	if (v->nc == 1 && any_sel) {
		u32 a = sel_lo(v, 0), b = sel_hi(v, 0);
		if (b - a < sizeof occ && text_line_of(t, a) == text_line_of(t, b)) {
			text_copy(t, a, b - a, occ);
			occ_n = b - a;
			bool ws = true;
			for (u32 i = 0; i < occ_n; i++)
				if (occ[i] != ' ' && occ[i] != '\t')
					ws = false;
			if (ws)
				occ_n = 0;
		}
	}
	u32 ba = 0, bb = 0;
	bool brackets = !any_sel && match_bracket(t, v->head[0], &ba, &bb);
	if (app.find_open)
		find_matches(v);

	draw_clip(x, y, w - mm - SCROLL_W, h);
	/* backgrounds: current line, selections, matches */
	for (u32 l = first; l < last; l++) {
		int ly = y0 + (int)(l - first) * lh;
		if (l == cur_line && !any_sel && v->nc == 1) {
			/* lineHighlightBorder: a 2 px box over the text area */
			draw_frame(text_x, ly, text_w, lh, COL(C_LINE_HIGHLIGHT_BORDER));
			draw_frame(text_x + 1, ly + 1, text_w - 2, lh - 2, COL(C_LINE_HIGHLIGHT_BORDER));
		}
		draw_clip(text_x, y, text_w, h);
		u32 n;
		const char *s = text_line(t, l, &n);
		u32 ls = text_line_start(t, l);
		if (app.find_open && app.fm_n) {
			u32 le = ls + n;
			for (u32 k = match_at_or_after(ls > 4096 ? ls - 4096 : 0); k < app.fm_n && app.fm_a[k] <= le; k++) {
				u32 ma = app.fm_a[k], mb = app.fm_b[k];
				if (mb < ls || (mb == ls && ma < ls))
					continue;
				u32 sa = VX_MAX(ma, ls), sb = VX_MIN(mb, le);
				bool cur = v->nc == 1 && ma == sel_lo(v, 0) && mb == sel_hi(v, 0);
				int xa = tx + x_of(t, l, sa), xb = tx + x_of(t, l, sb);
				if (mb > le)
					xb += app.char_w / 2; /* the match continues on the next line */
				if (xb == xa)
					xb = xa + PX(2); /* empty match: a thin mark */
				draw_rect(xa, ly, xb - xa, lh, cur ? COL(C_FIND_MATCH) : COL(C_FIND_MATCH_OTHER));
			}
			s = text_line(t, l, &n);
		}
		if (occ_n && !app.find_open) {
			u32 pos = 0;
			while (pos + occ_n <= n) {
				u32 m = find_in(s + pos, n - pos, occ, occ_n, SEARCH_CASE, 0, false);
				if (m == UINT32_MAX)
					break;
				m += pos;
				if (ls + m != sel_lo(v, 0)) {
					int a = tx + x_of(t, l, ls + m), b = tx + x_of(t, l, ls + m + occ_n);
					draw_rect(a, ly, b - a, lh, COL(C_SELECTION_HIGHLIGHT));
				}
				pos = m + occ_n;
				s = text_line(t, l, &n);
			}
		}
		draw_selection_line(t, v, l, tx, ly, text_x + text_w);
		draw_clip(x, y, w - mm - SCROLL_W, h);
	}

	/* merge conflict regions: coloured, the markers stay editable text */
	draw_clip(text_x, y, text_w, h);
	conflict_draw(v, first, last, x, text_x, text_w, y0, lh, tx);

	/* indent guides */
	draw_clip(text_x, y, text_w, h);
	{
		int cur_ind = -1;
		bool blank;
		int ci = line_indent_cols(t, cur_line, &blank);
		cur_ind = blank ? -1 : ci;
		for (u32 l = first; l < last; l++) {
			int ly = y0 + (int)(l - first) * lh;
			int ind = line_indent_cols(t, l, &blank);
			if (blank) {
				/* use the next non-blank line's indent (look ahead a little) */
				ind = 0;
				for (u32 k = l + 1; k < VX_MIN(nl, l + 50); k++) {
					bool b2;
					int i2 = line_indent_cols(t, k, &b2);
					if (!b2) {
						ind = i2;
						break;
					}
				}
				int prev = 0;
				for (u32 k = l; k-- > 0 && k + 50 > l;) {
					bool b2;
					int i2 = line_indent_cols(t, k, &b2);
					if (!b2) {
						prev = i2;
						break;
					}
				}
				ind = VX_MIN(ind, prev);
			}
			for (int c = 0; c < ind; c += TAB) {
				bool active = cur_ind > 0 && c == ((cur_ind - 1) / TAB) * TAB && l != cur_line;
				(void)active;
				draw_rect(tx + c * app.char_w, ly, PX(1), lh, COL(C_INDENT_GUIDE));
			}
		}
	}

	/* bracket match boxes */
	if (brackets) {
		u32 pa[2] = {ba, bb};
		for (int k = 0; k < 2; k++) {
			u32 l = text_line_of(t, pa[k]);
			if (l < first || l >= last)
				continue;
			int ly = y0 + (int)(l - first) * lh;
			int bx = tx + x_of(t, l, pa[k]);
			draw_rect(bx, ly, app.char_w, lh, COL(C_BRACKET_MATCH_BG));
			draw_frame(bx, ly, app.char_w, lh, COL(C_BRACKET_MATCH_BORDER));
		}
	}

	/* text */
	for (u32 l = first; l < last; l++) {
		int ly = y0 + (int)(l - first) * lh;
		u32 n;
		const char *s = text_line(t, l, &n);
		u32 nn = VX_MIN(n, (u32)MAX_LINE_BYTES);
		/* copy: classify_line's lexer may reuse the scratch buffer */
		static char buf[MAX_LINE_BYTES];
		memcpy(buf, s, nn);
		classify_line(d, l, buf, nn);
		int px = 0;
		for (u32 i = 0; i < nn;) {
			u8 c = (u8)buf[i];
			if (c == '\t') {
				px += tab_px() - px % tab_px();
				i++;
				continue;
			}
			u32 cp = c;
			u32 k = 1;
			if (c >= 0x80)
				k = utf8_decode(buf + i, nn - i, &cp);
			u8 cls = line_cls[i];
			u8 st = tk_style[cls];
			int font = (st & TS_BOLD) ? ((st & TS_ITALIC) ? FONT_MONO_BOLD_ITALIC : FONT_MONO_BOLD)
			                          : ((st & TS_ITALIC) ? FONT_MONO_ITALIC : FONT_MONO);
			u32 gid = font_glyph(font, cp);
			int gx = tx + px;
			if (gx > text_x + text_w)
				break;
			int adv = app.char_w;
			if (cp >= 0x80)
				adv = font_glyphs.adv[font_glyph(FONT_MONO, cp)];
			if (gx + adv > text_x - app.char_w) {
				u32 col = TOK(cls);
				if (c != ' ')
					draw_glyph(gid, gx, ly, col);
				if (st & TS_UNDERLINE)
					draw_rect(gx, ly + font_m[FONT_MONO].ascent + PX(2), adv, PX(1), col);
				if (st & TS_STRIKE)
					draw_rect(gx, ly + lh / 2, adv, PX(1), col);
			}
			px += adv;
			i += k;
		}
	}

	/* inline git blame after the end of each cursor line */
	if (app.blame && app.blame_on) {
		const char *rel = doc_repo_rel(d);
		u32 key = blame_key_of(d);
		u32 shown = UINT32_MAX;
		for (u32 c = 0; rel && c < v->nc; c++) {
			u32 l = text_line_of(t, v->head[c]);
			if (l < first || l >= last || l == shown)
				continue;
			shown = l;
			BlameLine bl;
			if (!blame_line(app.blame, rel, key, l, &bl))
				continue;
			char msg[600];
			if (bl.uncommitted) {
				snprintf(msg, sizeof msg, "Not Committed Yet");
			} else {
				char ago[48];
				blame_ago(bl.time, (i64)time(NULL), ago, sizeof ago);
				snprintf(msg, sizeof msg, "%s, %s (%s)", bl.summary, bl.author, ago);
			}
			int ly = y0 + (int)(l - first) * lh;
			int bx = tx + x_of(t, l, text_line_start(t, l) + text_line_len(t, l)) + app.char_w * 3;
			int ex = draw_text_fit(FONT_MONO, bx, ly, msg, strlen(msg), VX_MAX(text_x + text_w - bx, 0), COL(C_CODELENS));
			if (!bl.uncommitted && c == 0) {
				blame_rect_x0 = bx;
				blame_rect_x1 = ex;
				blame_rect_y = ly;
				blame_rect_doc = d;
				snprintf(blame_rect_hash, sizeof blame_rect_hash, "%s", bl.hash);
			}
		}
	}

	/* diagnostics squiggles */
	for (u32 i = 0; i < D->dn[d]; i++) {
		u32 l0 = D->d_line0[d][i], l1 = D->d_line1[d][i];
		if (l1 < first || l0 >= last)
			continue;
		u8 sev = D->d_sev[d][i];
		u32 col = sev == LSP_SEV_ERROR ? COL(C_ERROR) : sev == LSP_SEV_WARNING ? COL(C_WARNING) : COL(C_INFO);
		if (sev == LSP_SEV_HINT)
			continue;
		for (u32 l = VX_MAX(l0, first); l <= l1 && l < last; l++) {
			int ly = y0 + (int)(l - first) * lh;
			u32 ls = text_line_start(t, l);
			u32 len = text_line_len(t, l);
			u32 a = l == l0 ? VX_MIN(D->d_col0[d][i], len) : 0;
			u32 b = l == l1 ? VX_MIN(D->d_col1[d][i], len) : len;
			int xa = tx + x_of(t, l, ls + a);
			int xb = tx + x_of(t, l, ls + b);
			if (xb <= xa)
				xb = xa + app.char_w; /* empty range: one cell */
			draw_squiggle(xa, ly + lh - PX(4), xb - xa, col);
		}
	}

	/* cursors (blink 530 ms on/off, solid while typing) */
	u64 phase = (vx_now_ns() - app.blink_t0) / 530000000ull;
	bool vim_block = app.vim_on && vim_block_cursor();
	if (focused && (app.headless || vim_block || phase % 2 == 0)) {
		for (u32 c = 0; c < v->nc; c++) {
			u32 h = vim_block ? vim_cursor_off(v, c) : v->head[c];
			u32 l = text_line_of(t, h);
			if (l < first || l >= last)
				continue;
			int ly = y0 + (int)(l - first) * lh;
			int cx = tx + x_of(t, l, h);
			if (vim_block) {
				/* vim normal/visual: a filled cell, the glyph in the background colour */
				u32 le = text_line_start(t, l) + text_line_len(t, l);
				int cw = h < le ? x_of(t, l, next_char(t, h)) - x_of(t, l, h) : app.char_w;
				draw_rect(cx, ly, VX_MAX(cw, PX(2)), lh, COL(C_CURSOR));
				if (h < le) {
					char gb[4];
					u32 gn = VX_MIN(4u, le - h), cp;
					text_copy(t, h, gn, gb);
					utf8_decode(gb, gn, &cp);
					if (cp != ' ' && cp != '\t')
						draw_glyph(font_glyph(FONT_MONO, cp), cx, ly, COL(C_EDITOR_BG));
				}
			} else if (app.overtype) {
				/* block cursor over the character it will replace */
				u32 le = text_line_start(t, l) + text_line_len(t, l);
				int cw = h < le ? x_of(t, l, next_char(t, h)) - x_of(t, l, h) : app.char_w;
				draw_rect(cx, ly, VX_MAX(cw, PX(2)), lh, (COL(C_CURSOR) & 0xFFFFFF00u) | 0x80u);
			} else {
				draw_rect(cx, ly, PX(2), lh, COL(C_CURSOR));
			}
		}
	}
	draw_noclip();

	/* gutter */
	draw_clip(x, y, gut, h);
	draw_rect(x, y, gut - PX(26) + PX(22), h, COL(C_EDITOR_BG));
	int nd = VX_MAX(digits(nl), 5);
	int num_r = x + PX(18) + nd * app.char_w;
	for (u32 l = first; l < last; l++) {
		int ly = y0 + (int)(l - first) * lh;
		char num[16];
		int k = snprintf(num, sizeof num, "%u", l + 1);
		bool active = false;
		for (u32 c = 0; c < v->nc; c++)
			if (text_line_of(t, v->head[c]) == l)
				active = true;
		draw_text(FONT_MONO, num_r - k * app.char_w, ly, num, (size_t)k,
		          active ? COL(C_LINE_NUMBER_ACTIVE) : COL(C_LINE_NUMBER));
		if (D->gm_n[d] > l && D->gm[d][l] && D->gm_for[d] != UINT32_MAX) {
			u8 m = D->gm[d][l];
			int gx = num_r + PX(5);
			if (m == GM_ADDED)
				draw_rect(gx, ly, PX(3), lh, COL(C_GUTTER_ADDED));
			else if (m == GM_MODIFIED)
				draw_rect(gx, ly, PX(3), lh, COL(C_GUTTER_MODIFIED));
			else if (m == GM_DELETED_ABOVE) {
				/* a small triangle at the top of the line */
				for (int r = 0; r < PX(4); r++)
					draw_rect(gx, ly - PX(4) + r, PX(4) - r, PX(1), COL(C_GUTTER_DELETED));
				for (int r = 0; r < PX(4); r++)
					draw_rect(gx, ly + r, PX(4) - (PX(4) - 1 - r), PX(1), COL(C_GUTTER_DELETED));
			}
		}
	}
	draw_noclip();

	/* minimap */
	i64 content = (i64)(nl - 1) * lh + h;
	if (mm) {
		int mx = x + w - SCROLL_W - mm;
		draw_clip(mx, y, mm, h);
		int mlh = PX(2);
		i64 mm_content = (i64)nl * mlh;
		i64 mm_scroll = mm_content > h ? (mm_content - h) * v->scroll_y / VX_MAX(content - h, 1) : 0;
		u32 mfirst = (u32)(mm_scroll / mlh);
		u32 mlast = VX_MIN(nl, mfirst + (u32)(h / mlh) + 1);
		for (u32 l = mfirst; l < mlast; l++) {
			int ly = y + (int)((i64)l * mlh - mm_scroll);
			u32 n;
			const char *s = text_line(t, l, &n);
			u32 nn = VX_MIN(n, 512u);
			static char buf[512];
			memcpy(buf, s, nn);
			classify_line(d, l, buf, nn);
			int col = 0;
			u32 i = 0;
			while (i < nn && col < mm) {
				u8 c = (u8)buf[i];
				if (c == ' ') {
					col++;
					i++;
					continue;
				}
				if (c == '\t') {
					col += TAB - col % TAB;
					i++;
					continue;
				}
				/* a run of same-class non-space chars as one rect */
				u8 cls = line_cls[i];
				int start = col;
				while (i < nn && buf[i] != ' ' && buf[i] != '\t' && line_cls[i] == cls) {
					if (!utf8_cont((u8)buf[i]))
						col++;
					i++;
				}
				u32 rgba = (TOK(cls) & 0xFFFFFF00u) | 0x99u;
				draw_rect(mx + PX(start), ly, PX(col - start), mlh, rgba);
			}
		}
		/* visible region slider on hover */
		if (app.hover_x >= mx && app.hover_x < mx + mm && app.hover_y >= y && app.hover_y < y + h) {
			int sy = (int)((i64)(v->scroll_y * mlh / lh) - mm_scroll);
			draw_rect(mx, y + sy, mm, (int)((i64)h * mlh / lh), COL(C_SCROLL_SLIDER));
		}
		draw_noclip();
	}

	/* vertical scrollbar with overview ruler */
	{
		int sx = x + w - SCROLL_W;
		draw_rect(sx, y, PX(1), h, 0x00000000u);
		/* overview: git marks and diagnostics */
		int lane = SCROLL_W / 3;
		if (nl > 0) {
			for (u32 l = 0; l < D->gm_n[d] && l < nl; l++) {
				u8 m = D->gm[d][l];
				if (!m || D->gm_for[d] == UINT32_MAX)
					continue;
				int my = y + (int)((i64)l * h / VX_MAX((i64)nl, 1));
				u32 c = m == GM_ADDED ? COL(C_GUTTER_ADDED) : m == GM_MODIFIED ? COL(C_GUTTER_MODIFIED) : COL(C_GUTTER_DELETED);
				draw_rect(sx + PX(1), my, lane, PX(3), c);
			}
			for (u32 i = 0; i < D->dn[d]; i++) {
				u8 sev = D->d_sev[d][i];
				if (sev > LSP_SEV_WARNING)
					continue;
				int my = y + (int)((i64)D->d_line0[d][i] * h / VX_MAX((i64)nl, 1));
				draw_rect(sx + SCROLL_W - lane, my, lane, PX(3), sev == LSP_SEV_ERROR ? COL(C_ERROR) : COL(C_WARNING));
			}
			int cy = y + (int)((i64)cur_line * h / VX_MAX((i64)nl, 1));
			draw_rect(sx + PX(1), cy, SCROLL_W - PX(1), PX(2), (COL(C_CURSOR) & 0xFFFFFF00u) | 0xA0u);
		}
		bool over = app.hover_x >= x && app.hover_x < x + w && app.hover_y >= y && app.hover_y < y + h;
		if (content > h && (over || drag_mode == 4)) {
			i64 slider_h = VX_MAX((i64)h * h / content, (i64)PX(20));
			i64 slider_y = (i64)(h - slider_h) * v->scroll_y / VX_MAX(content - h, 1);
			bool hov = app.hover_x >= sx && app.hover_x < sx + SCROLL_W && app.hover_y >= y && app.hover_y < y + h;
			draw_rect(sx + PX(1), y + (int)slider_y, SCROLL_W - PX(1), (int)slider_h,
			          drag_mode == 4 ? COL(C_SCROLL_SLIDER_ACTIVE) : hov ? COL(C_SCROLL_SLIDER_HOVER) : COL(C_SCROLL_SLIDER));
		}
		if (v->scroll_y > 0)
			draw_rect(x, y, w - SCROLL_W, PX(3), (COL(C_SCROLLBAR_SHADOW) & 0xFFFFFF00u) | 0x40u);
	}

	/* find widget */
	if (app.find_open && g == &app.groups[app.active_group])
		find_widget_draw(x, y, w);
}

/* ---- popups (drawn last, over everything) ------------------------------------------ */

static bool cursor_screen(View *v, int *cx, int *cy)
{
	Group *g = &app.groups[app.active_group];
	int x = g->x, y = g->y + group_head_h(g);
	Text *t = T(v);
	u32 l = text_line_of(t, v->head[0]);
	i64 ly = (i64)l * app.line_h - v->scroll_y;
	if (ly < 0 || ly > g->h)
		return false;
	*cx = x + ed_gutter_w(v) - v->scroll_x + x_of(t, l, v->head[0]);
	*cy = y + (int)ly;
	return true;
}

static const char *kind_letter(u8 k, u32 *color)
{
	/* LSP CompletionItemKind -> codicon-ish letter and colour */
	switch (k) {
	case 2: /* method */
	case 3: /* function */
	case 4: /* constructor */
		*color = 0xB180D7FF;
		return "f";
	case 5: /* field */
	case 6: /* variable */
	case 10: /* property */
		*color = 0x75BEFFFF;
		return "v";
	case 7: /* class */
	case 22: /* struct */
		*color = 0xEE9D28FF;
		return "S";
	case 13: /* enum */
		*color = 0xEE9D28FF;
		return "E";
	case 20: /* enum member */
		*color = 0x75BEFFFF;
		return "e";
	case 21: /* constant */
		*color = 0x75BEFFFF;
		return "c";
	case 14: /* keyword */
		*color = COL(C_FG);
		return "k";
	case 9: /* module */
		*color = COL(C_FG);
		return "{}";
	case 17: /* file */
		*color = COL(C_FG);
		return "\xE2\x96\xA1";
	case 25: /* type parameter */
		*color = 0xEE9D28FF;
		return "T";
	default:
		*color = COL(C_FG);
		return "\xC2\xB7";
	}
}

static void wrap_draw(int font, int x, int *y, int w, const char *s, size_t n, u32 color, bool draw_it)
{
	/* greedy word wrap within w */
	size_t i = 0;
	int lh = font_m[font].line;
	while (i < n) {
		size_t best = i;
		int pen = 0;
		size_t j = i;
		size_t last_space = (size_t)-1;
		while (j < n) {
			u32 cp;
			u32 k = utf8_decode(s + j, n - j, &cp);
			int a = font_glyphs.adv[font_glyph(font, cp)];
			if (pen + a > w && j > i)
				break;
			if (s[j] == ' ')
				last_space = j;
			pen += a;
			j += k;
		}
		best = j;
		if (j < n && last_space != (size_t)-1 && last_space > i)
			best = last_space + 1;
		if (draw_it)
			draw_text(font, x, *y, s + i, best - i, color);
		*y += lh;
		i = best;
	}
	if (n == 0)
		*y += lh;
}

/* the code hover's box on screen (last frame), 0 width when none */
static int hover_box_x, hover_box_y, hover_box_w, hover_box_h;

void ed_draw_popups(View *v)
{
	int cx, cy;
	if (app.compl_open && app.compl_nf && cursor_screen(v, &cx, &cy) && app.focus == FOCUS_EDITOR) {
		int rh = app.line_h + PX(3);
		u32 rows = VX_MIN(app.compl_nf, 12u);
		int pw = PX(430);
		int ph = (int)rows * rh + PX(2);
		Text *t = T(v);
		int sx = cx - (int)(x_of(t, text_line_of(t, v->head[0]), v->head[0]) -
		                    x_of(t, text_line_of(t, app.compl_start), app.compl_start));
		int px = VX_MIN(sx - PX(24), app.w - pw - PX(4));
		int py = cy + app.line_h;
		if (py + ph > app.h - app.status_h)
			py = cy - ph;
		if (app.compl_sel < app.compl_top)
			app.compl_top = app.compl_sel;
		if (app.compl_sel >= app.compl_top + rows)
			app.compl_top = app.compl_sel - rows + 1;
		draw_rect(px - PX(1), py, pw + PX(2), ph + PX(3), COL(C_WIDGET_SHADOW));
		draw_rect(px, py, pw, ph, COL(C_WIDGET_BG));
		draw_frame(px, py, pw, ph, COL(C_WIDGET_BORDER));
		char pre[128];
		u32 pn = VX_MIN(v->head[0] - app.compl_start, (u32)sizeof pre - 1);
		text_copy(t, app.compl_start, pn, pre);
		draw_clip(px + PX(1), py + PX(1), pw - PX(2), ph - PX(2));
		for (u32 r = 0; r < rows; r++) {
			u32 fi = app.compl_top + r;
			if (fi >= app.compl_nf)
				break;
			u32 i = app.compl_filtered[fi];
			int ry = py + PX(1) + (int)r * rh;
			bool sel = fi == app.compl_sel;
			if (sel) {
				draw_rect(px + PX(1), ry, pw - PX(2), rh, COL(C_QUICK_ACTIVE_BG));
				draw_frame(px + PX(1), ry, pw - PX(2), rh, COL(C_FOCUS_BORDER));
			}
			u32 kc;
			const char *kl = kind_letter(app.compl_kind[i], &kc);
			int ty = ry + (rh - app.line_h) / 2;
			draw_text(FONT_MONO_BOLD, px + PX(6), ty, kl, strlen(kl), kc);
			const char *label = app.compl_pool.b + app.compl_label[i];
			u64 mask;
			fuzzy_score(pre, pn, label, strlen(label), &mask);
			int lx = px + PX(26);
			size_t ln = strlen(label);
			u32 fg = sel ? COL(C_QUICK_ACTIVE_FG) : COL(C_EDITOR_FG);
			for (size_t k = 0; k < ln && lx < px + pw - PX(8); k++) {
				bool hl = k < 63 && ((mask >> k) & 1);
				lx = draw_text(FONT_MONO, lx, ty, label + k, 1, hl ? COL(C_QUICK_HIGHLIGHT) : fg);
			}
			if (sel) {
				const char *det = app.compl_pool.b + app.compl_detail[i];
				size_t dn = strlen(det);
				int dw = font_width(FONT_MONO, det, dn);
				int room = px + pw - PX(8) - (lx + PX(16));
				if (room > PX(30)) {
					int dx = px + pw - PX(8) - VX_MIN(dw, room);
					draw_text_fit(FONT_MONO, dx, ty, det, dn, room, COL(C_DESCRIPTION_FG));
				}
			}
		}
		draw_noclip();
	}
	if (app.hover_mouse && (app.hover_open || app.hover_due)) {
		/* a pointer hover lives while the pointer is over the text it was
		 * opened for or over the hover itself. Checked every frame: the
		 * editor sees no move once the pointer has left its area, and a
		 * fast move leaves it in one jump. */
		Group *g = &app.groups[app.active_group];
		int top = g->y + group_head_h(g);
		bool in_text = app.hover_x >= g->x + ed_gutter_w(v) && app.hover_x < g->x + g->w - minimap_w(g->w) &&
		               app.hover_y >= top && app.hover_y < g->y + g->h;
		bool in_box = hover_box_w > 0 && app.hover_x >= hover_box_x && app.hover_x < hover_box_x + hover_box_w &&
		              app.hover_y >= hover_box_y && app.hover_y < hover_box_y + hover_box_h;
		if (!in_text && !in_box) {
			app.hover_open = false;
			app.hover_due = 0;
		}
	}
	hover_box_w = 0;
	if (app.hover_open && app.hover_doc == v->doc) {
		Text *t = T(v);
		u32 off = VX_MIN(app.hover_off, t->len);
		u32 save = v->head[0];
		v->head[0] = off;
		bool vis = cursor_screen(v, &cx, &cy);
		v->head[0] = save;
		if (!vis)
			return;
		/* gather: diagnostics on that line + hover text */
		Docs *D = &app.docs;
		u32 l = text_line_of(t, off);
		u32 col = off - text_line_start(t, l);
		int pw = PX(500);
		int pad = PX(8);
		/* measure */
		int hgt = pad;
		const int MAXL = 40;
		for (int pass = 0; pass < 2; pass++) {
			int py = 0, px = 0;
			if (pass == 1) {
				int boxh = hgt;
				px = VX_MIN(cx, app.w - pw - PX(4));
				py = cy - boxh - PX(2);
				if (py < 0)
					py = cy + app.line_h + PX(2);
				draw_rect(px - PX(1), py, pw + PX(2), boxh + PX(3), COL(C_WIDGET_SHADOW));
				draw_rect(px, py, pw, boxh, COL(C_WIDGET_BG));
				draw_frame(px, py, pw, boxh, COL(C_WIDGET_BORDER));
				draw_clip(px, py, pw, boxh);
				/* plus the gap to the word, so the pointer can travel onto the box */
				hover_box_x = px;
				hover_box_y = VX_MIN(py, cy);
				hover_box_w = pw;
				hover_box_h = VX_MAX(py + boxh, cy + app.line_h) - hover_box_y;
			}
			int y = py + pad;
			int lines = 0;
			bool any = false;
			for (u32 i = 0; i < D->dn[v->doc]; i++) {
				bool on = D->d_line0[v->doc][i] <= l && D->d_line1[v->doc][i] >= l;
				if (on && D->d_line0[v->doc][i] == l && D->d_line1[v->doc][i] == l)
					on = col + 1 >= D->d_col0[v->doc][i] && col <= D->d_col1[v->doc][i] + 1;
				if (!on)
					continue;
				const char *m = D->d_pool[v->doc].b + D->d_msg[v->doc][i];
				size_t mlen = strcspn(m, "\x1f");
				u8 sev = D->d_sev[v->doc][i];
				u32 c = sev == LSP_SEV_ERROR ? COL(C_ERROR) : sev == LSP_SEV_WARNING ? COL(C_WARNING) : COL(C_INFO);
				if (pass == 1)
					draw_icon(sev == LSP_SEV_ERROR ? ICON_ERROR : sev == LSP_SEV_WARNING ? ICON_WARNING : ICON_INFO,
					          px + pad, y + (font_m[FONT_UI].line - PX(16)) / 2, c);
				wrap_draw(FONT_UI, px + pad + PX(22), &y, pw - 2 * pad - PX(22), m, mlen, COL(C_FG), pass == 1);
				if (m[mlen]) {
					if (pass == 1)
						draw_text(FONT_UI, px + pad + PX(22), y, m + mlen + 1, strlen(m + mlen + 1), COL(C_DESCRIPTION_FG));
					y += font_m[FONT_UI].line;
				}
				any = true;
			}
			if (app.hover_text) {
				if (any) {
					if (pass == 1)
						draw_rect(px, y + PX(3), pw, PX(1), COL(C_WIDGET_BORDER));
					y += PX(8);
				}
				/* markdown-lite: fenced code in mono, the rest in UI font */
				const char *s = app.hover_text;
				bool code = false;
				while (*s && lines < MAXL) {
					const char *e = strchr(s, '\n');
					size_t n = e ? (size_t)(e - s) : strlen(s);
					if (n >= 3 && strncmp(s, "```", 3) == 0) {
						code = !code;
					} else if (n == 3 && strncmp(s, "---", 3) == 0) {
						if (pass == 1)
							draw_rect(px + pad, y + PX(4), pw - 2 * pad, PX(1), COL(C_WIDGET_BORDER));
						y += PX(9);
					} else {
						/* drop markdown emphasis markers */
						char buf[1024];
						size_t k = 0;
						for (size_t i = 0; i < n && k < sizeof buf - 1; i++)
							if (code || (s[i] != '`' && s[i] != '*' && s[i] != '\\'))
								buf[k++] = s[i];
						if (code) {
							/* syntax colour code lines as C */
							if (pass == 1) {
								SynSpans sp = {sp_start, sp_len, sp_cls, 0, 2048};
								syn_line(LANG_C, 0, buf, (u32)k, &sp);
								memset(line_cls, TK_TEXT, k);
								for (u32 q = 0; q < sp.count; q++)
									memset(line_cls + sp.start[q], sp.cls[q], VX_MIN(sp.len[q], (u32)k - sp.start[q]));
								int xx = px + pad;
								for (size_t q = 0; q < k && xx < px + pw - pad; q++)
									xx = draw_text(FONT_MONO, xx, y, buf + q, 1, TOK(line_cls[q]));
							}
							y += app.line_h;
						} else {
							size_t h0 = 0;
							while (h0 < k && buf[h0] == '#')
								h0++;
							bool head = h0 > 0 && h0 < k && buf[h0] == ' ';
							int f = head ? FONT_UI_BOLD : FONT_UI;
							size_t skip = head ? h0 + 1 : 0;
							wrap_draw(f, px + pad, &y, pw - 2 * pad, buf + skip, k - skip, COL(C_FG), pass == 1);
						}
					}
					lines++;
					if (!e)
						break;
					s = e + 1;
				}
				any = true;
			}
			if (!any) {
				if (pass == 0)
					return; /* nothing to show */
			}
			hgt = y - py + pad;
		}
		draw_noclip();
	}
}

int ed_x_of(Text *t, u32 l, u32 off) { return x_of(t, l, off); }
u32 ed_off_at_x(Text *t, u32 l, int px) { return off_at_x(t, l, px); }
