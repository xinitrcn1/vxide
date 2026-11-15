/* Workbench: layout, input routing, keybindings, commands, documents,
 * tabs, status bar, quick input, and the LSP/git plumbing. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>

App app;

enum { DRAG_NONE, DRAG_SIDE, DRAG_PANEL, DRAG_EDITOR, DRAG_GROUP };

/* ---- messages ----------------------------------------------------------- */

void ui_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(app.status_msg, sizeof app.status_msg, fmt, ap);
	va_end(ap);
	app.status_until = vx_now_ns() + 5000000000ull;
	app.dirty = true;
}

void ui_log(const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	n = VX_MIN(n, (int)sizeof buf - 1);
	/* split into lines */
	char *s = buf;
	while (s < buf + n) {
		char *e = memchr(s, '\n', (size_t)(buf + n - s));
		u32 l = e ? (u32)(e - s) : (u32)(buf + n - s);
		VX_GROW(app.out_lines, app.out_cap, app.out_n + 1);
		app.out_lines[app.out_n++] = pool_add(&app.output, s, l);
		s += l + 1;
	}
	if (getenv("VXIDE_LOG"))
		fprintf(stderr, "%s\n", buf);
	app.dirty = true;
}

/* ---- clipboard ------------------------------------------------------------ */

char *ui_clipboard_get(void)
{
	if (!app.headless && SDL_HasClipboardText()) {
		char *s = SDL_GetClipboardText();
		char *r = vx_strdup(s ? s : "");
		SDL_free(s);
		return r;
	}
	return vx_strdup(app.clip ? app.clip : "");
}

void ui_clipboard_set(const char *s, size_t n)
{
	free(app.clip);
	app.clip = vx_strndup(s, n);
	if (!app.headless)
		SDL_SetClipboardText(app.clip);
}

/* ---- documents ------------------------------------------------------------ */

static const char *rel_of(const char *abs)
{
	size_t n = strlen(app.root);
	if (strncmp(abs, app.root, n) == 0 && abs[n] == '/')
		return abs + n + 1;
	return abs;
}

static void lsp_sync_open(u32 d)
{
	Docs *D = &app.docs;
	if (!app.lsp || D->untitled[d] || D->lsp_open[d])
		return;
	const char *id = syn_lang_lsp_id(D->lang[d]);
	if (!id || strcmp(id, "c") != 0)
		return; /* clangd serves C here */
	Text *t = &D->text[d];
	D->lsp_ver[d] = 1;
	lsp_did_open(app.lsp, D->path[d], id, D->lsp_ver[d], text_flat(t), t->len);
	D->lsp_open[d] = true;
	D->lsp_sent[d] = t->version;
	D->sem_req[d] = -1;
	D->sem_for[d] = UINT32_MAX;
}

u32 doc_open(const char *abs)
{
	Docs *D = &app.docs;
	for (u32 i = 0; i < D->n; i++)
		if (D->path[i] && strcmp(D->path[i], abs) == 0)
			return i;
	/* reuse a free slot */
	u32 d = UINT32_MAX;
	for (u32 i = 0; i < D->n; i++)
		if (!D->path[i] && !D->untitled[i]) {
			d = i;
			break;
		}
	if (d == UINT32_MAX) {
		if (D->n == MAX_DOCS) {
			ui_status("Too many open files");
			return UINT32_MAX;
		}
		d = D->n++;
	}
	if (text_open(&D->text[d], abs) != 0) {
		ui_status("Cannot open %s", rel_of(abs));
		if (d == D->n - 1)
			D->n--;
		return UINT32_MAX;
	}
	D->path[d] = vx_strdup(abs);
	D->rel[d] = vx_strdup(rel_of(abs));
	Text *t = &D->text[d];
	char head[512];
	u32 hn = VX_MIN(t->len, (u32)sizeof head);
	text_copy(t, 0, hn, head);
	D->lang[d] = (u8)syn_lang_for_path(abs, head, hn);
	D->untitled[d] = 0;
	{
		/* detectIndentation: lines indented with a tab vs spaces */
		u32 tabs = 0, spaces = 0, nl = VX_MIN(text_lines(t), 2000u);
		for (u32 l = 0; l < nl; l++) {
			u8 c = text_byte(t, text_line_start(t, l));
			tabs += c == '\t';
			spaces += c == ' ' && text_byte(t, text_line_start(t, l) + 1) == ' ';
		}
		D->tabs[d] = tabs > spaces;
	}
	D->refs[d] = 0;
	D->lstate_ok[d] = 0;
	VX_GROW(D->lstate[d], D->lstate_cap[d], 1);
	D->lstate[d][0] = 0;
	D->lsp_open[d] = false;
	D->dn[d] = 0;
	D->sn[d] = 0;
	D->gm_n[d] = 0;
	D->gm_for[d] = UINT32_MAX;
	D->gm_due_ns[d] = 0;
	lsp_sync_open(d);
	return d;
}

u32 doc_new_untitled(void)
{
	Docs *D = &app.docs;
	u32 d = D->n < MAX_DOCS ? D->n++ : UINT32_MAX;
	if (d == UINT32_MAX)
		return d;
	text_init(&D->text[d]);
	int k = 1;
	for (u32 i = 0; i < D->n; i++)
		if (D->untitled[i])
			k++;
	char name[32];
	snprintf(name, sizeof name, "Untitled-%d", k);
	D->path[d] = NULL;
	D->rel[d] = vx_strdup(name);
	D->lang[d] = LANG_NONE;
	D->untitled[d] = 1;
	D->refs[d] = 0;
	D->lstate_ok[d] = 0;
	VX_GROW(D->lstate[d], D->lstate_cap[d], 1);
	D->lstate[d][0] = 0;
	D->dn[d] = D->sn[d] = D->gm_n[d] = 0;
	D->lsp_open[d] = false;
	return d;
}

void doc_release(u32 d)
{
	Docs *D = &app.docs;
	if (--D->refs[d] > 0)
		return;
	if (D->lsp_open[d] && app.lsp)
		lsp_did_close(app.lsp, D->path[d]);
	D->lsp_open[d] = false;
	text_free(&D->text[d]);
	free(D->path[d]);
	free(D->rel[d]);
	D->path[d] = NULL;
	D->rel[d] = NULL;
	D->untitled[d] = 0;
	D->dn[d] = D->sn[d] = D->gm_n[d] = 0;
	pool_clear(&D->d_pool[d]);
}

static void git_marks_due(u32 d, u64 delay)
{
	app.docs.gm_due_ns[d] = vx_now_ns() + delay;
}

bool doc_save(u32 d)
{
	Docs *D = &app.docs;
	if (D->untitled[d]) {
		/* Save As: prompt for a path relative to the project. */
		app.quick = QUICK_PROMPT;
		app.quick_prompt_kind = 1;
		app.quick_arg = d;
		snprintf(app.quick_title, sizeof app.quick_title, "Save As (path relative to %s)",
		         app.tree.root);
		input_set(&app.quick_in, "", 0);
		app.focus = FOCUS_QUICK;
		return false;
	}
	if (text_save(&D->text[d], D->path[d]) != 0) {
		ui_status("Failed to save '%s'", D->rel[d]);
		return false;
	}
	if (D->lsp_open[d] && app.lsp)
		lsp_did_save(app.lsp, D->path[d]);
	app.git_ns = 0; /* refresh status soon */
	git_marks_due(d, 0);
	app.tree_scan_ns = 0;
	return true;
}

/* Shift line-anchored data (diagnostics, semantic tokens) after an edit
 * that turned lines [L, L + removed] into [L, L + added]. */
static void shift_lines(u32 d, u32 L, u32 removed, u32 added)
{
	Docs *D = &app.docs;
	i64 delta = (i64)added - (i64)removed;
	u32 k = 0;
	for (u32 i = 0; i < D->sn[d]; i++) {
		u32 l = D->s_line[d][i];
		if (l >= L && l <= L + removed)
			continue; /* edited lines: drop until the next refresh */
		if (l > L + removed)
			l = (u32)((i64)l + delta);
		D->s_line[d][k] = l;
		D->s_col[d][k] = D->s_col[d][i];
		D->s_len[d][k] = D->s_len[d][i];
		D->s_cls[d][k] = D->s_cls[d][i];
		k++;
	}
	D->sn[d] = k;
	if (delta == 0)
		return;
	for (u32 i = 0; i < D->dn[d]; i++) {
		if (D->d_line0[d][i] > L + removed)
			D->d_line0[d][i] = (u32)((i64)D->d_line0[d][i] + delta);
		else if (D->d_line0[d][i] > L)
			D->d_line0[d][i] = L;
		if (D->d_line1[d][i] > L + removed)
			D->d_line1[d][i] = (u32)((i64)D->d_line1[d][i] + delta);
		else if (D->d_line1[d][i] > L)
			D->d_line1[d][i] = L;
	}
	if (D->gm_n[d]) {
		/* keep marks roughly aligned until the next quick diff */
		u32 n = D->gm_n[d];
		u32 nn = (u32)VX_MAX((i64)n + delta, (i64)1);
		VX_GROW(D->gm[d], D->gm_cap[d], VX_MAX(n, nn));
		if (delta > 0) {
			memmove(D->gm[d] + L + 1 + delta, D->gm[d] + L + 1, n - VX_MIN(n, L + 1));
			memset(D->gm[d] + L + 1, GM_ADDED, (size_t)delta);
		} else if (L + 1 - delta <= n) {
			memmove(D->gm[d] + L + 1, D->gm[d] + L + 1 - delta, n - (L + 1 - delta));
		}
		D->gm_n[d] = nn;
	}
}

/* Every text change goes through here: keeps cursors of all views,
 * highlighter states, diagnostics and tokens in step. */
void doc_replace(u32 d, u32 pos, u32 del, const char *ins, u32 nins, u32 group)
{
	Docs *D = &app.docs;
	Text *t = &D->text[d];
	if (pos > t->len)
		pos = t->len;
	if (pos + del > t->len)
		del = t->len - pos;
	u32 L = text_line_of(t, pos);
	u32 L1 = text_line_of(t, pos + del);
	text_replace(t, pos, del, ins, nins, group);
	u32 added = 0;
	for (u32 i = 0; i < nins; i++)
		added += ins[i] == '\n';
	if (D->lstate_ok[d] > L)
		D->lstate_ok[d] = L;
	shift_lines(d, L, L1 - L, added);
	i64 delta = (i64)nins - (i64)del;
	for (u32 g = 0; g < app.ngroups; g++)
		for (u32 k = 0; k < app.groups[g].nt; k++) {
			View *v = app.groups[g].tab[k];
			if (v->doc != d)
				continue;
			for (u32 c = 0; c < v->nc; c++) {
				if (v->head[c] >= pos + del)
					v->head[c] = (u32)((i64)v->head[c] + delta);
				else if (v->head[c] > pos)
					v->head[c] = pos + nins;
				if (v->anchor[c] >= pos + del && !(v->anchor[c] == pos && del == 0 && v->anchor[c] != v->head[c]))
					v->anchor[c] = (u32)((i64)v->anchor[c] + delta);
				else if (v->anchor[c] > pos)
					v->anchor[c] = pos + nins;
			}
		}
	D->lsp_changed_ns[d] = vx_now_ns();
	git_marks_due(d, 300000000ull);
	if (D->lang[d] == LANG_NONE && D->untitled[d] == 0 && t->len < 4096) {
		/* nothing */
	}
	app.dirty = true;
}

/* ---- views, tabs ---------------------------------------------------------- */

View *active_view(void)
{
	Group *g = &app.groups[app.active_group];
	return g->nt ? g->tab[g->active] : NULL;
}

static void mru_touch(Group *g, u32 t)
{
	u32 i = 0;
	while (i < g->nt - 1 && g->mru[i] != t)
		i++;
	memmove(g->mru + 1, g->mru, i * sizeof(u32));
	g->mru[0] = t;
}

static void activate_tab(u32 gi, u32 t)
{
	Group *g = &app.groups[gi];
	g->active = t;
	mru_touch(g, t);
	app.active_group = gi;
	app.compl_open = false;
	app.hover_open = false;
	View *v = g->tab[t];
	if (v)
		explorer_reveal(v->doc);
	app.dirty = true;
}

void close_tab(u32 gi, u32 t)
{
	Group *g = &app.groups[gi];
	if (t >= g->nt)
		return;
	View *v = g->tab[t];
	u32 d = v->doc;
	free(v);
	memmove(g->tab + t, g->tab + t + 1, (g->nt - t - 1) * sizeof(View *));
	/* fix MRU: drop t, shift indices above it */
	u32 k = 0;
	for (u32 i = 0; i < g->nt; i++) {
		if (g->mru[i] == t)
			continue;
		g->mru[k++] = g->mru[i] > t ? g->mru[i] - 1 : g->mru[i];
	}
	g->nt--;
	doc_release(d);
	if (g->nt) {
		g->active = g->mru[0];
	} else {
		g->active = 0;
		/* an empty group other than the first closes */
		if (app.ngroups > 1) {
			memmove(app.groups + gi, app.groups + gi + 1, (app.ngroups - gi - 1) * sizeof(Group));
			app.ngroups--;
			if (app.active_group >= app.ngroups)
				app.active_group = app.ngroups - 1;
			ui_layout();
		}
	}
	app.compl_open = false;
	app.hover_open = false;
	app.dirty = true;
}

static void nav_push(u32 doc, u32 off)
{
	if (app.nav_pos < app.nav_n && app.nav_doc[app.nav_pos] == doc &&
	    app.nav_off[app.nav_pos] == off)
		return;
	if (app.nav_pos + 1 >= VX_COUNT(app.nav_doc)) {
		memmove(app.nav_doc, app.nav_doc + 1, sizeof app.nav_doc - sizeof app.nav_doc[0]);
		memmove(app.nav_off, app.nav_off + 1, sizeof app.nav_off - sizeof app.nav_off[0]);
		app.nav_pos--;
	}
	if (app.nav_n)
		app.nav_pos++;
	app.nav_doc[app.nav_pos] = doc;
	app.nav_off[app.nav_pos] = off;
	app.nav_n = app.nav_pos + 1;
}

static void remember_position(void)
{
	View *v = active_view();
	if (v)
		nav_push(v->doc, v->head[0]);
}

