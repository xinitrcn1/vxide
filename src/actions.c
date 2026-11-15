/* Context menus, the generic picker, and the commands behind them
 * (references, paths, explorer and tab actions, multi-cursor helpers). */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <stdio.h>
#include <unistd.h>
#include <SDL3/SDL.h>

/* ---- picker ------------------------------------------------------------------ */

enum { PICK_REFERENCES = 1, PICK_LOCATIONS, PICK_THEME = 301, PICK_LANGUAGE = 302 };

static u32 lang_doc;

/* Change Language Mode (the status bar's language item) */
void language_pick(u32 d)
{
	lang_doc = d;
	pick_begin(PICK_LANGUAGE, "Select Language Mode", NULL);
	for (int l = 0; l < LANG_COUNT; l++) {
		const char *id = syn_lang_lsp_id(l);
		pick_add(syn_lang_name(l), l == app.docs.lang[d] ? "current" : (id ? id : ""), (u32)l);
	}
	pick_show();
	for (u32 i = 0; i < app.quick_n; i++)
		if (app.pick_val[app.quick_items[i]] == app.docs.lang[d])
			app.quick_sel = i;
}

void theme_pick(void)
{
	app.theme_before = app.theme;
	pick_begin(PICK_THEME, "Select Color Theme (Up/Down to preview)", NULL);
	static const int order[] = {THEME_DARK, THEME_DARKER, THEME_LIGHT};
	for (size_t i = 0; i < VX_COUNT(order); i++)
		pick_add(themes[order[i]].name, order[i] == app.theme ? "current" : "", (u32)order[i]);
	pick_show();
	/* start on the current theme */
	for (u32 i = 0; i < app.quick_n; i++)
		if (app.pick_val[app.quick_items[i]] == (u32)app.theme)
			app.quick_sel = i;
}


void pick_begin(int kind, const char *title, const char *placeholder)
{
	(void)placeholder;
	app.pick_kind = kind;
	app.pick_n = 0;
	pool_clear(&app.pick_pool);
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title ? title : "");
}

void pick_add(const char *label, const char *desc, u32 val)
{
	if (app.pick_n == app.pick_cap) {
		u32 c = app.pick_cap ? app.pick_cap * 2 : 64;
		app.pick_label = vx_realloc(app.pick_label, c * sizeof(u32));
		app.pick_desc = vx_realloc(app.pick_desc, c * sizeof(u32));
		app.pick_val = vx_realloc(app.pick_val, c * sizeof(u32));
		app.pick_cap = c;
	}
	u32 i = app.pick_n++;
	app.pick_label[i] = pool_add(&app.pick_pool, label, (u32)strlen(label));
	app.pick_desc[i] = pool_add(&app.pick_pool, desc ? desc : "", desc ? (u32)strlen(desc) : 0);
	app.pick_val[i] = val;
}

extern void quick_show_pick(void);
void pick_show(void) { quick_show_pick(); }

/* References are kept here until a pick (paths + positions). */
static Pool loc_pool;
static u32 *loc_path, *loc_line, *loc_col, loc_n, loc_cap;

void pick_accept(int kind, u32 item)
{
	u32 v = app.pick_val[item];
	switch (kind) {
	case PICK_LANGUAGE:
		if (lang_doc < app.docs.n)
			doc_set_lang(lang_doc, (int)v);
		break;
	case PICK_THEME:
		app.theme = app.theme_before = (int)v;
		ui_status("Color Theme: %s", themes[app.theme].name);
		settings_save();
		break;
	case PICK_REFERENCES:
	case PICK_LOCATIONS:
		if (v < loc_n)
			open_file(loc_pool.b + loc_path[v], false, loc_line[v], loc_col[v]);
		break;
	default: {
		extern void gitui_pick_accept(int kind, u32 item);
		gitui_pick_accept(kind, item);
	}
	}
}

/* Preview text of a line in a file (open buffer or disk). */
static void line_preview(const char *path, u32 line, char *out, size_t cap)
{
	out[0] = 0;
	u32 d = doc_find(path);
	if (d != UINT32_MAX) {
		Text *t = &app.docs.text[d];
		if (line < text_lines(t)) {
			u32 n;
			const char *s = text_line(t, line, &n);
			snprintf(out, cap, "%.*s", (int)VX_MIN(n, (u32)cap - 1), s);
		}
	} else {
		FILE *f = fopen(path, "r");
		if (!f)
			return;
		char buf[1024];
		for (u32 l = 0; fgets(buf, sizeof buf, f); l++)
			if (l == line) {
				buf[strcspn(buf, "\r\n")] = 0;
				snprintf(out, cap, "%s", buf);
				break;
			}
		fclose(f);
	}
	/* trim the indentation */
	size_t k = strspn(out, " \t");
	memmove(out, out + k, strlen(out + k) + 1);
}

