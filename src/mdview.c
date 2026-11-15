/* Markdown preview. A Markdown document's tab has two subtabs: Edit (the
 * editor) and Preview (the rendered text, read-only; Ctrl+Shift+V toggles).
 * A small CommonMark + GFM subset: ATX / setext headings, paragraphs with
 * emphasis, strong, code spans, strikethrough, links and autolinks, hard
 * breaks, lists (nested, ordered, tasks), block quotes, fenced code
 * (highlighted by the fence's language) and indented code, tables, rules.
 * Images show their alt text; HTML tags are dropped.
 *
 * The layout is a flat list of positioned text runs and rectangles in
 * content coordinates, rebuilt when the text, the width or the scale
 * changes. Colours are theme slots, resolved when drawn. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <stdio.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#define NONE UINT32_MAX
enum { IT_TEXT, IT_RECT };
#define TOKCOL 0x8000u /* colour is a token class, not a C_* slot */

typedef struct {
	u32 doc, ver;
	int width, scale;
	bool used;
	/* items (SoA) */
	u32 n, cap;
	u8 *kind, *font, *alpha;
	u16 *col;
	i32 *x, *y, *w, *h; /* text: w = clip width (0: none), h = its row height */
	u32 *str, *len, *link; /* pool offsets; link + 1, 0 = none */
	Pool pool;
	int height;
	/* block starts: source line -> y */
	u32 na, acap;
	u32 *a_line;
	i32 *a_y;
	/* headings, for #anchor links */
	u32 nh, hcap;
	u32 *h_slug;
	i32 *h_y;
} Layout;

#define IT_COLS(L)                                                                                                     \
	SOA((L)->kind), SOA((L)->font), SOA((L)->alpha), SOA((L)->col), SOA((L)->x), SOA((L)->y), SOA((L)->w),          \
	    SOA((L)->h), SOA((L)->str), SOA((L)->len), SOA((L)->link)
#define AN_COLS(L) SOA((L)->a_line), SOA((L)->a_y)
#define HD_COLS(L) SOA((L)->h_slug), SOA((L)->h_y)

static Layout lays[4];
static u32 lay_next;

static void lay_free(Layout *L)
{
	SoaCol a[] = {IT_COLS(L)}, b[] = {AN_COLS(L)}, c[] = {HD_COLS(L)};
	soa_free(a, 11);
	soa_free(b, 2);
	soa_free(c, 2);
	pool_free(&L->pool);
	memset(L, 0, sizeof *L);
}

static u32 item(Layout *L, u8 kind, int font, u16 col, u8 alpha, int x, int y, int w, int h)
{
	SoaCol c[] = {IT_COLS(L)};
	soa_grow(c, 11, &L->cap, L->n + 1, L->n);
	u32 i = L->n++;
	L->kind[i] = kind;
	L->font[i] = (u8)font;
	L->col[i] = col;
	L->alpha[i] = alpha;
	L->x[i] = x;
	L->y[i] = y;
	L->w[i] = w;
	L->h[i] = h;
	L->str[i] = L->len[i] = L->link[i] = 0;
	return i;
}

static void rect(Layout *L, u16 col, u8 alpha, int x, int y, int w, int h) { item(L, IT_RECT, 0, col, alpha, x, y, w, h); }

/* ---- rows: runs of text placed left to right, wrapped, baseline-aligned ------------------- */

typedef struct {
	Layout *L;
	int y;             /* top of the next row / block */
	int x0, x1;        /* the current text area */
	int rx;            /* pen */
	u32 row_first;     /* first item of the row being built */
	int asc, desc;     /* the row's tallest ascent / descent */
	bool empty;
	int clip;          /* text clip width for table cells (0: none) */
} Ctx;

static void row_begin(Ctx *c)
{
	c->rx = c->x0;
	c->row_first = c->L->n;
	c->asc = c->desc = 0;
	c->empty = true;
}

static void row_flush(Ctx *c)
{
	if (c->empty) {
		row_begin(c);
		return;
	}
	Layout *L = c->L;
	int top = c->y + PX(2);
	for (u32 i = c->row_first; i < L->n; i++) {
		int f = L->font[i];
		int ty = top + c->asc - font_m[f].ascent;
		if (L->kind[i] == IT_TEXT) {
			L->y[i] = ty;
			L->h[i] = font_m[f].line;
		} else if (L->h[i] == -1) { /* code span background */
			L->y[i] = ty - PX(1);
			L->h[i] = font_m[f].line + PX(2);
		} else { /* strikethrough */
			L->y[i] = ty + font_m[f].ascent * 2 / 3;
			L->h[i] = PX(1);
		}
	}
	c->y = top + c->asc + c->desc + PX(3);
	row_begin(c);
}

typedef struct {
	int font;
	u16 col;
	u32 link; /* pool offset + 1 */
	bool code, strike;
} Style;

/* place s[0..n) (no newlines) with style st, breaking at spaces */
static void put(Ctx *c, const char *s, size_t n, Style st)
{
	Layout *L = c->L;
	size_t i = 0;
	int sp = font_width(st.font, " ", 1);
	while (i < n) {
		if (s[i] == ' ') {
			if (!c->empty)
				c->rx += sp;
			i++;
			continue;
		}
		size_t j = i;
		while (j < n && s[j] != ' ')
			j++;
		int w = font_width(st.font, s + i, j - i);
		if (!c->empty && c->rx + w > c->x1 && !c->clip) {
			row_flush(c);
		}
		if (st.code)
			item(L, IT_RECT, st.font, C_FG, 0x1C, c->rx - PX(2), 0, w + PX(4), -1);
		u32 t = item(L, IT_TEXT, st.font, st.col, 0, c->rx, 0, c->clip ? VX_MAX(c->x1 - c->rx, 1) : 0, 0);
		L->str[t] = pool_add(&L->pool, s + i, (u32)(j - i));
		L->len[t] = (u32)(j - i);
		L->link[t] = st.link;
		if (st.strike)
			item(L, IT_RECT, st.font, st.col, 0, c->rx, 0, w, -2);
		c->asc = VX_MAX(c->asc, font_m[st.font].ascent);
		c->desc = VX_MAX(c->desc, font_m[st.font].line - font_m[st.font].ascent);
		c->rx += w;
		c->empty = false;
		/* a space right after the word belongs to the same style */
		i = j;
	}
}

/* ---- inline syntax ---------------------------------------------------------------------- */