static u32 show_doc(u32 d, bool preview)
{
	Group *g = &app.groups[app.active_group];
	for (u32 t = 0; t < g->nt; t++)
		if (g->tab[t]->doc == d) {
			if (!preview)
				g->tab[t]->preview = false;
			activate_tab(app.active_group, t);
			return t;
		}
	/* a preview tab is replaced by the next preview */
	if (preview)
		for (u32 t = 0; t < g->nt; t++)
			if (g->tab[t]->preview && !text_dirty(&app.docs.text[g->tab[t]->doc])) {
				View *v = g->tab[t];
				doc_release(v->doc);
				memset(v, 0, sizeof *v);
				v->doc = d;
				v->nc = 1;
				v->want_x[0] = -1;
				v->preview = true;
				app.docs.refs[d]++;
				activate_tab(app.active_group, t);
				return t;
			}
	if (g->nt == MAX_TABS)
		close_tab(app.active_group, g->mru[g->nt - 1]);
	View *v = vx_alloc(sizeof *v);
	v->doc = d;
	v->nc = 1;
	v->want_x[0] = -1;
	v->preview = preview;
	app.docs.refs[d]++;
	/* VSCodium opens new tabs to the right of the active one */
	u32 at = g->nt ? g->active + 1 : 0;
	memmove(g->tab + at + 1, g->tab + at, (g->nt - at) * sizeof(View *));
	for (u32 i = 0; i < g->nt; i++)
		if (g->mru[i] >= at)
			g->mru[i]++;
	g->tab[at] = v;
	g->mru[g->nt] = at;
	g->nt++;
	activate_tab(app.active_group, at);
	return at;
}

void open_file(const char *abs, bool preview, u32 line, u32 col)
{
	struct stat st;
	if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	remember_position();
	u32 d = doc_open(abs);
	if (d == UINT32_MAX)
		return;
	show_doc(d, preview);
	View *v = active_view();
	if (line != UINT32_MAX) {
		u32 off = ed_line_col_to_off(&app.docs.text[d], line, col);
		ed_set_cursor(v, off, false);
		ed_reveal(v, off, true);
		nav_push(d, off);
	}
	app.focus = FOCUS_EDITOR;
}

void open_rel(const char *rel, bool preview, u32 line, u32 col)
{
	char p[8192];
	snprintf(p, sizeof p, "%s/%s", app.root, rel);
	open_file(p, preview, line, col);
}

/* ---- layout ----------------------------------------------------------- */

void ui_layout(void)
{
	app.act_w = PX(48);
	app.status_h = PX(22);
	app.tabs_h = PX(35);
	app.crumbs_h = PX(22);
	app.row_h = PX(22);
	int x = app.act_w;
	app.side_x = x;
	if (app.side_visible)
		x += app.side_w;
	int bottom = app.h - app.status_h;
	if (app.panel_visible) {
		int maxp = bottom - PX(120);
		app.panel_h = VX_CLAMP(app.panel_h, PX(80), VX_MAX(maxp, PX(80)));
		app.panel_y = bottom - app.panel_h;
		bottom = app.panel_y;
	}
	app.ed_x = x;
	app.ed_y = 0;
	app.ed_w = app.w - x;
	app.ed_h = bottom;
	int gw = app.ed_w / (int)app.ngroups;
	for (u32 i = 0; i < app.ngroups; i++) {
		Group *g = &app.groups[i];
		g->x = app.ed_x + (int)i * gw;
		g->y = 0;
		g->w = i + 1 == app.ngroups ? app.ed_w - (int)i * gw : gw;
		g->h = app.ed_h;
	}
	app.dirty = true;
}

/* ---- keybindings ---------------------------------------------------------- */

enum { W_ANY, W_EDITOR, W_NOT_INPUT };

typedef struct {
	int mods;
	u32 key;
	u8 when;
	u16 cmd;
	bool chord; /* second key after Ctrl+K */
} Binding;

#define C MOD_CTRL
#define S MOD_SHIFT
#define A MOD_ALT
static const Binding bindings[] = {
    {C, SDLK_P, W_ANY, CMD_QUICK_OPEN, 0},
    {C | S, SDLK_P, W_ANY, CMD_COMMANDS, 0},
    {0, SDLK_F1, W_ANY, CMD_COMMANDS, 0},
    {C, SDLK_S, W_ANY, CMD_SAVE, 0},
    {C, SDLK_S, W_ANY, CMD_SAVE_ALL, 1},
    {0, SDLK_S, W_ANY, CMD_SAVE_ALL, 1},
    {C, SDLK_N, W_ANY, CMD_NEW_FILE, 0},
    {C, SDLK_W, W_ANY, CMD_CLOSE_EDITOR, 0},
    {C, SDLK_F4, W_ANY, CMD_CLOSE_EDITOR, 0},
    {C, SDLK_W, W_ANY, CMD_CLOSE_ALL, 1},
    {0, SDLK_W, W_ANY, CMD_CLOSE_ALL, 1},
    {C, SDLK_PAGEDOWN, W_ANY, CMD_NEXT_EDITOR, 0},
    {C, SDLK_PAGEUP, W_ANY, CMD_PREV_EDITOR, 0},
    {C, SDLK_TAB, W_ANY, CMD_NEXT_EDITOR, 0},
    {C | S, SDLK_TAB, W_ANY, CMD_PREV_EDITOR, 0},
    {C, SDLK_B, W_ANY, CMD_TOGGLE_SIDEBAR, 0},
    {C, SDLK_J, W_ANY, CMD_TOGGLE_PANEL, 0},
    {C | S, SDLK_E, W_ANY, CMD_VIEW_EXPLORER, 0},
    {C | S, SDLK_F, W_ANY, CMD_VIEW_SEARCH, 0},
    {C | S, SDLK_G, W_ANY, CMD_VIEW_SCM, 0},
    {C | S, SDLK_M, W_ANY, CMD_VIEW_PROBLEMS, 0},
    {C | S, SDLK_U, W_ANY, CMD_VIEW_OUTPUT, 0},
    {C, SDLK_BACKSLASH, W_ANY, CMD_SPLIT, 0},
    {C, SDLK_1, W_ANY, CMD_FOCUS_GROUP1, 0},
    {C, SDLK_2, W_ANY, CMD_FOCUS_GROUP2, 0},
    {C, SDLK_3, W_ANY, CMD_FOCUS_GROUP3, 0},
    {C, SDLK_G, W_ANY, CMD_GOTO_LINE, 0},
    {C, SDLK_F, W_ANY, CMD_FIND, 0},
    {C, SDLK_H, W_ANY, CMD_REPLACE, 0},
    {0, SDLK_F3, W_ANY, CMD_FIND_NEXT, 0},
    {C | S, SDLK_H, W_ANY, CMD_REPLACE_IN_FILES, 0},
    {0, SDLK_F4, W_ANY, CMD_SEARCH_NEXT_RESULT, 0},
    {S, SDLK_F4, W_ANY, CMD_SEARCH_PREV_RESULT, 0},
    {C | S, SDLK_J, W_ANY, CMD_SEARCH_TOGGLE_DETAILS, 0},
    {S, SDLK_F3, W_ANY, CMD_FIND_PREV, 0},
    {C, SDLK_Z, W_EDITOR, CMD_UNDO, 0},
    {C, SDLK_Y, W_EDITOR, CMD_REDO, 0},
    {C | S, SDLK_Z, W_EDITOR, CMD_REDO, 0},
    {C, SDLK_X, W_EDITOR, CMD_CUT, 0},
    {C, SDLK_C, W_EDITOR, CMD_COPY, 0},
    {C, SDLK_V, W_EDITOR, CMD_PASTE, 0},
    {S, SDLK_DELETE, W_EDITOR, CMD_CUT, 0},
    {C, SDLK_INSERT, W_EDITOR, CMD_COPY, 0},
    {S, SDLK_INSERT, W_EDITOR, CMD_PASTE, 0},
    {C, SDLK_A, W_EDITOR, CMD_SELECT_ALL, 0},
    {C, SDLK_L, W_EDITOR, CMD_SELECT_LINE, 0},
    {C, SDLK_D, W_EDITOR, CMD_ADD_NEXT_MATCH, 0},
    {C | S, SDLK_L, W_EDITOR, CMD_SELECT_ALL_MATCHES, 0},
    {C, SDLK_SLASH, W_EDITOR, CMD_TOGGLE_COMMENT, 0},
    {C, SDLK_RIGHTBRACKET, W_EDITOR, CMD_INDENT, 0},
    {C, SDLK_LEFTBRACKET, W_EDITOR, CMD_OUTDENT, 0},
    {A, SDLK_UP, W_EDITOR, CMD_MOVE_LINE_UP, 0},
    {A, SDLK_DOWN, W_EDITOR, CMD_MOVE_LINE_DOWN, 0},
    {C | S | A, SDLK_UP, W_EDITOR, CMD_COPY_LINE_UP, 0},
    {C | S | A, SDLK_DOWN, W_EDITOR, CMD_COPY_LINE_DOWN, 0},
    {C | S, SDLK_K, W_EDITOR, CMD_DELETE_LINE, 0},
    {C, SDLK_RETURN, W_EDITOR, CMD_INSERT_LINE_BELOW, 0},
    {C | S, SDLK_RETURN, W_EDITOR, CMD_INSERT_LINE_ABOVE, 0},
    {S | A, SDLK_UP, W_EDITOR, CMD_CURSOR_ABOVE, 0},
    {S | A, SDLK_DOWN, W_EDITOR, CMD_CURSOR_BELOW, 0},
    {C | A, SDLK_UP, W_EDITOR, CMD_CURSOR_ABOVE, 0},
    {C | A, SDLK_DOWN, W_EDITOR, CMD_CURSOR_BELOW, 0},
    {0, SDLK_F12, W_EDITOR, CMD_GOTO_DEFINITION, 0},
    {C | A, SDLK_MINUS, W_ANY, CMD_NAV_BACK, 0},
    {C | S, SDLK_MINUS, W_ANY, CMD_NAV_FORWARD, 0},
    {A, SDLK_LEFT, W_ANY, CMD_NAV_BACK, 0},
    {A, SDLK_RIGHT, W_ANY, CMD_NAV_FORWARD, 0},
    {0, SDLK_F8, W_ANY, CMD_NEXT_PROBLEM, 0},
    {S, SDLK_F8, W_ANY, CMD_PREV_PROBLEM, 0},
    {C, SDLK_SPACE, W_EDITOR, CMD_TRIGGER_SUGGEST, 0},
    {C, SDLK_I, W_EDITOR, CMD_SHOW_HOVER, 1},
    {C, SDLK_T, W_ANY, CMD_TOGGLE_THEME, 1},
    {C, SDLK_Q, W_ANY, CMD_QUIT, 0},
};
#undef C
#undef S
#undef A

static const char *cmd_titles[CMD_COUNT] = {
    [CMD_QUICK_OPEN] = "Go to File...",
    [CMD_COMMANDS] = "Show All Commands",
    [CMD_SAVE] = "File: Save",
    [CMD_SAVE_ALL] = "File: Save All Files",
    [CMD_NEW_FILE] = "File: New Untitled Text File",
    [CMD_CLOSE_EDITOR] = "View: Close Editor",
    [CMD_CLOSE_ALL] = "View: Close All Editors",
    [CMD_NEXT_EDITOR] = "View: Open Next Editor",
    [CMD_PREV_EDITOR] = "View: Open Previous Editor",
    [CMD_TOGGLE_SIDEBAR] = "View: Toggle Primary Side Bar Visibility",
    [CMD_TOGGLE_PANEL] = "View: Toggle Panel Visibility",
    [CMD_VIEW_EXPLORER] = "View: Show Explorer",
    [CMD_VIEW_SEARCH] = "View: Show Search",
    [CMD_VIEW_SCM] = "View: Show Source Control",
    [CMD_VIEW_PROBLEMS] = "View: Focus Problems",
    [CMD_VIEW_OUTPUT] = "View: Toggle Output",
    [CMD_SPLIT] = "View: Split Editor",
    [CMD_FOCUS_GROUP1] = "View: Focus First Editor Group",
    [CMD_FOCUS_GROUP2] = "View: Focus Second Editor Group",
    [CMD_FOCUS_GROUP3] = "View: Focus Third Editor Group",
    [CMD_GOTO_LINE] = "Go to Line/Column...",
    [CMD_FIND] = "Find",
    [CMD_REPLACE] = "Replace",
    [CMD_FIND_NEXT] = "Find Next",
    [CMD_FIND_PREV] = "Find Previous",
    [CMD_REPLACE_IN_FILES] = "Search: Replace in Files",
    [CMD_SEARCH_REPLACE_ALL] = "Search: Replace All",
    [CMD_SEARCH_NEXT_RESULT] = "Search: Go to Next Search Result",
    [CMD_SEARCH_PREV_RESULT] = "Search: Go to Previous Search Result",
    [CMD_SEARCH_TOGGLE_DETAILS] = "Search: Toggle Search Details",
    [CMD_UNDO] = "Undo",
    [CMD_REDO] = "Redo",
    [CMD_CUT] = "Cut",
    [CMD_COPY] = "Copy",
    [CMD_PASTE] = "Paste",
    [CMD_SELECT_ALL] = "Select All",
    [CMD_SELECT_LINE] = "Expand Line Selection",
    [CMD_ADD_NEXT_MATCH] = "Add Selection To Next Find Match",
    [CMD_SELECT_ALL_MATCHES] = "Select All Occurrences of Find Match",
    [CMD_TOGGLE_COMMENT] = "Toggle Line Comment",
    [CMD_INDENT] = "Indent Line",
    [CMD_OUTDENT] = "Outdent Line",
    [CMD_MOVE_LINE_UP] = "Move Line Up",
    [CMD_MOVE_LINE_DOWN] = "Move Line Down",
    [CMD_COPY_LINE_UP] = "Copy Line Up",
    [CMD_COPY_LINE_DOWN] = "Copy Line Down",
    [CMD_DELETE_LINE] = "Delete Line",
    [CMD_INSERT_LINE_BELOW] = "Insert Line Below",
    [CMD_INSERT_LINE_ABOVE] = "Insert Line Above",
    [CMD_CURSOR_ABOVE] = "Add Cursor Above",
    [CMD_CURSOR_BELOW] = "Add Cursor Below",
    [CMD_GOTO_DEFINITION] = "Go to Definition",
    [CMD_NAV_BACK] = "Go Back",
    [CMD_NAV_FORWARD] = "Go Forward",
    [CMD_NEXT_PROBLEM] = "Go to Next Problem in Files (Error, Warning, Info)",
    [CMD_PREV_PROBLEM] = "Go to Previous Problem in Files (Error, Warning, Info)",
    [CMD_TRIGGER_SUGGEST] = "Trigger Suggest",
    [CMD_SHOW_HOVER] = "Show or Focus Hover",
    [CMD_TOGGLE_THEME] = "Preferences: Toggle between Light/Dark Themes",
    [CMD_THEME_DARK] = "Preferences: Color Theme: Dark Modern",
    [CMD_THEME_LIGHT] = "Preferences: Color Theme: Light Modern",
    [CMD_GIT_REFRESH] = "Git: Refresh",
    [CMD_GIT_COMMIT] = "Git: Commit Staged",
    [CMD_GIT_STAGE_ALL] = "Git: Stage All Changes",
    [CMD_GIT_STAGE_FILE] = "Git: Stage Changes (active file)",
    [CMD_GIT_UNSTAGE_FILE] = "Git: Unstage Changes (active file)",
    [CMD_GIT_DISCARD_FILE] = "Git: Discard Changes (active file)",
    [CMD_GIT_TOGGLE_BLAME] = "Git: Toggle Git Blame Editor Decoration",
    [CMD_REFRESH_EXPLORER] = "File: Refresh Explorer",
    [CMD_COLLAPSE_FOLDERS] = "File: Collapse Folders in Explorer",
    [CMD_REVEAL_IN_EXPLORER] = "File: Reveal Active File in Explorer View",
    [CMD_RESTART_LSP] = "clangd: Restart language server",
    [CMD_QUIT] = "File: Exit",
};