void actions_take_locations(const LspEvent *ev)
{
	bool refs = ev->req == app.refs_req;
	if (refs)
		app.refs_req = -1;
	else
		app.decl_req = -1;
	const LspLocations *L = ev->locations;
	if (!L->count) {
		ui_status(refs ? "No references found" : "No declaration found");
		return;
	}
	if (!refs && L->count == 1) {
		open_file(pool_str(ev->pool, L->path[0]), false, L->line[0], L->col[0]);
		return;
	}
	pool_clear(&loc_pool);
	loc_n = 0;
	pick_begin(refs ? PICK_REFERENCES : PICK_LOCATIONS, NULL, NULL);
	snprintf(app.quick_title, sizeof app.quick_title, "%u reference%s", L->count, L->count == 1 ? "" : "s");
	for (u32 i = 0; i < L->count; i++) {
		const char *p = pool_str(ev->pool, L->path[i]);
		VX_GROW(loc_path, loc_cap, loc_n + 1);
		loc_line = vx_realloc(loc_line, loc_cap * sizeof(u32));
		loc_col = vx_realloc(loc_col, loc_cap * sizeof(u32));
		loc_path[loc_n] = pool_add(&loc_pool, p, (u32)strlen(p));
		loc_line[loc_n] = L->line[i];
		loc_col[loc_n] = L->col[i];
		char prev[300], desc[600];
		line_preview(p, L->line[i], prev, sizeof prev);
		size_t rl = strlen(app.root);
		const char *rel = strncmp(p, app.root, rl) == 0 && p[rl] == '/' ? p + rl + 1 : p;
		snprintf(desc, sizeof desc, "%s:%u", rel, L->line[i] + 1);
		pick_add(prev[0] ? prev : rel, desc, loc_n);
		loc_n++;
	}
	pick_show();
}

/* ---- context menus ------------------------------------------------------------- */

void ctx_menu_editor(View *v, int x, int y, u32 off)
{
	app.ctx = CTX_EDITOR;
	app.ctx_off = off;
	bool lsp = doc_lsp(v->doc) != NULL;
	bool sel = false;
	for (u32 c = 0; c < v->nc; c++)
		sel |= v->head[c] != v->anchor[c];
	bool ro = app.docs.readonly[v->doc];
	menu_open(x, y);
	menu_item("Go to Definition", CMD_GOTO_DEFINITION, lsp);
	menu_item("Go to Declaration", CMD_GOTO_DECLARATION, lsp);
	menu_item("Go to Type Definition", CMD_GOTO_TYPE_DEFINITION, lsp);
	menu_item("Go to References", CMD_GOTO_REFERENCES, lsp);
	menu_sep();
	menu_item("Find All References", CMD_FIND_ALL_REFERENCES, lsp);
	menu_item("Open Manpage", CMD_OPEN_MANPAGE, true);
	menu_sep();
	menu_item("Change All Occurrences", CMD_CHANGE_ALL_OCCURRENCES, !ro);
	menu_sep();
	menu_item("Cut", CMD_CUT, !ro);
	menu_item("Copy", CMD_COPY, true);
	menu_item("Paste", CMD_PASTE, !ro);
	menu_sep();
	menu_item("Toggle Line Comment", CMD_TOGGLE_COMMENT, !ro);
	menu_sep();
	menu_item("Command Palette...", CMD_COMMANDS, true);
	(void)sel;
}

void ctx_menu_explorer(int x, int y, u32 node)
{
	app.ctx = CTX_EXPLORER;
	app.ctx_a = node;
	menu_open(x, y);
	menu_item("New File...", CMD_EXPLORER_NEW_FILE, true);
	menu_item("New Folder...", CMD_EXPLORER_NEW_FOLDER, true);
	menu_sep();
	menu_item("Copy Path", CMD_COPY_PATH, node != FS_NONE);
	menu_item("Copy Relative Path", CMD_COPY_REL_PATH, node != FS_NONE);
	menu_sep();
	menu_item("Rename...", CMD_EXPLORER_RENAME, node != FS_NONE);
	menu_item("Delete", CMD_EXPLORER_DELETE, node != FS_NONE);
	menu_sep();
	menu_item("Refresh Explorer", CMD_REFRESH_EXPLORER, true);
	menu_item("Collapse Folders in Explorer", CMD_COLLAPSE_FOLDERS, true);
	projtools_explorer_menu(node);
}