static bool is_punct(char ch) { return ch && strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", ch); }
static bool is_alnum(char ch)
{
	return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (u8)ch >= 0x80;
}

static const char *find_str(const char *s, size_t n, const char *w, size_t k)
{
	for (size_t i = 0; i + k <= n; i++)
		if (!memcmp(s + i, w, k))
			return s + i;
	return NULL;
}

typedef struct {
	char *b;
	u32 n, cap;
} Buf;

static void bput(Buf *b, const char *s, size_t n)
{
	VX_GROW(b->b, b->cap, b->n + (u32)n + 1);
	memcpy(b->b + b->n, s, n);
	b->n += (u32)n;
}

static Style style_of(int base, bool bold, bool ital, bool code, bool strike, u32 link, u16 col)
{
	Style st = {base, col, link, code, strike};
	if (code)
		st.font = FONT_MONO;
	else if (base == FONT_UI || base == FONT_UI_ITALIC) {
		if (bold)
			st.font = FONT_UI_BOLD;
		else if (ital)
			st.font = FONT_UI_ITALIC;
	}
	if (link)
		st.col = C_LINK;
	return st;
}

static void inl(Ctx *c, const char *s, size_t n, int base, u16 col, u32 link);

static void flush_text(Ctx *c, Buf *t, Style st)
{
	size_t a = 0;
	for (size_t i = 0; i <= t->n; i++)
		if (i == t->n || t->b[i] == '\n') {
			if (i > a)
				put(c, t->b + a, i - a, st);
			if (i < t->n)
				row_flush(c); /* a hard break */
			a = i + 1;
		}
	t->n = 0;
}

static void inl(Ctx *c, const char *s, size_t n, int base, u16 col, u32 link)
{
	Layout *L = c->L;
	bool bold = false, ital = false, strike = false;
	Buf t = {0};
	size_t i = 0;
#define CUR style_of(base, bold, ital, false, strike, link, col)
	while (i < n) {
		char ch = s[i];
		if (ch == '\\' && i + 1 < n && is_punct(s[i + 1])) {
			bput(&t, s + i + 1, 1);
			i += 2;
			continue;
		}
		if (ch == '`') {
			size_t k = 0;
			while (i + k < n && s[i + k] == '`')
				k++;
			char fence[16];
			size_t fk = VX_MIN(k, sizeof fence - 1);
			memset(fence, '`', fk);
			const char *e = find_str(s + i + k, n - i - k, fence, fk);
			if (e) {
				flush_text(c, &t, CUR);
				const char *a = s + i + k;
				size_t an = (size_t)(e - a);
				if (an >= 2 && a[0] == ' ' && a[an - 1] == ' ')
					a++, an -= 2;
				if (!c->empty)
					c->rx += PX(3); /* room for the span's background */
				put(c, a, an, style_of(base, bold, ital, true, strike, link, C_EDITOR_FG));
				c->rx += PX(3);
				i = (size_t)(e - s) + k;
				continue;
			}
			bput(&t, s + i, k);
			i += k;
			continue;
		}
		if (ch == '*' || ch == '_' || (ch == '~' && i + 1 < n && s[i + 1] == '~')) {
			size_t k = 0;
			while (i + k < n && s[i + k] == ch)
				k++;
			char prev = i ? s[i - 1] : ' ', next = i + k < n ? s[i + k] : ' ';
			bool intraword = ch == '_' && is_alnum(prev) && is_alnum(next);
			bool can_open = next != ' ' && !intraword, can_close = prev != ' ' && !intraword;
			char d[4] = {ch, ch, ch, 0};
			if (ch == '~') {
				bool closing = strike && can_close;
				bool opening = !strike && can_open && find_str(s + i + 2, n - i - 2, "~~", 2);
				if (k == 2 && (closing || opening)) {
					flush_text(c, &t, CUR);
					strike = !strike;
					i += 2;
					continue;
				}
				bput(&t, s + i, k);
				i += k;
				continue;
			}
			size_t kk = VX_MIN(k, (size_t)3);
			bool want_b = kk >= 2, want_i = kk != 2;
			bool closes = (want_b ? bold : true) && (want_i ? ital : true) && can_close;
			bool opens = !closes && can_open && find_str(s + i + kk, n - i - kk, d + (3 - kk), kk);
			if (closes || opens) {
				flush_text(c, &t, CUR);
				if (want_b)
					bold = !bold;
				if (want_i)
					ital = !ital;
				i += kk;
				continue;
			}
			bput(&t, s + i, k);
			i += k;
			continue;
		}
		if (ch == '[' || (ch == '!' && i + 1 < n && s[i + 1] == '[')) {
			bool img = ch == '!';
			size_t a = i + (img ? 2 : 1), j = a;
			int depth = 1;
			while (j < n && depth) {
				if (s[j] == '\\')
					j++;
				else if (s[j] == '[')
					depth++;
				else if (s[j] == ']')
					depth--;
				if (depth)
					j++;
			}
			if (j < n && j + 1 < n && s[j + 1] == '(') {
				size_t u = j + 2, e = u;
				int pd = 1;
				while (e < n && pd) {
					if (s[e] == '(')
						pd++;
					else if (s[e] == ')')
						pd--;
					if (pd)
						e++;
				}
				if (e < n) {
					/* the URL, without a "title" */
					size_t ue = u;
					while (ue < e && s[ue] != ' ')
						ue++;
					const char *url = s + u;
					size_t un = ue - u;
					if (un >= 2 && url[0] == '<' && url[un - 1] == '>')
						url++, un -= 2;
					flush_text(c, &t, CUR);
					u32 lk = pool_add(&L->pool, url, (u32)un) + 1;
					if (img) {
						char alt[300];
						snprintf(alt, sizeof alt, "[image: %.*s]", (int)VX_MIN(j - a, (size_t)250), s + a);
						Style st = style_of(FONT_UI_ITALIC, false, false, false, false, lk, C_DESCRIPTION_FG);
						st.font = FONT_UI_ITALIC;
						put(c, alt, strlen(alt), st);
					} else
						inl(c, s + a, j - a, bold ? FONT_UI_BOLD : ital ? FONT_UI_ITALIC : base, col, lk);
					i = e + 1;
					continue;
				}
			}
			bput(&t, s + i, 1);
			i++;
			continue;
		}
		if (ch == '<') {
			size_t e = i + 1;
			while (e < n && s[e] != '>' && s[e] != ' ' && s[e] != '<')
				e++;
			if (e < n && s[e] == '>' && e > i + 1) {
				const char *w = s + i + 1;
				size_t wn = e - i - 1;
				if (find_str(w, wn, "://", 3) || (wn > 7 && !memcmp(w, "mailto:", 7))) {
					flush_text(c, &t, CUR);
					u32 lk = pool_add(&L->pool, w, (u32)wn) + 1;
					put(c, w, wn, style_of(base, bold, ital, false, strike, lk, col));
					i = e + 1;
					continue;
				}
			}
			/* an HTML tag: <br> breaks the line, others are dropped */
			size_t te = i + 1;
			while (te < n && s[te] != '>')
				te++;
			if (te < n && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '!' || ((s[i + 1] | 32) >= 'a' && (s[i + 1] | 32) <= 'z'))) {
				if (te - i >= 3 && (s[i + 1] | 32) == 'b' && (s[i + 2] | 32) == 'r')
					bput(&t, "\n", 1);
				i = te + 1;
				continue;
			}
			bput(&t, s + i, 1);
			i++;
			continue;
		}
		if ((ch == 'h') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '(') &&
		    (n - i > 8 && (!memcmp(s + i, "https://", 8) || !memcmp(s + i, "http://", 7)))) {
			size_t e = i;
			while (e < n && s[e] != ' ' && s[e] != '\n')
				e++;
			while (e > i && strchr(".,;:!?)", s[e - 1]))
				e--;
			flush_text(c, &t, CUR);
			u32 lk = pool_add(&L->pool, s + i, (u32)(e - i)) + 1;
			put(c, s + i, e - i, style_of(base, bold, ital, false, strike, lk, col));
			i = e;
			continue;
		}
		if (ch == '&') {
			static const struct {
				const char *e, *r;
			} ent[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
			           {"&nbsp;", "\xC2\xA0"}, {"&copy;", "\xC2\xA9"}, {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"}};
			bool done = false;
			for (size_t k = 0; k < VX_COUNT(ent) && !done; k++) {
				size_t el = strlen(ent[k].e);
				if (n - i >= el && !memcmp(s + i, ent[k].e, el)) {
					bput(&t, ent[k].r, strlen(ent[k].r));
					i += el;
					done = true;
				}
			}
			if (done)
				continue;
		}
		bput(&t, s + i, 1);
		i++;
	}
	flush_text(c, &t, CUR);
#undef CUR
	free(t.b);
}

/* ---- blocks ---------------------------------------------------------------------------- */

typedef struct {
	const char *s;
	u32 n, src;
} Ln;

static int indent_of(const Ln *l, u32 *bytes)
{
	int col = 0;
	u32 i = 0;
	while (i < l->n && (l->s[i] == ' ' || l->s[i] == '\t')) {
		col = l->s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
		i++;
	}
	if (bytes)
		*bytes = i;
	return col;
}

static bool blank(const Ln *l)
{
	for (u32 i = 0; i < l->n; i++)
		if (l->s[i] != ' ' && l->s[i] != '\t' && l->s[i] != '\r')
			return false;
	return true;
}

/* skip up to `cols` columns of indentation */
static Ln dedent(Ln l, int cols)
{
	int col = 0;
	u32 i = 0;
	while (i < l.n && col < cols && (l.s[i] == ' ' || l.s[i] == '\t')) {
		col = l.s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
		i++;
	}
	l.s += i;
	l.n -= i;
	return l;
}

static bool is_hr(const Ln *l)
{
	u32 b;
	if (indent_of(l, &b) > 3)
		return false;
	char m = 0;
	int k = 0;
	for (u32 i = b; i < l->n; i++) {
		char ch = l->s[i];
		if (ch == ' ' || ch == '\t' || ch == '\r')
			continue;
		if (ch != '-' && ch != '*' && ch != '_')
			return false;
		if (m && ch != m)
			return false;
		m = ch;
		k++;
	}
	return k >= 3;
}

static int fence_of(const Ln *l, char *fc)
{
	u32 b;
	if (indent_of(l, &b) > 3 || b >= l->n)
		return 0;
	char ch = l->s[b];
	if (ch != '`' && ch != '~')
		return 0;
	int k = 0;
	while (b + (u32)k < l->n && l->s[b + (u32)k] == ch)
		k++;
	*fc = ch;
	return k >= 3 ? k : 0;
}

static int heading_of(const Ln *l, const char **text, u32 *tn)
{
	u32 b;
	if (indent_of(l, &b) > 3)
		return 0;
	int k = 0;
	while (b + (u32)k < l->n && l->s[b + (u32)k] == '#')
		k++;
	if (k < 1 || k > 6 || (b + (u32)k < l->n && l->s[b + (u32)k] != ' ' && l->s[b + (u32)k] != '\t'))
		return 0;
	const char *s = l->s + b + k;
	u32 n = l->n - b - (u32)k;
	while (n && (*s == ' ' || *s == '\t'))
		s++, n--;
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\r'))
		n--;
	/* closing #s */
	u32 e = n;
	while (e && s[e - 1] == '#')
		e--;
	if (e < n && (e == 0 || s[e - 1] == ' ')) {
		n = e;
		while (n && s[n - 1] == ' ')
			n--;
	}
	*text = s;
	*tn = n;
	return k;
}

/* list marker: 1 bullet, 2 ordered; *content = column where the text starts */
static int list_marker(const Ln *l, int *content, u32 *num, char *bullet)
{
	u32 b;
	int ind = indent_of(l, &b);
	if (b >= l->n)
		return 0;
	char ch = l->s[b];
	u32 m = b;
	int kind = 0;
	if (ch == '-' || ch == '*' || ch == '+') {
		m = b + 1;
		kind = 1;
		*bullet = ch;
	} else if (ch >= '0' && ch <= '9') {
		u32 v = 0, d = 0;
		while (m < l->n && l->s[m] >= '0' && l->s[m] <= '9' && d < 9)
			v = v * 10 + (u32)(l->s[m++] - '0'), d++;
		if (m >= l->n || (l->s[m] != '.' && l->s[m] != ')'))
			return 0;
		*bullet = l->s[m];
		m++;
		*num = v;
		kind = 2;
	} else
		return 0;
	if (m < l->n && l->s[m] != ' ' && l->s[m] != '\t' && l->s[m] != '\r')
		return 0;
	u32 sp = 0;
	while (m + sp < l->n && l->s[m + sp] == ' ' && sp < 4)
		sp++;
	if (m + sp >= l->n || sp == 4 || l->s[m + sp] == '\r')
		sp = 1; /* empty item, or indented code inside: one space */
	*content = ind + (int)(m - b) + (int)sp;
	return kind;
}

/* an item's first line without its marker ("- ", "12. ") */
static Ln after_marker(Ln l)
{
	u32 b;
	indent_of(&l, &b);
	u32 p = b;
	while (p < l.n && l.s[p] != ' ' && l.s[p] != '\t' && l.s[p] != '\r')
		p++;
	u32 sp = 0;
	while (p + sp < l.n && l.s[p + sp] == ' ' && sp < 5)
		sp++;
	if (sp > 4)
		sp = 1; /* indented code inside the item: keep its indentation */
	l.s += p + sp;
	l.n -= p + sp;
	while (l.n && (l.s[l.n - 1] == '\r'))
		l.n--;
	return l;
}

static bool table_delim(const Ln *l)
{
	bool dash = false, pipe = false;
	for (u32 i = 0; i < l->n; i++) {
		char ch = l->s[i];
		if (ch == '-')
			dash = true;
		else if (ch == '|')
			pipe = true;
		else if (ch != ':' && ch != ' ' && ch != '\t' && ch != '\r')
			return false;
	}
	return dash && pipe;
}

static bool block_start(const Ln *l)
{
	char fc;
	const char *t;
	u32 tn;
	int ct;
	u32 num;
	char bl;
	u32 b;
	if (indent_of(l, &b) > 3)
		return false;
	return fence_of(l, &fc) || heading_of(l, &t, &tn) || is_hr(l) || (b < l->n && l->s[b] == '>') ||
	       (list_marker(l, &ct, &num, &bl) && !blank(&(Ln){l->s + b + 1, l->n > b + 1 ? l->n - b - 1 : 0, 0}));
}

static void anchor(Ctx *c, u32 src)
{
	Layout *L = c->L;
	SoaCol a[] = {AN_COLS(L)};
	soa_grow(a, 2, &L->acap, L->na + 1, L->na);
	L->a_line[L->na] = src;
	L->a_y[L->na] = c->y;
	L->na++;
}

static void heading_slug(Layout *L, const char *s, u32 n, int y)
{
	char b[256];
	u32 k = 0;
	for (u32 i = 0; i < n && k + 1 < sizeof b; i++) {
		char ch = s[i];
		if (ch >= 'A' && ch <= 'Z')
			b[k++] = (char)(ch + 32);
		else if (is_alnum(ch) || ch == '-' || ch == '_')
			b[k++] = ch;
		else if (ch == ' ')
			b[k++] = '-';
	}
	SoaCol h[] = {HD_COLS(L)};
	soa_grow(h, 2, &L->hcap, L->nh + 1, L->nh);
	L->h_slug[L->nh] = pool_add(&L->pool, b, k);
	L->h_y[L->nh] = y;
	L->nh++;
}

static void paragraph(Ctx *c, const char *s, u32 n, int font, u16 col)
{
	row_begin(c);
	inl(c, s, n, font, col, 0);
	row_flush(c);
}

static void code_block(Ctx *c, Ln *L, u32 n, int lang)
{
	Layout *Y = c->L;
	int lh = font_m[FONT_MONO].line;
	int pad = PX(10);
	int top = c->y;
	rect(Y, C_FG, 0x12, c->x0, top, c->x1 - c->x0, (int)n * lh + 2 * pad);
	static u32 st[512], sl[512];
	static u8 sc[512];
	u32 state = 0;
	char buf[1024];
	for (u32 i = 0; i < n; i++) {
		/* tabs to spaces */
		u32 k = 0;
		for (u32 j = 0; j < L[i].n && k + 5 < sizeof buf; j++) {
			if (L[i].s[j] == '\r')
				continue;
			if (L[i].s[j] == '\t')
				do
					buf[k++] = ' ';
				while (k % 4);
			else
				buf[k++] = L[i].s[j];
		}
		int y = top + pad + (int)i * lh;
		int px = c->x0 + pad;
		int clip = c->x1 - px - pad;
		SynSpans sp = {st, sl, sc, 0, 512};
		if (lang != LANG_NONE)
			state = syn_line(lang, state, buf, k, &sp);
		u32 pos = 0;
		for (u32 q = 0; q <= sp.count; q++) {
			u32 a = q < sp.count ? sp.start[q] : k;
			u32 segs[2][3] = {{pos, a, TK_TEXT}, {a, q < sp.count ? a + sp.len[q] : a, q < sp.count ? sp.cls[q] : TK_TEXT}};
			for (int g = 0; g < 2; g++) {
				u32 s0 = segs[g][0], s1 = segs[g][1];
				if (s1 <= s0)
					continue;
				int xo = font_width(FONT_MONO, buf, s0);
				u32 t = item(Y, IT_TEXT, FONT_MONO, (u16)(TOKCOL | segs[g][2]), 0, px + xo, y,
				             VX_MAX(clip - xo, 1), lh);
				Y->str[t] = pool_add(&Y->pool, buf + s0, s1 - s0);
				Y->len[t] = s1 - s0;
			}
			if (q < sp.count)
				pos = sp.start[q] + sp.len[q];
		}
	}
	c->y = top + (int)n * lh + 2 * pad;
}

static void blocks(Ctx *c, Ln *L, u32 n, bool tight);

static void table(Ctx *c, Ln *L, u32 n)
{
	Layout *Y = c->L;
	/* cells of a row */
	enum { MAXC = 32 };
	u8 align[MAXC] = {0};
	int ncol = 0;
	{
		const Ln *d = &L[1];
		u32 i = 0;
		while (i < d->n && (d->s[i] == ' ' || d->s[i] == '|'))
			i++;
		while (i < d->n && ncol < MAXC) {
			u32 a = i;
			while (i < d->n && d->s[i] != '|')
				i++;
			u32 b = i;
			while (a < b && d->s[a] == ' ')
				a++;
			while (b > a && (d->s[b - 1] == ' ' || d->s[b - 1] == '\r'))
				b--;
			if (b > a)
				align[ncol++] = (u8)((d->s[a] == ':' ? 1 : 0) | (d->s[b - 1] == ':' ? 2 : 0));
			i++;
		}
	}
	if (!ncol)
		return;
	/* split rows into cells */
	const char *cs[64][MAXC];
	u32 cn[64][MAXC];
	u32 nrow = 0;
	for (u32 r = 0; r < n && nrow < 64; r++) {
		if (r == 1)
			continue;
		const Ln *l = &L[r];
		u32 i = 0;
		while (i < l->n && l->s[i] == ' ')
			i++;
		if (i < l->n && l->s[i] == '|')
			i++;
		for (int k = 0; k < ncol; k++) {
			u32 a = i;
			while (i < l->n && !(l->s[i] == '|' && (i == 0 || l->s[i - 1] != '\\')))
				i++;
			u32 b = i;
			while (a < b && l->s[a] == ' ')
				a++;
			while (b > a && (l->s[b - 1] == ' ' || l->s[b - 1] == '\r'))
				b--;
			cs[nrow][k] = l->s + a;
			cn[nrow][k] = b > a ? b - a : 0;
			if (i < l->n)
				i++;
		}
		nrow++;
	}
	/* column widths: natural, scaled down to fit */
	int pad = PX(10), cw[MAXC], tot = 0;
	for (int k = 0; k < ncol; k++) {
		int w = PX(30);
		for (u32 r = 0; r < nrow; r++)
			w = VX_MAX(w, font_width(r ? FONT_UI : FONT_UI_BOLD, cs[r][k], cn[r][k]) + 2 * pad);
		cw[k] = w;
		tot += w;
	}
	int avail = c->x1 - c->x0;
	if (tot > avail)
		for (int k = 0; k < ncol; k++)
			cw[k] = VX_MAX(cw[k] * avail / tot, PX(30));
	tot = 0;
	for (int k = 0; k < ncol; k++)
		tot += cw[k];
	int top = c->y, x0 = c->x0, x1s = c->x1;
	for (u32 r = 0; r < nrow; r++) {
		int ry = c->y, rh = 0;
		u32 first = Y->n;
		if (r == 0)
			rect(Y, C_FG, 0x10, x0, ry, tot, 0); /* height fixed below */
		int cx = x0;
		for (int k = 0; k < ncol; k++) {
			c->y = ry + PX(3);
			c->x0 = cx + pad;
			c->x1 = cx + cw[k] - pad;
			u32 fi = Y->n;
			paragraph(c, cs[r][k], cn[r][k], r ? FONT_UI : FONT_UI_BOLD, C_EDITOR_FG);
			/* right / centre: shift a one-row cell */
			if (align[k] >= 2 && Y->n > fi) {
				int used = 0;
				bool one = true;
				for (u32 i = fi; i < Y->n; i++) {
					one &= Y->y[i] == Y->y[fi] || Y->kind[i] != IT_TEXT;
					if (Y->kind[i] == IT_TEXT)
						used = VX_MAX(used, Y->x[i] + font_width(Y->font[i], Y->pool.b + Y->str[i], Y->len[i]) - c->x0);
				}
				int free_w = (c->x1 - c->x0) - used;
				int sh = align[k] == 3 ? free_w / 2 : free_w;
				if (one && sh > 0)
					for (u32 i = fi; i < Y->n; i++)
						Y->x[i] += sh;
			}
			rh = VX_MAX(rh, c->y - ry + PX(3));
			cx += cw[k];
		}
		if (r == 0)
			Y->h[first] = rh;
		c->y = ry + rh;
		rect(Y, C_TABS_BORDER, 0, x0, c->y, tot, PX(1));
	}
	/* outline and column separators */
	rect(Y, C_TABS_BORDER, 0, x0, top, tot, PX(1));
	int cx = x0;
	for (int k = 0; k <= ncol; k++) {
		rect(Y, C_TABS_BORDER, 0, cx, top, PX(1), c->y - top + PX(1));
		if (k < ncol)
			cx += cw[k];
	}
	c->x0 = x0;
	c->x1 = x1s;
}

static void blocks(Ctx *c, Ln *L, u32 n, bool tight)
{
	Layout *Y = c->L;
	int gap = tight ? PX(2) : PX(10);
	Buf para = {0};
	u32 i = 0;
	while (i < n) {
		if (blank(&L[i])) {
			i++;
			continue;
		}
		anchor(c, L[i].src);
		char fc;
		int fk = fence_of(&L[i], &fc);
		const char *ht;
		u32 hn;
		int hl = heading_of(&L[i], &ht, &hn);
		u32 b;
		int ind = indent_of(&L[i], &b);
		int content;
		u32 num = 1;
		char bullet = 0;
		if (fk) {
			/* fenced code */
			const char *info = L[i].s + b + fk;
			u32 in = L[i].n - b - (u32)fk;
			while (in && *info == ' ')
				info++, in--;
			u32 w = 0;
			while (w < in && info[w] != ' ' && info[w] != '\r' && info[w] != '{')
				w++;
			int lang = syn_fence_lang(info, w);
			u32 j = i + 1;
			while (j < n) {
				char f2;
				int k2 = fence_of(&L[j], &f2);
				if (k2 >= fk && f2 == fc)
					break;
				j++;
			}
			Ln *body = vx_alloc((j - i + 1) * sizeof(Ln));
			for (u32 k = i + 1; k < j; k++)
				body[k - i - 1] = dedent(L[k], ind);
			code_block(c, body, j - i - 1, lang);
			free(body);
			i = j < n ? j + 1 : j;
		} else if (ind >= 4) {
			u32 j = i;
			while (j < n && (blank(&L[j]) || indent_of(&L[j], NULL) >= 4))
				j++;
			while (j > i && blank(&L[j - 1]))
				j--;
			Ln *body = vx_alloc((j - i + 1) * sizeof(Ln));
			for (u32 k = i; k < j; k++)
				body[k - i] = dedent(L[k], 4);
			code_block(c, body, j - i, LANG_NONE);
			free(body);
			i = j;
		} else if (hl) {
			int font = hl == 1 ? FONT_H1 : hl == 2 ? FONT_H2 : hl == 3 ? FONT_H3 : FONT_UI_BOLD;
			if (c->y > PX(4))
				c->y += PX(8);
			heading_slug(Y, ht, hn, c->y);
			paragraph(c, ht, hn, font, C_FG);
			if (hl <= 2) {
				c->y += PX(3);
				rect(Y, C_TABS_BORDER, 0, c->x0, c->y, c->x1 - c->x0, PX(1));
				c->y += PX(4);
			}
			i++;
		} else if (is_hr(&L[i])) {
			c->y += PX(6);
			rect(Y, C_TABS_BORDER, 0, c->x0, c->y, c->x1 - c->x0, PX(2));
			c->y += PX(8);
			i++;
		} else if (b < L[i].n && L[i].s[b] == '>') {
			/* block quote: strip one '>' level, lazy continuation lines join */
			u32 j = i;
			Ln *q = vx_alloc((n - i + 1) * sizeof(Ln));
			u32 qn = 0;
			bool text = false;
			while (j < n) {
				u32 bb;
				indent_of(&L[j], &bb);
				if (bb < L[j].n && L[j].s[bb] == '>' && indent_of(&L[j], NULL) <= 3) {
					Ln s = L[j];
					s.s += bb + 1;
					s.n -= bb + 1;
					if (s.n && s.s[0] == ' ')
						s.s++, s.n--;
					q[qn++] = s;
					text = !blank(&s);
				} else if (text && !blank(&L[j]) && !block_start(&L[j])) {
					q[qn++] = L[j];
				} else
					break;
				j++;
			}
			int top = c->y;
			int x0 = c->x0;
			c->x0 += PX(18);
			blocks(c, q, qn, false);
			c->x0 = x0;
			rect(Y, C_TREE_INDENT_GUIDE, 0, x0 + PX(2), top, PX(4), c->y - top - gap);
			free(q);
			i = j;
			continue; /* the inner blocks left their own gap */
		} else if (list_marker(&L[i], &content, &num, &bullet)) {
			/* a list: items of the same kind, each its own blocks */
			int kind = list_marker(&L[i], &content, &num, &bullet);
			char kb = bullet;
			u32 number = num;
			bool loose = false;
			int x0 = c->x0;
			int depth_bullet = (c->x0 / PX(26)) % 2;
			while (i < n) {
				int ct;
				u32 nn;
				char bl;
				int k2 = list_marker(&L[i], &ct, &nn, &bl);
				if (k2 != kind || bl != kb)
					break;
				Ln *it = vx_alloc((n - i + 1) * sizeof(Ln));
				u32 itn = 0;
				it[itn++] = after_marker(L[i]);
				u32 j = i + 1;
				bool prev_blank = false;
				while (j < n) {
					if (blank(&L[j])) {
						it[itn++] = L[j];
						prev_blank = true;
						j++;
						continue;
					}
					if (indent_of(&L[j], NULL) >= ct) {
						it[itn++] = dedent(L[j], ct);
					} else if (!prev_blank && !block_start(&L[j])) {
						it[itn++] = dedent(L[j], 99); /* lazy continuation */
					} else
						break;
					prev_blank = false;
					j++;
				}
				u32 used = itn;
				while (used && blank(&it[used - 1]))
					used--;
				/* a blank line between items, or inside one, makes the list loose */
				for (u32 k = 0; k < used; k++)
					loose |= blank(&it[k]) && k > 0;
				if (used < itn && j < n)
					loose = true;
				/* the marker */
				int mx = x0, my = c->y + PX(2);
				int text_x = x0 + PX(26);
				char mk[24];
				bool task = false, done = false;
				if (used && it[0].n >= 3 && it[0].s[0] == '[' && it[0].s[2] == ']' &&
				    (it[0].s[1] == ' ' || it[0].s[1] == 'x' || it[0].s[1] == 'X') && (it[0].n == 3 || it[0].s[3] == ' ')) {
					task = true;
					done = it[0].s[1] != ' ';
					it[0].s += VX_MIN(it[0].n, 4u);
					it[0].n -= VX_MIN(it[0].n, 4u);
				}
				int asc = font_m[FONT_UI].ascent;
				if (task) {
					int bs = PX(12);
					int by = my + asc - bs + PX(1);
					rect(Y, C_DESCRIPTION_FG, 0, mx + PX(6), by, bs, PX(1));
					rect(Y, C_DESCRIPTION_FG, 0, mx + PX(6), by + bs - PX(1), bs, PX(1));
					rect(Y, C_DESCRIPTION_FG, 0, mx + PX(6), by, PX(1), bs);
					rect(Y, C_DESCRIPTION_FG, 0, mx + PX(6) + bs - PX(1), by, PX(1), bs);
					if (done)
						rect(Y, C_LINK, 0, mx + PX(9), by + PX(3), bs - PX(6), bs - PX(6));
				} else {
					if (kind == 1)
						snprintf(mk, sizeof mk, "%s", depth_bullet ? "\xE2\x97\xA6" : "\xE2\x80\xA2");
					else
						snprintf(mk, sizeof mk, "%u%c", number, kb);
					int mw = font_width(FONT_UI, mk, strlen(mk));
					u32 t = item(Y, IT_TEXT, FONT_UI, C_DESCRIPTION_FG, 0, mx + PX(20) - mw, my, 0,
					             font_m[FONT_UI].line);
					Y->str[t] = pool_add(&Y->pool, mk, (u32)strlen(mk));
					Y->len[t] = (u32)strlen(mk);
				}
				c->x0 = text_x;
				int before = c->y;
				blocks(c, it, used, !loose);
				if (c->y == before) /* an empty item still takes a row */
					c->y += font_m[FONT_UI].line + PX(5);
				c->x0 = x0;
				free(it);
				number++;
				i = j;
			}
			c->y += tight ? 0 : gap;
			continue;
		} else if (i + 1 < n && memchr(L[i].s, '|', L[i].n) && table_delim(&L[i + 1])) {
			u32 j = i + 2;
			while (j < n && !blank(&L[j]) && memchr(L[j].s, '|', L[j].n))
				j++;
			table(c, L + i, j - i);
			i = j;
		} else {
			/* paragraph (setext heading when underlined) */
			para.n = 0;
			u32 j = i;
			int setext = 0;
			while (j < n && !blank(&L[j])) {
				if (j > i) {
					u32 bb;
					indent_of(&L[j], &bb);
					char ch = bb < L[j].n ? L[j].s[bb] : 0;
					if (ch == '=' || ch == '-') {
						bool all = true;
						for (u32 k = bb; k < L[j].n; k++)
							all &= L[j].s[k] == ch || L[j].s[k] == ' ' || L[j].s[k] == '\r';
						if (all) {
							setext = ch == '=' ? 1 : 2;
							break;
						}
					}
					if (block_start(&L[j]) ||
					    (j + 1 < n && memchr(L[j].s, '|', L[j].n) && table_delim(&L[j + 1])))
						break;
				}
				Ln l = dedent(L[j], 99);
				while (l.n && l.s[l.n - 1] == '\r')
					l.n--;
				bool hard = l.n >= 2 && l.s[l.n - 1] == ' ' && l.s[l.n - 2] == ' ';
				bool bs = l.n && l.s[l.n - 1] == '\\';
				while (l.n && l.s[l.n - 1] == ' ')
					l.n--;
				if (bs)
					l.n--;
				if (para.n)
					bput(&para, " ", 1);
				bput(&para, l.s, l.n);
				if ((hard || bs) && j + 1 < n && !blank(&L[j + 1]))
					bput(&para, "\n", 1);
				j++;
			}
			if (setext) {
				if (c->y > PX(4))
					c->y += PX(8);
				heading_slug(Y, para.b, para.n, c->y);
				paragraph(c, para.b, para.n, setext == 1 ? FONT_H1 : FONT_H2, C_FG);
				c->y += PX(3);
				rect(Y, C_TABS_BORDER, 0, c->x0, c->y, c->x1 - c->x0, PX(1));
				c->y += PX(4);
				j++;
			} else
				paragraph(c, para.b ? para.b : "", para.n, FONT_UI, C_EDITOR_FG);
			i = j;
		}
		c->y += gap;
	}
	free(para.b);
}

/* ---- layout cache ---------------------------------------------------------------------- */

static int content_w(int w) { return VX_MIN(w - PX(64), PX(880)); }

static Layout *layout_for(u32 d, int w)
{
	Text *t = &app.docs.text[d];
	int width = content_w(w);
	for (int i = 0; i < 4; i++)
		if (lays[i].used && lays[i].doc == d && lays[i].ver == t->version && lays[i].width == width &&
		    lays[i].scale == app.scale)
			return &lays[i];
	Layout *L = &lays[lay_next++ % 4];
	lay_free(L);
	L->used = true;
	L->doc = d;
	L->ver = t->version;
	L->width = width;
	L->scale = app.scale;
	pool_add(&L->pool, "", 0);
	/* the document's lines */
	const char *s = text_flat(t);
	u32 nl = text_lines(t);
	Ln *ln = vx_alloc((nl + 1) * sizeof(Ln));
	for (u32 l = 0; l < nl; l++) {
		u32 a = text_line_start(t, l);
		ln[l] = (Ln){s + a, text_line_len(t, l), l};
	}
	Ctx c = {L, 0, 0, width, 0, 0, 0, 0, true, 0};
	row_begin(&c);
	blocks(&c, ln, nl, false);
	L->height = c.y;
	free(ln);
	return L;
}

void mdview_doc_changed(u32 d)
{
	for (int i = 0; i < 4; i++)
		if (lays[i].used && lays[i].doc == d)
			lay_free(&lays[i]);
}

/* ---- views ---------------------------------------------------------------------------- */

static struct {
	View *v;
	i64 scroll;
} pv[32];

static i64 *scroll_of(View *v)
{
	for (int i = 0; i < 32; i++)
		if (pv[i].v == v)
			return &pv[i].scroll;
	for (int i = 0; i < 32; i++)
		if (!pv[i].v) {
			pv[i].v = v;
			pv[i].scroll = 0;
			return &pv[i].scroll;
		}
	pv[0].v = v;
	pv[0].scroll = 0;
	return &pv[0].scroll;
}

void mdview_closed(View *v)
{
	for (int i = 0; i < 32; i++)
		if (pv[i].v == v)
			pv[i].v = NULL;
}

static bool md_doc(View *v)
{
	return v && !app.docs.bin[v->doc] && v->diff_left == NONE && app.docs.lang[v->doc] == LANG_MARKDOWN &&
	       !app.docs.man[v->doc];
}

bool mdview_preview(View *v) { return md_doc(v) && v->nparts == 2 && v->part == 1; }

/* Markdown views get two parts: Edit and Preview. */
void mdview_sync(View *v)
{
	if (!v || app.docs.bin[v->doc] || v->diff_left != NONE)
		return;
	u32 want = md_doc(v) ? 2 : 1;
	if (v->nparts != want) {
		v->nparts = want;
		if (v->part >= want)
			v->part = 0;
	}
}

static int last_w = 800, last_h = 600;

/* switch parts, keeping the same place in view */
static void set_part(View *v, u32 part)
{
	if (part == v->part || !md_doc(v))
		return;
	Layout *L = layout_for(v->doc, last_w);
	i64 *sc = scroll_of(v);
	if (part == 1) {
		/* the block holding the editor's top line */
		u32 top = (u32)(v->scroll_y / app.line_h);
		i32 y = 0;
		for (u32 i = 0; i < L->na && L->a_line[i] <= top; i++)
			y = L->a_y[i];
		*sc = y;
		app.find_open = false;
	} else {
		u32 line = 0;
		for (u32 i = 0; i < L->na && L->a_y[i] <= *sc + PX(4); i++)
			line = L->a_line[i];
		v->scroll_y = (i64)line * app.line_h;
	}
	v->part = part;
	app.dirty = true;
}

int mdview_subtabs_height(View *v, int w)
{
	(void)v;
	(void)w;
	return app.subtabs_h;
}

static const char *const part_names[2] = {"Edit", "Preview"};

static int sub_w(int i) { return font_width(FONT_SMALL, part_names[i], strlen(part_names[i])) + PX(20); }

void mdview_subtabs_draw(View *v, int x, int y, int w)
{
	int hh = app.subtabs_h;
	draw_rect(x, y, w, hh, COL(C_EDITOR_BG));
	draw_rect(x, y + hh - PX(1), w, PX(1), COL(C_TABS_BORDER));
	int tx = x;
	for (int i = 0; i < 2; i++) {
		int tw = sub_w(i);
		bool active = (u32)i == v->part;
		if (active) {
			draw_rect(tx, y, tw, hh, COL(C_TAB_ACTIVE_BG));
			draw_rect(tx, y + hh - PX(2), tw, PX(2), COL(C_TAB_ACTIVE_BORDER_TOP));
		}
		draw_text(FONT_SMALL, tx + PX(10), y + (hh - font_m[FONT_SMALL].line) / 2, part_names[i],
		          strlen(part_names[i]), active ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG));
		ui_tip(tx, y, tw, hh, "Markdown: Toggle Preview (Ctrl+Shift+V)");
		tx += tw;
	}
}