const char *command_title(int cmd) { return cmd_titles[cmd] ? cmd_titles[cmd] : ""; }

static void key_name(char *out, size_t cap, int mods, u32 key)
{
	const char *kn = SDL_GetKeyName((SDL_Keycode)key);
	snprintf(out, cap, "%s%s%s%s", mods & MOD_CTRL ? "Ctrl+" : "", mods & MOD_SHIFT ? "Shift+" : "",
	         mods & MOD_ALT ? "Alt+" : "", kn && *kn ? kn : "?");
}

const char *command_keys(int cmd)
{
	static char buf[64];
	for (size_t i = 0; i < VX_COUNT(bindings); i++)
		if (bindings[i].cmd == cmd) {
			if (bindings[i].chord) {
				char k[32];
				key_name(k, sizeof k, bindings[i].mods, bindings[i].key);
				snprintf(buf, sizeof buf, "Ctrl+K %s", k);
			} else {
				key_name(buf, sizeof buf, bindings[i].mods, bindings[i].key);
			}
			return buf;
		}
	return NULL;
}

/* ---- quick input ---------------------------------------------------------- */

static void quick_close(void)
{
	app.quick = QUICK_NONE;
	app.focus = active_view() ? FOCUS_EDITOR : FOCUS_SIDEBAR;
	app.dirty = true;
}

static bool skip_ignored(void *ctx, const char *rel, bool dir)
{
	Git *g = ctx;
	return g && (git_path_flags(g, rel) & GS_IGNORED);
}

static void ensure_files(void);
void ui_ensure_files(void) { ensure_files(); }

static void ensure_files(void)
{
	if (app.files_ok)
		return;
	files_scan(&app.files, app.root, app.git ? skip_ignored : NULL, app.git, 200000);
	app.files_ok = true;
	free(app.search_collapsed);
	app.search_collapsed = vx_alloc(app.files.n + 1);
}

static void quick_reserve(u32 n)
{
	if (n <= app.quick_cap)
		return;
	u32 c = app.quick_cap ? app.quick_cap : 256;
	while (c < n)
		c *= 2;
	app.quick_items = vx_realloc(app.quick_items, c * sizeof(u32));
	app.quick_score = vx_realloc(app.quick_score, c * sizeof(int));
	app.quick_mask = vx_realloc(app.quick_mask, c * sizeof(u64));
	app.quick_cap = c;
}

static int quick_cmp_idx(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	int sx = app.quick_score[x], sy = app.quick_score[y];
	if (sx != sy)
		return sy - sx;
	return (int)x - (int)y;
}

typedef struct {
	int s;
	u32 item;
	u64 m;
} QPair;

static int qpair_cmp(const void *a, const void *b)
{
	const QPair *x = a, *y = b;
	if (x->s != y->s)
		return y->s > x->s ? 1 : -1;
	return x->item < y->item ? -1 : x->item > y->item;
}

/* Items: QUICK_FILES -> file list indices (or doc indices with the high
 * bit when listing open editors); QUICK_COMMANDS -> command ids. */
static void quick_update(void)
{
	Input *in = &app.quick_in;
	app.quick_sel = 0;
	app.quick_top = 0;
	app.quick_n = 0;
	if (app.quick == QUICK_FILES || app.quick == QUICK_COMMANDS || app.quick == QUICK_GOTO_LINE) {
		if (in->n && in->b[0] == '>')
			app.quick = QUICK_COMMANDS;
		else if (in->n && in->b[0] == ':')
			app.quick = QUICK_GOTO_LINE;
		else
			app.quick = QUICK_FILES;
	}
	if (app.quick == QUICK_COMMANDS) {
		const char *q = in->b + 1;
		u32 nq = in->n - 1;
		while (nq && *q == ' ') {
			q++;
			nq--;
		}
		quick_reserve(CMD_COUNT);
		u32 *tmp = vx_alloc(CMD_COUNT * sizeof(u32));
		u32 n = 0;
		int *score = vx_alloc(CMD_COUNT * sizeof(int));
		for (int c = 1; c < CMD_COUNT; c++) {
			if (c == CMD_COMMANDS || c == CMD_QUICK_OPEN)
				continue;
			u64 m;
			const char *t = command_title(c);
			int s = fuzzy_score(q, nq, t, strlen(t), &m);
			if (s < 0)
				continue;
			score[n] = s;
			tmp[n] = (u32)c;
			app.quick_mask[n] = m;
			n++;
		}
		/* stable sort by score when filtering, keep table order otherwise */
		for (u32 i = 0; i < n; i++) {
			app.quick_items[i] = tmp[i];
			app.quick_score[i] = nq ? score[i] : 0;
		}
		if (nq) {
			u32 *idx = vx_alloc(n * sizeof(u32) + 4);
			for (u32 i = 0; i < n; i++)
				idx[i] = i;
			qsort(idx, n, sizeof(u32), quick_cmp_idx);
			u64 *mm = vx_alloc(n * sizeof(u64) + 8);
			for (u32 i = 0; i < n; i++) {
				tmp[i] = app.quick_items[idx[i]];
				mm[i] = app.quick_mask[idx[i]];
			}
			memcpy(app.quick_items, tmp, n * sizeof(u32));
			memcpy(app.quick_mask, mm, n * sizeof(u64));
			free(idx);
			free(mm);
		}
		app.quick_n = n;
		free(tmp);
		free(score);
	} else if (app.quick == QUICK_FILES) {
		if (in->n == 0) {
			/* recently opened: open editors, MRU of the active group */
			Group *g = &app.groups[app.active_group];
			quick_reserve(g->nt + 1);
			for (u32 i = 0; i < g->nt; i++) {
				app.quick_items[app.quick_n] = 0x80000000u | g->tab[g->mru[i]]->doc;
				app.quick_mask[app.quick_n] = 0;
				app.quick_n++;
			}
			/* the current one goes second, like VSCodium (quick switch) */
			if (app.quick_n > 1)
				app.quick_sel = 1;
		} else {
			ensure_files();
			const FileList *fl = &app.files;
			quick_reserve(fl->n + 1);
			u32 n = 0;
			for (u32 i = 0; i < fl->n; i++) {
				const char *p = fl->pool.b + fl->path[i];
				const char *base = p + fl->base[i];
				u64 m;
				/* prefer basename matches, fall back to the whole path */
				int s = fuzzy_score(in->b, in->n, base, strlen(base), &m);
				if (s >= 0) {
					s += 100;
					m <<= 0; /* mask relative to the basename */
				} else {
					s = fuzzy_score(in->b, in->n, p, strlen(p), &m);
					if (s < 0)
						continue;
					m |= 1ull << 63; /* mask relative to the path */
				}
				s -= (int)VX_MIN(strlen(p), (size_t)200) / 20;
				app.quick_items[n] = i;
				app.quick_score[i] = s;
				app.quick_mask[n] = m;
				n++;
			}
			/* sort by score (scores are indexed by file id) */
			QPair *ps = vx_alloc((size_t)n * sizeof(QPair) + 1);
			for (u32 i = 0; i < n; i++) {
				ps[i].s = app.quick_score[app.quick_items[i]];
				ps[i].item = app.quick_items[i];
				ps[i].m = app.quick_mask[i];
			}
			qsort(ps, n, sizeof(QPair), qpair_cmp);
			for (u32 i = 0; i < n; i++) {
				app.quick_items[i] = ps[i].item;
				app.quick_mask[i] = ps[i].m;
			}
			free(ps);
			app.quick_n = VX_MIN(n, 1000u);
		}
	}
	app.dirty = true;
}

static void quick_open(int kind, const char *initial)
{
	app.quick = kind;
	app.focus = FOCUS_QUICK;
	app.quick_title[0] = 0;
	input_set(&app.quick_in, initial, strlen(initial));
	app.compl_open = false;
	app.hover_open = false;
	quick_update();
}

static void quick_accept(void)
{
	Input *in = &app.quick_in;
	int kind = app.quick;
	if (kind == QUICK_GOTO_LINE) {
		View *v = active_view();
		const char *p = in->b;
		while (*p == ':' || *p == ' ')
			p++;
		char *after;
		long line = strtol(p, &after, 10);
		const char *comma = strpbrk(after, ":,");
		long col = comma ? strtol(comma + 1, NULL, 10) : 1;
		quick_close();
		if (v && line > 0) {
			Text *t = &app.docs.text[v->doc];
			u32 l = (u32)VX_MIN((u32)line - 1, text_lines(t) - 1);
			remember_position();
			u32 off = ed_line_col_to_off(t, l, col > 0 ? (u32)col - 1 : 0);
			ed_set_cursor(v, off, false);
			ed_reveal(v, off, true);
		}
		return;
	}
	if (kind == QUICK_PROMPT) {
		char *s = vx_strndup(in->b, in->n);
		int pk = app.quick_prompt_kind;
		u32 arg = app.quick_arg;
		quick_close();
		if (pk == 1 && *s) { /* save as */
			char p[8192];
			if (s[0] == '/')
				snprintf(p, sizeof p, "%s", s);
			else
				snprintf(p, sizeof p, "%s/%s", app.root, s);
			Docs *D = &app.docs;
			if (text_save(&D->text[arg], p) == 0) {
				D->path[arg] = vx_strdup(p);
				free(D->rel[arg]);
				D->rel[arg] = vx_strdup(rel_of(p));
				D->untitled[arg] = 0;
				Text *t = &D->text[arg];
				char head[512];
				u32 hn = VX_MIN(t->len, (u32)sizeof head);
				text_copy(t, 0, hn, head);
				D->lang[arg] = (u8)syn_lang_for_path(p, head, hn);
				D->lstate_ok[arg] = 0;
				lsp_sync_open(arg);
				app.tree_scan_ns = 0;
				app.git_ns = 0;
				app.files_ok = false;
			} else {
				ui_status("Failed to save '%s'", s);
			}
		} else if ((pk == 2 || pk == 3) && *s) { /* new file / new folder */
			char p[8192];
			snprintf(p, sizeof p, "%s/%s", app.root, s);
			if (pk == 3) {
				char cmdbuf[8300];
				snprintf(cmdbuf, sizeof cmdbuf, "%s", p);
				for (char *c = cmdbuf + strlen(app.root) + 1; *c; c++)
					if (*c == '/') {
						*c = 0;
						mkdir(cmdbuf, 0755);
						*c = '/';
					}
				mkdir(cmdbuf, 0755);
			} else {
				for (char *c = p + strlen(app.root) + 1; *c; c++)
					if (*c == '/') {
						*c = 0;
						mkdir(p, 0755);
						*c = '/';
					}
				if (access(p, F_OK) != 0)
					vx_write_file(p, "", 0);
				open_file(p, false, UINT32_MAX, 0);
			}
			fs_refresh(&app.tree);
			fs_reveal(&app.tree, s);
			app.files_ok = false;
			app.git_ns = 0;
		} else if (pk == 4 && *s) { /* rename */
			char from[8192], to[8192], rel[4096];
			fs_relpath(&app.tree, arg, rel, sizeof rel);
			snprintf(from, sizeof from, "%s/%s", app.root, rel);
			snprintf(to, sizeof to, "%s/%s", app.root, s);
			if (rename(from, to) != 0)
				ui_status("Rename failed");
			else {
				/* re-point open documents */
				Docs *D = &app.docs;
				for (u32 d = 0; d < D->n; d++)
					if (D->path[d] && strcmp(D->path[d], from) == 0) {
						free(D->path[d]);
						free(D->rel[d]);
						D->path[d] = vx_strdup(to);
						D->rel[d] = vx_strdup(s);
					}
			}
			fs_refresh(&app.tree);
			app.files_ok = false;
			app.git_ns = 0;
		} else if (pk == 5) { /* commit message for Git: Commit */
			if (app.git && *s) {
				if (git_commit_staged(app.git, s) == 0)
					ui_status("Committed");
				else
					ui_status("Commit failed: %s", git_last_error());
				app.git_ns = 0;
			}
		}
		free(s);
		return;
	}
	if (kind == QUICK_CONFIRM) {
		u32 arg = app.quick_arg;
		int pk = app.quick_prompt_kind;
		quick_close();
		if (pk == 8 && app.git) { /* smart commit */
			git_stage_all(app.git);
			git_refresh(app.git);
			run_command(CMD_GIT_COMMIT);
			return;
		}
		if (pk == 6) { /* delete file/folder */
			char rel[4096], p[8192], cmdbuf[8400];
			fs_relpath(&app.tree, arg, rel, sizeof rel);
			snprintf(p, sizeof p, "%s/%s", app.root, rel);
			if (app.tree.flags[arg] & FS_DIR) {
				snprintf(cmdbuf, sizeof cmdbuf, "rm -rf -- '%s'", p);
				if (strchr(p, '\'') || system(cmdbuf) != 0)
					ui_status("Delete failed");
			} else if (unlink(p) != 0) {
				ui_status("Delete failed");
			}
			fs_refresh(&app.tree);
			app.files_ok = false;
			app.git_ns = 0;
		}
		return;
	}
	if (app.quick_n == 0) {
		quick_close();
		return;
	}
	u32 item = app.quick_items[app.quick_sel];
	quick_close();
	if (kind == QUICK_COMMANDS) {
		run_command((int)item);
	} else if (item & 0x80000000u) {
		u32 d = item & 0x7FFFFFFFu;
		show_doc(d, false);
		app.focus = FOCUS_EDITOR;
	} else {
		open_rel(app.files.pool.b + app.files.path[item], false, UINT32_MAX, 0);
	}
}

