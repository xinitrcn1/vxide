/* Vim emulation (off by default), after VSCodium's Vim extension
 * (VSCodeVim): normal, insert, visual, visual line, replace,
 * operator-pending and command-line modes on top of the editor's own
 * primitives (piece table, cursors, undo groups, clipboard, regex).
 *
 * Keys become tokens: text events give code points, key events give the
 * K_* specials and CTRL(c). Normal and visual mode collect tokens in
 * S.pend and parse them on every token as
 *     ["x][count] (operator [count] (motion | object | same op) | motion | action)
 * An incomplete parse waits for the next token, an invalid one is dropped.
 * Insert mode leaves typing to the editor (auto-closing, indentation,
 * completion) and only records the tokens, so that `.` and counted
 * inserts can feed them again. A whole insert session (with the change
 * that started it) is one undo step: its undo log entries are put in one
 * group when it ends.
 *
 * Registers: an internal unnamed register (VSCodeVim's
 * useSystemClipboard=false), a-z (A-Z append), 0 (last yank), 1-9
 * (deletes of lines), - (small deletes), _ and "+ / "* for the system
 * clipboard. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "regex.h"

#include <ctype.h>
#include <stdio.h>
#include <wctype.h>
#include <SDL3/SDL.h>

enum { M_NORMAL, M_INSERT, M_VISUAL, M_VLINE, M_REPLACE, M_CMDLINE };
enum { MT_EXCL, MT_INCL, MT_LINE };
enum { C_BL, C_PUNCT, C_WORD, C_NL };

#define K_SPECIAL 0x200000u
enum {
	K_ESC = K_SPECIAL + 1,
	K_RET,
	K_BS,
	K_DEL,
	K_TAB,
	K_LEFT,
	K_RIGHT,
	K_UP,
	K_DOWN,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
};
#define CTRL(c) (0x300000u | (u32)(c))
#define GP(c) (0x400000u | (u32)(c)) /* g-prefixed key */
#define NONE UINT32_MAX
#define STICKY_END (INT32_MAX / 4) /* want_x after $ */

typedef struct {
	u32 reg, count, op, mot, marg, obj, objc, act, aarg;
	bool lineop; /* dd cc yy >> << guu ... */
} Cmd;

typedef struct {
	char *s;
	u32 n;
	bool lw;
} Reg;

enum { R_UNNAMED, R_A = 1, R_0 = 27, R_MINUS = 37, R_COUNT };

typedef struct {
	u32 *b;
	u32 n, cap;
} Toks;

typedef struct {
	u32 to;
	int type;
	bool jump;
	i32 want; /* -1: reset the wanted column, else the new one */
} Mot;

static struct {
	int mode;
	View *view;
	u32 pend[64];
	int npend;
	Cmd cur; /* command being executed */
	/* visual: start and cursor (inclusive char offsets) */
	u32 vstart, vcur, set_anchor, set_head;
	u32 lv_doc, lv_start, lv_cur; /* last visual selection (gv, '<, '>) */
	int lv_mode;
	/* command line */
	char cl[512];
	u32 cln;
	char clk;
	int cl_prev;
	Cmd cl_op; /* d/foo: operator waiting for the search */
	bool cl_has_op;
	/* insert / replace session */
	u32 ins_count, ins_undo0;
	Cmd ins_cmd;
	Toks rec;
	u32 *rstack; /* replace mode: replaced code points, NONE = appended */
	u32 rs_n, rs_cap;
	/* `.` */
	Cmd dot;
	bool dot_ok;
	Toks dot_ins;
	bool feeding, dotting, in_vim;
	/* f F t T */
	u32 ft_key, ft_ch;
	/* search */
	char pat[512];
	bool pat_ok, back, pat_icase;
	Regex *re;
	char re_src[512];
	int re_flags;
	/* jumps */
	u32 j_doc[100], j_off[100];
	u32 jn, jpos;
	u32 prev_doc, prev_off;
	bool prev_ok;
	char label[600];
} S;

static Reg regs[R_COUNT];
static u32 mark_line[MAX_DOCS][26], mark_col[MAX_DOCS][26]; /* line + 1; 0 = unset */

static void exec(View *v, Cmd *c);
static void feed(View *v, u32 tok);
static void feed_ins(View *v, u32 tok);

/* ---- text helpers ------------------------------------------------------- */

static Text *TX(View *v) { return &app.docs.text[v->doc]; }
static u32 lnof(Text *t, u32 o) { return text_line_of(t, VX_MIN(o, t->len)); }
static u32 lst(Text *t, u32 l) { return text_line_start(t, l); }
static u32 lend(Text *t, u32 l) { return text_line_start(t, l) + text_line_len(t, l); }
static u32 lnext(Text *t, u32 l) { return l + 1 < text_lines(t) ? text_line_start(t, l + 1) : t->len; }
static u8 byte_at(Text *t, u32 o) { return o < t->len ? text_byte(t, o) : 0; }
static bool blank_byte(u8 c) { return c == ' ' || c == '\t'; }
static bool is_kw(u8 c) { return c >= 0x80 || isalnum(c) || c == '_'; }

static u32 nxt(Text *t, u32 o)
{
	if (o >= t->len)
		return t->len;
	if (text_byte(t, o) == '\r' && byte_at(t, o + 1) == '\n')
		return o + 2;
	o++;
	while (o < t->len && utf8_cont(text_byte(t, o)))
		o++;
	return o;
}

static u32 prv(Text *t, u32 o)
{
	if (o == 0)
		return 0;
	o--;
	while (o > 0 && utf8_cont(text_byte(t, o)))
		o--;
	if (o > 0 && text_byte(t, o) == '\n' && text_byte(t, o - 1) == '\r')
		o--;
	return o;
}

static u32 first_nb(Text *t, u32 l)
{
	u32 o = lst(t, l), e = lend(t, l);
	while (o < e && blank_byte(text_byte(t, o)))
		o++;
	return o;
}

/* Normal mode keeps the cursor on a character: never past the last one. */
static u32 clampn(Text *t, u32 o)
{
	o = VX_MIN(o, t->len);
	u32 l = lnof(t, o), s = lst(t, l), e = lend(t, l);
	if (o >= e)
		return e > s ? prv(t, e) : s;
	return o;
}

static bool line_blank(Text *t, u32 l) { return first_nb(t, l) == lend(t, l); }
static bool empty_at(Text *t, u32 o)
{
	u32 l = lnof(t, o);
	return o == lst(t, l) && text_line_len(t, l) == 0;
}

static int cls(Text *t, u32 o, bool big)
{
	if (o >= t->len)
		return C_NL;
	u8 c = text_byte(t, o);
	if (c == '\n' || c == '\r')
		return C_NL;
	if (blank_byte(c))
		return C_BL;
	if (big || is_kw(c))
		return C_WORD;
	return C_PUNCT;
}

static u32 cp_at(Text *t, u32 o, u32 *len)
{
	char b[4];
	u32 n = VX_MIN(4u, t->len - o);
	text_copy(t, o, n, b);
	u32 cp;
	*len = utf8_decode(b, n, &cp);
	return cp;
}

static void set_cur(View *v, u32 o)
{
	v->nc = 1;
	v->head[0] = v->anchor[0] = VX_MIN(o, TX(v)->len);
	v->want_x[0] = -1;
}

static void collapse(View *v)
{
	v->nc = 1;
	v->anchor[0] = v->head[0];
}

static void edit(View *v, u32 pos, u32 del, const char *s, u32 n, u32 g) { doc_replace(v->doc, pos, del, s, n, g); }

static void toks_push(Toks *k, u32 t)
{
	VX_GROW(k->b, k->cap, k->n + 1);
	k->b[k->n++] = t;
}

static void fail(void) { app.dirty = true; }

/* Put the undo entries of [from, upos) in one group: one `u` undoes them. */
static void merge_undo(Text *t, u32 from)
{
	if (from + 1 >= t->upos)
		return;
	u32 g = ed_new_group();
	for (u32 i = from; i < t->upos; i++)
		t->u_group[i] = g;
}

/* ---- registers -------------------------------------------------------- */

static int reg_idx(u32 r)
{
	if (r == 0 || r == '"')
		return R_UNNAMED;
	if (r >= 'a' && r <= 'z')
		return R_A + (int)(r - 'a');
	if (r >= 'A' && r <= 'Z')
		return R_A + (int)(r - 'A');
	if (r >= '0' && r <= '9')
		return R_0 + (int)(r - '0');
	if (r == '-')
		return R_MINUS;
	return -1;
}

static void reg_store(int i, const char *s, u32 n, bool lw, bool append)
{
	Reg *r = &regs[i];
	if (append && r->s) {
		bool nl = r->lw && !lw; /* charwise appended to lines: own line */
		r->s = vx_realloc(r->s, r->n + n + 2);
		memcpy(r->s + r->n, s, n);
		r->n += n;
		if (nl)
			r->s[r->n++] = '\n';
		r->lw = r->lw || lw;
	} else {
		char *b = vx_alloc(n + 1);
		memcpy(b, s, n);
		free(r->s);
		r->s = b;
		r->n = n;
		r->lw = lw;
	}
	r->s[r->n] = 0;
}

static void reg_set(u32 name, const char *s, u32 n, bool lw, bool yank)
{
	if (name == '_')
		return;
	if (name == '+' || name == '*') {
		ui_clipboard_set(s, n);
		app.clip_whole_line = lw;
		reg_store(R_UNNAMED, s, n, lw, false);
		return;
	}
	int i = reg_idx(name);
	if (i < 0) {
		ui_status("E354: Invalid register name: '%c'", (int)name);
		return;
	}
	if (i != R_UNNAMED && i < R_0) {
		reg_store(i, s, n, lw, name >= 'A' && name <= 'Z');
		reg_store(R_UNNAMED, regs[i].s, regs[i].n, regs[i].lw, false);
		return;
	}
	reg_store(R_UNNAMED, s, n, lw, false);
	if (yank) {
		reg_store(R_0, s, n, lw, false);
	} else if (lw || memchr(s, '\n', n)) {
		free(regs[R_0 + 9].s);
		memmove(&regs[R_0 + 2], &regs[R_0 + 1], 8 * sizeof(Reg));
		regs[R_0 + 1] = (Reg){0};
		reg_store(R_0 + 1, s, n, lw, false);
	} else {
		reg_store(R_MINUS, s, n, lw, false);
	}
}

/* A malloc'd copy of a register, NULL when empty. */
static char *reg_get(u32 name, u32 *n, bool *lw)
{
	if (name == '+' || name == '*') {
		char *s = ui_clipboard_get();
		*n = (u32)strlen(s);
		*lw = app.clip_whole_line || (*n && s[*n - 1] == '\n');
		return s;
	}
	int i = reg_idx(name);
	if (i < 0 || !regs[i].s)
		return NULL;
	*n = regs[i].n;
	*lw = regs[i].lw;
	return vx_strndup(regs[i].s, regs[i].n);
}

static void yank_range(Text *t, u32 a, u32 b, bool lw, u32 reg, bool yank)
{
	const char *eol = text_eol(t);
	u32 el = (u32)strlen(eol);
	char *s = vx_alloc(b - a + el + 1);
	text_copy(t, a, b - a, s);
	u32 n = b - a;
	if (lw && (n == 0 || s[n - 1] != '\n')) {
		memcpy(s + n, eol, el);
		n += el;
	}
	reg_set(reg, s, n, lw, yank);
	free(s);
}

/* ---- marks and jumps ------------------------------------------------- */

static void jump_push(View *v, u32 off)
{
	S.prev_doc = v->doc;
	S.prev_off = off;
	S.prev_ok = true;
	if (S.jpos < S.jn)
		S.jn = S.jpos;
	if (S.jn == VX_COUNT(S.j_doc)) {
		memmove(S.j_doc, S.j_doc + 1, (S.jn - 1) * sizeof(u32));
		memmove(S.j_off, S.j_off + 1, (S.jn - 1) * sizeof(u32));
		S.jn--;
	}
	S.j_doc[S.jn] = v->doc;
	S.j_off[S.jn] = off;
	S.jn++;
	S.jpos = S.jn;
}

static void jump_go(View *v, int dir, u32 n)
{
	if (dir < 0) {
		if (S.jpos == S.jn) {
			jump_push(v, v->head[0]);
			S.jpos = S.jn - 1;
		}
		if (S.jpos < n) {
			fail();
			return;
		}
		S.jpos -= n;
	} else {
		if (S.jpos + n >= S.jn) {
			fail();
			return;
		}
		S.jpos += n;
	}
	u32 d = S.j_doc[S.jpos], off = S.j_off[S.jpos];
	if (d == v->doc) {
		set_cur(v, clampn(TX(v), off));
		ed_reveal(v, v->head[0], true);
	} else if (d < app.docs.n && app.docs.path[d]) {
		Text *t = &app.docs.text[d];
		off = VX_MIN(off, t->len);
		u32 l = text_line_of(t, off);
		S.view = NULL;
		open_file(app.docs.path[d], false, l, off - text_line_start(t, l));
	}
}