void mdview_subtab_click(View *v, int rx, int ry, int w)
{
	(void)ry;
	(void)w;
	if (rx < sub_w(0))
		set_part(v, 0);
	else if (rx < sub_w(0) + sub_w(1))
		set_part(v, 1);
}

static u32 col_of(u16 c, u8 alpha)
{
	u32 rgba = (c & TOKCOL) ? TOK(c & 0xFF) : COL(c);
	return alpha ? (rgba & 0xFFFFFF00u) | alpha : rgba;
}

static int ox_of(int x, int w) { return x + VX_MAX((w - content_w(w)) / 2, PX(24)); }

#define TOP_PAD PX(18)

/* the link item under the pointer */
static u32 link_at(Layout *L, i64 scroll, int x, int y, int w, int mx, int my)
{
	int ox = ox_of(x, w);
	for (u32 i = 0; i < L->n; i++) {
		if (L->kind[i] != IT_TEXT || !L->link[i])
			continue;
		int iy = y + TOP_PAD + L->y[i] - (int)scroll, ix = ox + L->x[i];
		int iw = font_width(L->font[i], L->pool.b + L->str[i], L->len[i]);
		if (mx >= ix && mx < ix + iw && my >= iy && my < iy + L->h[i])
			return i;
	}
	return NONE;
}