static bool quick_key(const Event *e)
{
	u32 k = e->key;
	int page = 10;
	if (k == SDLK_ESCAPE) {
		quick_close();
		return true;
	}
	if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
		quick_accept();
		return true;
	}
	if (k == SDLK_DOWN || (k == SDLK_N && e->mods == MOD_CTRL) ||
	    (k == SDLK_P && (e->mods & MOD_CTRL) && app.quick == QUICK_FILES)) {
		if (app.quick_n)
			app.quick_sel = (app.quick_sel + 1) % app.quick_n;
		app.dirty = true;
		return true;
	}
	if (k == SDLK_UP) {
		if (app.quick_n)
			app.quick_sel = (app.quick_sel + app.quick_n - 1) % app.quick_n;
		app.dirty = true;
		return true;
	}
	if (k == SDLK_PAGEDOWN) {
		app.quick_sel = VX_MIN(app.quick_sel + (u32)page, app.quick_n ? app.quick_n - 1 : 0);
		app.dirty = true;
		return true;
	}
	if (k == SDLK_PAGEUP) {
		app.quick_sel = app.quick_sel > (u32)page ? app.quick_sel - (u32)page : 0;
		app.dirty = true;
		return true;
	}
	u32 before = app.quick_in.n;
	char *snap = vx_strndup(app.quick_in.b ? app.quick_in.b : "", app.quick_in.n);
	bool h = input_key(&app.quick_in, e);
	if (h && (app.quick_in.n != before || memcmp(snap, app.quick_in.b ? app.quick_in.b : "", before) != 0))
		quick_update();
	free(snap);
	return h;
}

static void quick_draw(void)
{
	int w = VX_MIN(PX(600), app.w - PX(40));
	int x = (app.w - w) / 2;
	int y = PX(4);
	int pad = PX(6);
	int ih = PX(26);
	int rh = PX(22);
	u32 maxrows = 16;
	bool listing = app.quick == QUICK_FILES || app.quick == QUICK_COMMANDS;
	if (app.quick == QUICK_CONFIRM)
		ih = -pad;
	u32 rows = listing ? VX_MIN(app.quick_n, maxrows) : 0;
	int title_h = app.quick_title[0] ? PX(22) : 0;
	int msg_h = (!listing || app.quick_n == 0) ? rh : 0;
	int h = pad + title_h + ih + pad + (int)rows * rh + msg_h + (rows ? pad : 0);
	/* shadow, then box */
	draw_rect(x - PX(2), y, w + PX(4), h + PX(4), COL(C_WIDGET_SHADOW));
	draw_rect(x, y, w, h, COL(C_QUICK_BG));
	draw_frame(x, y, w, h, COL(C_WIDGET_BORDER));
	int cy = y + pad;
	if (title_h) {
		int tw = font_width(FONT_UI, app.quick_title, strlen(app.quick_title));
		draw_text(FONT_UI, x + (w - tw) / 2, cy + (title_h - font_m[FONT_UI].line) / 2,
		          app.quick_title, strlen(app.quick_title), COL(C_QUICK_FG));
		cy += title_h;
	}
	const char *ph = app.quick == QUICK_FILES     ? "Search files by name (append : to go to line)"
	                 : app.quick == QUICK_GOTO_LINE ? ""
	                 : app.quick == QUICK_CONFIRM   ? ""
	                                                : "";
	if (app.quick != QUICK_CONFIRM) {
		input_draw(&app.quick_in, x + pad, cy, w - 2 * pad, ih, ph, true);
		cy += ih + pad;
	}
	if (msg_h) {
		char msg[256] = "";
		View *v = active_view();
		if (app.quick == QUICK_GOTO_LINE) {
			if (v)
				snprintf(msg, sizeof msg, "Current Line: %u, Character: %u. Type a line number between 1 and %u to navigate to.",
				         text_line_of(&app.docs.text[v->doc], v->head[0]) + 1, 1u,
				         text_lines(&app.docs.text[v->doc]));
			else
				snprintf(msg, sizeof msg, "Open a text editor first to go to a line.");
		} else if (app.quick == QUICK_CONFIRM) {
			snprintf(msg, sizeof msg, "Press 'Enter' to confirm or 'Escape' to cancel");
		} else if (app.quick == QUICK_PROMPT) {
			snprintf(msg, sizeof msg, "Press 'Enter' to confirm or 'Escape' to cancel");
		} else {
			snprintf(msg, sizeof msg, app.quick == QUICK_COMMANDS ? "No matching commands" : "No matching results");
		}
		draw_text_fit(FONT_UI, x + pad + PX(6), cy + (rh - font_m[FONT_UI].line) / 2, msg, strlen(msg),
		              w - 2 * pad - PX(12), COL(C_DESCRIPTION_FG));
		return;
	}
	if (app.quick_sel < app.quick_top)
		app.quick_top = app.quick_sel;
	if (app.quick_sel >= app.quick_top + maxrows)
		app.quick_top = app.quick_sel - maxrows + 1;
	draw_clip(x, cy, w, (int)rows * rh);
	for (u32 r = 0; r < rows; r++) {
		u32 i = app.quick_top + r;
		if (i >= app.quick_n)
			break;
		int ry = cy + (int)r * rh;
		bool sel = i == app.quick_sel;
		if (sel)
			draw_rect(x + PX(1), ry, w - PX(2), rh, COL(C_QUICK_ACTIVE_BG));
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		u32 fg = sel ? COL(C_QUICK_ACTIVE_FG) : COL(C_QUICK_FG);
		u32 item = app.quick_items[i];
		u64 mask = app.quick_mask[i];
		int tx = x + pad + PX(6);
		if (app.quick == QUICK_COMMANDS) {
			const char *t = command_title((int)item);
			size_t n = strlen(t);
			for (size_t k = 0; k < n; k++) {
				u32 c = (k < 63 && (mask >> k) & 1) ? COL(C_QUICK_HIGHLIGHT) : fg;
				tx = draw_text((k < 63 && (mask >> k) & 1) ? FONT_UI_BOLD : FONT_UI, tx, ty, t + k, 1, c);
			}
			const char *keys = command_keys((int)item);
			if (keys) {
				int kw = font_width(FONT_UI, keys, strlen(keys));
				draw_text(FONT_UI, x + w - pad - PX(8) - kw, ty, keys, strlen(keys), COL(C_DESCRIPTION_FG));
			}
		} else {
			const char *path;
			const char *base;
			bool dirty = false;
			if (item & 0x80000000u) {
				u32 d = item & 0x7FFFFFFFu;
				path = app.docs.rel[d];
				const char *sl = strrchr(path, '/');
				base = sl ? sl + 1 : path;
				dirty = text_dirty(&app.docs.text[d]);
				mask = 0;
			} else {
				path = app.files.pool.b + app.files.path[item];
				base = path + app.files.base[item];
			}
			tx = file_icon_draw(base, tx, ry, rh);
			bool path_mask = (mask >> 63) & 1;
			size_t bn = strlen(base);
			size_t off = (size_t)(base - path);
			for (size_t k = 0; k < bn; k++) {
				size_t bit = path_mask ? off + k : k;
				bool hl = bit < 63 && ((mask >> bit) & 1);
				tx = draw_text(hl ? FONT_UI_BOLD : FONT_UI, tx, ty, base + k, 1, hl ? COL(C_QUICK_HIGHLIGHT) : fg);
			}
			if (dirty)
				tx = draw_text(FONT_UI, tx, ty, " \xE2\x97\x8F", 4, fg);
			if (off > 1) {
				tx += PX(6);
				draw_text_fit(FONT_UI, tx, ty, path, off - 1, x + w - pad - tx, COL(C_DESCRIPTION_FG));
			}
		}
	}
	draw_noclip();
}

/* ---- commands --------------------------------------------------------------- */

static bool git_has_staged(void)
{
	const GitStatus *st = git_status(app.git);
	for (u32 i = 0; i < st->count; i++)
		if (st->flags[i] & (GS_INDEX_NEW | GS_INDEX_MODIFIED | GS_INDEX_DELETED | GS_INDEX_RENAMED))
			return true;
	return false;
}

static void focus_group(u32 gi)
{
	if (gi >= app.ngroups) {
		/* Ctrl+2 on one group splits, like VSCodium */
		if (app.ngroups < MAX_GROUPS && gi == app.ngroups) {
			run_command(CMD_SPLIT);
		}
		return;
	}
	app.active_group = gi;
	app.focus = FOCUS_EDITOR;
	app.dirty = true;
}

static void goto_problem(int dir)
{
	/* Next diagnostic after the cursor in the active doc, then other docs. */
	View *v = active_view();
	Docs *D = &app.docs;
	if (!v)
		return;
	u32 d = v->doc;
	Text *t = &D->text[d];
	u32 cl = text_line_of(t, v->head[0]);
	u32 cc = v->head[0] - text_line_start(t, cl);
	i64 best = -1;
	u32 bl = 0, bc = 0;
	for (u32 i = 0; i < D->dn[d]; i++) {
		u32 l = D->d_line0[d][i], c = D->d_col0[d][i];
		bool after = l > cl || (l == cl && c > cc);
		bool before = l < cl || (l == cl && c < cc);
		if ((dir > 0 && !after) || (dir < 0 && !before))
			continue;
		i64 key = (i64)l * 100000 + c;
		if (best < 0 || (dir > 0 ? key < best : key > best)) {
			best = key;
			bl = l;
			bc = c;
		}
	}
	if (best < 0 && D->dn[d]) {
		/* wrap */
		u32 i = 0;
		for (u32 k = 1; k < D->dn[d]; k++) {
			bool better = dir > 0 ? (D->d_line0[d][k] < D->d_line0[d][i]) : (D->d_line0[d][k] > D->d_line0[d][i]);
			if (better)
				i = k;
		}
		bl = D->d_line0[d][i];
		bc = D->d_col0[d][i];
		best = 0;
	}
	if (best < 0)
		return;
	remember_position();
	u32 off = ed_line_col_to_off(t, bl, bc);
	ed_set_cursor(v, off, false);
	ed_reveal(v, off, true);
	app.hover_open = false;
	/* show the message like VSCodium's marker widget (status line) */
	for (u32 i = 0; i < D->dn[d]; i++)
		if (D->d_line0[d][i] == bl && D->d_col0[d][i] == bc) {
			ui_status("%.*s", (int)strcspn(D->d_pool[d].b + D->d_msg[d][i], "\x1f"),
			          D->d_pool[d].b + D->d_msg[d][i]);
			break;
		}
}

static void restart_lsp(void)
{
	if (app.lsp) {
		lsp_stop(app.lsp);
		app.lsp = NULL;
	}
	app.lsp_ready = false;
	for (u32 d = 0; d < app.docs.n; d++)
		app.docs.lsp_open[d] = false;
	const char *cmd = getenv("VXIDE_CLANGD");
	char *argv[] = {(char *)(cmd ? cmd : "clangd"), "--log=error", "--background-index", NULL};
	app.lsp = lsp_start(app.root, argv);
	if (!app.lsp)
		ui_log("[lsp] could not start %s", argv[0]);
	else
		ui_log("[lsp] started %s in %s", argv[0], app.root);
	for (u32 d = 0; d < app.docs.n; d++)
		if (app.docs.path[d])
			lsp_sync_open(d);
}