void ctx_menu_tab(int x, int y, u32 group, u32 tab)
{
	app.ctx = CTX_TAB;
	app.ctx_a = group;
	app.ctx_b = tab;
	Group *g = &app.groups[group];
	menu_open(x, y);
	menu_item("Close", CMD_CLOSE_EDITOR, true);
	menu_item("Close Others", CMD_CLOSE_OTHERS, g->nt > 1);
	menu_item("Close to the Right", CMD_CLOSE_RIGHT, tab + 1 < g->nt);
	menu_item("Close All", CMD_CLOSE_ALL, true);
	menu_sep();
	bool has_path = tab < g->nt && app.docs.path[gview(g, tab)->doc];
	menu_item("Copy Path", CMD_COPY_PATH, has_path);
	menu_item("Copy Relative Path", CMD_COPY_REL_PATH, has_path);
	menu_sep();
	menu_item("Reveal in Explorer View", CMD_REVEAL_IN_EXPLORER, has_path);
	menu_item("Split Right", CMD_SPLIT, app.ngroups < MAX_GROUPS);
}

void ctx_menu_scm_file(int x, int y, const char *repo_rel, bool staged)
{
	app.ctx = CTX_SCM_FILE;
	app.ctx_a = staged;
	snprintf(app.ctx_path, sizeof app.ctx_path, "%s", repo_rel);
	menu_open(x, y);
	menu_item("Open Changes", CMD_SCM_OPEN_CHANGES, true);
	menu_item("Open File", CMD_SCM_OPEN_FILE, true);
	menu_sep();
	bool conflict = app.git && (git_path_flags(app.git, repo_rel) & GS_CONFLICT);
	if (conflict) {
		menu_item("Mark as Resolved", CMD_GIT_MARK_RESOLVED, true);
	} else if (staged) {
		menu_item("Unstage Changes", CMD_SCM_UNSTAGE, true);
	} else {
		menu_item("Stage Changes", CMD_SCM_STAGE, true);
		menu_item("Discard Changes", CMD_SCM_DISCARD, true);
	}
	menu_sep();
	menu_item("Copy Path", CMD_COPY_PATH, true);
	menu_item("Copy Relative Path", CMD_COPY_REL_PATH, true);
}

/* ---- multi-cursor helpers ------------------------------------------------------------ */

void cursor_history_push(View *v)
{
	if (app.ch_n == VX_COUNT(app.ch_doc)) {
		free(app.ch_head[0]); /* the anchors share the block */
		memmove(app.ch_doc, app.ch_doc + 1, sizeof app.ch_doc - sizeof app.ch_doc[0]);
		memmove(app.ch_nc, app.ch_nc + 1, sizeof app.ch_nc - sizeof app.ch_nc[0]);
		memmove(app.ch_head, app.ch_head + 1, sizeof app.ch_head - sizeof app.ch_head[0]);
		memmove(app.ch_anchor, app.ch_anchor + 1, sizeof app.ch_anchor - sizeof app.ch_anchor[0]);
		app.ch_n--;
	}
	u32 i = app.ch_n++;
	app.ch_doc[i] = v->doc;
	app.ch_nc[i] = v->nc;
	/* heads and anchors in one block */
	app.ch_head[i] = vx_alloc(2 * (size_t)v->nc * sizeof(u32));
	app.ch_anchor[i] = app.ch_head[i] + v->nc;
	memcpy(app.ch_head[i], v->head, v->nc * sizeof(u32));
	memcpy(app.ch_anchor[i], v->anchor, v->nc * sizeof(u32));
}