static bool mark_get(View *v, u32 ch, u32 *off)
{
	Text *t = TX(v);
	if (ch >= 'a' && ch <= 'z') {
		u32 l = mark_line[v->doc][ch - 'a'];
		if (!l)
			return false;
		*off = ed_line_col_to_off(t, l - 1, mark_col[v->doc][ch - 'a']);
		return true;
	}
	if (ch == '\'' || ch == '`') {
		if (!S.prev_ok || S.prev_doc != v->doc)
			return false;
		*off = VX_MIN(S.prev_off, t->len);
		return true;
	}
	if ((ch == '<' || ch == '>') && S.lv_mode && S.lv_doc == v->doc) {
		u32 lo = VX_MIN(VX_MIN(S.lv_start, S.lv_cur), t->len);
		u32 hi = VX_MIN(VX_MAX(S.lv_start, S.lv_cur), t->len);
		if (S.lv_mode == M_VLINE)
			*off = ch == '<' ? lst(t, lnof(t, lo)) : clampn(t, lend(t, lnof(t, hi)));
		else
			*off = ch == '<' ? lo : hi;
		return true;
	}
	return false;
}

/* ---- search ------------------------------------------------------------ */

static bool has_upper(const char *p)
{
	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (isupper((u8)*p))
			return true;
	}
	return false;
}

/* VSCodeVim defaults: ignorecase + smartcase (* and # ignore case). */
static bool compile_pat(void)
{
	int flags = (S.pat_icase || !has_upper(S.pat)) ? RE_ICASE : 0;
	if (S.re && S.re_flags == flags && !strcmp(S.re_src, S.pat))
		return true;
	if (S.re)
		re_free(S.re);
	char err[160];
	S.re = re_compile(S.pat, strlen(S.pat), flags, err, sizeof err);
	if (!S.re) {
		S.re_src[0] = 0;
		ui_status("E383: Invalid search string: %s (%s)", S.pat, err);
		return false;
	}
	snprintf(S.re_src, sizeof S.re_src, "%s", S.pat);
	S.re_flags = flags;
	return true;
}

static void set_pat(const char *p, bool icase)
{
	snprintf(S.pat, sizeof S.pat, "%s", p);
	S.pat_ok = true;
	S.pat_icase = icase;
}

/* Last match in line l starting before limit. */
static bool last_in_line(Text *t, const char *F, u32 l, u32 limit, u32 *out)
{
	u32 s = lst(t, l), e = lend(t, l), caps[2 * RE_MAX_CAPS];
	bool f = false;
	u32 pos = s;
	while (pos <= e && re_search(S.re, F, e, pos, caps)) {
		if (caps[0] >= limit || caps[0] > e)
			break;
		*out = caps[0];
		f = true;
		if (caps[0] >= e)
			break;
		pos = nxt(t, caps[0]);
	}
	return f;
}

static bool find_one(Text *t, u32 p, bool back, u32 *out)
{
	const char *F = text_flat(t);
	u32 len = t->len, caps[2 * RE_MAX_CAPS];
	if (!back) {
		u32 s = nxt(t, p);
		if (s <= len && re_search(S.re, F, len, s, caps)) {
			*out = caps[0];
			return true;
		}
		if (re_search(S.re, F, len, 0, caps) && caps[0] <= p) {
			ui_status("search hit BOTTOM, continuing at TOP");
			*out = caps[0];
			return true;
		}
		return false;
	}
	u32 nl = text_lines(t), l = lnof(t, p);
	for (u32 i = 0; i <= nl; i++) {
		u32 ll = (l + nl - i % nl) % nl;
		u32 m;
		if (last_in_line(t, F, ll, i == 0 ? p : NONE, &m)) {
			if (i > 0 && ll >= l)
				ui_status("search hit TOP, continuing at BOTTOM");
			*out = m;
			return true;
		}
	}
	return false;
}

static bool search_from(View *v, u32 p, bool back, u32 n, u32 *out)
{
	if (!S.pat_ok || !S.pat[0]) {
		ui_status("E35: No previous regular expression");
		return false;
	}
	if (!compile_pat())
		return false;
	Text *t = TX(v);
	for (u32 i = 0; i < n; i++)
		if (!find_one(t, p, back, &p)) {
			ui_status("E486: Pattern not found: %s", S.pat);
			return false;
		}
	*out = p;
	return true;
}

static void re_escape(char *dst, size_t cap, const char *s, u32 n)
{
	size_t k = 0;
	for (u32 i = 0; i < n && k + 3 < cap; i++) {
		if (strchr("\\^$.|?*+()[]{}", s[i]) && s[i])
			dst[k++] = '\\';
		dst[k++] = s[i];
	}
	dst[k] = 0;
}

/* * and #: the keyword under or after the cursor, as a whole word. */
static bool star_setup(View *v, u32 p, bool back, u32 *start)
{
	Text *t = TX(v);
	u32 l = lnof(t, p), s0 = lst(t, l), e = lend(t, l), q = p;
	while (q < e && !is_kw(text_byte(t, q)))
		q++;
	if (q >= e) {
		ui_status("E348: No string under cursor");
		return false;
	}
	u32 s = q, we = q;
	while (s > s0 && is_kw(text_byte(t, s - 1)))
		s--;
	while (we < e && is_kw(text_byte(t, we)))
		we++;
	char w[200], esc[450], pat[480];
	u32 n = VX_MIN(we - s, (u32)sizeof w - 1);
	text_copy(t, s, n, w);
	bool ascii = true;
	for (u32 i = 0; i < n; i++)
		if ((u8)w[i] >= 0x80)
			ascii = false;
	re_escape(esc, sizeof esc, w, n);
	snprintf(pat, sizeof pat, ascii ? "\\b%s\\b" : "%s", esc);
	set_pat(pat, true);
	S.back = back;
	*start = s;
	return true;
}

/* ---- motions ------------------------------------------------------------- */

static u32 m_w(Text *t, u32 p, bool big, bool stop_eol)
{
	u32 len = t->len;
	if (p >= len)
		return len;
	int c = cls(t, p, big);
	if (c == C_WORD || c == C_PUNCT)
		while (p < len && cls(t, p, big) == c)
			p = nxt(t, p);
	while (p < len) {
		int k = cls(t, p, big);
		if (k == C_BL) {
			p = nxt(t, p);
			continue;
		}
		if (k == C_NL) {
			if (stop_eol)
				return p;
			p = nxt(t, p);
			if (p < len && empty_at(t, p))
				return p;
			continue;
		}
		break;
	}
	return p;
}

/* cw on a word: like ce, but a cursor on a word's last char changes just it. */
static u32 m_cw(Text *t, u32 p, bool big, u32 n)
{
	u32 len = t->len;
	for (u32 i = 0; i < n && p < len; i++) {
		if (i > 0)
			while (p < len && (cls(t, p, big) == C_BL || (cls(t, p, big) == C_NL && i + 1 < n)))
				p = nxt(t, p);
		int c = cls(t, p, big);
		if (c == C_NL)
			break;
		while (p < len && cls(t, p, big) == c)
			p = nxt(t, p);
	}
	return p;
}

static u32 m_e(Text *t, u32 p, bool big)
{
	u32 len = t->len;
	if (p >= len)
		return p;
	p = nxt(t, p);
	while (p < len && (cls(t, p, big) == C_BL || cls(t, p, big) == C_NL))
		p = nxt(t, p);
	if (p >= len)
		return len ? prv(t, len) : 0;
	int c = cls(t, p, big);
	for (;;) {
		u32 q = nxt(t, p);
		if (q >= len || cls(t, q, big) != c)
			break;
		p = q;
	}
	return p;
}

static u32 m_b(Text *t, u32 p, bool big)
{
	if (p == 0)
		return 0;
	p = prv(t, p);
	while (p > 0) {
		int k = cls(t, p, big);
		if (k == C_BL) {
			p = prv(t, p);
			continue;
		}
		if (k == C_NL) {
			if (empty_at(t, p))
				return p;
			p = prv(t, p);
			continue;
		}
		break;
	}
	int c = cls(t, p, big);
	if (c == C_BL || c == C_NL)
		return p;
	while (p > 0) {
		u32 q = prv(t, p);
		if (cls(t, q, big) != c)
			break;
		p = q;
	}
	return p;
}

static u32 m_ge(Text *t, u32 p, bool big)
{
	u32 p0 = p;
	int c = cls(t, p, big);
	if (c == C_WORD || c == C_PUNCT) {
		while (p > 0 && cls(t, p, big) == c)
			p = prv(t, p);
		if (cls(t, p, big) == c)
			return 0;
	}
	while (p > 0) {
		int k = cls(t, p, big);
		if (k == C_BL) {
			p = prv(t, p);
			continue;
		}
		if (k == C_NL) {
			if (p != p0 && empty_at(t, p))
				return p;
			p = prv(t, p);
			continue;
		}
		break;
	}
	return p;
}

static bool bytes_at(Text *t, u32 o, const char *b, u32 n)
{
	if (o + n > t->len)
		return false;
	for (u32 i = 0; i < n; i++)
		if (text_byte(t, o + i) != (u8)b[i])
			return false;
	return true;
}

static bool find_char(Text *t, u32 p, u32 key, u32 ch, u32 n, bool rep, u32 *out)
{
	if (ch == K_TAB)
		ch = '\t';
	if (ch >= K_SPECIAL)
		return false;
	char cb[4];
	u32 cn = utf8_encode(ch, cb);
	u32 l = lnof(t, p), s = lst(t, l), e = lend(t, l);
	bool fwd = key == 'f' || key == 't', till = key == 't' || key == 'T';
	u32 q = p;
	for (u32 i = 0; i < n; i++) {
		bool found = false;
		if (fwd) {
			u32 o = nxt(t, q);
			if (i == 0 && rep && till)
				o = nxt(t, o);
			for (; o + cn <= e; o = nxt(t, o))
				if (bytes_at(t, o, cb, cn)) {
					found = true;
					break;
				}
			if (!found)
				return false;
			q = o;
		} else {
			if (q <= s)
				return false;
			u32 o = prv(t, q);
			if (i == 0 && rep && till) {
				if (o <= s)
					return false;
				o = prv(t, o);
			}
			for (;;) {
				if (bytes_at(t, o, cb, cn)) {
					found = true;
					break;
				}
				if (o <= s)
					break;
				o = prv(t, o);
			}
			if (!found)
				return false;
			q = o;
		}
	}
	*out = till ? (fwd ? prv(t, q) : nxt(t, q)) : q;
	return true;
}

static bool match_pair(const char *F, u32 len, u32 q, u32 *r)
{
	static const char pairs[] = "()[]{}";
	const char *pp = strchr(pairs, F[q]);
	if (!pp || !F[q])
		return false;
	int idx = (int)(pp - pairs);
	char c = F[q], mate = pairs[idx ^ 1];
	int d = 0;
	if (!(idx & 1)) {
		for (u32 i = q; i < len; i++) {
			if (F[i] == c)
				d++;
			else if (F[i] == mate && --d == 0) {
				*r = i;
				return true;
			}
		}
	} else {
		for (u32 i = q + 1; i-- > 0;) {
			if (F[i] == c)
				d++;
			else if (F[i] == mate && --d == 0) {
				*r = i;
				return true;
			}
		}
	}
	return false;
}

static u32 m_para(Text *t, u32 p, bool fwd, u32 n)
{
	u32 nl = text_lines(t), l = lnof(t, p);
	for (u32 i = 0; i < n; i++) {
		if (fwd) {
			while (l + 1 < nl && text_line_len(t, l) == 0)
				l++;
			while (l + 1 < nl && text_line_len(t, l) != 0)
				l++;
			if (l + 1 >= nl && text_line_len(t, l) != 0)
				return t->len;
		} else {
			while (l > 0 && text_line_len(t, l) == 0)
				l--;
			while (l > 0 && text_line_len(t, l) != 0)
				l--;
		}
	}
	return lst(t, l);
}

static void page_scroll(View *v, i64 lines)
{
	i64 maxy = (i64)(text_lines(TX(v)) - 1) * app.line_h;
	v->scroll_y += lines * app.line_h;
	if (v->scroll_y > maxy)
		v->scroll_y = maxy;
	if (v->scroll_y < 0)
		v->scroll_y = 0;
	app.dirty = true;
}