void mdview_draw(View *v, int x, int y, int w, int h)
{
	last_w = w;
	last_h = h;
	Layout *L = layout_for(v->doc, w);
	i64 *sc = scroll_of(v);
	i64 maxs = VX_MAX((i64)L->height + TOP_PAD * 2 - h, 0);
	*sc = VX_CLAMP(*sc, 0, maxs);
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	draw_clip(x, y, w, h);
	int ox = ox_of(x, w);
	int oy = y + TOP_PAD - (int)*sc;
	bool in = app.hover_x >= x && app.hover_x < x + w && app.hover_y >= y && app.hover_y < y + h;
	u32 hover = in ? link_at(L, *sc, x, y, w, app.hover_x, app.hover_y)
	                                                               : NONE;
	for (u32 i = 0; i < L->n; i++) {
		int iy = oy + L->y[i], ih = L->kind[i] == IT_TEXT ? L->h[i] : L->h[i];
		if (iy > y + h || iy + ih < y)
			continue;
		u32 c = col_of(L->col[i], L->alpha[i]);
		if (L->kind[i] == IT_RECT) {
			draw_rect(ox + L->x[i], iy, L->w[i], L->h[i], c);
			continue;
		}
		const char *s = L->pool.b + L->str[i];
		int ex;
		if (L->w[i] > 0)
			ex = draw_text_fit(L->font[i], ox + L->x[i], iy, s, L->len[i], L->w[i], c);
		else
			ex = draw_text(L->font[i], ox + L->x[i], iy, s, L->len[i], c);
		if (hover != NONE && L->link[i] && L->link[i] == L->link[hover])
			draw_rect(ox + L->x[i], iy + font_m[L->font[i]].ascent + PX(2), ex - ox - L->x[i], PX(1), c);
	}
	draw_noclip();
	if (hover != NONE) {
		const char *url = L->pool.b + L->link[hover] - 1;
		int lx = ox + L->x[hover], ly = oy + L->y[hover], lw = font_width(L->font[hover], L->pool.b + L->str[hover], L->len[hover]);
		ui_tip(lx, ly, lw, L->h[hover], url);
	}
	/* a thin scroll position marker */
	if (maxs > 0) {
		int th = VX_MAX((int)((i64)h * h / (L->height + TOP_PAD * 2)), PX(20));
		int ty = y + (int)((i64)(h - th) * *sc / maxs);
		draw_rect(x + w - PX(6), ty, PX(4), th, COL(C_SCROLL_SLIDER));
	}
}