void run_command(int cmd)
{
	View *v = active_view();
	app.dirty = true;
	if (v && ed_command(v, cmd))
		return;
	switch (cmd) {
	case CMD_QUICK_OPEN:
		quick_open(QUICK_FILES, "");
		break;
	case CMD_COMMANDS:
		quick_open(QUICK_COMMANDS, ">");
		break;
	case CMD_GOTO_LINE:
		quick_open(QUICK_GOTO_LINE, ":");
		break;
	case CMD_SAVE:
		if (v)
			doc_save(v->doc);
		break;
	case CMD_SAVE_ALL:
		for (u32 d = 0; d < app.docs.n; d++)
			if ((app.docs.path[d] || app.docs.untitled[d]) && text_dirty(&app.docs.text[d]) &&
			    !app.docs.untitled[d])
				doc_save(d);
		break;
	case CMD_NEW_FILE: {
		u32 d = doc_new_untitled();
		if (d != UINT32_MAX) {
			show_doc(d, false);
			app.focus = FOCUS_EDITOR;
		}
		break;
	}
	case CMD_CLOSE_EDITOR: {
		Group *g = &app.groups[app.active_group];
		if (g->nt)
			close_tab(app.active_group, g->active);
		else if (app.ngroups > 1) {
			memmove(app.groups + app.active_group, app.groups + app.active_group + 1,
			        (app.ngroups - app.active_group - 1) * sizeof(Group));
			app.ngroups--;
			app.active_group = 0;
			ui_layout();
		}
		break;
	}
	case CMD_CLOSE_ALL:
		for (u32 gi = app.ngroups; gi-- > 0;)
			while (app.groups[gi].nt)
				close_tab(gi, app.groups[gi].nt - 1);
		break;
	case CMD_NEXT_EDITOR:
	case CMD_PREV_EDITOR: {
		Group *g = &app.groups[app.active_group];
		if (g->nt > 1) {
			u32 t = cmd == CMD_NEXT_EDITOR ? (g->active + 1) % g->nt : (g->active + g->nt - 1) % g->nt;
			activate_tab(app.active_group, t);
		}
		break;
	}
	case CMD_REPLACE_IN_FILES:
		app.search_show_replace = true;
		run_command(CMD_VIEW_SEARCH);
		break;
	case CMD_SEARCH_REPLACE_ALL:
		search_replace(UINT32_MAX, UINT32_MAX);
		break;
	case CMD_SEARCH_NEXT_RESULT:
	case CMD_SEARCH_PREV_RESULT:
		search_step_result(cmd == CMD_SEARCH_NEXT_RESULT ? 1 : -1);
		break;
	case CMD_SEARCH_TOGGLE_DETAILS:
		app.search_show_details = !app.search_show_details;
		if (app.search_show_details) {
			app.side_view = SIDE_SEARCH;
			app.side_visible = true;
			app.focus = FOCUS_SEARCH_INPUT;
			app.search_box = 2;
		}
		ui_layout();
		break;
	case CMD_TOGGLE_SIDEBAR:
		app.side_visible = !app.side_visible;
		if (!app.side_visible && app.focus == FOCUS_SIDEBAR)
			app.focus = FOCUS_EDITOR;
		ui_layout();
		break;
	case CMD_TOGGLE_PANEL:
		app.panel_visible = !app.panel_visible;
		app.focus = app.panel_visible ? FOCUS_PANEL : FOCUS_EDITOR;
		ui_layout();
		break;
	case CMD_VIEW_EXPLORER:
	case CMD_VIEW_SEARCH:
	case CMD_VIEW_SCM: {
		int view = cmd == CMD_VIEW_EXPLORER ? SIDE_EXPLORER : cmd == CMD_VIEW_SEARCH ? SIDE_SEARCH : SIDE_SCM;
		app.side_view = view;
		app.side_visible = true;
		app.focus = view == SIDE_SEARCH ? FOCUS_SEARCH_INPUT : view == SIDE_SCM ? FOCUS_SCM_INPUT : FOCUS_SIDEBAR;
		if (view == SIDE_SEARCH)
			app.search_box = 0;
		if (view == SIDE_SEARCH && v && v->head[0] != v->anchor[0]) {
			/* seed with the selection like VSCodium */
			u32 a = VX_MIN(v->head[0], v->anchor[0]), b = VX_MAX(v->head[0], v->anchor[0]);
			if (b - a < 200) {
				char buf[200];
				text_copy(&app.docs.text[v->doc], a, b - a, buf);
				if (!memchr(buf, '\n', b - a))
					input_set(&app.search_in, buf, b - a);
			}
			app.search_in.anchor = 0;
			app.search_in.cur = app.search_in.n;
		} else if (view == SIDE_SEARCH) {
			app.search_in.anchor = 0;
			app.search_in.cur = app.search_in.n;
		}
		if (view == SIDE_SCM)
			scm_refresh();
		ui_layout();
		break;
	}
	case CMD_VIEW_PROBLEMS:
	case CMD_VIEW_OUTPUT: {
		int pv = cmd == CMD_VIEW_PROBLEMS ? PANEL_PROBLEMS : PANEL_OUTPUT;
		if (cmd == CMD_VIEW_OUTPUT && app.panel_visible && app.panel_view == pv) {
			app.panel_visible = false;
			app.focus = FOCUS_EDITOR;
		} else {
			app.panel_visible = true;
			app.panel_view = pv;
			app.focus = FOCUS_PANEL;
		}
		ui_layout();
		break;
	}
	case CMD_SPLIT:
		if (v && app.ngroups < MAX_GROUPS) {
			u32 gi = app.active_group + 1;
			memmove(app.groups + gi + 1, app.groups + gi, (app.ngroups - gi) * sizeof(Group));
			memset(&app.groups[gi], 0, sizeof(Group));
			app.ngroups++;
			ui_layout();
			View *src = v;
			app.active_group = gi;
			u32 d = src->doc;
			show_doc(d, false);
			View *nv = active_view();
			nv->nc = 1;
			nv->head[0] = src->head[0];
			nv->anchor[0] = src->anchor[0];
			nv->scroll_y = src->scroll_y;
			app.focus = FOCUS_EDITOR;
		}
		break;
	case CMD_FOCUS_GROUP1:
		focus_group(0);
		break;
	case CMD_FOCUS_GROUP2:
		focus_group(1);
		break;
	case CMD_FOCUS_GROUP3:
		focus_group(2);
		break;
	case CMD_NAV_BACK:
	case CMD_NAV_FORWARD:
		if (app.nav_n) {
			if (cmd == CMD_NAV_BACK && v)
				nav_push(v->doc, v->head[0]);
			u32 p = app.nav_pos;
			if (cmd == CMD_NAV_BACK && p > 0)
				p--;
			else if (cmd == CMD_NAV_FORWARD && p + 1 < app.nav_n)
				p++;
			else
				break;
			app.nav_pos = p;
			u32 d = app.nav_doc[p];
			if (d < app.docs.n && (app.docs.path[d] || app.docs.untitled[d])) {
				show_doc(d, false);
				View *nv = active_view();
				ed_set_cursor(nv, VX_MIN(app.nav_off[p], app.docs.text[d].len), false);
				ed_reveal(nv, nv->head[0], true);
				app.focus = FOCUS_EDITOR;
			}
		}
		break;
	case CMD_NEXT_PROBLEM:
		goto_problem(1);
		break;
	case CMD_PREV_PROBLEM:
		goto_problem(-1);
		break;
	case CMD_TOGGLE_THEME:
		app.theme = app.theme == THEME_DARK ? THEME_LIGHT : THEME_DARK;
		ui_status("Color Theme: %s", themes[app.theme].name);
		break;
	case CMD_THEME_DARK:
		app.theme = THEME_DARK;
		break;
	case CMD_THEME_LIGHT:
		app.theme = THEME_LIGHT;
		break;
	case CMD_GIT_REFRESH:
		app.git_ns = 0;
		scm_refresh();
		break;
	case CMD_GIT_COMMIT:
		if (app.commit_in.n && app.git && !git_has_staged()) {
			/* VSCodium asks before a smart commit of all changes */
			app.quick = QUICK_CONFIRM;
			app.quick_prompt_kind = 8;
			snprintf(app.quick_title, sizeof app.quick_title,
			         "There are no staged changes to commit. Stage all your changes and commit them directly?");
			input_set(&app.quick_in, "", 0);
			app.focus = FOCUS_QUICK;
		} else if (app.commit_in.n && app.git) {
			char *msg = vx_strndup(app.commit_in.b, app.commit_in.n);
			if (git_commit_staged(app.git, msg) == 0) {
				input_set(&app.commit_in, "", 0);
				ui_status("Committed to %s", git_branch(app.git));
			} else {
				ui_status("Git: %s", git_last_error());
			}
			free(msg);
			scm_refresh();
		} else if (app.git) {
			app.quick = QUICK_PROMPT;
			app.quick_prompt_kind = 5;
			snprintf(app.quick_title, sizeof app.quick_title, "Commit message (staged changes)");
			input_set(&app.quick_in, "", 0);
			app.focus = FOCUS_QUICK;
		}
		break;
	case CMD_GIT_STAGE_ALL:
		if (app.git) {
			git_stage_all(app.git);
			scm_refresh();
		}
		break;
	case CMD_GIT_STAGE_FILE:
	case CMD_GIT_UNSTAGE_FILE:
	case CMD_GIT_DISCARD_FILE:
		if (app.git && v && app.docs.path[v->doc]) {
			const char *wd = git_workdir(app.git);
			const char *p = app.docs.path[v->doc];
			size_t wl = strlen(wd);
			if (strncmp(p, wd, wl) == 0) {
				if (cmd == CMD_GIT_STAGE_FILE)
					git_stage(app.git, p + wl);
				else if (cmd == CMD_GIT_UNSTAGE_FILE)
					git_unstage(app.git, p + wl);
				else
					git_discard(app.git, p + wl);
			}
			scm_refresh();
		}
		break;
	case CMD_GIT_TOGGLE_BLAME:
		app.blame_on = !app.blame_on;
		break;
	case CMD_REFRESH_EXPLORER:
		fs_refresh(&app.tree);
		app.files_ok = false;
		break;
	case CMD_COLLAPSE_FOLDERS:
		fs_collapse_all(&app.tree);
		break;
	case CMD_REVEAL_IN_EXPLORER:
		if (v) {
			app.side_view = SIDE_EXPLORER;
			app.side_visible = true;
			explorer_reveal(v->doc);
			app.focus = FOCUS_SIDEBAR;
			ui_layout();
		}
		break;
	case CMD_RESTART_LSP:
		restart_lsp();
		break;
	case CMD_QUIT:
		app.running = false;
		break;
	default:
		break;
	}
}

/* ---- input routing ---------------------------------------------------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
	return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static bool focus_is_input(void)
{
	return app.focus == FOCUS_QUICK || app.focus == FOCUS_FIND ||
	       app.focus == FOCUS_SEARCH_INPUT || app.focus == FOCUS_SCM_INPUT;
}

static int key_mods(int m) { return m & (MOD_CTRL | MOD_SHIFT | MOD_ALT); }

static bool try_bindings(const Event *e)
{
	int mods = key_mods(e->mods);
	bool chord = app.chord;
	if (!chord && mods == MOD_CTRL && e->key == SDLK_K) {
		app.chord = true;
		ui_status("(Ctrl+K) was pressed. Waiting for second key of chord...");
		return true;
	}
	if (chord) {
		app.chord = false;
		app.status_until = 0;
	}
	for (size_t i = 0; i < VX_COUNT(bindings); i++) {
		const Binding *b = &bindings[i];
		if (b->key != e->key || b->mods != mods || b->chord != chord)
			continue;
		if (b->when == W_EDITOR && app.focus != FOCUS_EDITOR)
			continue;
		/* inputs keep their own editing keys */
		if (focus_is_input() && b->when != W_EDITOR &&
		    (b->cmd == CMD_UNDO || b->cmd == CMD_SELECT_ALL))
			continue;
		run_command(b->cmd);
		return true;
	}
	if (chord) {
		char k[32];
		key_name(k, sizeof k, mods, e->key);
		ui_status("The key combination (Ctrl+K, %s) is not a command.", k);
		return true;
	}
	return false;
}

/* Sidebar title + activity bar hit testing. */
static int activity_hit(int y)
{
	int i = y / PX(48);
	return i < SIDE_COUNT ? i : -1;
}

static void sidebar_area(int *x, int *y, int *w, int *h)
{
	*x = app.side_x;
	*y = 0;
	*w = app.side_w;
	*h = app.h - app.status_h;
}

static void panel_area(int *x, int *y, int *w, int *h)
{
	*x = app.ed_x;
	*y = app.panel_y;
	*w = app.ed_w;
	*h = app.panel_h;
}

static bool tabs_mouse(Group *g, u32 gi, const Event *e);
static bool panel_mouse(const Event *e);

void ui_event(const Event *e)
{
	app.dirty = true;
	app.blink_t0 = vx_now_ns();
	if (e->type == EV_RESIZE) {
		app.w = e->x;
		app.h = e->y;
		ui_layout();
		return;
	}
	if (e->type == EV_QUIT) {
		app.running = false;
		return;
	}
	if (e->type == EV_KEY) {
		/* any key dismisses the hover (Ctrl+K Ctrl+I reopens it) */
		app.hover_open = false;
		app.hover_due = 0;
		if (app.quick && (e->key == SDLK_ESCAPE || e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER ||
		                  e->key == SDLK_UP || e->key == SDLK_DOWN))
			if (quick_key(e))
				return;
		if (e->key == SDLK_ESCAPE && !app.quick) {
			if (app.compl_open || app.hover_open) {
				app.compl_open = app.hover_open = false;
				return;
			}
			if (app.find_open) {
				app.find_open = false;
				app.focus = FOCUS_EDITOR;
				return;
			}
		}
		if (try_bindings(e))
			return;
		switch (app.focus) {
		case FOCUS_QUICK:
			quick_key(e);
			return;
		case FOCUS_FIND: {
			View *v = active_view();
			Input *in = app.find_replace && app.find_in.n == UINT32_MAX ? &app.repl_in : NULL;
			(void)in;
			if (!v)
				return;
			bool in_repl = app.find_flags & 0x100;
			if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
				if ((e->mods & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT))
					ed_replace_all(v);
				else if (in_repl)
					ed_replace_one(v);
				else
					ed_find_next(v, e->mods & MOD_SHIFT);
				return;
			}
			if (e->key == SDLK_TAB && app.find_replace) {
				/* toggle between find and replace boxes */
				app.find_flags ^= 0x100;
				return;
			}
			int toggle = e->mods == MOD_ALT ? (e->key == SDLK_C ? SEARCH_CASE : e->key == SDLK_W ? SEARCH_WORD
			                                   : e->key == SDLK_R ? SEARCH_REGEX : 0)
			                                : 0;
			if (toggle) {
				app.find_flags ^= toggle;
				ed_find_update(v);
				return;
			}
			Input *box = in_repl ? &app.repl_in : &app.find_in;
			u32 before = app.find_in.n;
			input_key(box, e);
			if (box == &app.find_in && app.find_in.n != before)
				ed_find_update(v);
			return;
		}
		case FOCUS_SEARCH_INPUT:
		case FOCUS_SCM_INPUT:
		case FOCUS_SIDEBAR: {
			int x, y, w, h;
			sidebar_area(&x, &y, &w, &h);
			if (app.side_view == SIDE_EXPLORER)
				explorer_event(e, x, y, w, h);
			else if (app.side_view == SIDE_SEARCH)
				search_event(e, x, y, w, h);
			else
				scm_event(e, x, y, w, h);
			return;
		}
		case FOCUS_PANEL: {
			int x, y, w, h;
			panel_area(&x, &y, &w, &h);
			if (app.panel_view == PANEL_PROBLEMS)
				problems_event(e, x, y, w, h);
			else
				output_event(e, x, y, w, h);
			return;
		}
		default: {
			View *v = active_view();
			if (v)
				ed_key(v, e);
			return;
		}
		}
	}
	if (e->type == EV_TEXT) {
		if (app.chord)
			return;
		switch (app.focus) {
		case FOCUS_QUICK:
			input_text(&app.quick_in, e->text, strlen(e->text));
			quick_update();
			return;
		case FOCUS_FIND: {
			Input *box = (app.find_flags & 0x100) ? &app.repl_in : &app.find_in;
			input_text(box, e->text, strlen(e->text));
			View *v = active_view();
			if (v && box == &app.find_in)
				ed_find_update(v);
			return;
		}
		case FOCUS_SEARCH_INPUT:
		case FOCUS_SCM_INPUT:
		case FOCUS_SIDEBAR: {
			int x, y, w, h;
			sidebar_area(&x, &y, &w, &h);
			if (app.side_view == SIDE_SEARCH)
				search_event(e, x, y, w, h);
			else if (app.side_view == SIDE_SCM)
				scm_event(e, x, y, w, h);
			else
				explorer_event(e, x, y, w, h);
			return;
		}
		case FOCUS_EDITOR: {
			View *v = active_view();
			if (v)
				ed_text(v, e->text, strlen(e->text));
			return;
		}
		default:
			return;
		}
	}
	/* mouse */
	int mx = e->x, my = e->y;
	if (e->type == EV_MMOVE) {
		app.hover_x = mx;
		app.hover_y = my;
	}
	if (e->type == EV_MUP) {
		app.mouse_down = false;
		int d = app.drag;
		app.drag = DRAG_NONE;
		if (d == DRAG_EDITOR) {
			View *v = active_view();
			Group *g = &app.groups[app.active_group];
			if (v)
				ed_mouse(v, e, g->x, g->y + app.tabs_h + app.crumbs_h, g->w,
				         g->h - app.tabs_h - app.crumbs_h);
		}
		return;
	}
	if (e->type == EV_MMOVE && app.drag) {
		if (app.drag == DRAG_SIDE) {
			app.side_w = VX_CLAMP(mx - app.side_x, PX(170), app.w - app.act_w - PX(200));
			ui_layout();
		} else if (app.drag == DRAG_PANEL) {
			app.panel_h = app.h - app.status_h - my;
			ui_layout();
		} else if (app.drag == DRAG_EDITOR) {
			View *v = active_view();
			Group *g = &app.groups[app.active_group];
			if (v)
				ed_mouse(v, e, g->x, g->y + app.tabs_h + app.crumbs_h, g->w,
				         g->h - app.tabs_h - app.crumbs_h);
		}
		return;
	}
	if (app.quick && e->type == EV_MDOWN) {
		/* click inside the quick list selects; outside closes */
		int w = VX_MIN(PX(600), app.w - PX(40));
		int x = (app.w - w) / 2;
		int top = PX(4) + PX(6) + (app.quick_title[0] ? PX(22) : 0) + PX(26) + PX(6);
		if (in_rect(mx, my, x, top, w, PX(22) * 16)) {
			u32 r = (u32)((my - top) / PX(22)) + app.quick_top;
			if (r < app.quick_n) {
				app.quick_sel = r;
				quick_accept();
			}
			return;
		}
		if (!in_rect(mx, my, x, PX(4), w, top))
			quick_close();
		return;
	}
	if (e->type == EV_MDOWN) {
		app.mouse_down = true;
		app.compl_open = false;
		/* sashes */
		if (app.side_visible && abs(mx - (app.side_x + app.side_w)) <= PX(2) && my < app.h - app.status_h) {
			app.drag = DRAG_SIDE;
			return;
		}
		if (app.panel_visible && abs(my - app.panel_y) <= PX(2) && mx >= app.ed_x) {
			app.drag = DRAG_PANEL;
			return;
		}
	}
	if (my >= app.h - app.status_h) {
		if (e->type == EV_MDOWN) {
			/* status bar: problems item toggles the panel */
			if (mx < PX(260) && mx > PX(100))
				run_command(CMD_VIEW_PROBLEMS);
		}
		return;
	}
	if (mx < app.act_w) {
		if (e->type == EV_MDOWN) {
			int i = activity_hit(my);
			if (i >= 0) {
				if (app.side_visible && app.side_view == i)
					run_command(CMD_TOGGLE_SIDEBAR);
				else
					run_command(i == SIDE_EXPLORER ? CMD_VIEW_EXPLORER
					            : i == SIDE_SEARCH ? CMD_VIEW_SEARCH
					                               : CMD_VIEW_SCM);
			} else if (my > app.h - app.status_h - PX(48)) {
				run_command(CMD_TOGGLE_THEME);
			}
		}
		return;
	}
	if (app.side_visible && mx < app.side_x + app.side_w) {
		int x, y, w, h;
		sidebar_area(&x, &y, &w, &h);
		if (e->type == EV_MDOWN && app.focus != FOCUS_SEARCH_INPUT && app.focus != FOCUS_SCM_INPUT)
			app.focus = FOCUS_SIDEBAR;
		if (app.side_view == SIDE_EXPLORER)
			explorer_event(e, x, y, w, h);
		else if (app.side_view == SIDE_SEARCH)
			search_event(e, x, y, w, h);
		else
			scm_event(e, x, y, w, h);
		return;
	}
	if (app.panel_visible && my >= app.panel_y) {
		if (e->type == EV_MDOWN)
			app.focus = FOCUS_PANEL;
		panel_mouse(e);
		return;
	}
	for (u32 gi = 0; gi < app.ngroups; gi++) {
		Group *g = &app.groups[gi];
		if (!in_rect(mx, my, g->x, g->y, g->w, g->h))
			continue;
		if (my < g->y + app.tabs_h) {
			tabs_mouse(g, gi, e);
			return;
		}
		if (my < g->y + app.tabs_h + app.crumbs_h)
			return;
		if (g->nt == 0)
			return;
		if (e->type == EV_MDOWN) {
			app.active_group = gi;
			if (app.focus != FOCUS_FIND)
				app.focus = FOCUS_EDITOR;
		}
		View *v = g->tab[g->active];
		if (e->type == EV_MDOWN && e->button == 1)
			app.drag = DRAG_EDITOR;
		ed_mouse(v, e, g->x, g->y + app.tabs_h + app.crumbs_h, g->w, g->h - app.tabs_h - app.crumbs_h);
		return;
	}
}