static bool motion(View *v, const Cmd *c, u32 p, bool op, u32 ci, Mot *m)
{
	Text *t = TX(v);
	u32 n = c->count ? c->count : 1;
	u32 l = lnof(t, p), nl = text_lines(t);
	u32 k = c->mot;
	m->type = MT_EXCL;
	m->jump = false;
	m->want = -1;
	switch (k) {
	case 'h':
	case K_LEFT:
	case K_BS:
		for (u32 i = 0; i < n; i++) {
			u32 ll = lnof(t, p);
			if (p > lst(t, ll))
				p = prv(t, p);
			else if (k == K_BS && ll > 0)
				p = op ? lend(t, ll - 1) : clampn(t, lend(t, ll - 1));
			else
				break;
		}
		break;
	case 'l':
	case K_RIGHT:
	case ' ':
		for (u32 i = 0; i < n; i++) {
			u32 ll = lnof(t, p), e = lend(t, ll);
			if (k == ' ' && (p >= e || (!op && nxt(t, p) >= e)) && ll + 1 < nl) {
				p = lst(t, ll + 1);
				continue;
			}
			if (p >= e)
				break;
			u32 q = nxt(t, p);
			if (!op && q >= e)
				break;
			p = q;
		}
		break;
	case 'j':
	case 'k':
	case K_UP:
	case K_DOWN:
	case GP('j'):
	case GP('k'): {
		bool down = k == 'j' || k == K_DOWN || k == GP('j');
		if (down ? l + 1 >= nl : l == 0)
			return false;
		u32 tl = down ? VX_MIN(l + n, nl - 1) : (l >= n ? l - n : 0);
		int x = v->want_x[ci] >= 0 ? v->want_x[ci] : ed_x_of(t, l, p);
		m->to = ed_off_at_x(t, tl, x);
		m->type = MT_LINE;
		m->want = x;
		return true;
	}
	case '+':
	case K_RET:
	case '-':
	case '_': {
		u32 tl;
		if (k == '-') {
			if (l == 0)
				return false;
			tl = l >= n ? l - n : 0;
		} else if (k == '_') {
			tl = VX_MIN(l + n - 1, nl - 1);
		} else {
			if (l + 1 >= nl)
				return false;
			tl = VX_MIN(l + n, nl - 1);
		}
		m->to = first_nb(t, tl);
		m->type = MT_LINE;
		return true;
	}
	case '0':
	case K_HOME:
	case GP('0'):
		p = lst(t, l);
		break;
	case '^':
		p = first_nb(t, l);
		break;
	case '|':
		p = ed_line_col_to_off(t, l, n - 1);
		break;
	case '$':
	case K_END:
	case GP('$'): {
		u32 tl = VX_MIN(l + n - 1, nl - 1);
		p = lend(t, tl); /* exclusive at the line end = inclusive of the last char */
		m->to = op ? p : clampn(t, p);
		m->want = STICKY_END;
		return true;
	}
	case GP('_'): {
		u32 tl = VX_MIN(l + n - 1, nl - 1), s = lst(t, tl), q = lend(t, tl);
		while (q > s && blank_byte(text_byte(t, q - 1)))
			q--;
		m->to = q > s ? prv(t, q) : s;
		m->type = MT_INCL;
		return true;
	}
	case 'w':
	case 'W': {
		bool big = k == 'W';
		int c0 = cls(t, p, big);
		if (op && c->op == 'c' && c0 != C_BL && c0 != C_NL) {
			p = m_cw(t, p, big, n);
			break;
		}
		for (u32 i = 0; i < n; i++)
			p = m_w(t, p, big, op && i + 1 == n);
		break;
	}
	case 'b':
	case 'B':
		for (u32 i = 0; i < n; i++)
			p = m_b(t, p, k == 'B');
		break;
	case 'e':
	case 'E':
		for (u32 i = 0; i < n; i++)
			p = m_e(t, p, k == 'E');
		m->type = MT_INCL;
		break;
	case GP('e'):
	case GP('E'):
		for (u32 i = 0; i < n; i++)
			p = m_ge(t, p, k == GP('E'));
		m->type = MT_INCL;
		break;
	case GP('g'):
	case 'G': {
		u32 tl = c->count ? VX_MIN(c->count, nl) - 1 : (k == 'G' ? nl - 1 : 0);
		m->to = first_nb(t, tl);
		m->type = MT_LINE;
		m->jump = true;
		return true;
	}
	case 'f':
	case 'F':
	case 't':
	case 'T': {
		u32 q;
		S.ft_key = k;
		S.ft_ch = c->marg;
		if (!find_char(t, p, k, c->marg, n, false, &q))
			return false;
		p = q;
		m->type = (k == 'f' || k == 't') ? MT_INCL : MT_EXCL;
		break;
	}
	case ';':
	case ',': {
		if (!S.ft_key)
			return false;
		u32 kk = S.ft_key, q;
		if (k == ',')
			kk = kk == 'f' ? 'F' : kk == 'F' ? 'f' : kk == 't' ? 'T' : 't';
		if (!find_char(t, p, kk, S.ft_ch, n, true, &q))
			return false;
		p = q;
		m->type = (kk == 'f' || kk == 't') ? MT_INCL : MT_EXCL;
		break;
	}
	case '%': {
		m->jump = true;
		if (c->count) {
			u32 tl = (u32)(((u64)VX_MIN(c->count, 100u) * nl + 99) / 100);
			m->to = first_nb(t, tl ? tl - 1 : 0);
			m->type = MT_LINE;
			return true;
		}
		const char *F = text_flat(t);
		u32 e = lend(t, l), q = p, r;
		while (q < e && !(F[q] && strchr("()[]{}", F[q])))
			q++;
		if (q >= e || !match_pair(F, t->len, q, &r))
			return false;
		p = r;
		m->type = MT_INCL;
		break;
	}
	case '{':
	case '}':
		p = m_para(t, p, k == '}', n);
		m->jump = true;
		break;
	case 'H':
	case 'M':
	case 'L': {
		u32 vl = (u32)ed_view_lines();
		u32 top = (u32)((v->scroll_y + app.line_h - 1) / app.line_h);
		if (top >= nl)
			top = nl - 1;
		u32 bot = VX_MIN(top + vl - 1, nl - 1);
		u32 tl = k == 'H' ? VX_MIN(top + n - 1, bot) : k == 'L' ? (bot >= top + n - 1 ? bot - (n - 1) : top)
		                                                      : (top + bot) / 2;
		m->to = first_nb(t, tl);
		m->type = MT_LINE;
		m->jump = true;
		return true;
	}
	case '*':
	case '#': {
		u32 s, q;
		if (!star_setup(v, p, k == '#', &s) || !search_from(v, s, k == '#', n, &q))
			return false;
		p = q;
		m->jump = true;
		break;
	}
	case 'n':
	case 'N':
	case '/':
	case '?': {
		bool back = k == 'n' ? S.back : k == 'N' ? !S.back : k == '?';
		u32 q;
		if (!search_from(v, p, back, n, &q))
			return false;
		p = q;
		m->jump = true;
		break;
	}
	case '\'':
	case '`': {
		u32 off;
		if (!mark_get(v, c->marg, &off)) {
			ui_status("E20: Mark not set");
			return false;
		}
		m->jump = true;
		if (k == '\'') {
			m->to = first_nb(t, lnof(t, off));
			m->type = MT_LINE;
			return true;
		}
		p = off;
		break;
	}
	default:
		return false;
	}
	m->to = p;
	return true;
}

/* ---- text objects ------------------------------------------------------- */

static bool obj_valid(u32 c) { return c < 128 && c && strchr("wWp\"'`()bB{}[]<>t", (int)c); }

static bool obj_word(Text *t, u32 p, bool around, bool big, u32 n, u32 *a, u32 *b)
{
	u32 l = lnof(t, p), s0 = lst(t, l), e0 = lend(t, l);
	if (s0 == e0)
		return false;
	if (p >= e0)
		p = prv(t, e0);
	int c = cls(t, p, big);
	u32 s = p, e = p;
	while (s > s0 && cls(t, prv(t, s), big) == c)
		s = prv(t, s);
	while (e < e0 && cls(t, e, big) == c)
		e = nxt(t, e);
	if (around) {
		if (c == C_BL) {
			if (e < e0) {
				int c2 = cls(t, e, big);
				while (e < e0 && cls(t, e, big) == c2)
					e = nxt(t, e);
			}
		} else if (e < e0 && cls(t, e, big) == C_BL) {
			while (e < e0 && cls(t, e, big) == C_BL)
				e = nxt(t, e);
		} else {
			u32 s2 = s;
			while (s2 > s0 && cls(t, prv(t, s2), big) == C_BL)
				s2 = prv(t, s2);
			if (s2 > s0) /* never the indentation */
				s = s2;
		}
	}
	for (u32 i = 1; i < n && e < e0; i++) {
		int c2 = cls(t, e, big);
		while (e < e0 && cls(t, e, big) == c2)
			e = nxt(t, e);
		if (around && c2 != C_BL)
			while (e < e0 && cls(t, e, big) == C_BL)
				e = nxt(t, e);
	}
	*a = s;
	*b = e;
	return true;
}

static bool obj_quote(Text *t, u32 p, bool around, u8 ch, u32 *a, u32 *b)
{
	u32 l = lnof(t, p), s0 = lst(t, l), e0 = lend(t, l);
	u32 q[256], nq = 0;
	for (u32 o = s0; o < e0 && nq < 256; o++)
		if (text_byte(t, o) == ch && !(o > s0 && text_byte(t, o - 1) == '\\'))
			q[nq++] = o;
	int k = -1;
	for (u32 i = 0; i + 1 < nq; i += 2)
		if (p >= q[i] && p <= q[i + 1]) {
			k = (int)i;
			break;
		}
	if (k < 0)
		for (u32 i = 0; i + 1 < nq; i += 2)
			if (q[i] > p) {
				k = (int)i;
				break;
			}
	if (k < 0)
		return false;
	if (!around) {
		*a = q[k] + 1;
		*b = q[k + 1];
		return true;
	}
	*a = q[k];
	*b = q[k + 1] + 1;
	u32 e = *b;
	while (e < e0 && blank_byte(text_byte(t, e)))
		e++;
	if (e > *b) {
		*b = e;
	} else {
		u32 s = *a;
		while (s > s0 && blank_byte(text_byte(t, s - 1)))
			s--;
		if (s > s0)
			*a = s;
	}
	return true;
}

static bool obj_pair(Text *t, u32 p, bool around, char o, char c, u32 n, u32 *a, u32 *b, bool *lw)
{
	const char *F = text_flat(t);
	u32 len = t->len;
	if (!len)
		return false;
	p = VX_MIN(p, len - 1);
	u32 op = NONE, from = p;
	for (u32 k = 0; k < n; k++) {
		u32 found = NONE;
		int d = 0;
		for (u32 i = from + 1; i-- > 0;) {
			char ch = F[i];
			if (ch == c && !(k == 0 && i == p))
				d++;
			else if (ch == o) {
				if (d == 0) {
					found = i;
					break;
				}
				d--;
			}
		}
		if (found == NONE)
			return false;
		op = found;
		if (k + 1 < n) {
			if (found == 0)
				return false;
			from = found - 1;
		}
	}
	int d = 0;
	u32 cl = NONE;
	for (u32 i = op + 1; i < len; i++) {
		if (F[i] == o)
			d++;
		else if (F[i] == c) {
			if (d == 0) {
				cl = i;
				break;
			}
			d--;
		}
	}
	if (cl == NONE)
		return false;
	if (around) {
		*a = op;
		*b = cl + 1;
		return true;
	}
	*a = op + 1;
	*b = cl;
	/* "{\n ... \n}": the inner lines, linewise */
	u32 lo = lnof(t, op), lc = lnof(t, cl);
	if (lc > lo && *a == lend(t, lo) && first_nb(t, lc) == cl) {
		if (lc == lo + 1) {
			*a = *b = cl;
			return true;
		}
		*a = lst(t, lo + 1);
		*b = lst(t, lc);
		*lw = true;
	}
	return true;
}

static bool tag_char(const char *F, u32 len, u32 i)
{
	return i < len && (isalnum((u8)F[i]) || F[i] == ':' || F[i] == '-' || F[i] == '_' || F[i] == '.');
}

static bool obj_tag(Text *t, u32 p, bool around, u32 n, u32 *a, u32 *b)
{
	const char *F = text_flat(t);
	u32 len = t->len;
	if (!len)
		return false;
	u32 best_o = NONE, best_ce = 0, best_ge = 0, best_cs = 0, count = 0, tries = 0;
	for (u32 i = VX_MIN(p, len - 1) + 1; i-- > 0 && tries < 400;) {
		if (F[i] != '<' || i + 1 >= len || F[i + 1] == '/' || F[i + 1] == '!' || F[i + 1] == '?')
			continue;
		u32 ns = i + 1, ne = ns;
		while (tag_char(F, len, ne))
			ne++;
		if (ne == ns)
			continue;
		u32 ge = ne;
		while (ge < len && F[ge] != '>')
			ge++;
		if (ge >= len || F[ge - 1] == '/')
			continue;
		ge++;
		tries++;
		u32 nlen = ne - ns, cs = NONE, ce = NONE;
		int d = 0;
		for (u32 j = ge; j + 1 < len; j++) {
			if (F[j] != '<')
				continue;
			if (F[j + 1] == '/' && j + 2 + nlen <= len && !memcmp(F + j + 2, F + ns, nlen) &&
			    !tag_char(F, len, j + 2 + nlen)) {
				if (d == 0) {
					u32 k = j;
					while (k < len && F[k] != '>')
						k++;
					cs = j;
					ce = k < len ? k + 1 : len;
					break;
				}
				d--;
			} else if (j + 1 + nlen <= len && !memcmp(F + j + 1, F + ns, nlen) && !tag_char(F, len, j + 1 + nlen)) {
				u32 k = j;
				while (k < len && F[k] != '>')
					k++;
				if (k < len && F[k - 1] != '/')
					d++;
			}
		}
		if (cs == NONE || ce <= p || (best_o != NONE && ce <= best_ce))
			continue;
		best_o = i;
		best_ce = ce;
		best_ge = ge;
		best_cs = cs;
		if (++count >= n)
			break;
	}
	if (best_o == NONE)
		return false;
	*a = around ? best_o : best_ge;
	*b = around ? best_ce : best_cs;
	return true;
}