static void cursor_undo(View *v)
{
	while (app.ch_n) {
		u32 i = --app.ch_n;
		bool ok = app.ch_doc[i] == v->doc;
		if (ok) {
			u32 len = app.docs.text[v->doc].len;
			view_reserve(v, app.ch_nc[i]);
			v->nc = app.ch_nc[i];
			for (u32 c = 0; c < v->nc; c++) {
				v->head[c] = VX_MIN(app.ch_head[i][c], len);
				v->anchor[c] = VX_MIN(app.ch_anchor[i][c], len);
				v->want_x[c] = -1;
			}
		}
		free(app.ch_head[i]); /* the anchors share the block */
		if (ok) {
			ed_reveal(v, v->head[v->nc - 1], false);
			return;
		}
	}
}

/* Shift+Alt+I: a cursor at the end of every line of each selection. */
static void cursors_at_line_ends(View *v)
{
	Text *t = &app.docs.text[v->doc];
	cursor_history_push(v);
	u32 nh[MAX_CURSORS];
	u32 n = 0;
	for (u32 c = 0; c < v->nc && n < MAX_CURSORS; c++) {
		u32 a = VX_MIN(v->head[c], v->anchor[c]), b = VX_MAX(v->head[c], v->anchor[c]);
		u32 l0 = text_line_of(t, a), l1 = text_line_of(t, b);
		if (l1 > l0 && b == text_line_start(t, l1))
			l1--;
		for (u32 l = l0; l <= l1 && n < MAX_CURSORS; l++)
			nh[n++] = text_line_start(t, l) + text_line_len(t, l);
	}
	view_reserve(v, n);
	v->nc = n ? n : 1;
	for (u32 c = 0; c < n; c++) {
		v->head[c] = v->anchor[c] = nh[c];
		v->want_x[c] = -1;
	}
}

/* ---- paths ------------------------------------------------------------------- */

static bool ctx_abs_path(char *out, size_t cap)
{
	out[0] = 0;
	if (app.ctx == CTX_EXPLORER && app.ctx_a != FS_NONE) {
		char rel[4096];
		fs_relpath(&app.tree, app.ctx_a, rel, sizeof rel);
		snprintf(out, cap, "%s/%s", app.root, rel);
	} else if (app.ctx == CTX_TAB && app.ctx_a < app.ngroups && app.ctx_b < app.groups[app.ctx_a].nt) {
		u32 d = gview(&app.groups[app.ctx_a], app.ctx_b)->doc;
		if (app.docs.path[d])
			snprintf(out, cap, "%s", app.docs.path[d]);
	} else if (app.ctx == CTX_SCM_FILE && app.git) {
		snprintf(out, cap, "%s%s", git_workdir(app.git), app.ctx_path);
	} else {
		View *v = active_view();
		if (v && app.docs.path[v->doc])
			snprintf(out, cap, "%s", app.docs.path[v->doc]);
	}
	return out[0] != 0;
}

static void prompt(int kind, u32 arg, const char *title, const char *initial)
{
	app.quick = QUICK_PROMPT;
	app.quick_prompt_kind = kind;
	app.quick_arg = arg;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, initial, strlen(initial));
	app.focus = FOCUS_QUICK;
}

/* ---- commands ------------------------------------------------------------------- */

static void lsp_pos(View *v, u32 off, u32 *line, u32 *col)
{
	Text *t = &app.docs.text[v->doc];
	*line = text_line_of(t, off);
	*col = off - text_line_start(t, *line);
}

static bool lsp_ok(View *v)
{
	if (!doc_lsp(v->doc)) {
		ui_status("No language server for this file");
		return false;
	}
	/* make sure the server has the current text */
	Docs *D = &app.docs;
	if (D->lsp_sent[v->doc] != D->text[v->doc].version) {
		D->lsp_ver[v->doc]++;
		lsp_did_change(doc_lsp(v->doc), D->path[v->doc], D->lsp_ver[v->doc], text_flat(&D->text[v->doc]),
		               D->text[v->doc].len);
		D->lsp_sent[v->doc] = D->text[v->doc].version;
	}
	return true;
}

