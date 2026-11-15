/* Manual pages: a reader and an apropos search (the "Manpages" top tab).
 *
 * A page is rendered by mandoc (-T utf8, no pager), its backspace
 * overstrikes become styles (c\bc bold, _\bc italic) and every name(sec)
 * becomes a link. Pages open as read-only editor tabs, so several can be
 * open and tabbed between; a click or Enter on a link opens that page,
 * like info. The editor context menu has "Open Manpage" for the word
 * under the mouse (socket( -> socket(2); C and C++ files prefer the
 * programming sections 2, 3, 9, 4). */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"
#include "menu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <SDL3/SDL.h>

static bool man_focus_title(const char *title);
static void man_add_tab(u32 d);

/* ---- running man / mandoc / apropos -------------------------------------------- */

/* Run argv (no shell), collect stdout. Returns the exit status, -1 on error. */
static int capture(char *const argv[], char **out, size_t *n)
{
	*out = NULL;
	*n = 0;
	int p[2];
	if (pipe(p) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	size_t cap = 0;
	char *b = NULL;
	for (;;) {
		if (*n + 65536 > cap) {
			cap = cap ? cap * 2 : 131072;
			b = vx_realloc(b, cap);
		}
		ssize_t k = read(p[0], b + *n, cap - *n - 1);
		if (k > 0)
			*n += (size_t)k;
		else if (k < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	close(p[0]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	if (b)
		b[*n] = 0;
	*out = b;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static bool name_ok(const char *s)
{
	if (!*s || *s == '-')
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
		      strchr("_.:+-@", *s)))
			return false;
	return true;
}

/* man -w [sec] name -> the page file */
static bool man_where(const char *name, const char *sec, char *path, size_t cap)
{
	char *argv[5] = {"man", "-w", NULL, NULL, NULL};
	if (sec && *sec) {
		argv[2] = (char *)sec;
		argv[3] = (char *)name;
	} else {
		argv[2] = (char *)name;
	}
	char *o;
	size_t n;
	int rc = capture(argv, &o, &n);
	bool ok = rc == 0 && o && n > 1 && o[0] == '/';
	if (ok) {
		size_t k = strcspn(o, "\n");
		snprintf(path, cap, "%.*s", (int)k, o);
	}
	free(o);
	return ok;
}

/* section from the file name: .../man2/socket.2.gz -> "2" */
static void sec_of_path(const char *path, char *sec, size_t cap)
{
	sec[0] = 0;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", base);
	size_t n = strlen(tmp);
	if (n > 3 && !strcmp(tmp + n - 3, ".gz"))
		tmp[n - 3] = 0;
	const char *dot = strrchr(tmp, '.');
	if (dot && dot[1])
		snprintf(sec, cap, "%s", dot + 1);
}

/* ---- overstrikes to text + styles ----------------------------------------------- */

static u32 utf8_len(u8 c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

typedef struct {
	char *t;
	u32 n, cap;
	/* spans (SoA) */
	u32 sn, scap;
	u32 *line, *col, *len;
	u8 *cls;
} Page;

static void put(Page *p, const char *s, u32 n)
{
	VX_GROW(p->t, p->cap, p->n + n + 1);
	memcpy(p->t + p->n, s, n);
	p->n += n;
}

static void span(Page *p, u32 line, u32 col, u32 len, u8 cls)
{
	if (!len)
		return;
	if (p->sn && p->line[p->sn - 1] == line && p->cls[p->sn - 1] == cls &&
	    p->col[p->sn - 1] + p->len[p->sn - 1] == col) {
		p->len[p->sn - 1] += len; /* extend the run */
		return;
	}
	if (p->sn == p->scap) {
		p->scap = p->scap ? p->scap * 2 : 1024;
		p->line = vx_realloc(p->line, p->scap * sizeof(u32));
		p->col = vx_realloc(p->col, p->scap * sizeof(u32));
		p->len = vx_realloc(p->len, p->scap * sizeof(u32));
		p->cls = vx_realloc(p->cls, p->scap);
	}
	p->line[p->sn] = line;
	p->col[p->sn] = col;
	p->len[p->sn] = len;
	p->cls[p->sn] = cls;
	p->sn++;
}

static bool is_name_ch(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
	       c == '-' || c == ':' || c == '+';
}

/* A reference name(sec) ending at the ')' at s[i]; start/len of the whole. */
static bool ref_at(const char *s, u32 n, u32 open, u32 *a, u32 *b)
{
	/* s[open] == '(' : digits then optional letters, then ')' */
	u32 k = open + 1;
	if (k >= n || !(s[k] >= '0' && s[k] <= '9'))
		return false;
	k++;
	while (k < n && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z')) && k - open < 6)
		k++;
	if (k >= n || s[k] != ')')
		return false;
	u32 st = open;
	while (st > 0 && is_name_ch(s[st - 1]))
		st--;
	while (st < open && (s[st] == '.' || s[st] == '-' || s[st] == ':'))
		st++; /* not a leading punctuation */
	if (st == open)
		return false;
	*a = st;
	*b = k + 1;
	return true;
}

/* Links on one finished line: name(sec) spans, appended after the styles
 * of that line so they win. */
static void line_links(Page *p, u32 line, u32 ls)
{
	const char *s = p->t + ls;
	u32 n = p->n - ls;
	for (u32 i = 0; i < n; i++)
		if (s[i] == '(') {
			u32 a, b;
			if (ref_at(s, n, i, &a, &b)) {
				span(p, line, a, b - a, TK_LINK);
				i = b - 1;
			}
		}
}

static void parse_page(Page *p, const char *in, size_t n)
{
	u32 line = 0, ls = 0;
	for (size_t i = 0; i < n;) {
		u8 c = (u8)in[i];
		if (c == '\n') {
			line_links(p, line, ls);
			put(p, "\n", 1);
			line++;
			ls = p->n;
			i++;
			continue;
		}
		u32 cl = utf8_len(c);
		if (i + cl > n)
			break;
		/* x \b y [\b y]...: an overstruck cell */
		size_t j = i + cl;
		u8 style = 0; /* 1 bold, 2 italic */
		const char *glyph = in + i;
		u32 gl = cl;
		while (j < n && in[j] == '\b' && j + 1 < n) {
			u32 l2 = utf8_len((u8)in[j + 1]);
			if (j + 1 + l2 > n)
				break;
			if (gl == 1 && glyph[0] == '_' && l2 == 1 && in[j + 1] == '_') {
				/* "_\b_": an italic or a bold underscore; follow the cell before */
				u32 c0 = p->n - ls;
				bool it = p->sn && p->line[p->sn - 1] == line && p->cls[p->sn - 1] == TK_ITALIC &&
				          p->col[p->sn - 1] + p->len[p->sn - 1] == c0;
				style |= it ? 2 : 1;
			} else if (gl == 1 && glyph[0] == '_')
				style |= 2;
			else if (l2 == gl && !memcmp(glyph, in + j + 1, gl) && !(gl == 1 && glyph[0] == '_'))
				style |= 1;
			else if (l2 == 1 && in[j + 1] == '_')
				style |= 2; /* c\b_ */
			if (!(l2 == 1 && in[j + 1] == '_'))
				glyph = in + j + 1, gl = l2;
			j += 1 + l2;
		}
		u32 col = p->n - ls;
		put(p, glyph, gl);
		if (style)
			span(p, line, col, gl, (style & 1) ? (col == 0 || (p->sn && p->line[p->sn - 1] == line && p->cls[p->sn - 1] == TK_HEADING && p->col[p->sn - 1] == 0) ? TK_HEADING : TK_BOLD) : TK_ITALIC);
		i = j;
	}
	if (p->n > ls)
		line_links(p, line, ls);
	/* a bold run that started a line but ends early (".Nm" in the
	 * synopsis) is plain bold, not a heading: headings are the whole line */
	for (u32 k = 0; k < p->sn; k++)
		if (p->cls[k] == TK_HEADING) {
			u32 L = p->line[k];
			const char *s = p->t;
			u32 o = 0;
			for (u32 l = 0; l < L && o < p->n; o++)
				if (s[o] == '\n')
					l++;
			u32 e = o;
			while (e < p->n && s[e] != '\n')
				e++;
			if (p->col[k] + p->len[k] < e - o)
				p->cls[k] = TK_BOLD;
		}
}

/* ---- opening pages ------------------------------------------------------------- */

static int page_width(void)
{
	/* the workspace spans the window, less the gutter */
	int cols = app.char_w ? (app.w - PX(120)) / app.char_w : 80;
	return VX_CLAMP(cols, 50, 120);
}

/* Open name(sec) in a tab (or focus it). sec may be NULL; prefer_c tries
 * the programming sections first. */
bool man_open(const char *name, const char *sec, bool prefer_c)
{
	if (!name_ok(name) || (sec && *sec && !name_ok(sec))) {
		ui_status("Not a manual page name: %s", name);
		return false;
	}
	char path[4096] = "";
	bool found = false;
	if (sec && *sec)
		found = man_where(name, sec, path, sizeof path);
	if (!found && prefer_c) {
		static const char *const secs[] = {"2", "3", "9", "4"};
		for (size_t i = 0; i < VX_COUNT(secs) && !found; i++)
			found = man_where(name, secs[i], path, sizeof path);
	}
	if (!found)
		found = man_where(name, NULL, path, sizeof path);
	if (!found) {
		ui_status("No manual entry for %s%s%s%s", name, sec && *sec ? "(" : "", sec && *sec ? sec : "",
		          sec && *sec ? ")" : "");
		return false;
	}
	char s[32], title[300];
	sec_of_path(path, s, sizeof s);
	snprintf(title, sizeof title, "%s(%s)", name, s);
	Docs *D = &app.docs;
	if (man_focus_title(title))
		return true;
	char wopt[32];
	snprintf(wopt, sizeof wopt, "width=%d", page_width());
	char *argv[] = {"mandoc", "-T", "utf8", "-O", wopt, path, NULL};
	char *out;
	size_t n;
	capture(argv, &out, &n);
	if (!out || !n) {
		free(out);
		ui_status("mandoc could not format %s", title);
		return false;
	}
	Page p = {0};
	parse_page(&p, out, n);
	free(out);
	u32 d = doc_new_virtual(title, p.t ? p.t : "", p.n, LANG_NONE);
	if (d != UINT32_MAX) {
		D->man[d] = 1;
		VX_GROW(D->s_line[d], D->scap[d], p.sn + 1);
		D->s_col[d] = vx_realloc(D->s_col[d], D->scap[d] * sizeof(u32));
		D->s_len[d] = vx_realloc(D->s_len[d], D->scap[d] * sizeof(u32));
		D->s_cls[d] = vx_realloc(D->s_cls[d], D->scap[d]);
		if (p.sn) {
			memcpy(D->s_line[d], p.line, p.sn * sizeof(u32));
			memcpy(D->s_col[d], p.col, p.sn * sizeof(u32));
			memcpy(D->s_len[d], p.len, p.sn * sizeof(u32));
			memcpy(D->s_cls[d], p.cls, p.sn);
		}
		D->sn[d] = p.sn;
		man_add_tab(d);
	}
	free(p.t);
	free(p.line);
	free(p.col);
	free(p.len);
	free(p.cls);
	return d != UINT32_MAX;
}

/* The name(sec) reference covering byte off of a man page, if any. */
static bool link_at(u32 d, u32 off, char *name, size_t nc, char *sec, size_t sc)
{
	Text *t = &app.docs.text[d];
	u32 l = text_line_of(t, off);
	u32 n;
	const char *s = text_line(t, l, &n);
	u32 col = off - text_line_start(t, l);
	for (u32 i = 0; i < n; i++)
		if (s[i] == '(') {
			u32 a, b;
			if (ref_at(s, n, i, &a, &b)) {
				if (col >= a && col < b) {
					snprintf(name, nc, "%.*s", (int)(i - a), s + a);
					snprintf(sec, sc, "%.*s", (int)(b - 1 - (i + 1)), s + i + 1);
					return true;
				}
				i = b - 1;
			}
		}
	return false;
}

/* A click or Enter on a link of a man page: open it. */
bool man_follow(u32 d, u32 off)
{
	char name[256], sec[32];
	if (!app.docs.man[d] || !link_at(d, off, name, sizeof name, sec, sizeof sec))
		return false;
	man_open(name, sec, false);
	return true;
}

/* Editor context menu: the word at off ("socket" in "socket(", or a whole
 * "socket(2)" reference). */
bool man_word_at(u32 d, u32 off, char *name, size_t nc, char *sec, size_t sc)
{
	Text *t = &app.docs.text[d];
	if (off > t->len)
		return false;
	u32 l = text_line_of(t, off);
	u32 n;
	const char *s = text_line(t, l, &n);
	u32 col = off - text_line_start(t, l);
	u32 a = col, b = col;
	while (a > 0 && (is_name_ch(s[a - 1]) && s[a - 1] != '.'))
		a--;
	while (b < n && is_name_ch(s[b]) && s[b] != '.' && s[b] != ':')
		b++;
	while (a < b && (s[a] == '-' || s[a] == '+'))
		a++;
	if (a == b || b - a >= nc)
		return false;
	snprintf(name, nc, "%.*s", (int)(b - a), s + a);
	sec[0] = 0;
	u32 x, y;
	if (b < n && s[b] == '(' && ref_at(s, n, b, &x, &y))
		snprintf(sec, sc, "%.*s", (int)(y - 1 - (b + 1)), s + b + 1);
	return true;
}

void man_open_at(View *v, u32 off)
{
	char name[256], sec[32];
	if (!man_word_at(v->doc, off, name, sizeof name, sec, sizeof sec)) {
		ui_status("No word to look up");
		return;
	}
	const char *id = syn_lang_lsp_id(app.docs.lang[v->doc]);
	bool c = id && (!strcmp(id, "c") || !strcmp(id, "cpp"));
	man_open(name, sec, c && !sec[0]);
}

/* ---- the Manpages workspace ------------------------------------------------------
 *
 *  [ search manual pages (apropos)            ]
 *  | Index | socket(2) x | bind(2) x |
 *  page text (the editor's renderer, read-only)
 *
 * "Index" lists every page (or the apropos matches of the search) as
 * links; it is always there and cannot be closed. */

#define MAN_TABS 64

static Input man_in;
static u64 man_due;       /* run the search at this time (typing debounce) */
static char man_last[512] = "\x01"; /* query the Index shows; \x01 = not built */
static View *mv[MAN_TABS]; /* [0] = Index */
static u32 mn, mact;
static Group man_g;        /* ed_draw wants a group; this one is never in app.groups */

static View *view_new(u32 d)
{
	View *v = vx_alloc(sizeof *v);
	v->diff_left = UINT32_MAX;
	v->doc = d;
	v->nc = 1;
	v->want_x[0] = -1;
	app.docs.refs[d]++;
	return v;
}

static void view_free(View *v)
{
	u32 d = v->doc;
	free(v);
	doc_release(d);
}

/* The Index text: "name(sec)   description" per page, links on the names. */
static void build_index(const char *q)
{
	snprintf(man_last, sizeof man_last, "%s", q);
	char *argv[] = {"apropos", "--", (char *)(*q ? q : "."), NULL};
	char *o;
	size_t n;
	capture(argv, &o, &n);
	Page p = {0};
	u32 line = 0, count = 0;
	const char *hdr = *q ? "Manual pages matching \"" : "All manual pages";
	put(&p, hdr, (u32)strlen(hdr));
	if (*q) {
		put(&p, q, (u32)strlen(q));
		put(&p, "\"", 1);
	}
	span(&p, 0, 0, p.n, TK_HEADING);
	put(&p, "\n\n", 2);
	line = 2;
	for (char *l = o; l && *l;) {
		char *eol = strchr(l, '\n');
		if (eol)
			*eol = 0;
		char *dash = strstr(l, " - ");
		const char *desc = dash ? dash + 3 : "";
		char *names_end = dash ? dash : l + strlen(l);
		/* "a, b(2)" or "a(2), b(3)": one row per name */
		char *last_op = NULL;
		for (char *c = l; c < names_end; c++)
			if (*c == '(')
				last_op = c;
		char *last_cp = last_op ? strchr(last_op, ')') : NULL;
		if (last_op && last_cp && last_cp <= names_end) {
			char gsec[32];
			u32 gl = (u32)(last_cp - last_op - 1);
			snprintf(gsec, sizeof gsec, "%.*s", (int)gl, last_op + 1);
			gsec[strcspn(gsec, ", ")] = 0;
			for (char *c = l; c < names_end;) {
				while (c < names_end && (*c == ' ' || *c == ','))
					c++;
				char *e = c;
				while (e < names_end && *e != ',' && *e != '(' && *e != ' ')
					e++;
				if (e == c)
					break;
				char sec[32];
				snprintf(sec, sizeof sec, "%s", gsec);
				char *nx = e;
				if (*e == '(') {
					char *cp = strchr(e, ')');
					if (cp && cp <= names_end) {
						snprintf(sec, sizeof sec, "%.*s", (int)(cp - e - 1), e + 1);
						sec[strcspn(sec, ", ")] = 0;
						nx = cp + 1;
					}
				}
				u32 ls = p.n;
				char nm[300];
				int k = snprintf(nm, sizeof nm, "%.*s(%s)", (int)(e - c), c, sec);
				put(&p, nm, (u32)k);
				for (int pad = k; pad < 32; pad++)
					put(&p, " ", 1);
				put(&p, " ", 1);
				u32 dc = p.n - ls;
				put(&p, desc, (u32)strlen(desc));
				span(&p, line, dc, (u32)strlen(desc), TK_COMMENT);
				line_links(&p, line, ls);
				put(&p, "\n", 1);
				line++;
				count++;
				c = nx;
				while (c < names_end && *c != ',')
					c++;
			}
		}
		l = eol ? eol + 1 : NULL;
	}
	free(o);
	if (!count) {
		const char *m = "Nothing found.";
		put(&p, m, (u32)strlen(m));
	}
	/* spans must be in line order for the renderer's binary search: the
	 * description span of a line comes before its link, both on that line,
	 * so the order holds */
	char title[] = "Index";
	u32 d = doc_new_virtual(title, p.t ? p.t : "", p.n, LANG_NONE);
	if (d != UINT32_MAX) {
		Docs *D = &app.docs;
		D->man[d] = 1;
		VX_GROW(D->s_line[d], D->scap[d], p.sn + 1);
		D->s_col[d] = vx_realloc(D->s_col[d], D->scap[d] * sizeof(u32));
		D->s_len[d] = vx_realloc(D->s_len[d], D->scap[d] * sizeof(u32));
		D->s_cls[d] = vx_realloc(D->s_cls[d], D->scap[d]);
		if (p.sn) {
			memcpy(D->s_line[d], p.line, p.sn * sizeof(u32));
			memcpy(D->s_col[d], p.col, p.sn * sizeof(u32));
			memcpy(D->s_len[d], p.len, p.sn * sizeof(u32));
			memcpy(D->s_cls[d], p.cls, p.sn);
		}
		D->sn[d] = p.sn;
		View *nv = view_new(d);
		if (mn == 0) {
			mv[0] = nv;
			mn = 1;
		} else {
			view_free(mv[0]);
			mv[0] = nv;
		}
	}
	free(p.t);
	free(p.line);
	free(p.col);
	free(p.len);
	free(p.cls);
	app.dirty = true;
}

static void ensure_index(void)
{
	if (man_last[0] == '\x01')
		build_index("");
}

static bool man_focus_title(const char *title)
{
	for (u32 i = 1; i < mn; i++)
		if (!strcmp(app.docs.rel[mv[i]->doc], title)) {
			mact = i;
			app.side_view = SIDE_MAN;
			app.focus = FOCUS_WORKSPACE;
			return true;
		}
	return false;
}

static void man_add_tab(u32 d)
{
	ensure_index();
	if (mn == MAN_TABS) { /* drop the oldest page */
		view_free(mv[1]);
		memmove(mv + 1, mv + 2, (MAN_TABS - 2) * sizeof(View *));
		mn--;
	}
	/* new tabs open right of the active one, like the editor's */
	u32 at = mact + 1;
	memmove(mv + at + 1, mv + at, (mn - at) * sizeof(View *));
	mv[at] = view_new(d);
	mn++;
	mact = at;
	app.side_view = SIDE_MAN;
	app.focus = FOCUS_WORKSPACE;
	app.dirty = true;
}

static void close_tab_at(u32 i)
{
	if (i == 0 || i >= mn)
		return; /* the Index stays */
	view_free(mv[i]);
	memmove(mv + i, mv + i + 1, (mn - i - 1) * sizeof(View *));
	mn--;
	if (mact >= i && mact > 0)
		mact--;
	if (mact >= mn)
		mact = mn - 1;
}

View *man_active_view(void)
{
	ensure_index();
	return mn ? mv[mact] : NULL;
}

static int search_h(void) { return PX(44); }

static int mtab_w(u32 i)
{
	const char *t = i == 0 ? "Index" : app.docs.rel[mv[i]->doc];
	return PX(12) + font_width(FONT_UI, t, strlen(t)) + (i ? PX(36) : PX(12));
}

/* tab rows wrap like the editor's */
static int mtab_layout(int w, u32 want, int *ox, int *oy)
{
	int x = 0, row = 0;
	for (u32 i = 0; i < mn; i++) {
		int tw = mtab_w(i);
		if (x + tw > w && x > 0) {
			x = 0;
			row++;
		}
		if (i == want) {
			*ox = x;
			*oy = row * app.tabs_h;
		}
		x += tw;
	}
	return row + 1;
}

static int content_y(int y, int w)
{
	int ox, oy;
	return y + search_h() + mtab_layout(w, UINT32_MAX, &ox, &oy) * app.tabs_h;
}

void man_ws_draw(int x, int y, int w, int h)
{
	ensure_index();
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	/* search bar */
	draw_rect(x, y, w, search_h(), COL(C_SIDEBAR_BG));
	const char *lbl = "MANPAGES";
	draw_text(FONT_SMALL, x + PX(20), y + (search_h() - font_m[FONT_SMALL].line) / 2, lbl, strlen(lbl),
	          COL(C_SIDEBAR_TITLE_FG));
	int bx = x + PX(110), bw = VX_MIN(w - PX(130), PX(640));
	int ih = input_height(&man_in, bw, 0);
	input_draw_ex(&man_in, bx, y + (search_h() - ih) / 2, bw, 0, "Search manual pages (apropos)",
	              app.focus == FOCUS_MAN_INPUT);
	/* tabs */
	int ty0 = y + search_h();
	int cy = content_y(y, w);
	draw_rect(x, ty0, w, cy - ty0, COL(C_TABS_BG));
	for (u32 i = 0; i < mn; i++) {
		int ox, oy;
		mtab_layout(w, i, &ox, &oy);
		int tw = mtab_w(i), tx = x + ox, ty = ty0 + oy;
		bool act = i == mact;
		draw_rect(tx, ty, tw, app.tabs_h, act ? COL(C_TAB_ACTIVE_BG) : COL(C_TAB_INACTIVE_BG));
		draw_rect(tx + tw - PX(1), ty, PX(1), app.tabs_h, COL(C_TAB_BORDER));
		if (act)
			draw_rect(tx, ty, tw, PX(1), COL(C_TAB_ACTIVE_BORDER_TOP));
		const char *t = i == 0 ? "Index" : app.docs.rel[mv[i]->doc];
		u32 fg = act ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG);
		draw_text(FONT_UI, tx + PX(12), ty + (app.tabs_h - font_m[FONT_UI].line) / 2, t, strlen(t), fg);
		bool hover = app.hover_x >= tx && app.hover_x < tx + tw && app.hover_y >= ty && app.hover_y < ty + app.tabs_h;
		if (i && (act || hover))
			draw_icon(ICON_CLOSE, tx + tw - PX(28), ty + (app.tabs_h - PX(16)) / 2, fg);
	}
	draw_rect(x, cy - PX(1), w, PX(1), COL(C_TABS_BORDER));
	if (!mn)
		return;
	man_g.x = x;
	man_g.y = cy;
	man_g.w = w;
	man_g.h = y + h - cy;
	ed_draw(&man_g, mv[mact], x, cy, w, y + h - cy, app.focus == FOCUS_WORKSPACE);
}

static void run_search_now(void)
{
	man_due = 0;
	char q[512];
	snprintf(q, sizeof q, "%.*s", (int)man_in.n, man_in.b ? man_in.b : "");
	if (strcmp(q, man_last) != 0) {
		build_index(q);
		mact = 0;
	}
}

void man_tick(void)
{
	if (man_due && app.now >= man_due) {
		run_search_now();
		app.dirty = true;
	}
}

u64 man_wait_hint(void) { return man_due ? 30 : 1000; }

/* Keys the workspace takes before the global bindings. */
bool man_ws_key(const Event *e)
{
	int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
	ensure_index();
	if (mods == MOD_CTRL && e->key == SDLK_W) {
		close_tab_at(mact);
		return true;
	}
	if ((mods == MOD_CTRL && (e->key == SDLK_TAB || e->key == SDLK_PAGEDOWN)) && mn) {
		mact = (mact + 1) % mn;
		return true;
	}
	if (((mods == (MOD_CTRL | MOD_SHIFT) && e->key == SDLK_TAB) || (mods == MOD_CTRL && e->key == SDLK_PAGEUP)) &&
	    mn) {
		mact = (mact + mn - 1) % mn;
		return true;
	}
	if (mods == MOD_CTRL && (e->key == SDLK_F || e->key == SDLK_L)) {
		app.focus = FOCUS_MAN_INPUT;
		man_in.anchor = 0;
		man_in.cur = man_in.n;
		return true;
	}
	if (app.focus == FOCUS_MAN_INPUT) {
		if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
			run_search_now();
			/* a page with exactly this name opens directly, like man(1) */
			char q[512];
			snprintf(q, sizeof q, "%.*s", (int)man_in.n, man_in.b ? man_in.b : "");
			char name[256], sec[32] = "";
			if (sscanf(q, "%255[^( ](%31[^)])", name, sec) >= 1 && name_ok(name)) {
				char path[4096];
				if (man_where(name, sec[0] ? sec : NULL, path, sizeof path))
					man_open(name, sec[0] ? sec : NULL, false);
				else
					app.focus = FOCUS_WORKSPACE;
			} else {
				app.focus = FOCUS_WORKSPACE;
			}
			return true;
		}
		if (e->key == SDLK_ESCAPE || e->key == SDLK_DOWN) {
			app.focus = FOCUS_WORKSPACE;
			return true;
		}
		return false;
	}
	if (app.focus == FOCUS_WORKSPACE && mn && !mods && (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER)) {
		man_follow(mv[mact]->doc, mv[mact]->head[0]);
		return true;
	}
	return false;
}

/* Everything else: mouse over the workspace, keys and text by focus. */
void man_ws_event(const Event *e, int x, int y, int w, int h)
{
	ensure_index();
	app.dirty = true;
	int bx = x + PX(110), bw = VX_MIN(w - PX(130), PX(640));
	int ih = input_height(&man_in, bw, 0);
	int iy = y + (search_h() - ih) / 2;
	int ty0 = y + search_h();
	int cy = content_y(y, w);
	if (e->type == EV_KEY) {
		if (app.focus == FOCUS_MAN_INPUT) {
			u32 before = man_in.n;
			input_key(&man_in, e);
			if (man_in.n != before)
				man_due = vx_now_ns() + 250000000ull;
		} else if (mn) {
			ed_key(mv[mact], e);
		}
		return;
	}
	if (e->type == EV_TEXT) {
		/* typing anywhere in the workspace searches */
		if (app.focus != FOCUS_MAN_INPUT) {
			app.focus = FOCUS_MAN_INPUT;
			input_set(&man_in, "", 0);
		}
		input_text(&man_in, e->text, strlen(e->text));
		man_due = vx_now_ns() + 250000000ull;
		return;
	}
	if (e->type == EV_MDOWN && e->y < ty0) {
		if (e->x >= bx && e->x < bx + bw && e->y >= iy && e->y < iy + ih) {
			app.focus = FOCUS_MAN_INPUT;
			input_click_ex(&man_in, bx, iy, bw, 0, e->x, e->y, e->clicks);
		}
		return;
	}
	if (e->type == EV_MDOWN && e->y < cy) {
		for (u32 i = 0; i < mn; i++) {
			int ox, oy;
			mtab_layout(w, i, &ox, &oy);
			int tw = mtab_w(i), tx = x + ox, tyy = ty0 + oy;
			if (e->x >= tx && e->x < tx + tw && e->y >= tyy && e->y < tyy + app.tabs_h) {
				if (i && (e->button == 2 || e->x >= tx + tw - PX(30)))
					close_tab_at(i);
				else
					mact = i;
				app.focus = FOCUS_WORKSPACE;
				return;
			}
		}
		return;
	}
	if (!mn || e->y < cy)
		return;
	View *v = mv[mact];
	if (e->type == EV_MDOWN && e->button == 1) {
		app.focus = FOCUS_WORKSPACE;
		if (!(e->mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT))) {
			u32 off = ed_offset_at(v, e->x, e->y, x, cy);
			if (man_follow(v->doc, off))
				return;
		}
	}
	ed_mouse(v, e, x, cy, w, y + h - cy);
}

/* Right click in a page: copy, or look up the word under the mouse. */
bool man_ws_context(int mx, int my, int x, int y, int w)
{
	int cy = content_y(y, w);
	if (!mn || my < cy)
		return true;
	View *v = mv[mact];
	app.ctx = CTX_EDITOR;
	app.ctx_off = ed_offset_at(v, mx, my, x, cy);
	menu_open(mx, my);
	menu_item("Open Manpage", CMD_OPEN_MANPAGE, true);
	menu_sep();
	menu_item("Copy", CMD_COPY, v->head[0] != v->anchor[0]);
	menu_item("Select All", CMD_SELECT_ALL, true);
	return true;
}

/* A page closed from outside (doc released): nothing to do, views hold refs. */
void man_shutdown(void)
{
	for (u32 i = 0; i < mn; i++)
		view_free(mv[i]);
	mn = mact = 0;
	man_last[0] = '\x01';
	man_last[1] = 0;
}