static bool obj_para(Text *t, u32 p, bool around, u32 n, u32 *a, u32 *b)
{
	u32 nl = text_lines(t), l = lnof(t, p);
	bool bl = line_blank(t, l);
	u32 l0 = l, l1 = l;
	while (l0 > 0 && line_blank(t, l0 - 1) == bl)
		l0--;
	while (l1 + 1 < nl && line_blank(t, l1 + 1) == bl)
		l1++;
	for (u32 i = 1; i < n && l1 + 1 < nl; i++) {
		bool b2 = line_blank(t, l1 + 1);
		while (l1 + 1 < nl && line_blank(t, l1 + 1) == b2)
			l1++;
	}
	if (around) {
		bool b2 = line_blank(t, l1);
		if (l1 + 1 < nl) {
			while (l1 + 1 < nl && line_blank(t, l1 + 1) != b2)
				l1++;
		} else if (!b2) {
			while (l0 > 0 && line_blank(t, l0 - 1))
				l0--;
		}
	}
	*a = lst(t, l0);
	*b = lnext(t, l1);
	return true;
}

static bool textobj(View *v, u32 p, u32 ai, u32 ch, u32 n, u32 *a, u32 *b, bool *lw)
{
	Text *t = TX(v);
	bool ar = ai == 'a';
	*lw = false;
	switch (ch) {
	case 'w':
	case 'W':
		return obj_word(t, p, ar, ch == 'W', n, a, b);
	case '"':
	case '\'':
	case '`':
		return obj_quote(t, p, ar, (u8)ch, a, b);
	case '(':
	case ')':
	case 'b':
		return obj_pair(t, p, ar, '(', ')', n, a, b, lw);
	case '{':
	case '}':
	case 'B':
		return obj_pair(t, p, ar, '{', '}', n, a, b, lw);
	case '[':
	case ']':
		return obj_pair(t, p, ar, '[', ']', n, a, b, lw);
	case '<':
	case '>':
		return obj_pair(t, p, ar, '<', '>', n, a, b, lw);
	case 't':
		return obj_tag(t, p, ar, n, a, b);
	case 'p':
		*lw = true;
		return obj_para(t, p, ar, n, a, b);
	}
	return false;
}

/* ---- editing --------------------------------------------------------------- */

static void enter_insert(View *v, int mode, u32 count, u32 upos0)
{
	S.mode = mode;
	S.ins_count = count ? count : 1;
	S.ins_undo0 = upos0;
	S.ins_cmd = S.cur;
	if (!S.feeding)
		S.rec.n = 0;
	S.rs_n = 0;
	v->group_kind = 0; /* typing starts a new undo group */
	app.compl_open = false;
	app.dirty = true;
}

static void leave_insert(View *v)
{
	Text *t = TX(v);
	if (S.ins_count > 1 && S.rec.n) {
		bool was = S.feeding;
		u32 cnt = S.ins_count, key = S.ins_cmd.act;
		S.feeding = true;
		S.ins_count = 1;
		for (u32 r = 1; r < cnt; r++) {
			if (key == 'o' || key == 'O')
				feed_ins(v, K_RET);
			for (u32 i = 0; i < S.rec.n; i++)
				feed_ins(v, S.rec.b[i]);
		}
		S.feeding = was;
	}
	merge_undo(t, S.ins_undo0);
	if (!S.dotting) {
		S.dot = S.ins_cmd;
		S.dot_ok = true;
		S.dot_ins.n = 0;
		for (u32 i = 0; i < S.rec.n; i++)
			toks_push(&S.dot_ins, S.rec.b[i]);
	}
	S.mode = M_NORMAL;
	app.compl_open = false;
	for (u32 c = 0; c < v->nc; c++) {
		u32 h = v->head[c], l = lnof(t, h);
		if (h > lst(t, l))
			h = prv(t, h);
		v->head[c] = v->anchor[c] = clampn(t, h);
		v->want_x[c] = -1;
	}
	app.dirty = true;
}

static void delete_range(View *v, u32 a, u32 b, bool lw, u32 g)
{
	Text *t = TX(v);
	if (lw && b == t->len && a > 0 && (a == b || text_byte(t, t->len - 1) != '\n')) {
		u32 el = (u32)strlen(text_eol(t));
		a = a >= el ? a - el : 0;
	}
	if (b > a)
		edit(v, a, b - a, "", 0, g);
}

static void case_range(View *v, u32 a, u32 b, u32 op, u32 g)
{
	Text *t = TX(v);
	if (b <= a)
		return;
	char *src = vx_alloc(b - a + 1);
	text_copy(t, a, b - a, src);
	char *dst = vx_alloc((b - a) * 2 + 8);
	u32 dn = 0;
	for (u32 i = 0; i < b - a;) {
		u32 cp;
		u32 k = utf8_decode(src + i, b - a - i, &cp);
		i += k ? k : 1;
		u32 r = cp;
		if (op == GP('u'))
			r = (u32)towlower((wint_t)cp);
		else if (op == GP('U'))
			r = (u32)towupper((wint_t)cp);
		else
			r = iswupper((wint_t)cp) ? (u32)towlower((wint_t)cp) : (u32)towupper((wint_t)cp);
		dn += utf8_encode(r, dst + dn);
	}
	if (dn != b - a || memcmp(src, dst, dn))
		edit(v, a, b - a, dst, dn, g);
	free(src);
	free(dst);
}

static void indent_range(View *v, u32 l0, u32 l1, bool out, u32 times)
{
	Text *t = TX(v);
	for (u32 i = 0; i < times; i++) {
		v->nc = 1;
		v->anchor[0] = lst(t, l0);
		v->head[0] = lend(t, l1);
		ed_command(v, out ? CMD_OUTDENT : CMD_INDENT);
	}
	set_cur(v, first_nb(t, VX_MIN(l0, text_lines(t) - 1)));
}

/* Apply an operator to [a, b); lw: whole lines (a at a line start, b the
 * start of the line after the last one). */
static void apply_op(View *v, const Cmd *c, u32 a, u32 b, bool lw, u32 upos0, bool vis)
{
	Text *t = TX(v);
	u32 op = c->op, reg = c->reg;
	u32 g = ed_new_group();
	u32 orig = v->head[0];
	u32 l0 = lnof(t, a), l1 = lw ? (b > a ? lnof(t, b - 1) : l0) : lnof(t, b);
	if (lw && b > a && b == t->len && text_byte(t, b - 1) != '\n')
		l1 = lnof(t, b);
	if (!lw && b <= a && op != 'c') { /* nothing to act on: registers stay */
		set_cur(v, clampn(t, a));
		return;
	}
	switch (op) {
	case 'y': {
		yank_range(t, a, b, lw, reg, true);
		if (!lw || vis)
			set_cur(v, clampn(t, vis && lw ? orig : a));
		else if (lnof(t, orig) != l0)
			set_cur(v, clampn(t, ed_line_col_to_off(t, l0, orig - lst(t, lnof(t, orig)))));
		u32 nlines = l1 - l0 + 1;
		if (lw && nlines > 2)
			ui_status("%u lines yanked", nlines);
		break;
	}
	case 'd':
		yank_range(t, a, b, lw, reg, false);
		delete_range(v, a, b, lw, g);
		if (lw)
			set_cur(v, first_nb(t, VX_MIN(l0, text_lines(t) - 1)));
		else
			set_cur(v, clampn(t, a));
		break;
	case 'c':
		if (lw) {
			yank_range(t, a, b, true, reg, false);
			u32 ls = lst(t, l0), ind = first_nb(t, l0) - ls;
			char *in = vx_alloc(ind + 1);
			text_copy(t, ls, ind, in);
			edit(v, ls, lend(t, l1) - ls, in, ind, g);
			free(in);
			set_cur(v, ls + ind);
		} else {
			if (b > a) {
				yank_range(t, a, b, false, reg, false);
				edit(v, a, b - a, "", 0, g);
			}
			set_cur(v, a);
		}
		enter_insert(v, M_INSERT, 1, upos0);
		break;
	case '>':
	case '<':
		if (!lw && l1 > l0 && b == lst(t, l1))
			l1--;
		indent_range(v, l0, l1, op == '<', vis && c->count ? c->count : 1);
		break;
	case GP('u'):
	case GP('U'):
	case GP('~'):
		case_range(v, a, lw ? lend(t, l1) : b, op, g);
		if (lw && !vis)
			set_cur(v, clampn(t, lnof(t, orig) == l0 ? orig : lst(t, l0)));
		else
			set_cur(v, clampn(t, a));
		break;
	}
}

static void range_from_motion(Text *t, u32 p, const Mot *m, u32 *a, u32 *b, bool *lw)
{
	*lw = false;
	if (m->type == MT_LINE) {
		u32 l0 = VX_MIN(lnof(t, p), lnof(t, m->to)), l1 = VX_MAX(lnof(t, p), lnof(t, m->to));
		*a = lst(t, l0);
		*b = lnext(t, l1);
		*lw = true;
		return;
	}
	*a = VX_MIN(p, m->to);
	*b = VX_MAX(p, m->to);
	if (m->type == MT_INCL && *b < t->len && text_byte(t, *b) != '\n' && text_byte(t, *b) != '\r')
		*b = nxt(t, *b);
	if (m->type == MT_EXCL && *b > *a) {
		/* an exclusive motion ending in column 0 stops at the previous line's end;
		 * linewise when it also started at or before the first non-blank */
		u32 la = lnof(t, *a), lb = lnof(t, *b);
		if (lb > la && *b == lst(t, lb)) {
			if (*a <= first_nb(t, la)) {
				*a = lst(t, la);
				*b = lst(t, lb);
				*lw = true;
			} else {
				*b = lend(t, lb - 1);
			}
		}
	}
}

static void exec_op(View *v, Cmd *c, u32 upos0)
{
	Text *t = TX(v);
	collapse(v);
	u32 p = v->head[0], n = c->count ? c->count : 1;
	u32 a, b;
	bool lw = false;
	if (c->lineop) {
		u32 l0 = lnof(t, p), nl = text_lines(t);
		u32 l1 = VX_MIN(l0 + n - 1, nl - 1);
		a = lst(t, l0);
		b = lnext(t, l1);
		lw = true;
	} else if (c->obj) {
		if (!textobj(v, p, c->obj, c->objc, n, &a, &b, &lw)) {
			fail();
			return;
		}
	} else if (c->mot == '/' || c->mot == '?') {
		/* the search pattern comes from the command line */
		S.cl_op = *c;
		S.cl_has_op = true;
		S.cl_prev = M_NORMAL;
		S.mode = M_CMDLINE;
		S.clk = (char)c->mot;
		S.cln = 0;
		return;
	} else {
		Mot m;
		if (!motion(v, c, p, true, 0, &m)) {
			fail();
			return;
		}
		range_from_motion(t, p, &m, &a, &b, &lw);
	}
	apply_op(v, c, a, b, lw, upos0, false);
}

static void put(View *v, u32 reg, bool before, u32 count)
{
	u32 n;
	bool lw;
	char *s = reg_get(reg, &n, &lw);
	if (!s || !n) {
		free(s);
		ui_status("E353: Nothing in register %c", reg ? (int)reg : '"');
		return;
	}
	Text *t = TX(v);
	collapse(v);
	u32 p = v->head[0], l = lnof(t, p), nl = text_lines(t);
	const char *eol = text_eol(t);
	u32 el = (u32)strlen(eol);
	count = VX_MAX(count, 1u);
	char *buf = vx_alloc((size_t)n * count + el + 1);
	u32 bn = 0;
	if (lw && !before && l + 1 >= nl && t->len && text_byte(t, t->len - 1) != '\n') {
		memcpy(buf, eol, el); /* after the last line, which has no line end */
		bn = el;
	}
	u32 pre = bn;
	for (u32 i = 0; i < count; i++) {
		memcpy(buf + bn, s, n);
		bn += n;
	}
	if (pre && bn >= pre + el && !memcmp(buf + bn - el, eol, el))
		bn -= el;
	else if (pre && bn > pre && buf[bn - 1] == '\n')
		bn--;
	u32 g = ed_new_group();
	if (lw) {
		u32 at = before ? lst(t, l) : (l + 1 < nl ? lst(t, l + 1) : t->len);
		edit(v, at, 0, buf, bn, g);
		set_cur(v, first_nb(t, lnof(t, at + pre)));
	} else {
		u32 at = p;
		if (!before && p < lend(t, l))
			at = nxt(t, p);
		edit(v, at, 0, buf, bn, g);
		if (memchr(buf, '\n', bn))
			set_cur(v, at);
		else
			set_cur(v, clampn(t, prv(t, at + bn)));
	}
	free(buf);
	free(s);
}

static void join_lines(View *v, u32 l, u32 n, bool spaces, u32 g)
{
	Text *t = TX(v);
	u32 at = v->head[0];
	for (u32 k = 1; k < n && l + 1 < text_lines(t); k++) {
		u32 le = lend(t, l), nb = first_nb(t, l + 1), ne = lend(t, l + 1);
		bool sp = spaces && le > lst(t, l) && !blank_byte(text_byte(t, le - 1)) && nb < ne && text_byte(t, nb) != ')';
		edit(v, le, (spaces ? nb : lst(t, l + 1)) - le, " ", sp ? 1 : 0, g);
		at = le;
	}
	set_cur(v, clampn(t, at));
}