/* ---- input ------------------------------------------------------------------------------ */

static void open_link(View *v, const char *url)
{
	if (url[0] == '#') {
		Layout *L = layout_for(v->doc, last_w);
		for (u32 i = 0; i < L->nh; i++)
			if (!strcmp(L->pool.b + L->h_slug[i], url + 1)) {
				*scroll_of(v) = L->h_y[i] - PX(8);
				return;
			}
		ui_status("No heading #%s", url + 1);
		return;
	}
	if (strstr(url, "://") || !strncmp(url, "mailto:", 7)) {
		if (!app.headless && !SDL_OpenURL(url))
			ui_status("Cannot open %s", url);
		return;
	}
	/* a file next to the document (a #fragment is ignored) */
	const char *dp = app.docs.path[v->doc];
	char path[4096], rel[2048];
	snprintf(rel, sizeof rel, "%s", url);
	char *hash = strchr(rel, '#');
	if (hash)
		*hash = 0;
	if (rel[0] == '/')
		snprintf(path, sizeof path, "%s%s", app.root ? app.root : "", rel);
	else if (dp) {
		const char *sl = strrchr(dp, '/');
		snprintf(path, sizeof path, "%.*s/%s", sl ? (int)(sl - dp) : 1, sl ? dp : ".", rel);
	} else
		snprintf(path, sizeof path, "%s/%s", app.root ? app.root : ".", rel);
	struct stat st;
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		open_file(path, false, UINT32_MAX, 0);
	else
		ui_status("Not found: %s", rel);
}