/* ---- file icons (Seti-like coloured letters) ------------------------------ */

typedef struct {
	const char *ext;
	const char *glyph;
	u32 color;
} IconRule;

static const IconRule icon_rules[] = {
    {".c", "C", 0x519ABAFF},     {".h", "h", 0xA074C4FF},     {".s", "S", 0x8DC149FF},
    {".S", "S", 0x8DC149FF},     {".asm", "S", 0x8DC149FF},   {".nasm", "S", 0x8DC149FF},
    {".inc", "S", 0x8DC149FF},   {".lisp", "\xCE\xBB", 0xA074C4FF}, {".lsp", "\xCE\xBB", 0xA074C4FF},
    {".cl", "\xCE\xBB", 0xA074C4FF}, {".el", "\xCE\xBB", 0xA074C4FF}, {".scm", "\xCE\xBB", 0xA074C4FF},
    {".rkt", "\xCE\xBB", 0xA074C4FF}, {".clj", "\xCE\xBB", 0x8DC149FF}, {".md", "M", 0x519ABAFF},
    {".json", "{}", 0xCBCB41FF}, {".py", "py", 0x519ABAFF},   {".sh", ">", 0x4D5A5EFF},
    {".txt", "\xE2\x89\xA1", 0x6D8086FF}, {".cpp", "C", 0xA074C4FF}, {".hpp", "h", 0xA074C4FF},
    {".glsl", "G", 0x519ABAFF},  {".vert", "G", 0x519ABAFF},  {".frag", "G", 0x519ABAFF},
    {".comp", "G", 0x519ABAFF},  {".yml", "y", 0xA074C4FF},   {".yaml", "y", 0xA074C4FF},
    {".toml", "t", 0x6D8086FF},  {".pl", "pl", 0x519ABAFF},  {".pm", "pm", 0x519ABAFF},
    {".bash", ">", 0x4D5A5EFF},  {".zsh", ">", 0x4D5A5EFF},  {".markdown", "M", 0x519ABAFF},  {".png", "\xE2\x96\xA3", 0xA074C4FF}, {".mk", "M", 0xE37933FF},
};

int file_icon_draw(const char *name, int x, int y, int h)
{
	const char *glyph = NULL;
	u32 color = 0;
	if (strcmp(name, "Makefile") == 0 || strcmp(name, "GNUmakefile") == 0 ||
	    strcmp(name, "BSDmakefile") == 0 || strcmp(name, "makefile") == 0) {
		glyph = "M";
		color = 0xE37933FF;
	} else if (strcmp(name, ".gitignore") == 0 || strcmp(name, ".gitattributes") == 0) {
		glyph = "\xE2\x97\x86";
		color = 0x41535BFF;
	} else if (strncmp(name, "LICENSE", 7) == 0 || strncmp(name, "COPYING", 7) == 0) {
		glyph = "\xC2\xA7";
		color = 0xCBCB41FF;
	} else {
		const char *dot = strrchr(name, '.');
		if (dot)
			for (size_t i = 0; i < VX_COUNT(icon_rules); i++)
				if (strcmp(dot, icon_rules[i].ext) == 0) {
					glyph = icon_rules[i].glyph;
					color = icon_rules[i].color;
					break;
				}
	}
	int box = PX(16);
	if (glyph) {
		int gw = font_width(FONT_MONO_BOLD, glyph, strlen(glyph));
		int gy = y + (h - font_m[FONT_MONO_BOLD].line) / 2;
		draw_text(FONT_MONO_BOLD, x + (box - gw) / 2, gy, glyph, strlen(glyph), color);
	} else {
		/* default file: a small page outline */
		int px = x + PX(3), py = y + (h - PX(12)) / 2;
		draw_frame(px, py, PX(10), PX(12), 0x6D8086FF);
	}
	return x + box + PX(6);
}

/* ---- tabs, breadcrumbs ------------------------------------------------- */

static int tab_width(View *v)
{
	const char *rel = app.docs.rel[v->doc];
	const char *base = strrchr(rel, '/');
	base = base ? base + 1 : rel;
	return PX(10) + PX(22) + font_width(FONT_UI, base, strlen(base)) + PX(4) + PX(28);
}

static int tabs_scroll_for(Group *g)
{
	/* keep the active tab visible: shift left if needed */
	int x = 0, ax0 = 0, ax1 = 0;
	for (u32 t = 0; t < g->nt; t++) {
		int w = tab_width(g->tab[t]);
		if (t == g->active) {
			ax0 = x;
			ax1 = x + w;
		}
		x += w;
	}
	int scroll = 0;
	if (ax1 > g->w)
		scroll = ax1 - g->w;
	if (ax0 < scroll)
		scroll = ax0;
	return scroll;
}

static bool tabs_mouse(Group *g, u32 gi, const Event *e)
{
	if (e->type != EV_MDOWN)
		return false;
	int x = g->x - tabs_scroll_for(g);
	for (u32 t = 0; t < g->nt; t++) {
		int w = tab_width(g->tab[t]);
		if (e->x >= x && e->x < x + w) {
			bool close = e->x >= x + w - PX(28) || e->button == 2;
			if (close) {
				close_tab(gi, t);
			} else {
				activate_tab(gi, t);
				app.focus = FOCUS_EDITOR;
				if (e->clicks >= 2)
					g->tab[t]->preview = false;
			}
			return true;
		}
		x += w;
	}
	if (e->clicks >= 2) {
		app.active_group = gi;
		run_command(CMD_NEW_FILE);
	}
	return true;
}

static u32 git_color(u16 f, bool *has, char *letter)
{
	*has = true;
	if (f & GS_CONFLICT) {
		*letter = '!';
		return COL(C_GIT_CONFLICT);
	}
	if (f & (GS_WT_DELETED | GS_INDEX_DELETED)) {
		*letter = 'D';
		return COL(C_GIT_DELETED);
	}
	if (f & GS_WT_MODIFIED) {
		*letter = 'M';
		return COL(C_GIT_MODIFIED);
	}
	if (f & GS_WT_NEW) {
		*letter = 'U';
		return COL(C_GIT_UNTRACKED);
	}
	if (f & GS_INDEX_NEW) {
		*letter = 'A';
		return COL(C_GIT_ADDED);
	}
	if (f & (GS_INDEX_MODIFIED | GS_INDEX_RENAMED | GS_WT_RENAMED)) {
		*letter = (f & (GS_INDEX_RENAMED | GS_WT_RENAMED)) ? 'R' : 'M';
		return COL(C_GIT_MODIFIED);
	}
	if (f & GS_IGNORED) {
		*letter = 0;
		return COL(C_GIT_IGNORED);
	}
	*has = false;
	*letter = 0;
	return 0;
}

u32 doc_git_color(u32 d, char *letter, bool *has)
{
	*has = false;
	*letter = 0;
	if (!app.git || !app.docs.path[d])
		return 0;
	const char *wd = git_workdir(app.git);
	size_t wl = strlen(wd);
	if (strncmp(app.docs.path[d], wd, wl) != 0)
		return 0;
	return git_color(git_path_flags(app.git, app.docs.path[d] + wl), has, letter);
}

u32 path_git_color(const char *rel, char *letter, bool *has)
{
	*has = false;
	*letter = 0;
	if (!app.git)
		return 0;
	/* rel is relative to the project root; the repo may sit above it */
	char abs[8192];
	snprintf(abs, sizeof abs, "%s/%s", app.root, rel);
	const char *wd = git_workdir(app.git);
	size_t wl = strlen(wd);
	if (strncmp(abs, wd, wl) != 0)
		return 0;
	return git_color(git_path_flags(app.git, abs + wl), has, letter);
}

static void doc_problem_counts(u32 d, u32 *errs, u32 *warns)
{
	*errs = *warns = 0;
	for (u32 i = 0; i < app.docs.dn[d]; i++) {
		if (app.docs.d_sev[d][i] == LSP_SEV_ERROR)
			(*errs)++;
		else if (app.docs.d_sev[d][i] == LSP_SEV_WARNING)
			(*warns)++;
	}
}

static void draw_tabs(Group *g, u32 gi)
{
	int y = g->y;
	draw_rect(g->x, y, g->w, app.tabs_h, COL(C_TABS_BG));
	draw_rect(g->x, y + app.tabs_h - PX(1), g->w, PX(1), COL(C_TABS_BORDER));
	draw_clip(g->x, y, g->w, app.tabs_h);
	int x = g->x - tabs_scroll_for(g);
	bool group_active = gi == app.active_group;
	for (u32 t = 0; t < g->nt; t++) {
		View *v = g->tab[t];
		int w = tab_width(v);
		bool active = t == g->active;
		bool hover = in_rect(app.hover_x, app.hover_y, x, y, w, app.tabs_h);
		draw_rect(x, y, w, app.tabs_h - (active ? 0 : PX(1)), active ? COL(C_TAB_ACTIVE_BG) : COL(C_TAB_INACTIVE_BG));
		if (active && group_active)
			draw_rect(x, y, w, PX(1), COL(C_TAB_ACTIVE_BORDER_TOP));
		draw_rect(x + w - PX(1), y, PX(1), app.tabs_h, COL(C_TAB_BORDER));
		const char *rel = app.docs.rel[v->doc];
		const char *base = strrchr(rel, '/');
		base = base ? base + 1 : rel;
		int tx = file_icon_draw(base, x + PX(10), y, app.tabs_h);
		char letter;
		bool has;
		u32 gc = doc_git_color(v->doc, &letter, &has);
		u32 errs, warns;
		doc_problem_counts(v->doc, &errs, &warns);
		u32 fg = active ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG);
		if (errs)
			fg = COL(C_ERROR);
		else if (warns)
			fg = COL(C_WARNING);
		else if (has && letter)
			fg = gc;
		if (!active && (errs || warns || (has && letter)))
			fg = (fg & 0xFFFFFF00u) | 0xB0u;
		int font = FONT_UI;
		draw_text(font, tx, y + (app.tabs_h - font_m[font].line) / 2, base, strlen(base), fg);
		if (v->preview) {
			/* preview tabs are italic in VSCodium; underline-free hint: dim */
		}
		int cx = x + w - PX(28) + PX(6), cy = y + (app.tabs_h - PX(16)) / 2;
		bool dirty = text_dirty(&app.docs.text[v->doc]);
		if (hover && in_rect(app.hover_x, app.hover_y, x + w - PX(28), y, PX(28), app.tabs_h))
			draw_rect(cx - PX(2), cy - PX(2), PX(20), PX(20), COL(C_STATUS_HOVER_BG));
		if (dirty && !hover)
			draw_icon(ICON_DOT, cx, cy, active ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG));
		else if (active || hover)
			draw_icon(ICON_CLOSE, cx, cy, active ? COL(C_TAB_ACTIVE_FG) : COL(C_TAB_INACTIVE_FG));
		x += w;
	}
	draw_noclip();
}