static void replace_chars(View *v, u32 a, u32 b, u32 ch, u32 g)
{
	Text *t = TX(v);
	char cb[4];
	u32 cn = utf8_encode(ch, cb);
	char *buf = vx_alloc((b - a) * 4 + 8);
	u32 bn = 0;
	for (u32 o = a; o < b;) {
		u8 c = text_byte(t, o);
		u32 q = nxt(t, o);
		if (c == '\n' || c == '\r') {
			text_copy(t, o, q - o, buf + bn);
			bn += q - o;
		} else {
			memcpy(buf + bn, cb, cn);
			bn += cn;
		}
		o = q;
	}
	edit(v, a, b - a, buf, bn, g);
	free(buf);
}

/* insert mode Ctrl+W / Ctrl+U */
static void ins_kill(View *v, bool line)
{
	Text *t = TX(v);
	u32 g = ed_new_group();
	for (u32 c = v->nc; c-- > 0;) {
		u32 p = v->head[c], l = lnof(t, p), s = lst(t, l), q = p;
		if (p == s) {
			q = prv(t, p);
		} else if (line) {
			u32 nb = first_nb(t, l);
			q = p > nb ? nb : s;
		} else {
			while (q > s && blank_byte(text_byte(t, q - 1)))
				q--;
			if (q > s) {
				int k = cls(t, prv(t, q), false);
				while (q > s && cls(t, prv(t, q), false) == k)
					q = prv(t, q);
			}
		}
		if (q < p)
			edit(v, q, p - q, "", 0, g);
		v->head[c] = v->anchor[c] = q;
	}
}

static void replace_char(View *v, u32 cp)
{
	Text *t = TX(v);
	u32 p = v->head[0], e = lend(t, lnof(t, p)), del = 0, orig = NONE;
	if (p < e)
		orig = cp_at(t, p, &del);
	char b[4];
	u32 n = utf8_encode(cp, b);
	VX_GROW(S.rstack, S.rs_cap, S.rs_n + 1);
	S.rstack[S.rs_n++] = orig;
	edit(v, p, del, b, n, ed_new_group());
	set_cur(v, p + n);
}

static void replace_bs(View *v)
{
	Text *t = TX(v);
	u32 p = v->head[0];
	if (p == lst(t, lnof(t, p)))
		return;
	u32 q = prv(t, p);
	if (S.rs_n) {
		u32 orig = S.rstack[--S.rs_n];
		char b[4];
		u32 n = orig == NONE ? 0 : utf8_encode(orig, b);
		edit(v, q, p - q, b, n, ed_new_group());
	}
	set_cur(v, q);
}

/* ---- visual mode ----------------------------------------------------- */

static void vis_apply(View *v)
{
	Text *t = TX(v);
	u32 len = t->len, s = VX_MIN(S.vstart, len), c = VX_MIN(S.vcur, len), a, h;
	if (S.mode == M_VLINE) {
		if (c >= s) {
			a = lst(t, lnof(t, s));
			h = lnext(t, lnof(t, c));
		} else {
			a = lnext(t, lnof(t, s));
			h = lst(t, lnof(t, c));
		}
	} else if (c >= s) {
		a = s;
		h = c < len ? nxt(t, c) : len;
	} else {
		a = s < len ? nxt(t, s) : len;
		h = c;
	}
	v->nc = 1;
	v->anchor[0] = S.set_anchor = a;
	v->head[0] = S.set_head = h;
	S.lv_doc = v->doc;
	S.lv_start = S.vstart;
	S.lv_cur = S.vcur;
	S.lv_mode = S.mode;
	ed_reveal(v, c, false);
	app.dirty = true;
}

static void vis_enter(View *v, int mode, u32 start, u32 cur)
{
	S.mode = mode;
	S.vstart = start;
	S.vcur = cur;
	vis_apply(v);
}

static void vis_exit(View *v, u32 cur)
{
	S.mode = M_NORMAL;
	set_cur(v, clampn(TX(v), cur));
}

/* the selection as a range: [a, b), linewise for visual line */
static void vis_range(View *v, u32 *a, u32 *b, bool *lw)
{
	Text *t = TX(v);
	u32 lo = VX_MIN(S.vstart, S.vcur), hi = VX_MAX(S.vstart, S.vcur);
	lo = VX_MIN(lo, t->len);
	hi = VX_MIN(hi, t->len);
	if (S.mode == M_VLINE) {
		*a = lst(t, lnof(t, lo));
		*b = lnext(t, lnof(t, hi));
		*lw = true;
	} else {
		*a = lo;
		*b = hi < t->len ? nxt(t, hi) : t->len;
		*lw = false;
	}
}

static void vis_put(View *v, u32 reg, bool lw_sel, u32 a, u32 b)
{
	u32 n;
	bool lw;
	char *s = reg_get(reg, &n, &lw);
	if (!s) {
		ui_status("E353: Nothing in register %c", reg ? (int)reg : '"');
		vis_exit(v, S.vcur);
		return;
	}
	Text *t = TX(v);
	const char *eol = text_eol(t);
	u32 el = (u32)strlen(eol);
	char *buf = vx_alloc(n + 2 * el + 1);
	u32 bn = 0;
	if (lw && !lw_sel) {
		memcpy(buf, eol, el);
		bn = el;
		memcpy(buf + bn, s, n);
		bn += n;
	} else {
		memcpy(buf, s, n);
		bn = n;
		if (lw_sel && !lw) {
			memcpy(buf + bn, eol, el);
			bn += el;
		}
	}
	/* the replaced text goes to the unnamed register (vim's default) */
	yank_range(t, a, b, lw_sel, 0, false);
	if (lw_sel && b == t->len && t->len && text_byte(t, t->len - 1) != '\n' && bn >= el &&
	    !memcmp(buf + bn - el, eol, el))
		bn -= el;
	u32 g = ed_new_group();
	edit(v, a, b - a, buf, bn, g);
	S.mode = M_NORMAL;
	if (lw || lw_sel)
		set_cur(v, first_nb(t, lnof(t, a + (lw && !lw_sel ? el : 0))));
	else
		set_cur(v, clampn(t, bn ? prv(t, a + bn) : a));
	free(buf);
	free(s);
}

/* ---- scrolling --------------------------------------------------------- */

static u32 page_move(View *v, u32 act, u32 count, u32 p)
{
	Text *t = TX(v);
	u32 vl = (u32)ed_view_lines(), nl = text_lines(t), l = lnof(t, p);
	bool down = act == CTRL('d') || act == K_PGDN || act == CTRL('f');
	bool half = act == CTRL('d') || act == CTRL('u');
	u32 amount = half ? (count ? count : VX_MAX(vl / 2, 1u)) : VX_MAX(vl > 2 ? vl - 2 : 1, 1u) * VX_MAX(count, 1u);
	u32 tl = down ? VX_MIN(l + amount, nl - 1) : (l >= amount ? l - amount : 0);
	if (tl == l)
		return NONE;
	page_scroll(v, down ? (i64)amount : -(i64)amount);
	return first_nb(t, tl);
}

static void scroll_to(View *v, u32 p, u32 how)
{
	Text *t = TX(v);
	i64 y = (i64)lnof(t, p) * app.line_h, h = (i64)ed_view_lines() * app.line_h;
	if (how == 'z' || how == '.')
		v->scroll_y = y - h / 2 + app.line_h / 2;
	else if (how == 't' || how == K_RET)
		v->scroll_y = y;
	else
		v->scroll_y = y - h + app.line_h;
	if (v->scroll_y < 0)
		v->scroll_y = 0;
	app.dirty = true;
}

/* ---- ex commands -------------------------------------------------------- */

static bool addr(View *v, char **ps, u32 *out)
{
	Text *t = TX(v);
	char *s = *ps;
	u32 nl = text_lines(t);
	i64 x = lnof(t, v->head[0]);
	bool ok = false;
	if (isdigit((u8)*s)) {
		x = strtol(s, &s, 10) - 1;
		ok = true;
	} else if (*s == '.') {
		s++;
		ok = true;
	} else if (*s == '$') {
		x = nl - 1;
		s++;
		ok = true;
	} else if (*s == '\'' && s[1]) {
		u32 off;
		if (!mark_get(v, (u8)s[1], &off)) {
			ui_status("E20: Mark not set");
			return false;
		}
		x = lnof(t, off);
		s += 2;
		ok = true;
	}
	while (*s == '+' || *s == '-') {
		int sg = *s++ == '+' ? 1 : -1;
		i64 d = 1;
		if (isdigit((u8)*s))
			d = strtol(s, &s, 10);
		x += sg * d;
		ok = true;
	}
	if (!ok)
		return false;
	*out = (u32)VX_CLAMP(x, 0, (i64)nl - 1);
	*ps = s;
	return true;
}

/* Split "pat/rep/flags" at unescaped delimiters; \<delim> becomes <delim>. */
static char *ex_part(char *s, char d, char *out, size_t cap)
{
	size_t k = 0;
	while (*s && *s != d) {
		if (*s == '\\' && s[1] == d) {
			s++;
		} else if (*s == '\\' && s[1]) {
			if (k + 1 < cap)
				out[k++] = *s;
			s++;
		}
		if (k + 1 < cap)
			out[k++] = *s;
		s++;
	}
	out[k] = 0;
	return *s == d ? s + 1 : s;
}

/* vim replacement syntax to the engine's: \1 -> ${1}, & -> $0, \r -> \n */
static void vim_repl(const char *r, char *out, size_t cap)
{
	size_t k = 0;
	for (; *r && k + 6 < cap; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			if (isdigit((u8)*r))
				k += (size_t)snprintf(out + k, cap - k, "${%c}", *r);
			else if (*r == '&')
				out[k++] = '&';
			else if (*r == 'r' || *r == 'n') {
				out[k++] = '\\';
				out[k++] = 'n';
			} else {
				out[k++] = '\\';
				out[k++] = *r;
			}
		} else if (*r == '&') {
			out[k++] = '$';
			out[k++] = '0';
		} else if (*r == '$' && !(isdigit((u8)r[1]) || r[1] == '{' || r[1] == '&' || r[1] == '$')) {
			out[k++] = '$';
			out[k++] = '$';
		} else {
			out[k++] = *r;
		}
	}
	out[k] = 0;
}

static void ex_substitute(View *v, u32 l0, u32 l1, char *s)
{
	Text *t = TX(v);
	char d = *s;
	if (!d || isalnum((u8)d) || d == ' ' || d == '\\' || d == '"' || d == '|') {
		ui_status("E146: Regular expressions can't be delimited by letters");
		return;
	}
	char pat[512], rep[512], erep[1100];
	s = ex_part(s + 1, d, pat, sizeof pat);
	s = ex_part(s, d, rep, sizeof rep);
	bool all = false;
	int icase = -1;
	for (; *s; s++) {
		if (*s == 'g')
			all = true;
		else if (*s == 'i')
			icase = 1;
		else if (*s == 'I')
			icase = 0;
	}
	if (pat[0])
		set_pat(pat, false);
	else if (!S.pat_ok) {
		ui_status("E35: No previous regular expression");
		return;
	}
	vim_repl(rep, erep, sizeof erep);
	int flags = icase == 1 || (icase < 0 && !has_upper(S.pat)) ? RE_ICASE : 0;
	char err[160];
	Regex *re = re_compile(S.pat, strlen(S.pat), flags, err, sizeof err);
	if (!re) {
		ui_status("E383: Invalid search string: %s (%s)", S.pat, err);
		return;
	}
	const char *F = text_flat(t);
	u32 caps[2 * RE_MAX_CAPS], nm = 0, cap = 0, nlines = 0, last_l = NONE;
	u32 *ma = NULL, *mb = NULL, *ro = NULL, *rl = NULL;
	Pool reps = {0};
	for (u32 l = l0; l <= l1; l++) {
		u32 s0 = lst(t, l), e = lend(t, l), pos = s0;
		bool hit = false;
		while (pos <= e && re_search(re, F, e, pos, caps) && caps[0] <= e) {
			char *o = NULL;
			u32 on = 0, oc = 0;
			re_expand(re, erep, strlen(erep), F, caps, &o, &on, &oc);
			if (nm + 1 > cap) {
				cap = cap ? cap * 2 : 64;
				ma = vx_realloc(ma, cap * sizeof(u32));
				mb = vx_realloc(mb, cap * sizeof(u32));
				ro = vx_realloc(ro, cap * sizeof(u32));
				rl = vx_realloc(rl, cap * sizeof(u32));
			}
			ma[nm] = caps[0];
			mb[nm] = caps[1];
			ro[nm] = pool_add(&reps, o ? o : "", on);
			rl[nm] = on;
			nm++;
			free(o);
			hit = true;
			if (!all || caps[1] >= e)
				break;
			pos = caps[1] > caps[0] ? caps[1] : nxt(t, caps[0]);
		}
		if (hit) {
			nlines++;
			last_l = l;
		}
	}
	re_free(re);
	if (!nm) {
		ui_status("E486: Pattern not found: %s", S.pat);
	} else {
		/* the first non-blank of the last changed line, after the edits */
		i64 shift = 0;
		for (u32 k = 0; k < nm && lnof(t, ma[k]) < last_l; k++)
			shift += (i64)rl[k] - (i64)(mb[k] - ma[k]);
		u32 last_s = (u32)((i64)lst(t, last_l) + shift);
		u32 g = ed_new_group();
		for (u32 k = nm; k-- > 0;)
			edit(v, ma[k], mb[k] - ma[k], reps.b + ro[k], rl[k], g);
		set_cur(v, first_nb(t, lnof(t, last_s)));
		ui_status("%u substitution%s on %u line%s", nm, nm == 1 ? "" : "s", nlines, nlines == 1 ? "" : "s");
	}
	free(ma);
	free(mb);
	free(ro);
	free(rl);
	pool_free(&reps);
}