bool mdview_key(View *v, const Event *e)
{
	i64 *sc = scroll_of(v);
	int lh = app.line_h;
	bool ctrl = e->mods & MOD_CTRL;
	app.dirty = true;
	switch (e->key) {
	case SDLK_UP: *sc -= lh * 2; return true;
	case SDLK_DOWN: *sc += lh * 2; return true;
	case SDLK_PAGEUP: *sc -= last_h - lh * 2; return true;
	case SDLK_PAGEDOWN:
	case SDLK_SPACE: *sc += (e->mods & MOD_SHIFT ? -1 : 1) * (last_h - lh * 2); return true;
	case SDLK_HOME: *sc = 0; return true;
	case SDLK_END: *sc = INT32_MAX; return true;
	default: (void)ctrl; return false;
	}
}

void mdview_mouse(View *v, const Event *e, int x, int y, int w, int h)
{
	(void)h;
	i64 *sc = scroll_of(v);
	app.dirty = true;
	if (e->type == EV_WHEEL) {
		*sc -= (i64)(e->wy * 3 * app.line_h);
		return;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return;
	Layout *L = layout_for(v->doc, w);
	u32 k = link_at(L, *sc, x, y, w, e->x, e->y);
	if (k != NONE) {
		open_link(v, L->pool.b + L->link[k] - 1);
		return;
	}
	if (e->clicks == 2) {
		/* edit here: the source line of the block under the pointer */
		i64 cy = e->y - y - TOP_PAD + *sc;
		u32 line = 0;
		for (u32 i = 0; i < L->na && L->a_y[i] <= cy; i++)
			line = L->a_line[i];
		v->part = 0;
		Text *t = &app.docs.text[v->doc];
		u32 off = text_line_start(t, VX_MIN(line, text_lines(t) - 1));
		ed_set_cursor(v, off, false);
		ed_reveal(v, off, true);
	}
}

int mdview_cursor(View *v, int mx, int my, int x, int y, int w)
{
	Layout *L = layout_for(v->doc, w);
	return link_at(L, *scroll_of(v), x, y, w, mx, my) != NONE ? CURSOR_HAND : CURSOR_ARROW;
}

/* Ctrl+Shift+V (true: handled). While the preview shows, find and go to
 * line switch to the source; the editor ignores everything else there. */
bool mdview_command(View *v, int cmd)
{
	if (cmd == CMD_MD_PREVIEW) {
		mdview_sync(v);
		if (!md_doc(v)) {
			ui_status("Not a Markdown file");
			return true;
		}
		set_part(v, v->part == 1 ? 0 : 1);
		return true;
	}
	if (!mdview_preview(v))
		return false;
	if (cmd == CMD_FIND || cmd == CMD_REPLACE || cmd == CMD_GOTO_LINE)
		set_part(v, 0);
	return false;
}