static void draw_breadcrumbs(Group *g)
{
	View *v = g->nt ? g->tab[g->active] : NULL;
	int y = g->y + app.tabs_h;
	draw_rect(g->x, y, g->w, app.crumbs_h, COL(C_EDITOR_BG));
	if (!v)
		return;
	const char *rel = app.docs.rel[v->doc];
	int x = g->x + PX(14);
	int ty = y + (app.crumbs_h - font_m[FONT_UI].line) / 2;
	draw_clip(g->x, y, g->w, app.crumbs_h);
	const char *s = rel;
	for (;;) {
		const char *sl = strchr(s, '/');
		if (!sl) {
			x = file_icon_draw(s, x - PX(2), y, app.crumbs_h) - PX(2);
			draw_text(FONT_UI, x, ty, s, strlen(s), COL(C_BREADCRUMB_FG));
			break;
		}
		x = draw_text(FONT_UI, x, ty, s, (size_t)(sl - s), COL(C_BREADCRUMB_FG));
		draw_icon(ICON_CHEVRON_RIGHT, x + PX(2), y + (app.crumbs_h - PX(16)) / 2, COL(C_BREADCRUMB_FG));
		x += PX(20);
		s = sl + 1;
	}
	draw_noclip();
}

static void draw_group(Group *g, u32 gi)
{
	draw_tabs(g, gi);
	draw_breadcrumbs(g);
	int y = g->y + app.tabs_h + app.crumbs_h;
	int h = g->h - app.tabs_h - app.crumbs_h;
	if (!g->nt) {
		draw_rect(g->x, g->y + app.tabs_h, g->w, g->h - app.tabs_h, COL(C_EDITOR_BG));
		if (gi == 0 && app.ngroups == 1) {
			/* empty editor hints, like the VSCodium watermark */
			static const int cmds[] = {CMD_COMMANDS, CMD_QUICK_OPEN, CMD_VIEW_SEARCH, CMD_TOGGLE_PANEL,
			                           CMD_TOGGLE_THEME};
			static const char *labels[] = {"Show All Commands", "Go to File", "Find in Files",
			                               "Toggle Panel", "Toggle Light/Dark Theme"};
			int cy = g->y + g->h / 2 - PX(60);
			for (size_t i = 0; i < VX_COUNT(cmds); i++) {
				const char *k = command_keys(cmds[i]);
				int lw = font_width(FONT_UI, labels[i], strlen(labels[i]));
				int mid = g->x + g->w / 2;
				u32 c = COL(C_FG_DIM);
				draw_text(FONT_UI, mid - lw - PX(8), cy, labels[i], strlen(labels[i]), c);
				if (k) {
					/* key caps: square boxes */
					int kx = mid + PX(8);
					const char *p = k;
					while (*p) {
						const char *plus = strchr(p, '+');
						const char *sp = strchr(p, ' ');
						const char *end = plus ? plus : p + strlen(p);
						if (sp && sp < end)
							end = sp;
						int kw = font_width(FONT_UI, p, (size_t)(end - p));
						draw_rect(kx, cy - PX(1), kw + PX(10), font_m[FONT_UI].line + PX(2), COL(C_INPUT_BG));
						draw_frame(kx, cy - PX(1), kw + PX(10), font_m[FONT_UI].line + PX(2), COL(C_INPUT_BORDER));
						draw_text(FONT_UI, kx + PX(5), cy, p, (size_t)(end - p), c);
						kx += kw + PX(14);
						if (!*end)
							break;
						p = end + 1;
					}
				}
				cy += PX(28);
			}
		}
		return;
	}
	View *v = g->tab[g->active];
	ed_draw(g, v, g->x, y, g->w, h, gi == app.active_group && app.focus == FOCUS_EDITOR);
}

/* ---- activity bar, sidebar, panel, status -------------------------------- */

static void draw_activity_bar(void)
{
	int h = app.h - app.status_h;
	draw_rect(0, 0, app.act_w, h, COL(C_ACTBAR_BG));
	draw_rect(app.act_w - PX(1), 0, PX(1), h, COL(C_ACTBAR_BORDER));
	static const int icons[SIDE_COUNT] = {ICON_FILES, ICON_SEARCH, ICON_SCM};
	for (int i = 0; i < SIDE_COUNT; i++) {
		int y = i * PX(48);
		bool active = app.side_visible && app.side_view == i;
		bool hover = in_rect(app.hover_x, app.hover_y, 0, y, app.act_w, PX(48));
		u32 c = active || hover ? COL(C_ACTBAR_FG) : COL(C_ACTBAR_FG_INACTIVE);
		if (active)
			draw_rect(0, y, PX(2), PX(48), COL(C_ACTBAR_ACTIVE_BORDER));
		draw_icon(icons[i], (app.act_w - PX(24)) / 2, y + PX(12), c);
		if (i == SIDE_SCM && app.git) {
			/* badge with the number of changes */
			const GitStatus *st = git_status(app.git);
			u32 n = 0;
			for (u32 k = 0; k < st->count; k++)
				if (!(st->flags[k] & GS_IGNORED))
					n++;
			if (n) {
				char b[16];
				int bn = snprintf(b, sizeof b, "%u", n > 9999 ? 9999 : n);
				int bw = VX_MAX(PX(16), font_width(FONT_SMALL, b, (size_t)bn) + PX(8));
				int bx = app.act_w - PX(10) - bw + PX(6), by = y + PX(26);
				draw_rect(bx, by, bw, PX(16), COL(C_BADGE_BG));
				int tw = font_width(FONT_SMALL, b, (size_t)bn);
				draw_text(FONT_SMALL, bx + (bw - tw) / 2, by + (PX(16) - font_m[FONT_SMALL].line) / 2, b,
				          (size_t)bn, COL(C_BADGE_FG));
			}
		}
	}
	/* theme toggle (gear position) at the bottom */
	int gy = h - PX(48);
	bool hover = in_rect(app.hover_x, app.hover_y, 0, gy, app.act_w, PX(48));
	draw_icon(ICON_GEAR, (app.act_w - PX(24)) / 2, gy + PX(12),
	          hover ? COL(C_ACTBAR_FG) : COL(C_ACTBAR_FG_INACTIVE));
}

static void draw_sidebar(void)
{
	int x, y, w, h;
	sidebar_area(&x, &y, &w, &h);
	draw_rect(x, y, w, h, COL(C_SIDEBAR_BG));
	draw_rect(x + w - PX(1), y, PX(1), h, COL(C_SIDEBAR_BORDER));
	draw_clip(x, y, w - PX(1), h);
	if (app.side_view == SIDE_EXPLORER)
		explorer_draw(x, y, w - PX(1), h);
	else if (app.side_view == SIDE_SEARCH)
		search_draw(x, y, w - PX(1), h);
	else
		scm_draw(x, y, w - PX(1), h);
	draw_noclip();
	if (app.drag == DRAG_SIDE || (abs(app.hover_x - (x + w)) <= PX(2) && app.hover_y < h))
		draw_rect(x + w - PX(2), y, PX(4), h, COL(C_SASH_HOVER));
}

u32 problems_count(int sev)
{
	u32 n = 0;
	for (u32 d = 0; d < app.docs.n; d++)
		for (u32 i = 0; i < app.docs.dn[d]; i++)
			if (sev == 0 || app.docs.d_sev[d][i] == sev)
				n++;
	return n;
}

static bool panel_mouse(const Event *e)
{
	int x, y, w, h;
	panel_area(&x, &y, &w, &h);
	if (e->type == EV_MDOWN && e->y < y + PX(35)) {
		static const char *titles[PANEL_COUNT] = {"PROBLEMS", "OUTPUT"};
		int tx = x + PX(8);
		for (int i = 0; i < PANEL_COUNT; i++) {
			int tw = font_width(FONT_SMALL, titles[i], strlen(titles[i])) + PX(20);
			if (e->x >= tx && e->x < tx + tw) {
				app.panel_view = i;
				app.dirty = true;
				return true;
			}
			tx += tw;
		}
		if (e->x > x + w - PX(30)) {
			app.panel_visible = false;
			app.focus = FOCUS_EDITOR;
			ui_layout();
		}
		return true;
	}
	if (app.panel_view == PANEL_PROBLEMS)
		return problems_event(e, x, y, w, h);
	return output_event(e, x, y, w, h);
}

static void draw_panel(void)
{
	int x, y, w, h;
	panel_area(&x, &y, &w, &h);
	draw_rect(x, y, w, h, COL(C_PANEL_BG));
	draw_rect(x, y, w, PX(1), COL(C_PANEL_BORDER));
	static const char *titles[PANEL_COUNT] = {"PROBLEMS", "OUTPUT"};
	int tx = x + PX(8);
	for (int i = 0; i < PANEL_COUNT; i++) {
		int tw = font_width(FONT_SMALL, titles[i], strlen(titles[i]));
		bool active = app.panel_view == i;
		int ty = y + (PX(35) - font_m[FONT_SMALL].line) / 2;
		draw_text(FONT_SMALL, tx + PX(10), ty, titles[i], strlen(titles[i]),
		          active ? COL(C_PANEL_TITLE_ACTIVE_FG) : COL(C_PANEL_TITLE_INACTIVE_FG));
		if (i == PANEL_PROBLEMS) {
			u32 n = problems_count(0);
			if (n) {
				char b[16];
				int bn = snprintf(b, sizeof b, "%u", n);
				int bw = VX_MAX(PX(18), font_width(FONT_SMALL, b, (size_t)bn) + PX(10));
				int bx = tx + PX(10) + tw + PX(6);
				draw_rect(bx, y + (PX(35) - PX(18)) / 2, bw, PX(18), COL(C_BADGE_BG));
				draw_text(FONT_SMALL, bx + (bw - font_width(FONT_SMALL, b, (size_t)bn)) / 2, ty, b,
				          (size_t)bn, COL(C_BADGE_FG));
				tw += bw + PX(6);
			}
		}
		if (active)
			draw_rect(tx + PX(10), y + PX(35) - PX(4), tw, PX(1), COL(C_PANEL_TITLE_ACTIVE_BORDER));
		tx += tw + PX(20);
	}
	draw_icon(ICON_CLOSE, x + w - PX(26), y + (PX(35) - PX(16)) / 2, COL(C_FG));
	draw_clip(x, y + PX(35), w, h - PX(35));
	if (app.panel_view == PANEL_PROBLEMS)
		problems_draw(x, y + PX(35), w, h - PX(35));
	else
		output_draw(x, y + PX(35), w, h - PX(35));
	draw_noclip();
	if (app.drag == DRAG_PANEL || (abs(app.hover_y - y) <= PX(2) && app.hover_x >= x))
		draw_rect(x, y - PX(2), w, PX(4), COL(C_SASH_HOVER));
}

static int status_item(int x, int y, const char *s, size_t n, bool right)
{
	int w = font_width(FONT_UI, s, n) + PX(10);
	if (right)
		x -= w;
	bool hover = in_rect(app.hover_x, app.hover_y, x, y, w, app.status_h);
	if (hover)
		draw_rect(x, y, w, app.status_h, COL(C_STATUS_HOVER_BG));
	draw_text(FONT_UI, x + PX(5), y + (app.status_h - font_m[FONT_UI].line) / 2, s, n, COL(C_STATUS_FG));
	return right ? x : x + w;
}