/* Runs an ex command line; the view may be gone afterwards (:q, :e). */
static void ex(View *v, char *s)
{
	Text *t = TX(v);
	while (*s == ' ' || *s == ':')
		s++;
	u32 nl = text_lines(t), cur = lnof(t, v->head[0]);
	u32 l0 = cur, l1 = cur;
	bool range = false;
	if (*s == '%') {
		l0 = 0;
		l1 = nl - 1;
		range = true;
		s++;
	} else if (addr(v, &s, &l0)) {
		l1 = l0;
		range = true;
		if (*s == ',' || *s == ';') {
			s++;
			if (!addr(v, &s, &l1)) {
				ui_status("E14: Invalid address");
				return;
			}
		}
	}
	if (l0 > l1) {
		u32 x = l0;
		l0 = l1;
		l1 = x;
	}
	while (*s == ' ')
		s++;
	char name[32];
	u32 k = 0;
	while (isalpha((u8)*s) && k + 1 < sizeof name)
		name[k++] = *s++;
	name[k] = 0;
	bool bang = *s == '!';
	if (bang)
		s++;
	while (*s == ' ')
		s++;
	char *arg = s;
	size_t al = strlen(arg);
	while (al && arg[al - 1] == ' ')
		arg[--al] = 0;
#define IS(a, b) (!strcmp(name, a) || !strcmp(name, b))
	if (!name[0]) {
		if (*arg && *arg != 's') {
			ui_status("E492: Not an editor command: %s", arg);
		} else if (range) {
			jump_push(v, v->head[0]);
			set_cur(v, first_nb(t, l1));
			ed_reveal(v, v->head[0], true);
		}
		return;
	}
	if (!strcmp(name, "s") || !strcmp(name, "substitute")) {
		ex_substitute(v, l0, l1, arg);
		ed_reveal(v, v->head[0], true);
		return;
	}
	if (IS("w", "write")) {
		if (*arg)
			ui_status("E: :w with a file name is not supported (use Save As)");
		else
			run_command(CMD_SAVE);
		return;
	}
	if (IS("wa", "wall")) {
		run_command(CMD_SAVE_ALL);
		return;
	}
	if (IS("q", "quit") || IS("clo", "close")) {
		if (text_dirty(t) && !bang && app.docs.refs[v->doc] <= 1) {
			ui_status("E37: No write since last change (add ! to override)");
			return;
		}
		S.view = NULL;
		run_command(bang ? CMD_REVERT_AND_CLOSE : CMD_CLOSE_EDITOR);
		return;
	}
	if (IS("wq", "x") || IS("xit", "exit")) {
		if (!strcmp(name, "wq") || text_dirty(t))
			run_command(CMD_SAVE);
		if (text_dirty(t) && !bang) /* the save failed or asked for a path */
			return;
		S.view = NULL;
		run_command(CMD_CLOSE_EDITOR);
		return;
	}
	if (IS("qa", "qall") || IS("quitall", "qa")) {
		S.view = NULL;
		run_command(CMD_CLOSE_ALL);
		return;
	}
	if (IS("e", "edit")) {
		if (!*arg) {
			ui_status("E32: No file name");
			return;
		}
		char path[4096];
		if (arg[0] == '/')
			snprintf(path, sizeof path, "%s", arg);
		else if (arg[0] == '~' && arg[1] == '/' && getenv("HOME"))
			snprintf(path, sizeof path, "%s/%s", getenv("HOME"), arg + 2);
		else
			snprintf(path, sizeof path, "%s/%s", app.root, arg);
		S.view = NULL;
		open_file(path, false, UINT32_MAX, 0);
		return;
	}
	if (IS("d", "delete") || IS("y", "yank")) {
		Cmd c = {.op = name[0] == 'd' ? 'd' : 'y', .reg = (u8)*arg};
		apply_op(v, &c, lst(t, l0), lnext(t, l1), true, t->upos, false);
		return;
	}
	if (IS("j", "join")) {
		set_cur(v, lst(t, l0));
		join_lines(v, l0, VX_MAX(l1 - l0 + 1, 2u), !bang, ed_new_group());
		return;
	}
	if (IS("u", "undo")) {
		ed_undo_redo(v, false, NULL);
		set_cur(v, clampn(t, v->head[0]));
		return;
	}
	if (IS("red", "redo")) {
		ed_undo_redo(v, true, NULL);
		set_cur(v, clampn(t, v->head[0]));
		return;
	}
	if (IS("noh", "nohlsearch") || IS("se", "set")) /* no search highlight; options are fixed */
		return;
	if (IS("sp", "split") || IS("vs", "vsplit")) {
		run_command(CMD_SPLIT);
		return;
	}
	if (IS("bn", "bnext") || IS("tabn", "tabnext")) {
		run_command(CMD_NEXT_EDITOR);
		return;
	}
	if (IS("bp", "bprevious") || IS("tabp", "tabprevious")) {
		run_command(CMD_PREV_EDITOR);
		return;
	}
#undef IS
	ui_status("E492: Not an editor command: %s", name);
}

static void cl_cancel(View *v)
{
	S.mode = S.cl_prev;
	S.cl_has_op = false;
	if (S.mode == M_VISUAL || S.mode == M_VLINE)
		vis_apply(v);
	app.dirty = true;
}

static void cl_accept(View *v)
{
	char line[sizeof S.cl + 1];
	memcpy(line, S.cl, S.cln);
	line[S.cln] = 0;
	char kind = S.clk;
	S.mode = S.cl_prev;
	if (kind == ':') {
		if (S.mode == M_VISUAL || S.mode == M_VLINE)
			vis_exit(v, S.vcur);
		S.in_vim = true;
		ex(v, line);
		S.in_vim = false;
		return;
	}
	if (line[0])
		set_pat(line, false);
	S.back = kind == '?';
	if (S.cl_has_op) {
		S.cl_has_op = false;
		Cmd c = S.cl_op;
		c.mot = 'n'; /* the pattern is set: search in its direction */
		S.cur = c;
		exec(v, &c);
		return;
	}
	Text *t = TX(v);
	bool vis = S.mode == M_VISUAL || S.mode == M_VLINE;
	u32 p = vis ? S.vcur : v->head[0], q;
	if (!search_from(v, p, S.back, 1, &q)) {
		if (vis)
			vis_apply(v);
		return;
	}
	if (vis) {
		S.vcur = q;
		vis_apply(v);
	} else {
		jump_push(v, p);
		set_cur(v, clampn(t, q));
		ed_reveal(v, q, true);
	}
}

/* ---- command execution ------------------------------------------------- */

static bool is_change(const Cmd *c)
{
	if (c->op)
		return c->op != 'y';
	u32 a = c->act;
	return a && ((a < 128 && strchr("xXsSJ~pPDCraiIAoOR", (int)a)) || a == GP('J') || a == K_DEL);
}

static void dedupe(View *v)
{
	u32 k = 0;
	for (u32 i = 0; i < v->nc; i++) {
		bool dup = false;
		for (u32 j = 0; j < k; j++)
			if (v->head[j] == v->head[i])
				dup = true;
		if (dup)
			continue;
		v->head[k] = v->head[i];
		v->anchor[k] = v->anchor[i];
		v->want_x[k] = v->want_x[i];
		k++;
	}
	v->nc = k ? k : 1;
}

static void dot_repeat(View *v, u32 count)
{
	if (!S.dot_ok) {
		fail();
		return;
	}
	Cmd c = S.dot;
	if (count)
		c.count = count;
	S.dotting = true;
	S.feeding = true;
	S.cur = c;
	exec(v, &c);
	if (S.mode == M_INSERT || S.mode == M_REPLACE) {
		S.rec.n = 0;
		for (u32 i = 0; i < S.dot_ins.n; i++)
			toks_push(&S.rec, S.dot_ins.b[i]);
		for (u32 i = 0; i < S.dot_ins.n; i++)
			feed_ins(v, S.dot_ins.b[i]);
		leave_insert(v);
	}
	S.feeding = false;
	S.dotting = false;
	if (count)
		S.dot.count = count;
}

static void exec_visual(View *v, Cmd *c)
{
	Text *t = TX(v);
	u32 n = c->count ? c->count : 1, upos0 = t->upos;
	if (c->mot) {
		Mot m;
		u32 old = S.vcur;
		if (!motion(v, c, S.vcur, false, 0, &m)) {
			fail();
			return;
		}
		S.vcur = m.type == MT_LINE || c->mot == '$' ? clampn(t, m.to) : VX_MIN(m.to, t->len);
		v->want_x[0] = m.want;
		if (m.jump)
			jump_push(v, old);
		vis_apply(v);
		return;
	}
	if (c->obj) {
		u32 a, b;
		bool lw;
		if (!textobj(v, S.vcur, c->obj, c->objc, n, &a, &b, &lw) || b <= a) {
			fail();
			return;
		}
		if (lw && c->objc == 'p') {
			S.mode = M_VLINE;
			S.vstart = a;
			S.vcur = prv(t, b);
		} else {
			if (S.vstart == S.vcur || S.vstart > a)
				S.vstart = a;
			S.vcur = prv(t, b);
		}
		vis_apply(v);
		return;
	}
	u32 a, b;
	bool lw;
	vis_range(v, &a, &b, &lw);
	u32 lo = VX_MIN(S.vstart, S.vcur);
	if (c->op) {
		S.mode = M_NORMAL;
		set_cur(v, lo);
		apply_op(v, c, a, b, lw, upos0, true);
		if (c->op == 'y')
			set_cur(v, clampn(t, lw ? VX_MIN(lo, t->len) : a));
		return;
	}
	u32 l0 = lnof(t, a), l1 = lw ? lnof(t, b > a ? prv(t, b) : a) : lnof(t, b > a ? prv(t, b) : a);
	if (lw && b == t->len && b > a && text_byte(t, b - 1) != '\n')
		l1 = lnof(t, b);
	Cmd o = {.reg = c->reg, .count = c->count};
	switch (c->act) {
	case 'x':
	case K_DEL:
		o.op = 'd';
		S.mode = M_NORMAL;
		set_cur(v, lo);
		apply_op(v, &o, a, b, lw, upos0, true);
		return;
	case 's':
		o.op = 'c';
		S.mode = M_NORMAL;
		set_cur(v, lo);
		apply_op(v, &o, a, b, lw, upos0, true);
		return;
	case 'X':
	case 'D':
	case 'Y':
	case 'S':
	case 'C':
	case 'R':
		o.op = c->act == 'Y' ? 'y' : (c->act == 'X' || c->act == 'D') ? 'd' : 'c';
		S.mode = M_NORMAL;
		set_cur(v, lo);
		apply_op(v, &o, lst(t, l0), lnext(t, l1), true, upos0, true);
		return;
	case 'J':
	case GP('J'):
		S.mode = M_NORMAL;
		set_cur(v, lst(t, l0));
		join_lines(v, l0, VX_MAX(l1 - l0 + 1, 2u), c->act == 'J', ed_new_group());
		return;
	case 'r':
		if (c->aarg >= K_SPECIAL)
			return;
		replace_chars(v, a, b, c->aarg, ed_new_group());
		vis_exit(v, lw ? lst(t, l0) : a);
		return;
	case 'p':
	case 'P':
		vis_put(v, c->reg, lw, a, b);
		return;
	case 'o':
	case 'O': {
		u32 x = S.vstart;
		S.vstart = S.vcur;
		S.vcur = x;
		v->want_x[0] = -1;
		vis_apply(v);
		return;
	}
	case 'v':
	case 'V': {
		int want = c->act == 'v' ? M_VISUAL : M_VLINE;
		if (S.mode == want)
			vis_exit(v, S.vcur);
		else {
			S.mode = want;
			vis_apply(v);
		}
		return;
	}
	case ':':
		S.cl_prev = S.mode;
		S.mode = M_CMDLINE;
		S.clk = ':';
		S.cln = (u32)snprintf(S.cl, sizeof S.cl, "'<,'>");
		return;
	case '/':
	case '?':
		S.cl_prev = S.mode;
		S.mode = M_CMDLINE;
		S.clk = (char)c->act;
		S.cln = 0;
		return;
	case 'I':
	case 'A':
		S.mode = M_NORMAL;
		if (c->act == 'I')
			set_cur(v, lw ? first_nb(t, l0) : a);
		else
			set_cur(v, lw ? lend(t, l1) : VX_MIN(b, lend(t, lnof(t, prv(t, b)))));
		enter_insert(v, M_INSERT, 1, upos0);
		return;
	case CTRL('d'):
	case CTRL('u'):
	case CTRL('f'):
	case CTRL('b'):
	case K_PGDN:
	case K_PGUP: {
		u32 q = page_move(v, c->act, c->count, S.vcur);
		if (q != NONE) {
			S.vcur = q;
			vis_apply(v);
		}
		return;
	}
	case 'z':
		scroll_to(v, S.vcur, c->aarg);
		return;
	case GP('v'):
		return;
	default:
		fail();
		return;
	}
}