bool actions_command(int cmd)
{
	View *v = active_view();
	switch (cmd) {
	case CMD_GOTO_DECLARATION:
	case CMD_GOTO_TYPE_DEFINITION:
	case CMD_GOTO_REFERENCES:
	case CMD_FIND_ALL_REFERENCES: {
		if (!v || !lsp_ok(v))
			return true;
		u32 off = app.ctx == CTX_EDITOR ? app.ctx_off : v->head[0];
		app.ctx = CTX_NONE;
		u32 line, col;
		lsp_pos(v, off, &line, &col);
		const char *p = app.docs.path[v->doc];
		if (cmd == CMD_GOTO_REFERENCES || cmd == CMD_FIND_ALL_REFERENCES)
			app.refs_req = lsp_references(doc_lsp(v->doc), p, line, col);
		else if (cmd == CMD_GOTO_DECLARATION)
			app.decl_req = lsp_declaration(doc_lsp(v->doc), p, line, col);
		else
			app.decl_req = lsp_type_definition(doc_lsp(v->doc), p, line, col);
		return true;
	}
	case CMD_CHANGE_ALL_OCCURRENCES:
		if (v) {
			cursor_history_push(v);
			run_command(CMD_SELECT_ALL_MATCHES);
		}
		return true;
	case CMD_CURSOR_UNDO:
		if (v)
			cursor_undo(v);
		return true;
	case CMD_CURSORS_LINE_ENDS:
		if (v)
			cursors_at_line_ends(v);
		return true;
	case CMD_COPY_PATH:
	case CMD_COPY_REL_PATH: {
		char p[8192];
		if (ctx_abs_path(p, sizeof p)) {
			const char *s = p;
			size_t rl = strlen(app.root);
			if (cmd == CMD_COPY_REL_PATH && strncmp(p, app.root, rl) == 0 && p[rl] == '/')
				s = p + rl + 1;
			ui_clipboard_set(s, strlen(s));
		}
		app.ctx = CTX_NONE;
		return true;
	}
	case CMD_EXPLORER_NEW_FILE:
	case CMD_EXPLORER_NEW_FOLDER: {
		char base[4096] = "";
		u32 sel = app.ctx == CTX_EXPLORER ? app.ctx_a : app.tree_sel;
		if (sel != FS_NONE) {
			u32 dir = (app.tree.flags[sel] & FS_DIR) ? sel : app.tree.parent[sel];
			if (dir != 0 && dir != FS_NONE) {
				u32 n = fs_relpath(&app.tree, dir, base, sizeof base - 2);
				base[n] = '/';
				base[n + 1] = 0;
			}
		}
		prompt(cmd == CMD_EXPLORER_NEW_FILE ? 2 : 3, 0,
		       cmd == CMD_EXPLORER_NEW_FILE ? "New File..." : "New Folder...", base);
		return true;
	}
	case CMD_EXPLORER_RENAME:
	case CMD_EXPLORER_DELETE: {
		u32 node = app.ctx == CTX_EXPLORER ? app.ctx_a : app.tree_sel;
		if (node == FS_NONE)
			return true;
		app.tree_sel = node;
		Event e = {0};
		e.type = EV_KEY;
		e.key = cmd == CMD_EXPLORER_RENAME ? SDLK_F2 : SDLK_DELETE;
		explorer_event(&e, 0, 0, 0, 0);
		return true;
	}
	case CMD_CLOSE_OTHERS:
	case CMD_CLOSE_RIGHT: {
		u32 gi = app.ctx == CTX_TAB ? app.ctx_a : app.active_group;
		Group *g = &app.groups[gi];
		u32 keep = app.ctx == CTX_TAB ? app.ctx_b : g->active;
		View *kv = keep < g->nt ? gview(g, keep) : NULL;
		for (u32 t = g->nt; t-- > 0;) {
			if (gview(g, t) == kv)
				continue;
			if (cmd == CMD_CLOSE_RIGHT && t < keep)
				continue;
			if (text_dirty(&app.docs.text[gview(g, t)->doc]) && app.docs.refs[gview(g, t)->doc] == 1)
				continue; /* keep unsaved editors open */
			close_tab(gi, t);
		}
		app.ctx = CTX_NONE;
		return true;
	}
	default:
		return false;
	}
}

/* Virtual read-only documents (diffs, commit views). */
u32 doc_new_virtual(const char *name, const char *text, size_t n, int lang)
{
	u32 d = doc_new_untitled();
	if (d == UINT32_MAX)
		return d;
	Docs *D = &app.docs;
	free(D->rel[d]);
	D->rel[d] = vx_strdup(name);
	D->untitled[d] = 0;
	D->readonly[d] = 1;
	D->lang[d] = (u8)lang;
	text_replace(&D->text[d], 0, 0, text, (u32)n, 0);
	D->text[d].saved_upos = D->text[d].upos;
	D->lstate_ok[d] = 0;
	return d;
}