static void draw_status(void)
{
	int y = app.h - app.status_h;
	draw_rect(0, y, app.w, app.status_h, COL(C_STATUS_BG));
	draw_rect(0, y, app.w, PX(1), COL(C_STATUS_BORDER));
	int x = PX(4);
	int iy = y + (app.status_h - PX(16)) / 2;
	if (app.git) {
		bool hov = in_rect(app.hover_x, app.hover_y, x, y, PX(120), app.status_h);
		(void)hov;
		draw_icon(ICON_BRANCH, x + PX(5), iy, COL(C_STATUS_FG));
		char b[160];
		const GitStatus *st = git_status(app.git);
		bool dirty = false;
		for (u32 k = 0; k < st->count; k++)
			if (st->flags[k] & (GS_WT_MODIFIED | GS_WT_DELETED | GS_INDEX_MODIFIED | GS_INDEX_NEW |
			                    GS_INDEX_DELETED | GS_WT_RENAMED | GS_INDEX_RENAMED))
				dirty = true;
		int n = snprintf(b, sizeof b, "%s%s", git_branch(app.git), dirty ? "*" : "");
		x = status_item(x + PX(18), y, b, (size_t)n, false);
		u32 ahead, behind;
		git_ahead_behind(app.git, &ahead, &behind);
		if (ahead || behind) {
			n = snprintf(b, sizeof b, "%u\xE2\x86\x93 %u\xE2\x86\x91", behind, ahead);
			x = status_item(x, y, b, (size_t)n, false);
		}
	}
	/* problems */
	{
		u32 e = problems_count(LSP_SEV_ERROR), w = problems_count(LSP_SEV_WARNING);
		int x0 = x;
		draw_icon(ICON_ERROR, x + PX(5), iy, COL(C_STATUS_FG));
		char b[64];
		int n = snprintf(b, sizeof b, "%u", e);
		x = draw_text(FONT_UI, x + PX(24), y + (app.status_h - font_m[FONT_UI].line) / 2, b, (size_t)n,
		              COL(C_STATUS_FG));
		draw_icon(ICON_WARNING, x + PX(6), iy, COL(C_STATUS_FG));
		n = snprintf(b, sizeof b, "%u", w);
		x = draw_text(FONT_UI, x + PX(25), y + (app.status_h - font_m[FONT_UI].line) / 2, b, (size_t)n,
		              COL(C_STATUS_FG)) + PX(6);
		if (in_rect(app.hover_x, app.hover_y, x0, y, x - x0, app.status_h))
			draw_rect(x0, y, x - x0, app.status_h, COL(C_STATUS_HOVER_BG));
	}
	if (app.status_msg[0] && vx_now_ns() < app.status_until) {
		x += PX(8);
		draw_text_fit(FONT_UI, x, y + (app.status_h - font_m[FONT_UI].line) / 2, app.status_msg,
		              strlen(app.status_msg), app.w / 2 - x, COL(C_STATUS_FG));
	}
	/* right side */
	int rx = app.w - PX(8);
	View *v = active_view();
	if (app.lsp) {
		const char *s = app.lsp_ready ? "clangd" : "clangd (starting)";
		rx = status_item(rx, y, s, strlen(s), true);
	}
	if (v) {
		Docs *D = &app.docs;
		Text *t = &D->text[v->doc];
		const char *ln = syn_lang_name(D->lang[v->doc]);
		rx = status_item(rx, y, ln, strlen(ln), true);
		const char *eol = t->crlf ? "CRLF" : "LF";
		rx = status_item(rx, y, eol, strlen(eol), true);
		rx = status_item(rx, y, "UTF-8", 5, true);
		rx = D->tabs[v->doc] ? status_item(rx, y, "Tab Size: 4", 11, true)
		                     : status_item(rx, y, "Spaces: 4", 9, true);
		char b[96];
		u32 head = v->head[0];
		u32 l = text_line_of(t, head);
		/* column counts characters with tabs expanded, like VSCodium */
		u32 ls = text_line_start(t, l);
		u32 col = 0;
		for (u32 o = ls; o < head;) {
			u8 c = text_byte(t, o);
			if (c == '\t')
				col += 4 - col % 4;
			else if (!utf8_cont(c))
				col++;
			o++;
		}
		int n;
		u32 sel = 0;
		for (u32 c = 0; c < v->nc; c++)
			sel += v->head[c] > v->anchor[c] ? v->head[c] - v->anchor[c] : v->anchor[c] - v->head[c];
		if (v->nc > 1)
			n = snprintf(b, sizeof b, "%u selections", v->nc);
		else if (sel)
			n = snprintf(b, sizeof b, "Ln %u, Col %u (%u selected)", l + 1, col + 1, sel);
		else
			n = snprintf(b, sizeof b, "Ln %u, Col %u", l + 1, col + 1);
		rx = status_item(rx, y, b, (size_t)n, true);
	}
}

/* ---- frame ----------------------------------------------------------- */

void ui_draw(void)
{
	draw_begin(app.w, app.h);
	for (u32 gi = 0; gi < app.ngroups; gi++) {
		Group *g = &app.groups[gi];
		draw_group(g, gi);
		if (gi > 0)
			draw_rect(g->x - PX(1) + 1, g->y, PX(1), g->h, COL(C_TABS_BORDER));
	}
	if (app.side_visible)
		draw_sidebar();
	draw_activity_bar();
	if (app.panel_visible)
		draw_panel();
	draw_status();
	View *v = active_view();
	if (v)
		ed_draw_popups(v);
	if (app.quick)
		quick_draw();
	gpu_frame(draw.q, draw.n, COL(C_EDITOR_BG));
	app.dirty = false;
}

/* ---- LSP / git / timers ------------------------------------------------- */

static u32 doc_by_path(const char *p);
u32 doc_find(const char *p) { return doc_by_path(p); }

static u32 doc_by_path(const char *p)
{
	for (u32 d = 0; d < app.docs.n; d++)
		if (app.docs.path[d] && strcmp(app.docs.path[d], p) == 0)
			return d;
	return UINT32_MAX;
}

static void take_diags(const LspEvent *ev)
{
	u32 d = doc_by_path(ev->path);
	if (d == UINT32_MAX)
		return;
	Docs *D = &app.docs;
	const LspDiags *g = ev->diags;
	u32 n = g->count;
	if (n > D->dcap[d]) {
		u32 c = n;
		D->d_line0[d] = vx_realloc(D->d_line0[d], c * sizeof(u32));
		D->d_col0[d] = vx_realloc(D->d_col0[d], c * sizeof(u32));
		D->d_line1[d] = vx_realloc(D->d_line1[d], c * sizeof(u32));
		D->d_col1[d] = vx_realloc(D->d_col1[d], c * sizeof(u32));
		D->d_sev[d] = vx_realloc(D->d_sev[d], c);
		D->d_msg[d] = vx_realloc(D->d_msg[d], c * sizeof(u32));
		D->dcap[d] = c;
	}
	pool_clear(&D->d_pool[d]);
	for (u32 i = 0; i < n; i++) {
		D->d_line0[d][i] = g->line0[i];
		D->d_col0[d][i] = g->col0[i];
		D->d_line1[d][i] = g->line1[i];
		D->d_col1[d][i] = g->col1[i];
		D->d_sev[d][i] = g->severity[i];
		const char *m = pool_str(ev->pool, g->message[i]);
		const char *src = pool_str(ev->pool, g->source[i]);
		char buf[1024];
		/* "message\x1fsource": views draw the source dimmed */
		int k = snprintf(buf, sizeof buf, "%s%s%s", m, *src ? "\x1f" : "", src);
		D->d_msg[d][i] = pool_add(&D->d_pool[d], buf, (u32)VX_MIN(k, (int)sizeof buf - 1));
	}
	D->dn[d] = n;
}

static void take_semantic(const LspEvent *ev)
{
	u32 d = doc_by_path(ev->path);
	if (d == UINT32_MAX)
		return;
	Docs *D = &app.docs;
	if (ev->req != D->sem_req[d])
		return;
	D->sem_req[d] = -1;
	/* drop stale replies: the text changed since the request */
	if (D->sem_for[d] != D->text[d].version) {
		D->sem_for[d] = UINT32_MAX;
		return;
	}
	const LspSemantic *s = ev->semantic;
	u32 n = s->count;
	if (n > D->scap[d]) {
		D->s_line[d] = vx_realloc(D->s_line[d], n * sizeof(u32));
		D->s_col[d] = vx_realloc(D->s_col[d], n * sizeof(u32));
		D->s_len[d] = vx_realloc(D->s_len[d], n * sizeof(u32));
		D->s_cls[d] = vx_realloc(D->s_cls[d], n);
		D->scap[d] = n;
	}
	memcpy(D->s_line[d], s->line, n * sizeof(u32));
	memcpy(D->s_col[d], s->col, n * sizeof(u32));
	memcpy(D->s_len[d], s->len, n * sizeof(u32));
	memcpy(D->s_cls[d], s->cls, n);
	D->sn[d] = n;
}

extern void ed_take_completion(const LspEvent *ev);
extern void ed_take_hover(const LspEvent *ev);
extern void ed_take_definition(const LspEvent *ev);

static void lsp_tick(void)
{
	if (!app.lsp)
		return;
	lsp_pump(app.lsp);
	LspEvent ev;
	while (lsp_next(app.lsp, &ev)) {
		app.dirty = true;
		switch (ev.type) {
		case LSP_EV_READY:
			app.lsp_ready = true;
			ui_log("[lsp] ready");
			break;
		case LSP_EV_DIAGNOSTICS:
			take_diags(&ev);
			break;
		case LSP_EV_SEMANTIC:
			take_semantic(&ev);
			break;
		case LSP_EV_COMPLETION:
			ed_take_completion(&ev);
			break;
		case LSP_EV_HOVER:
			ed_take_hover(&ev);
			break;
		case LSP_EV_LOCATION:
			ed_take_definition(&ev);
			break;
		case LSP_EV_LOG:
			ui_log("[lsp] %s", ev.text ? ev.text : "");
			break;
		case LSP_EV_EXIT:
			ui_log("[lsp] server exited");
			app.lsp_ready = false;
			break;
		default:
			break;
		}
	}
	/* send changes (debounced) and ask for semantic tokens */
	u64 now = vx_now_ns();
	Docs *D = &app.docs;
	for (u32 d = 0; d < D->n; d++) {
		if (!D->lsp_open[d])
			continue;
		Text *t = &D->text[d];
		if (D->lsp_sent[d] != t->version && now - D->lsp_changed_ns[d] > 150000000ull) {
			D->lsp_ver[d]++;
			lsp_did_change(app.lsp, D->path[d], D->lsp_ver[d], text_flat(t), t->len);
			D->lsp_sent[d] = t->version;
		}
		if (app.lsp_ready && D->sem_req[d] < 0 && D->sem_for[d] != t->version &&
		    D->lsp_sent[d] == t->version && D->refs[d]) {
			D->sem_req[d] = lsp_semantic_tokens(app.lsp, D->path[d]);
			D->sem_for[d] = t->version;
		}
	}
}

/* Repo-relative path of a document inside the work tree, or NULL. */
const char *doc_repo_rel(u32 d)
{
	if (!app.git || !app.docs.path[d])
		return NULL;
	const char *wd = git_workdir(app.git);
	size_t wl = strlen(wd);
	return strncmp(app.docs.path[d], wd, wl) == 0 ? app.docs.path[d] + wl : NULL;
}

u32 blame_key_of(u32 d) { return app.docs.text[d].version * 2654435761u ^ app.blame_gen; }

static void blame_tick(void)
{
	if (!app.blame)
		return;
	if (blame_take_update(app.blame))
		app.dirty = true;
	View *v = active_view();
	if (!app.blame_on || !v)
		return;
	u32 d = v->doc;
	const char *rel = doc_repo_rel(d);
	if (!rel)
		return;
	u32 key = blame_key_of(d);
	if (app.blame_asked && app.blame_doc == d && app.blame_key == key)
		return;
	/* debounce while typing */
	if (vx_now_ns() - app.docs.lsp_changed_ns[d] < 200000000ull)
		return;
	Text *t = &app.docs.text[d];
	blame_request(app.blame, rel, text_flat(t), t->len, key);
	app.blame_asked = true;
	app.blame_doc = d;
	app.blame_key = key;
}

static void git_tick(void)
{
	u64 now = vx_now_ns();
	if (app.git && now - app.git_ns > 3000000000ull) {
		git_refresh(app.git);
		app.git_ns = now;
		app.blame_gen++;
		app.dirty = true;
		for (u32 d = 0; d < app.docs.n; d++)
			app.docs.gm_for[d] = UINT32_MAX; /* index may have changed */
	}
	if (!app.git)
		return;
	Docs *D = &app.docs;
	const char *wd = git_workdir(app.git);
	size_t wl = strlen(wd);
	for (u32 d = 0; d < D->n; d++) {
		if (!D->path[d] || !D->refs[d] || D->gm_for[d] == D->text[d].version || now < D->gm_due_ns[d])
			continue;
		if (strncmp(D->path[d], wd, wl) != 0)
			continue;
		Text *t = &D->text[d];
		u32 nl = text_lines(t);
		VX_GROW(D->gm[d], D->gm_cap[d], nl);
		u16 pf = git_path_flags(app.git, D->path[d] + wl);
		if (pf & (GS_WT_NEW | GS_IGNORED))
			D->gm_n[d] = 0; /* no quick diff for untracked or ignored files */
		else if (git_line_marks(app.git, D->path[d] + wl, text_flat(t), t->len, D->gm[d], nl) == 0)
			D->gm_n[d] = nl;
		else
			D->gm_n[d] = 0;
		/* a file identical to the index has no marks */
		D->gm_for[d] = t->version;
		app.dirty = true;
	}
}

void ui_tick(void)
{
	app.now = vx_now_ns();
	lsp_tick();
	git_tick();
	blame_tick();
	if (app.now - app.tree_scan_ns > 5000000000ull) {
		/* pick up files created outside the editor */
		if (app.tree_scan_ns) {
			fs_refresh(&app.tree);
			app.files_ok = false;
		}
		app.tree_scan_ns = app.now;
	}
	search_tick();
	if (app.hover_due && app.now >= app.hover_due) {
		app.hover_due = 0;
		View *v = active_view();
		if (v && app.hover_pending_off >= 0)
			ed_hover_request(v);
	}
	if (app.status_until && app.now >= app.status_until) {
		app.status_until = 0;
		app.dirty = true;
	}
}

u64 ui_wait_hint_ms(void)
{
	if (app.lsp && !app.lsp_ready)
		return 30;
	return 250;
}

/* ---- init ---------------------------------------------------------------- */

bool ui_init(const char *root, int w, int h, int scale, bool headless)
{
	memset(&app, 0, sizeof app);
	app.root = vx_strdup(root);
	app.w = w;
	app.h = h;
	app.scale = scale;
	app.headless = headless;
	app.running = true;
	app.theme = THEME_DARK;
	const char *th = getenv("VXIDE_THEME");
	if (th && strcmp(th, "light") == 0)
		app.theme = THEME_LIGHT;
	app.side_visible = true;
	app.side_w = PX(300);
	app.panel_h = h / 3;
	app.ngroups = 1;
	app.compl_req = app.hover_req = app.def_req = -1;
	app.hover_pending_off = -1;
	if (!font_init(PX(13), PX(14), PX(19), PX(11)))
		return false;
	app.line_h = font_m[FONT_MONO].line;
	app.char_w = font_m[FONT_MONO].adv;
	icons_init();
	font_atlas_was_reset(); /* pins the icon rows */
	gpu_set_atlas(font_atlas);
	fs_init(&app.tree, root);
	app.tree_sel = FS_NONE;
	app.git = git_open(root);
	if (app.git) {
		git_refresh(app.git);
		app.git_ns = vx_now_ns();
		app.blame_on = !getenv("VXIDE_NO_BLAME");
		app.blame = blame_start(git_workdir(app.git));
	}
	app.tree_scan_ns = vx_now_ns();
	if (!getenv("VXIDE_NO_LSP"))
		restart_lsp();
	ui_layout();
	app.focus = FOCUS_SIDEBAR;
	return true;
}

void ui_shutdown(void)
{
	/* tabs first: closing a document still talks to the language server */
	for (u32 gi = 0; gi < app.ngroups; gi++)
		while (app.groups[gi].nt)
			close_tab(gi, app.groups[gi].nt - 1);
	if (app.lsp)
		lsp_stop(app.lsp);
	app.lsp = NULL;
	blame_stop(app.blame);
	app.blame = NULL;
	if (app.git)
		git_close(app.git);
	app.git = NULL;
	fs_free(&app.tree);
	files_free(&app.files);
	search_free(&app.results);
	matcher_free(&app.find_m);
	matcher_free(&app.search_m);
	font_shutdown();
}