static void exec_action(View *v, Cmd *c, u32 upos0)
{
	Text *t = TX(v);
	u32 n = c->count ? c->count : 1;
	u32 act = c->act;
	if (act != 'i' && act != 'a' && act != 'I' && act != 'A')
		collapse(v); /* only inserts keep several cursors */
	u32 p = v->head[0], l = lnof(t, p);
	Cmd o = {.reg = c->reg, .count = c->count};
	switch (act) {
	case 'x':
	case K_DEL:
		o.op = 'd';
		o.mot = 'l';
		exec_op(v, &o, upos0);
		return;
	case 'X':
		o.op = 'd';
		o.mot = 'h';
		exec_op(v, &o, upos0);
		return;
	case 's':
		o.op = 'c';
		o.mot = 'l';
		exec_op(v, &o, upos0);
		return;
	case 'S':
		o.op = 'c';
		o.lineop = true;
		exec_op(v, &o, upos0);
		return;
	case 'C':
	case 'D':
		o.op = act == 'C' ? 'c' : 'd';
		o.mot = '$';
		exec_op(v, &o, upos0);
		return;
	case 'Y':
		o.op = 'y';
		o.lineop = true;
		exec_op(v, &o, upos0);
		return;
	case 'J':
	case GP('J'):
		join_lines(v, l, VX_MAX(n, 2u), act == 'J', ed_new_group());
		return;
	case '~': {
		u32 e = lend(t, l), q = p;
		for (u32 i = 0; i < n && q < e; i++)
			q = nxt(t, q);
		if (q > p)
			case_range(v, p, q, GP('~'), ed_new_group());
		set_cur(v, clampn(t, q));
		return;
	}
	case 'r': {
		u32 e = lend(t, l), q = p;
		u32 ch = c->aarg == K_TAB ? '\t' : c->aarg;
		for (u32 i = 0; i < n; i++) {
			if (q >= e) {
				fail();
				return;
			}
			q = nxt(t, q);
		}
		u32 g = ed_new_group();
		if (ch == K_RET) {
			edit(v, p, q - p, text_eol(t), (u32)strlen(text_eol(t)), g);
			set_cur(v, first_nb(t, l + 1));
		} else if (ch < K_SPECIAL) {
			replace_chars(v, p, q, ch, g);
			/* on the last replaced character */
			u32 cur = p;
			for (u32 i = 1; i < n; i++)
				cur = nxt(t, cur);
			set_cur(v, clampn(t, cur));
		}
		return;
	}
	case 'p':
	case 'P':
		put(v, c->reg, act == 'P', n);
		return;
	case 'u':
	case CTRL('r'): {
		u32 at = NONE;
		for (u32 i = 0; i < n; i++) {
			u32 pos;
			if (!ed_undo_redo(v, act != 'u', &pos)) {
				if (!i)
					ui_status(act == 'u' ? "Already at oldest change" : "Already at newest change");
				break;
			}
			at = pos;
		}
		if (at != NONE)
			set_cur(v, clampn(t, at));
		ed_reveal(v, v->head[0], false);
		return;
	}
	case '.':
		dot_repeat(v, c->count);
		return;
	case 'i':
	case 'a':
	case 'I':
	case 'A':
		for (u32 k = 0; k < v->nc; k++) {
			u32 h = v->head[k], hl = lnof(t, h);
			if (act == 'a' && h < lend(t, hl))
				h = nxt(t, h);
			else if (act == 'I')
				h = first_nb(t, hl);
			else if (act == 'A')
				h = lend(t, hl);
			v->head[k] = v->anchor[k] = h;
			v->want_x[k] = -1;
		}
		enter_insert(v, M_INSERT, c->count, upos0);
		return;
	case 'o':
	case 'O': {
		u32 ls = lst(t, l), ind = first_nb(t, l) - ls;
		const char *eol = text_eol(t);
		u32 el = (u32)strlen(eol);
		char *buf = vx_alloc(ind + el + 1);
		u32 g = ed_new_group();
		if (act == 'o') {
			memcpy(buf, eol, el);
			text_copy(t, ls, ind, buf + el);
			u32 e = lend(t, l);
			edit(v, e, 0, buf, el + ind, g);
			set_cur(v, e + el + ind);
		} else {
			text_copy(t, ls, ind, buf);
			memcpy(buf + ind, eol, el);
			edit(v, ls, 0, buf, ind + el, g);
			set_cur(v, ls + ind);
		}
		free(buf);
		enter_insert(v, M_INSERT, c->count, upos0);
		return;
	}
	case 'R':
		enter_insert(v, M_REPLACE, 1, upos0);
		return;
	case 'v':
		vis_enter(v, M_VISUAL, p, p);
		return;
	case 'V':
		vis_enter(v, M_VLINE, p, p);
		return;
	case GP('v'):
		if (S.lv_mode && S.lv_doc == v->doc) {
			S.mode = S.lv_mode;
			S.vstart = clampn(t, S.lv_start);
			S.vcur = clampn(t, S.lv_cur);
			vis_apply(v);
		} else
			fail();
		return;
	case ':':
	case '/':
	case '?':
		S.cl_prev = M_NORMAL;
		S.mode = M_CMDLINE;
		S.clk = (char)act;
		S.cln = 0;
		if (act == ':' && c->count)
			S.cln = (u32)snprintf(S.cl, sizeof S.cl, ".,.+%u", c->count - 1);
		return;
	case 'm':
		if (c->aarg >= 'a' && c->aarg <= 'z') {
			mark_line[v->doc][c->aarg - 'a'] = l + 1;
			mark_col[v->doc][c->aarg - 'a'] = p - lst(t, l);
		} else
			fail();
		return;
	case 'z':
		if (c->aarg == 'z' || c->aarg == 't' || c->aarg == 'b' || c->aarg == K_RET || c->aarg == '.' ||
		    c->aarg == '-') {
			if (c->count)
				set_cur(v, first_nb(t, VX_MIN(c->count, text_lines(t)) - 1));
			if (c->aarg == K_RET || c->aarg == '.' || c->aarg == '-')
				set_cur(v, first_nb(t, lnof(t, v->head[0])));
			scroll_to(v, v->head[0], c->aarg);
		} else
			fail();
		return;
	case 'Z': {
		char cmd[4] = "";
		if (c->aarg == 'Z')
			strcpy(cmd, "x");
		else if (c->aarg == 'Q')
			strcpy(cmd, "q!");
		if (cmd[0]) {
			S.in_vim = true;
			ex(v, cmd);
			S.in_vim = false;
		}
		return;
	}
	case CTRL('o'):
		jump_go(v, -1, n);
		return;
	case K_TAB:
		jump_go(v, 1, n);
		return;
	case CTRL('d'):
	case CTRL('u'):
	case CTRL('f'):
	case CTRL('b'):
	case K_PGDN:
	case K_PGUP: {
		u32 q = page_move(v, act, c->count, p);
		if (q != NONE)
			set_cur(v, q);
		return;
	}
	case CTRL('e'):
	case CTRL('y'):
		page_scroll(v, act == CTRL('e') ? (i64)n : -(i64)n);
		return;
	default:
		fail();
		return;
	}
}

static void exec(View *v, Cmd *c)
{
	Text *t = TX(v);
	if (S.mode == M_VISUAL || S.mode == M_VLINE) {
		exec_visual(v, c);
		return;
	}
	u32 upos0 = t->upos;
	S.cur = *c;
	bool change = is_change(c) && !S.dotting;
	if (c->op) {
		exec_op(v, c, upos0);
	} else if (c->mot) {
		u32 old0 = v->head[0];
		bool jumped = false;
		for (u32 ci = 0; ci < v->nc; ci++) {
			Mot m;
			if (!motion(v, c, v->head[ci], false, ci, &m)) {
				fail();
				continue;
			}
			v->head[ci] = v->anchor[ci] = clampn(t, m.to);
			v->want_x[ci] = m.want;
			if (m.jump && ci == 0)
				jumped = true;
		}
		dedupe(v);
		if (jumped)
			jump_push(v, old0);
		ed_reveal(v, v->head[0], jumped);
		return;
	} else {
		exec_action(v, c, upos0);
	}
	if (change && S.mode != M_INSERT && S.mode != M_REPLACE && S.mode != M_CMDLINE &&
	    !(c->op && c->mot == '/') && t->upos != upos0) {
		S.dot = *c;
		S.dot_ok = true;
		S.dot_ins.n = 0;
	}
	if (S.view == v && (S.mode == M_NORMAL)) {
		for (u32 k = 0; k < v->nc; k++)
			v->head[k] = v->anchor[k] = clampn(t, v->head[k]);
		ed_reveal(v, v->head[0], false);
	}
}

/* ---- parsing ---------------------------------------------------------- */

/* -1 invalid, 0 incomplete, else the index after the motion */
static int parse_motion(const u32 *k, int n, int i, Cmd *c)
{
	if (i >= n)
		return 0;
	u32 m = k[i];
	if (m && m < 128 && strchr("hjklwWbBeE0^$G;,%{}HMLnN*#+-_| ", (int)m)) {
		c->mot = m;
		return i + 1;
	}
	switch (m) {
	case K_LEFT:
	case K_RIGHT:
	case K_UP:
	case K_DOWN:
	case K_HOME:
	case K_END:
	case K_BS:
	case K_RET:
		c->mot = m;
		return i + 1;
	}
	if (m == 'f' || m == 'F' || m == 't' || m == 'T' || m == '\'' || m == '`') {
		if (i + 1 >= n)
			return 0;
		if (k[i + 1] >= K_SPECIAL && k[i + 1] != K_TAB)
			return -1;
		c->mot = m;
		c->marg = k[i + 1];
		return i + 2;
	}
	if (m == 'g') {
		if (i + 1 >= n)
			return 0;
		u32 m2 = k[i + 1];
		if (m2 && m2 < 128 && strchr("geE_jk0$", (int)m2)) {
			c->mot = GP(m2);
			return i + 2;
		}
		return -1;
	}
	return -1;
}

static bool parse_count(const u32 *k, int n, int *i, u32 *out)
{
	bool any = false;
	u32 cnt = 0;
	while (*i < n && k[*i] >= '0' && k[*i] <= '9' && (any || k[*i] != '0')) {
		cnt = VX_MIN(cnt * 10 + (k[*i] - '0'), 99999u);
		any = true;
		(*i)++;
	}
	*out = cnt;
	return any;
}

static int parse(const u32 *k, int n, bool vis, Cmd *c)
{
	memset(c, 0, sizeof *c);
	int i = 0;
	if (k[0] == '"') {
		if (n < 2)
			return 0;
		if (k[1] >= K_SPECIAL)
			return -1;
		c->reg = k[1];
		i = 2;
	}
	u32 cnt;
	bool hc = parse_count(k, n, &i, &cnt);
	if (i >= n)
		return 0;
	u32 key = k[i];
	if (key == K_ESC)
		return -1;
	u32 op = 0;
	int j = i + 1;
	if (key && key < 128 && strchr("dcy<>", (int)key))
		op = key;
	else if (key == 'g') {
		if (i + 1 >= n)
			return 0;
		u32 k2 = k[i + 1];
		if (k2 == 'u' || k2 == 'U' || k2 == '~') {
			op = GP(k2);
			j = i + 2;
		}
	} else if (vis && (key == 'u' || key == 'U' || key == '~'))
		op = GP(key);
	if (op) {
		c->op = op;
		c->count = cnt;
		if (vis)
			return 1;
		u32 c2;
		if (parse_count(k, n, &j, &c2))
			c->count = (hc ? cnt : 1) * c2;
		if (j >= n)
			return 0;
		u32 m = k[j];
		if (m == K_ESC)
			return -1;
		u32 ch = op < 128 ? op : op & 0xFFFF;
		if (m == ch && (op < 128 || op == GP(ch))) {
			c->lineop = true;
			return 1;
		}
		if (op >= GP(0) && m == 'g') {
			if (j + 1 >= n)
				return 0;
			if (k[j + 1] == ch) {
				c->lineop = true;
				return 1;
			}
		}
		if (m == 'i' || m == 'a') {
			if (j + 1 >= n)
				return 0;
			c->obj = m;
			c->objc = k[j + 1];
			return obj_valid(c->objc) ? 1 : -1;
		}
		if (m == '/' || m == '?') {
			c->mot = m;
			return 1;
		}
		int r = parse_motion(k, n, j, c);
		return r <= 0 ? r : 1;
	}
	c->count = cnt;
	if (vis && (key == 'i' || key == 'a')) {
		if (i + 1 >= n)
			return 0;
		c->obj = key;
		c->objc = k[i + 1];
		return obj_valid(c->objc) ? 1 : -1;
	}
	if (key == 'r' || key == 'm' || key == 'z' || key == 'Z') {
		if (i + 1 >= n)
			return 0;
		if (k[i + 1] == K_ESC)
			return -1;
		c->act = key;
		c->aarg = k[i + 1];
		return 1;
	}
	if (key == 'g') {
		if (i + 1 >= n)
			return 0;
		if (k[i + 1] == 'v' || k[i + 1] == 'J') {
			c->act = GP(k[i + 1]);
			return 1;
		}
	}
	int r = parse_motion(k, n, i, c);
	if (r != -1)
		return r > 0 ? 1 : 0;
	if (key && key < 128 && strchr("xXsSJpPuDCY.iaIAoOvVR:/?", (int)key)) {
		c->act = key;
		return 1;
	}
	switch (key) {
	case K_DEL:
	case K_TAB:
	case K_PGUP:
	case K_PGDN:
	case CTRL('r'):
	case CTRL('o'):
	case CTRL('d'):
	case CTRL('u'):
	case CTRL('f'):
	case CTRL('b'):
	case CTRL('e'):
	case CTRL('y'):
		c->act = key;
		return 1;
	}
	if (!vis && key == '~') {
		c->act = key;
		return 1;
	}
	return -1;
}

/* ---- input ------------------------------------------------------------- */

static void feed(View *v, u32 tok)
{
	bool vis = S.mode == M_VISUAL || S.mode == M_VLINE;
	if (tok == K_ESC && S.npend == 0) {
		if (vis)
			vis_exit(v, S.vcur);
		else
			collapse(v);
		app.dirty = true;
		return;
	}
	if (S.npend >= (int)VX_COUNT(S.pend))
		S.npend = 0;
	S.pend[S.npend++] = tok;
	Cmd c;
	int r = parse(S.pend, S.npend, vis, &c);
	if (r == 0)
		return;
	S.npend = 0;
	if (r < 0)
		return;
	S.in_vim = true;
	exec(v, &c);
	S.in_vim = false;
	app.dirty = true;
}

static void feed_ins(View *v, u32 tok)
{
	app.compl_open = false;
	if (tok < K_SPECIAL) {
		char b[4];
		u32 n = utf8_encode(tok, b);
		ed_text(v, b, n);
		return;
	}
	if (tok == CTRL('w') || tok == CTRL('u')) {
		ins_kill(v, tok == CTRL('u'));
		return;
	}
	Event e = {0};
	e.type = EV_KEY;
	e.key = tok == K_RET ? SDLK_RETURN : tok == K_BS ? SDLK_BACKSPACE : tok == K_DEL ? SDLK_DELETE : SDLK_TAB;
	ed_key(v, &e);
}

static u32 key_token(const Event *e)
{
	int m = e->mods & (MOD_CTRL | MOD_ALT | MOD_SHIFT | MOD_SUPER);
	u32 k = e->key;
	if (m == MOD_CTRL && k >= 'a' && k <= 'z')
		return CTRL(k);
	if (m == MOD_CTRL && k == SDLK_LEFTBRACKET)
		return K_ESC;
	if (m & (MOD_CTRL | MOD_ALT | MOD_SUPER))
		return 0;
	switch (k) {
	case SDLK_ESCAPE:
		return K_ESC;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		return K_RET;
	case SDLK_BACKSPACE:
		return K_BS;
	case SDLK_DELETE:
		return K_DEL;
	}
	if (m)
		return 0;
	switch (k) {
	case SDLK_TAB:
		return K_TAB;
	case SDLK_LEFT:
		return K_LEFT;
	case SDLK_RIGHT:
		return K_RIGHT;
	case SDLK_UP:
		return K_UP;
	case SDLK_DOWN:
		return K_DOWN;
	case SDLK_HOME:
		return K_HOME;
	case SDLK_END:
		return K_END;
	case SDLK_PAGEUP:
		return K_PGUP;
	case SDLK_PAGEDOWN:
		return K_PGDN;
	}
	return 0;
}

/* Bring the state in line with the view: another tab, a mouse selection,
 * a cursor left past the line end by the mouse or an editor command. */
static void sync(View *v)
{
	Text *t = TX(v);
	if (S.view != v) {
		S.view = v;
		if (S.mode != M_NORMAL)
			S.mode = M_NORMAL;
		S.npend = 0;
		S.cl_has_op = false;
	}
	if (S.mode == M_NORMAL) {
		if (v->nc == 1 && v->head[0] != v->anchor[0]) {
			u32 h = v->head[0], a = v->anchor[0];
			S.mode = M_VISUAL;
			S.vstart = h > a ? a : prv(t, a);
			S.vcur = h > a ? prv(t, h) : h;
			vis_apply(v);
			return;
		}
		for (u32 c = 0; c < v->nc; c++)
			v->head[c] = v->anchor[c] = clampn(t, v->head[c]);
	} else if (S.mode == M_VISUAL || S.mode == M_VLINE) {
		if (v->nc != 1 || v->head[0] != S.set_head || v->anchor[0] != S.set_anchor) {
			if (v->nc > 1 || v->head[0] == v->anchor[0]) {
				S.mode = M_NORMAL;
				collapse(v);
				v->head[0] = v->anchor[0] = clampn(t, v->head[0]);
			} else {
				u32 h = v->head[0], a = v->anchor[0];
				S.mode = M_VISUAL;
				S.vstart = h > a ? a : prv(t, a);
				S.vcur = h > a ? prv(t, h) : h;
				vis_apply(v);
			}
		}
	}
}

bool vim_key(View *v, const Event *e)
{
	sync(v);
	u32 tok = key_token(e);
	switch (S.mode) {
	case M_INSERT:
		if (tok == K_ESC) {
			leave_insert(v);
			return true;
		}
		if (tok == CTRL('w') || tok == CTRL('u')) {
			if (!S.feeding)
				toks_push(&S.rec, tok);
			ins_kill(v, tok == CTRL('u'));
			return true;
		}
		if (!S.feeding && (tok == K_RET || tok == K_BS || tok == K_DEL || tok == K_TAB) && !app.compl_open)
			toks_push(&S.rec, tok);
		return false;
	case M_REPLACE:
		if (tok == K_ESC) {
			leave_insert(v);
			return true;
		}
		if (tok == K_BS) {
			if (!S.feeding)
				toks_push(&S.rec, tok);
			replace_bs(v);
			return true;
		}
		if (tok == K_RET && !S.feeding)
			toks_push(&S.rec, tok);
		return false;
	case M_CMDLINE:
		if (tok == K_ESC) {
			cl_cancel(v);
		} else if (tok == K_RET) {
			cl_accept(v);
		} else if (tok == K_BS) {
			if (!S.cln)
				cl_cancel(v);
			else {
				S.cln--;
				while (S.cln && utf8_cont((u8)S.cl[S.cln]))
					S.cln--;
			}
		} else if (tok == CTRL('u')) {
			S.cln = 0;
		} else if (tok == CTRL('v')) {
			char *s = ui_clipboard_get();
			for (char *p = s; p && *p && *p != '\n' && S.cln + 1 < sizeof S.cl; p++)
				S.cl[S.cln++] = *p;
			free(s);
		} else if (e->mods & (MOD_CTRL | MOD_ALT)) {
			return false;
		}
		app.dirty = true;
		return true;
	default:
		if (!tok)
			return false;
		if (tok == CTRL('v') || ((tok & 0xFFFF0000u) == CTRL(0) && tok != CTRL('r') && tok != CTRL('o') &&
		                         tok != CTRL('d') && tok != CTRL('u') && tok != CTRL('e')))
			return false;
		feed(v, tok);
		return true;
	}
}

bool vim_text(View *v, const char *s, size_t n)
{
	sync(v);
	for (size_t i = 0; i < n;) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		if (!k)
			k = 1;
		switch (S.mode) {
		case M_INSERT:
			if (!S.feeding)
				for (size_t j = i; j < n;) {
					u32 c2;
					u32 k2 = utf8_decode(s + j, n - j, &c2);
					toks_push(&S.rec, c2);
					j += k2 ? k2 : 1;
				}
			if (i == 0)
				return false; /* the editor inserts it */
			ed_text(v, s + i, n - i);
			return true;
		case M_REPLACE:
			if (!S.feeding)
				toks_push(&S.rec, cp);
			replace_char(v, cp);
			break;
		case M_CMDLINE:
			if (S.cln + k < sizeof S.cl) {
				memcpy(S.cl + S.cln, s + i, k);
				S.cln += k;
			}
			break;
		default:
			feed(v, cp);
			if (S.view != v) /* :q, :e */
				return true;
			break;
		}
		i += k;
	}
	app.dirty = true;
	return true;
}

/* Keys VSCodium binds before the editor sees them (Ctrl+D, Ctrl+U, Ctrl+[,
 * Ctrl+W, Ctrl+V) arrive as commands; vim takes the ones it owns. */
/* Keys vim owns in the current mode although VSCodium binds them (Ctrl+D,
 * Ctrl+U, Ctrl+W, Ctrl+V, Ctrl+[): ui.c skips its bindings for these, so
 * they reach vim_key as keys and the commands themselves (from the palette)
 * keep their meaning. */
bool vim_claims_key(const Event *e)
{
	if ((e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT)) != MOD_CTRL)
		return false;
	u32 k = e->key;
	if (k == SDLK_LEFTBRACKET)
		return true;
	switch (S.mode) {
	case M_INSERT: return k == SDLK_W || k == SDLK_U;
	case M_REPLACE: return false;
	case M_CMDLINE: return k == SDLK_V || k == SDLK_U;
	default: return k == SDLK_D || k == SDLK_U;
	}
}

bool vim_command(View *v, int cmd)
{
	if (S.in_vim || app.focus != FOCUS_EDITOR || (app.docs.bin[v->doc] && v->diff_left == UINT32_MAX))
		return false;
	sync(v);
	switch (S.mode) {
	case M_INSERT:
	case M_REPLACE:
		if (cmd == CMD_OUTDENT) {
			leave_insert(v);
			return true;
		}
		if (S.mode == M_INSERT && (cmd == CMD_CLOSE_EDITOR || cmd == CMD_CURSOR_UNDO)) {
			u32 tok = cmd == CMD_CLOSE_EDITOR ? CTRL('w') : CTRL('u');
			if (!S.feeding)
				toks_push(&S.rec, tok);
			ins_kill(v, tok == CTRL('u'));
			return true;
		}
		return false;
	case M_CMDLINE:
		if (cmd == CMD_OUTDENT) {
			cl_cancel(v);
			return true;
		}
		if (cmd == CMD_PASTE) {
			char *s = ui_clipboard_get();
			for (char *p = s; *p && *p != '\n' && S.cln + 1 < sizeof S.cl; p++)
				S.cl[S.cln++] = *p;
			free(s);
			return true;
		}
		return false;
	default:
		if (cmd == CMD_ADD_NEXT_MATCH || cmd == CMD_CURSOR_UNDO || cmd == CMD_OUTDENT) {
			feed(v, cmd == CMD_ADD_NEXT_MATCH ? CTRL('d') : cmd == CMD_CURSOR_UNDO ? CTRL('u') : K_ESC);
			return true;
		}
		return false;
	}
}

const char *vim_mode_label(void)
{
	switch (S.mode) {
	case M_INSERT:
		return "-- INSERT --";
	case M_VISUAL:
		return "-- VISUAL --";
	case M_VLINE:
		return "-- VISUAL LINE --";
	case M_REPLACE:
		return "-- REPLACE --";
	case M_CMDLINE:
		snprintf(S.label, sizeof S.label, "%c%.*s|", S.clk, (int)S.cln, S.cl);
		return S.label;
	default: {
		int k = snprintf(S.label, sizeof S.label, "-- NORMAL --");
		if (S.npend) {
			k += snprintf(S.label + k, sizeof S.label - (size_t)k, "  ");
			for (int i = 0; i < S.npend && k + 8 < (int)sizeof S.label; i++) {
				u32 t = S.pend[i];
				if (t < K_SPECIAL)
					k += (int)utf8_encode(t, S.label + k);
				else if ((t & 0xFFFF0000u) == CTRL(0))
					k += snprintf(S.label + k, sizeof S.label - (size_t)k, "^%c", (int)(t & 0x7F) - 32);
			}
			S.label[k] = 0;
		}
		return S.label;
	}
	}
}

bool vim_block_cursor(void)
{
	if (S.mode == M_CMDLINE)
		return S.cl_prev != M_INSERT;
	return S.mode == M_NORMAL || S.mode == M_VISUAL || S.mode == M_VLINE;
}

u32 vim_cursor_off(View *v, u32 c)
{
	bool vis = S.mode == M_VISUAL || S.mode == M_VLINE ||
	           (S.mode == M_CMDLINE && (S.cl_prev == M_VISUAL || S.cl_prev == M_VLINE));
	if (vis && c == 0 && S.view == v)
		return VX_MIN(S.vcur, TX(v)->len);
	return v->head[c];
}

void vim_reset(void)
{
	S.mode = M_NORMAL;
	S.npend = 0;
	S.view = NULL;
	S.cl_has_op = false;
	app.dirty = true;
}
