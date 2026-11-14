/* Side bar views (Explorer, Search, Source Control), the panel views
 * (Problems, Output) and the single-line input box they share. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <stdio.h>
#include <SDL3/SDL.h>

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
	return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* ---- input box ---------------------------------------------------------------- */

void input_set(Input *in, const char *s, size_t n)
{
	VX_GROW(in->b, in->cap, (u32)n + 1);
	memcpy(in->b, s, n);
	in->b[n] = 0;
	in->n = (u32)n;
	in->cur = in->anchor = (u32)n;
	in->scroll = 0;
}

static void input_delete(Input *in, u32 a, u32 b)
{
	memmove(in->b + a, in->b + b, in->n - b + 1);
	in->n -= b - a;
	in->cur = in->anchor = a;
}

void input_text(Input *in, const char *s, size_t n)
{
	u32 a = VX_MIN(in->cur, in->anchor), b = VX_MAX(in->cur, in->anchor);
	if (b > a)
		input_delete(in, a, b);
	/* single line: drop newlines */
	char buf[1024];
	size_t k = 0;
	for (size_t i = 0; i < n && k < sizeof buf; i++)
		if (s[i] != '\n' && s[i] != '\r')
			buf[k++] = s[i];
	VX_GROW(in->b, in->cap, in->n + (u32)k + 1);
	memmove(in->b + in->cur + k, in->b + in->cur, in->n - in->cur + 1);
	memcpy(in->b + in->cur, buf, k);
	in->n += (u32)k;
	in->cur += (u32)k;
	in->anchor = in->cur;
}

static u32 in_prev(Input *in, u32 i)
{
	if (!i)
		return 0;
	i--;
	while (i && utf8_cont((u8)in->b[i]))
		i--;
	return i;
}

static u32 in_next(Input *in, u32 i)
{
	if (i >= in->n)
		return in->n;
	i++;
	while (i < in->n && utf8_cont((u8)in->b[i]))
		i++;
	return i;
}

static u32 in_word(Input *in, u32 i, int dir)
{
	if (dir < 0) {
		while (i && !is_word_byte((u8)in->b[i - 1]))
			i--;
		while (i && is_word_byte((u8)in->b[i - 1]))
			i--;
	} else {
		while (i < in->n && !is_word_byte((u8)in->b[i]))
			i++;
		while (i < in->n && is_word_byte((u8)in->b[i]))
			i++;
	}
	return i;
}

bool input_key(Input *in, const Event *e)
{
	if (!in->b)
		input_set(in, "", 0);
	bool shift = e->mods & MOD_SHIFT, ctrl = e->mods & MOD_CTRL;
	u32 a = VX_MIN(in->cur, in->anchor), b = VX_MAX(in->cur, in->anchor);
	switch (e->key) {
	case SDLK_LEFT:
		in->cur = (!shift && a != b) ? a : ctrl ? in_word(in, in->cur, -1) : in_prev(in, in->cur);
		break;
	case SDLK_RIGHT:
		in->cur = (!shift && a != b) ? b : ctrl ? in_word(in, in->cur, 1) : in_next(in, in->cur);
		break;
	case SDLK_HOME:
		in->cur = 0;
		break;
	case SDLK_END:
		in->cur = in->n;
		break;
	case SDLK_BACKSPACE:
		if (a == b)
			a = ctrl ? in_word(in, a, -1) : in_prev(in, a);
		input_delete(in, a, b);
		return true;
	case SDLK_DELETE:
		if (a == b)
			b = ctrl ? in_word(in, b, 1) : in_next(in, b);
		input_delete(in, a, b);
		return true;
	case SDLK_A:
		if (!ctrl)
			return false;
		in->anchor = 0;
		in->cur = in->n;
		return true;
	case SDLK_C:
	case SDLK_X:
		if (!ctrl)
			return false;
		if (b > a)
			ui_clipboard_set(in->b + a, b - a);
		if (e->key == SDLK_X)
			input_delete(in, a, b);
		return true;
	case SDLK_V:
		if (!ctrl)
			return false;
		{
			char *s = ui_clipboard_get();
			input_text(in, s, strlen(s));
			free(s);
		}
		return true;
	default:
		return false;
	}
	if (!shift)
		in->anchor = in->cur;
	return true;
}

void input_click(Input *in, int font, int x0, int mx)
{
	int x = x0 - in->scroll;
	u32 i = 0;
	while (i < in->n) {
		u32 nx = in_next(in, i);
		int w = font_width(font, in->b + i, nx - i);
		if (mx < x + w / 2)
			break;
		x += w;
		i = nx;
	}
	in->cur = in->anchor = i;
}

void input_draw(Input *in, int x, int y, int w, int h, const char *ph, bool focused)
{
	if (!in->b)
		input_set(in, "", 0);
	draw_rect(x, y, w, h, COL(C_INPUT_BG));
	draw_frame(x, y, w, h, focused ? COL(C_FOCUS_BORDER) : COL(C_INPUT_BORDER));
	int pad = PX(6);
	int tw = w - 2 * pad;
	int ty = y + (h - font_m[FONT_UI].line) / 2;
	int cx = font_width(FONT_UI, in->b, in->cur);
	if (cx - in->scroll > tw)
		in->scroll = cx - tw;
	if (cx < in->scroll)
		in->scroll = cx;
	draw_clip(x + pad, y, tw + PX(1), h);
	int tx = x + pad - in->scroll;
	if (!in->n && ph && *ph)
		draw_text(FONT_UI, x + pad, ty, ph, strlen(ph), COL(C_INPUT_PLACEHOLDER));
	u32 a = VX_MIN(in->cur, in->anchor), b = VX_MAX(in->cur, in->anchor);
	if (b > a) {
		int sx = font_width(FONT_UI, in->b, a), ex = font_width(FONT_UI, in->b, b);
		draw_rect(tx + sx, ty, ex - sx, font_m[FONT_UI].line, COL(C_SELECTION));
	}
	draw_text(FONT_UI, tx, ty, in->b, in->n, COL(C_INPUT_FG));
	if (focused)
		draw_rect(tx + cx, ty, PX(1), font_m[FONT_UI].line, COL(C_INPUT_FG));
	draw_noclip();
	draw_clip(0, 0, app.w, app.h);
}

/* ---- shared bits ------------------------------------------------------------- */

static int view_title(int x, int y, int w, const char *t)
{
	int h = PX(35);
	draw_text(FONT_SMALL, x + PX(20), y + (h - font_m[FONT_SMALL].line) / 2, t, strlen(t),
	          COL(C_SIDEBAR_TITLE_FG));
	(void)w;
	return y + h;
}

static int section_header(int x, int y, int w, const char *t, bool open, bool border)
{
	int h = PX(22);
	if (border)
		draw_rect(x, y, w, PX(1), COL(C_SECTION_HEADER_BORDER));
	draw_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + PX(2), y + (h - PX(16)) / 2,
	          COL(C_SECTION_HEADER_FG));
	draw_text(FONT_SMALL_BOLD, x + PX(20), y + (h - font_m[FONT_SMALL_BOLD].line) / 2, t, strlen(t),
	          COL(C_SECTION_HEADER_FG));
	return y + h;
}

static void upper(char *dst, const char *s, size_t cap)
{
	size_t i = 0;
	for (; s[i] && i + 1 < cap; i++)
		dst[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
	dst[i] = 0;
}

static void list_row_bg(int x, int y, int w, int h, bool selected, bool focused, bool hover)
{
	if (selected) {
		draw_rect(x, y, w, h, focused ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_INACTIVE_BG));
		if (focused)
			draw_frame(x, y, w, h, COL(C_LIST_FOCUS_OUTLINE));
	} else if (hover) {
		draw_rect(x, y, w, h, COL(C_LIST_HOVER_BG));
	}
}

static void badge(int right, int y, int h, const char *s, u32 bg, u32 fg)
{
	int tw = font_width(FONT_SMALL, s, strlen(s));
	int bw = VX_MAX(PX(18), tw + PX(10));
	int bh = PX(18);
	int bx = right - bw;
	int by = y + (h - bh) / 2;
	draw_rect(bx, by, bw, bh, bg);
	draw_text(FONT_SMALL, bx + (bw - tw) / 2, by + (bh - font_m[FONT_SMALL].line) / 2, s, strlen(s), fg);
}

/* ---- explorer ---------------------------------------------------------------- */

static int ex_list_y(int y) { return y + PX(35) + PX(22); }

void explorer_reveal(u32 d)
{
	if (!app.docs.path[d])
		return;
	const char *rel = app.docs.rel[d];
	if (rel[0] == '/')
		return;
	u32 n = fs_reveal(&app.tree, rel);
	if (n != FS_NONE)
		app.tree_sel = n;
	/* scroll into view happens in draw */
}

static void ex_open(u32 node, bool preview)
{
	if (app.tree.flags[node] & FS_DIR) {
		fs_toggle(&app.tree, node);
		return;
	}
	char rel[4096];
	fs_relpath(&app.tree, node, rel, sizeof rel);
	open_rel(rel, preview, UINT32_MAX, 0);
	if (preview)
		app.focus = FOCUS_SIDEBAR;
}

void explorer_draw(int x, int y, int w, int h)
{
	int cy = view_title(x, y, w, "EXPLORER");
	char name[256];
	upper(name, fs_name(&app.tree, 0), sizeof name);
	cy = section_header(x, cy, w, name, true, false);
	bool hov_section = in_rect(app.hover_x, app.hover_y, x, y, w, h);
	if (hov_section) {
		/* actions: new file, new folder, refresh, collapse */
		static const int icons[] = {ICON_NEW_FILE, ICON_NEW_FOLDER, ICON_REFRESH, ICON_COLLAPSE};
		for (int i = 0; i < 4; i++)
			draw_icon(icons[i], x + w - PX(24) * (4 - i) - PX(4), cy - PX(22) + PX(3), COL(C_SECTION_HEADER_FG));
	}
	app.nrows = fs_rows(&app.tree, &app.rows, &app.rows_cap);
	int rh = app.row_h;
	int list_h = y + h - cy;
	/* keep the selection visible */
	for (u32 r = 0; r < app.nrows; r++)
		if (app.rows[r] == app.tree_sel) {
			int ry = (int)r * rh;
			if (ry < app.tree_scroll)
				app.tree_scroll = ry;
			if (ry + rh > app.tree_scroll + list_h)
				app.tree_scroll = ry + rh - list_h;
			break;
		}
	int maxs = VX_MAX((int)app.nrows * rh - list_h, 0);
	app.tree_scroll = VX_CLAMP(app.tree_scroll, 0, maxs);
	draw_clip(x, cy, w, list_h);
	bool focused = app.focus == FOCUS_SIDEBAR;
	u32 r0 = (u32)(app.tree_scroll / rh);
	for (u32 r = r0; r < app.nrows; r++) {
		int ry = cy + (int)r * rh - app.tree_scroll;
		if (ry > y + h)
			break;
		u32 node = app.rows[r];
		u8 fl = app.tree.flags[node];
		int depth = app.tree.depth[node] - 1;
		bool hover = in_rect(app.hover_x, app.hover_y, x, ry, w, rh) && app.hover_y >= cy;
		list_row_bg(x, ry, w, rh, node == app.tree_sel, focused, hover);
		int ix = x + PX(8) + depth * PX(8);
		/* indent guides (rendered always, faint) */
		for (int k = 0; k < depth; k++)
			if (hover || focused)
				draw_rect(x + PX(8) + k * PX(8) + PX(7), ry, PX(1), rh, COL(C_TREE_INDENT_GUIDE) & 0xFFFFFF60u);
		const char *nm = fs_name(&app.tree, node);
		char rel[4096];
		fs_relpath(&app.tree, node, rel, sizeof rel);
		char letter;
		bool has;
		u32 gc = path_git_color(rel, &letter, &has);
		u32 fg = has ? gc : COL(C_SIDEBAR_FG);
		if (node == app.tree_sel && focused)
			fg = has ? gc : COL(C_LIST_ACTIVE_FG);
		int tx;
		if (fl & FS_DIR) {
			draw_icon((fl & FS_OPEN) ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ix, ry + (rh - PX(16)) / 2,
			          COL(C_SIDEBAR_FG));
			tx = ix + PX(20);
		} else {
			tx = file_icon_draw(nm, ix + PX(16), ry, rh);
		}
		/* errors/warnings colour files like VSCodium's problem decorations */
		int right = x + w - PX(12);
		if (!(fl & FS_DIR)) {
			char abs[8192];
			snprintf(abs, sizeof abs, "%s/%s", app.root, rel);
			for (u32 d = 0; d < app.docs.n; d++)
				if (app.docs.path[d] && strcmp(app.docs.path[d], abs) == 0) {
					u32 errs = 0;
					for (u32 i = 0; i < app.docs.dn[d]; i++)
						errs += app.docs.d_sev[d][i] == LSP_SEV_ERROR;
					if (errs) {
						fg = COL(C_ERROR);
						char b[16];
						snprintf(b, sizeof b, "%u", errs);
						int bw = font_width(FONT_UI, b, strlen(b));
						draw_text(FONT_UI, right - bw, ry + (rh - font_m[FONT_UI].line) / 2, b, strlen(b), COL(C_ERROR));
						right -= bw + PX(6);
					}
				}
		}
		if (has && letter && !(fl & FS_DIR)) {
			char b[2] = {letter, 0};
			int bw = font_width(FONT_UI, b, 1);
			draw_text(FONT_UI, right - bw, ry + (rh - font_m[FONT_UI].line) / 2, b, 1, gc);
			right -= bw + PX(8);
		} else if (has && letter && (fl & FS_DIR)) {
			draw_icon(ICON_DOT, right - PX(14), ry + (rh - PX(16)) / 2, (gc & 0xFFFFFF00u) | 0xCCu);
			right -= PX(20);
		}
		draw_text_fit(FONT_UI, tx, ry + (rh - font_m[FONT_UI].line) / 2, nm, strlen(nm), right - tx, fg);
	}
	draw_noclip();
	if (app.tree_scroll > 0)
		draw_rect(x, cy, w, PX(3), (COL(C_SCROLLBAR_SHADOW) & 0xFFFFFF00u) | 0x40u);
}

static u32 row_index(u32 node)
{
	for (u32 r = 0; r < app.nrows; r++)
		if (app.rows[r] == node)
			return r;
	return UINT32_MAX;
}

bool explorer_event(const Event *e, int x, int y, int w, int h)
{
	int cy = ex_list_y(y);
	int rh = app.row_h;
	if (e->type == EV_WHEEL) {
		app.tree_scroll -= (int)(e->wy * rh * 3);
		app.dirty = true;
		return true;
	}
	if (e->type == EV_MDOWN) {
		if (e->y < cy && e->y >= cy - PX(22)) {
			/* header actions */
			int i = (x + w - PX(4) - e->x) / PX(24);
			int idx = 3 - i;
			if (e->x > x + w - PX(4) - PX(24) * 4 && idx >= 0 && idx < 4) {
				if (idx == 0 || idx == 1) {
					/* new file/folder relative to the selected folder */
					char base[4096] = "";
					u32 sel = app.tree_sel;
					if (sel != FS_NONE) {
						u32 dir = (app.tree.flags[sel] & FS_DIR) ? sel : app.tree.parent[sel];
						if (dir != 0 && dir != FS_NONE) {
							u32 n = fs_relpath(&app.tree, dir, base, sizeof base - 2);
							base[n] = '/';
							base[n + 1] = 0;
						}
					}
					app.quick = QUICK_PROMPT;
					app.quick_prompt_kind = idx == 0 ? 2 : 3;
					snprintf(app.quick_title, sizeof app.quick_title, idx == 0 ? "New File..." : "New Folder...");
					input_set(&app.quick_in, base, strlen(base));
					app.focus = FOCUS_QUICK;
				} else if (idx == 2) {
					run_command(CMD_REFRESH_EXPLORER);
				} else {
					run_command(CMD_COLLAPSE_FOLDERS);
				}
			}
			return true;
		}
		if (e->y < cy)
			return true;
		u32 r = (u32)((e->y - cy + app.tree_scroll) / rh);
		if (r >= app.nrows)
			return true;
		u32 node = app.rows[r];
		app.tree_sel = node;
		app.focus = FOCUS_SIDEBAR;
		if (app.tree.flags[node] & FS_DIR)
			fs_toggle(&app.tree, node);
		else
			ex_open(node, e->clicks < 2);
		if (e->clicks >= 2 && !(app.tree.flags[node] & FS_DIR))
			app.focus = FOCUS_EDITOR;
		app.dirty = true;
		return true;
	}
	if (e->type == EV_TEXT)
		return true;
	if (e->type != EV_KEY)
		return false;
	u32 r = row_index(app.tree_sel);
	switch (e->key) {
	case SDLK_DOWN:
		app.tree_sel = app.nrows ? app.rows[r == UINT32_MAX ? 0 : VX_MIN(r + 1, app.nrows - 1)] : FS_NONE;
		break;
	case SDLK_UP:
		app.tree_sel = app.nrows ? app.rows[r == UINT32_MAX || r == 0 ? 0 : r - 1] : FS_NONE;
		break;
	case SDLK_HOME:
		if (app.nrows)
			app.tree_sel = app.rows[0];
		break;
	case SDLK_END:
		if (app.nrows)
			app.tree_sel = app.rows[app.nrows - 1];
		break;
	case SDLK_PAGEDOWN:
	case SDLK_PAGEUP: {
		u32 page = (u32)VX_MAX((h - PX(57)) / rh - 1, 1);
		if (r == UINT32_MAX)
			r = 0;
		r = e->key == SDLK_PAGEDOWN ? VX_MIN(r + page, app.nrows ? app.nrows - 1 : 0) : (r > page ? r - page : 0);
		if (app.nrows)
			app.tree_sel = app.rows[r];
		break;
	}
	case SDLK_RIGHT:
		if (app.tree_sel != FS_NONE && (app.tree.flags[app.tree_sel] & FS_DIR)) {
			if (!(app.tree.flags[app.tree_sel] & FS_OPEN))
				fs_toggle(&app.tree, app.tree_sel);
			else if (app.tree.first[app.tree_sel] != FS_NONE)
				app.tree_sel = app.tree.first[app.tree_sel];
		}
		break;
	case SDLK_LEFT:
		if (app.tree_sel != FS_NONE) {
			u32 n = app.tree_sel;
			if ((app.tree.flags[n] & FS_DIR) && (app.tree.flags[n] & FS_OPEN))
				fs_toggle(&app.tree, n);
			else if (app.tree.parent[n] != 0 && app.tree.parent[n] != FS_NONE)
				app.tree_sel = app.tree.parent[n];
		}
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if (app.tree_sel != FS_NONE) {
			ex_open(app.tree_sel, false);
			if (!(app.tree.flags[app.tree_sel] & FS_DIR))
				app.focus = FOCUS_EDITOR;
		}
		break;
	case SDLK_SPACE:
		if (app.tree_sel != FS_NONE)
			ex_open(app.tree_sel, true);
		break;
	case SDLK_F2:
		if (app.tree_sel != FS_NONE) {
			char rel[4096];
			fs_relpath(&app.tree, app.tree_sel, rel, sizeof rel);
			app.quick = QUICK_PROMPT;
			app.quick_prompt_kind = 4;
			app.quick_arg = app.tree_sel;
			snprintf(app.quick_title, sizeof app.quick_title, "Rename '%s'", fs_name(&app.tree, app.tree_sel));
			input_set(&app.quick_in, rel, strlen(rel));
			/* select the name without the extension, like VSCodium */
			const char *sl = strrchr(rel, '/');
			const char *nm = sl ? sl + 1 : rel;
			const char *dot = strrchr(nm, '.');
			app.quick_in.anchor = (u32)(nm - rel);
			app.quick_in.cur = dot && dot != nm ? (u32)(dot - rel) : (u32)strlen(rel);
			app.focus = FOCUS_QUICK;
		}
		break;
	case SDLK_DELETE:
		if (app.tree_sel != FS_NONE) {
			app.quick = QUICK_CONFIRM;
			app.quick_prompt_kind = 6;
			app.quick_arg = app.tree_sel;
			snprintf(app.quick_title, sizeof app.quick_title, "Delete '%s' permanently?",
			         fs_name(&app.tree, app.tree_sel));
			input_set(&app.quick_in, "", 0);
			app.focus = FOCUS_QUICK;
		}
		break;
	default:
		return false;
	}
	app.dirty = true;
	return true;
}

/* ---- search view ---------------------------------------------------------- */

static u64 search_due;
static u32 search_last_hash = 1;

static u32 hash_input(const Input *in, int flags)
{
	u32 h = 2166136261u ^ (u32)flags;
	for (u32 i = 0; i < in->n; i++)
		h = (h ^ (u8)in->b[i]) * 16777619u;
	return h;
}

void search_run(void)
{
	extern void ui_ensure_files(void);
	ui_ensure_files();
	search_text(&app.results, app.root, &app.files, app.search_in.b ? app.search_in.b : "", app.search_in.n,
	            app.search_flags, 20000);
	free(app.search_collapsed);
	app.search_collapsed = vx_alloc(app.files.n + 1);
	app.search_scroll = 0;
	app.search_sel = 0;
	search_last_hash = hash_input(&app.search_in, app.search_flags);
	app.dirty = true;
}

void search_tick(void)
{
	if (search_due && vx_now_ns() >= search_due) {
		search_due = 0;
		if (hash_input(&app.search_in, app.search_flags) != search_last_hash)
			search_run();
	}
}

/* Flattened rows: file headers then matches. row -> (file, match) */
static u32 srow_n;
static u32 *srow_res; /* result index, or UINT32_MAX - file for headers */
static u32 srow_cap;

static void search_rows(void)
{
	srow_n = 0;
	const SearchResults *r = &app.results;
	u32 prev = UINT32_MAX;
	for (u32 i = 0; i < r->n; i++) {
		if (r->file[i] != prev) {
			VX_GROW(srow_res, srow_cap, srow_n + 1);
			srow_res[srow_n++] = 0x80000000u | r->file[i];
			prev = r->file[i];
		}
		if (app.search_collapsed && app.search_collapsed[r->file[i]])
			continue;
		VX_GROW(srow_res, srow_cap, srow_n + 1);
		srow_res[srow_n++] = i;
	}
}

static int search_list_y(int y) { return y + PX(35) + PX(30) + PX(26); }

void search_draw(int x, int y, int w, int h)
{
	int cy = view_title(x, y, w, "SEARCH");
	int ix = x + PX(12), iw = w - PX(24);
	input_draw(&app.search_in, ix, cy, iw, PX(26), "Search", app.focus == FOCUS_SEARCH_INPUT);
	/* toggles */
	const char *tg[2] = {"Aa", "ab"};
	int fl[2] = {SEARCH_CASE, SEARCH_WORD};
	for (int k = 0; k < 2; k++) {
		int bx = ix + iw - PX(48) + k * PX(22);
		if (app.search_flags & fl[k]) {
			draw_rect(bx, cy + PX(3), PX(20), PX(20), (COL(C_FOCUS_BORDER) & 0xFFFFFF00u) | 0x50u);
			draw_frame(bx, cy + PX(3), PX(20), PX(20), COL(C_FOCUS_BORDER));
		}
		draw_text(FONT_SMALL_BOLD, bx + PX(3), cy + PX(3) + (PX(20) - font_m[FONT_SMALL].line) / 2, tg[k], 2, COL(C_FG));
		if (k == 1)
			draw_rect(bx + PX(3), cy + PX(19), PX(14), PX(1), COL(C_FG));
	}
	cy += PX(30);
	const SearchResults *r = &app.results;
	char msg[128];
	if (app.search_in.n && r->n)
		snprintf(msg, sizeof msg, "%u result%s in %u file%s%s", r->n, r->n == 1 ? "" : "s", r->files_hit,
		         r->files_hit == 1 ? "" : "s", r->truncated ? " (truncated)" : "");
	else if (app.search_in.n && hash_input(&app.search_in, app.search_flags) == search_last_hash)
		snprintf(msg, sizeof msg, "No results found.");
	else
		msg[0] = 0;
	draw_text(FONT_UI, x + PX(20), cy + (PX(26) - font_m[FONT_UI].line) / 2, msg, strlen(msg), COL(C_DESCRIPTION_FG));
	cy += PX(26);
	search_rows();
	int rh = app.row_h;
	int list_h = y + h - cy;
	int maxs = VX_MAX((int)srow_n * rh - list_h, 0);
	app.search_scroll = VX_CLAMP(app.search_scroll, 0, maxs);
	draw_clip(x, cy, w, list_h);
	bool focused = app.focus == FOCUS_SIDEBAR;
	for (u32 i = (u32)(app.search_scroll / rh); i < srow_n; i++) {
		int ry = cy + (int)i * rh - app.search_scroll;
		if (ry > y + h)
			break;
		bool hover = in_rect(app.hover_x, app.hover_y, x, ry, w, rh) && app.hover_y >= cy;
		list_row_bg(x, ry, w, rh, i == app.search_sel && focused, focused, hover);
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		u32 v = srow_res[i];
		if (v & 0x80000000u) {
			u32 f = v & 0x7FFFFFFFu;
			const char *p = app.files.pool.b + app.files.path[f];
			const char *base = p + app.files.base[f];
			bool col = app.search_collapsed && app.search_collapsed[f];
			draw_icon(col ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, x + PX(8), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			int tx = file_icon_draw(base, x + PX(24), ry, rh);
			tx = draw_text(FONT_UI, tx, ty, base, strlen(base), COL(C_SIDEBAR_FG));
			u32 cnt = 0;
			for (u32 k = 0; k < r->n; k++)
				cnt += r->file[k] == f;
			char b[16];
			snprintf(b, sizeof b, "%u", cnt);
			int bw = VX_MAX(PX(18), font_width(FONT_SMALL, b, strlen(b)) + PX(10));
			if (app.files.base[f] > 1)
				draw_text_fit(FONT_UI, tx + PX(6), ty, p, app.files.base[f] - 1, x + w - PX(16) - bw - tx - PX(6),
				              COL(C_DESCRIPTION_FG));
			badge(x + w - PX(12), ry, rh, b, COL(C_BADGE_BG), COL(C_BADGE_FG));
		} else {
			const char *t = r->pool.b + r->text[v];
			size_t n = strlen(t);
			u32 mc = r->text_col[v];
			int tx = x + PX(40);
			int a = tx + font_width(FONT_UI, t, VX_MIN(mc, (u32)n));
			int b = a + font_width(FONT_UI, t + VX_MIN(mc, (u32)n), VX_MIN(r->len[v], (u32)n - VX_MIN(mc, (u32)n)));
			draw_rect(a, ry + PX(2), b - a, rh - PX(4), COL(C_FIND_MATCH_OTHER));
			draw_text_fit(FONT_UI, tx, ty, t, n, x + w - tx - PX(8), COL(C_SIDEBAR_FG));
		}
	}
	draw_noclip();
}

static void search_open_row(u32 i, bool preview)
{
	u32 v = srow_res[i];
	if (v & 0x80000000u) {
		u32 f = v & 0x7FFFFFFFu;
		if (app.search_collapsed)
			app.search_collapsed[f] ^= 1;
		return;
	}
	const SearchResults *r = &app.results;
	open_rel(app.files.pool.b + app.files.path[r->file[v]], preview, r->line[v], r->col[v]);
	View *vw = active_view();
	if (vw) {
		vw->anchor[0] = vw->head[0];
		vw->head[0] = vw->anchor[0] + r->len[v];
	}
}

bool search_event(const Event *e, int x, int y, int w, int h)
{
	int iy = y + PX(35);
	int cy = search_list_y(y);
	int rh = app.row_h;
	if (e->type == EV_WHEEL) {
		app.search_scroll -= (int)(e->wy * rh * 3);
		return true;
	}
	if (e->type == EV_MDOWN) {
		if (e->y >= iy && e->y < iy + PX(26)) {
			int ix = x + PX(12), iw = w - PX(24);
			int bx = ix + iw - PX(48);
			if (e->x >= bx && e->x < bx + PX(44)) {
				app.search_flags ^= e->x < bx + PX(22) ? SEARCH_CASE : SEARCH_WORD;
				search_run();
			} else {
				input_click(&app.search_in, FONT_UI, ix + PX(6), e->x);
			}
			app.focus = FOCUS_SEARCH_INPUT;
			return true;
		}
		if (e->y >= cy) {
			search_rows();
			u32 i = (u32)((e->y - cy + app.search_scroll) / rh);
			if (i < srow_n) {
				app.search_sel = i;
				app.focus = FOCUS_SIDEBAR;
				search_open_row(i, e->clicks < 2);
				if (!(srow_res[i] & 0x80000000u))
					app.focus = e->clicks >= 2 ? FOCUS_EDITOR : FOCUS_SIDEBAR;
			}
		}
		return true;
	}
	if (app.focus == FOCUS_SEARCH_INPUT) {
		if (e->type == EV_TEXT) {
			input_text(&app.search_in, e->text, strlen(e->text));
			search_due = vx_now_ns() + 300000000ull;
			return true;
		}
		if (e->type == EV_KEY) {
			if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
				search_run();
				return true;
			}
			if (e->key == SDLK_DOWN) {
				app.focus = FOCUS_SIDEBAR;
				return true;
			}
			if (e->key == SDLK_C && e->mods == MOD_ALT) {
				app.search_flags ^= SEARCH_CASE;
				search_run();
				return true;
			}
			if (e->key == SDLK_W && e->mods == MOD_ALT) {
				app.search_flags ^= SEARCH_WORD;
				search_run();
				return true;
			}
			if (e->key == SDLK_ESCAPE) {
				app.focus = FOCUS_EDITOR;
				return true;
			}
			bool r = input_key(&app.search_in, e);
			if (r)
				search_due = vx_now_ns() + 300000000ull;
			return r;
		}
		return false;
	}
	if (e->type == EV_KEY) {
		search_rows();
		switch (e->key) {
		case SDLK_DOWN:
			if (srow_n)
				app.search_sel = VX_MIN(app.search_sel + 1, srow_n - 1);
			break;
		case SDLK_UP:
			if (app.search_sel == 0)
				app.focus = FOCUS_SEARCH_INPUT;
			else
				app.search_sel--;
			break;
		case SDLK_RETURN:
			if (app.search_sel < srow_n) {
				search_open_row(app.search_sel, false);
				if (!(srow_res[app.search_sel] & 0x80000000u))
					app.focus = FOCUS_EDITOR;
			}
			break;
		case SDLK_SPACE:
			if (app.search_sel < srow_n)
				search_open_row(app.search_sel, true);
			app.focus = FOCUS_SIDEBAR;
			break;
		default:
			return false;
		}
		/* scroll into view */
		int list_h = y + h - cy;
		int ry = (int)app.search_sel * rh;
		if (ry < app.search_scroll)
			app.search_scroll = ry;
		if (ry + rh > app.search_scroll + list_h)
			app.search_scroll = ry + rh - list_h;
		return true;
	}
	return false;
}

/* ---- source control --------------------------------------------------- */

/* rows: section header (staged / changes) then files */
static u32 scm_n, scm_cap;
static u32 *scm_item; /* status index | 0x40000000 staged | 0x80000000 header (0 changes, 1 staged) */
static bool scm_closed[2];

void scm_refresh(void)
{
	if (app.git) {
		git_refresh(app.git);
		app.git_ns = vx_now_ns();
		for (u32 d = 0; d < app.docs.n; d++)
			app.docs.gm_for[d] = UINT32_MAX;
	}
	app.dirty = true;
}

static bool st_staged(u16 f)
{
	return f & (GS_INDEX_NEW | GS_INDEX_MODIFIED | GS_INDEX_DELETED | GS_INDEX_RENAMED);
}

static bool st_changed(u16 f)
{
	return f & (GS_WT_NEW | GS_WT_MODIFIED | GS_WT_DELETED | GS_WT_RENAMED | GS_CONFLICT);
}

static void scm_rows(void)
{
	scm_n = 0;
	if (!app.git)
		return;
	const GitStatus *st = git_status(app.git);
	for (int sec = 1; sec >= 0; sec--) {
		u32 cnt = 0;
		for (u32 i = 0; i < st->count; i++)
			cnt += sec ? st_staged(st->flags[i]) : st_changed(st->flags[i]);
		if (!cnt)
			continue;
		VX_GROW(scm_item, scm_cap, scm_n + 1);
		scm_item[scm_n++] = 0x80000000u | (u32)sec;
		if (scm_closed[sec])
			continue;
		for (u32 i = 0; i < st->count; i++) {
			bool in = sec ? st_staged(st->flags[i]) : st_changed(st->flags[i]);
			if (!in)
				continue;
			VX_GROW(scm_item, scm_cap, scm_n + 1);
			scm_item[scm_n++] = i | (sec ? 0x40000000u : 0);
		}
	}
}

static u32 sec_count(int sec)
{
	const GitStatus *st = git_status(app.git);
	u32 cnt = 0;
	for (u32 i = 0; i < st->count; i++)
		cnt += sec ? st_staged(st->flags[i]) : st_changed(st->flags[i]);
	return cnt;
}

static int scm_list_y(int y) { return y + PX(35) + PX(8) + PX(26) + PX(8) + PX(28) + PX(8); }

static char status_letter(u16 f, bool staged, u32 *color)
{
	if (staged) {
		if (f & GS_INDEX_NEW) {
			*color = COL(C_GIT_ADDED);
			return 'A';
		}
		if (f & GS_INDEX_DELETED) {
			*color = COL(C_GIT_DELETED);
			return 'D';
		}
		if (f & GS_INDEX_RENAMED) {
			*color = COL(C_GIT_MODIFIED);
			return 'R';
		}
		*color = COL(C_GIT_MODIFIED);
		return 'M';
	}
	if (f & GS_CONFLICT) {
		*color = COL(C_GIT_CONFLICT);
		return '!';
	}
	if (f & GS_WT_NEW) {
		*color = COL(C_GIT_UNTRACKED);
		return 'U';
	}
	if (f & GS_WT_DELETED) {
		*color = COL(C_GIT_DELETED);
		return 'D';
	}
	*color = COL(C_GIT_MODIFIED);
	return 'M';
}

void scm_draw(int x, int y, int w, int h)
{
	int cy = view_title(x, y, w, "SOURCE CONTROL");
	if (!app.git) {
		const char *m = "The folder currently open doesn't have a git repository.";
		int yy = cy + PX(8);
		draw_text_fit(FONT_UI, x + PX(20), yy, m, strlen(m), w - PX(30), COL(C_SIDEBAR_FG));
		return;
	}
	cy += PX(8);
	char ph[160];
	snprintf(ph, sizeof ph, "Message (Ctrl+Enter to commit on '%s')", git_branch(app.git));
	input_draw(&app.commit_in, x + PX(12), cy, w - PX(24), PX(26), ph, app.focus == FOCUS_SCM_INPUT);
	cy += PX(26) + PX(8);
	/* commit button */
	int bx = x + PX(12), bw = w - PX(24), bh = PX(28);
	bool hov = in_rect(app.hover_x, app.hover_y, bx, cy, bw, bh);
	draw_rect(bx, cy, bw, bh, hov ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG));
	const char *bt = "Commit";
	int tw = font_width(FONT_UI, bt, strlen(bt));
	int tx = bx + (bw - tw - PX(22)) / 2;
	draw_icon(ICON_CHECK, tx, cy + (bh - PX(16)) / 2, COL(C_BUTTON_FG));
	draw_text(FONT_UI, tx + PX(22), cy + (bh - font_m[FONT_UI].line) / 2, bt, strlen(bt), COL(C_BUTTON_FG));
	cy += bh + PX(8);
	scm_rows();
	int rh = app.row_h;
	int list_h = y + h - cy;
	int maxs = VX_MAX((int)scm_n * rh - list_h, 0);
	app.scm_scroll = VX_CLAMP(app.scm_scroll, 0, maxs);
	draw_clip(x, cy, w, list_h);
	const GitStatus *st = git_status(app.git);
	bool focused = app.focus == FOCUS_SIDEBAR;
	for (u32 i = (u32)(app.scm_scroll / rh); i < scm_n; i++) {
		int ry = cy + (int)i * rh - app.scm_scroll;
		if (ry > y + h)
			break;
		bool hover = in_rect(app.hover_x, app.hover_y, x, ry, w, rh) && app.hover_y >= cy;
		u32 it = scm_item[i];
		list_row_bg(x, ry, w, rh, i == app.scm_sel && focused, focused, hover);
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		if (it & 0x80000000u) {
			int sec = (int)(it & 1);
			draw_icon(scm_closed[sec] ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, x + PX(8), ry + (rh - PX(16)) / 2,
			          COL(C_SIDEBAR_FG));
			const char *t = sec ? "Staged Changes" : "Changes";
			draw_text(FONT_UI, x + PX(26), ty, t, strlen(t), COL(C_SIDEBAR_FG));
			char b[16];
			snprintf(b, sizeof b, "%u", sec_count(sec));
			if (hover) {
				/* section actions: stage all / unstage all */
				draw_icon(sec ? ICON_MINUS : ICON_PLUS, x + w - PX(56), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			}
			badge(x + w - PX(12), ry, rh, b, COL(C_BADGE_BG), COL(C_BADGE_FG));
			continue;
		}
		bool staged = it & 0x40000000u;
		u32 si = it & 0x3FFFFFFFu;
		const char *p = st->pool.b + st->path[si];
		const char *sl = strrchr(p, '/');
		const char *base = sl ? sl + 1 : p;
		int fx = file_icon_draw(base, x + PX(26), ry, rh);
		u32 lc;
		char letter = status_letter(st->flags[si], staged, &lc);
		bool deleted = letter == 'D';
		int right = x + w - PX(14);
		char lb[2] = {letter, 0};
		int lw = font_width(FONT_UI, lb, 1);
		draw_text(FONT_UI, right - lw, ty, lb, 1, lc);
		right -= lw + PX(10);
		if (hover) {
			/* actions: discard (changes), stage/unstage */
			if (!staged) {
				draw_icon(ICON_DISCARD, right - PX(36), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
				draw_icon(ICON_PLUS, right - PX(16), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			} else {
				draw_icon(ICON_MINUS, right - PX(16), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			}
			right -= PX(42);
		}
		int ex = draw_text_fit(FONT_UI, fx, ty, base, strlen(base), right - fx, deleted ? lc : lc);
		if (deleted)
			draw_rect(fx, ty + font_m[FONT_UI].line / 2, ex - fx, PX(1), lc);
		if (sl && ex + PX(6) < right)
			draw_text_fit(FONT_UI, ex + PX(6), ty, p, (size_t)(sl - p), right - ex - PX(6), COL(C_DESCRIPTION_FG));
	}
	draw_noclip();
}

static void scm_commit(void)
{
	run_command(CMD_GIT_COMMIT);
}

bool scm_event(const Event *e, int x, int y, int w, int h)
{
	if (!app.git)
		return false;
	int iy = y + PX(35) + PX(8);
	int by = iy + PX(26) + PX(8);
	int cy = scm_list_y(y);
	int rh = app.row_h;
	(void)h;
	if (e->type == EV_WHEEL) {
		app.scm_scroll -= (int)(e->wy * rh * 3);
		return true;
	}
	if (e->type == EV_MDOWN) {
		if (e->y >= iy && e->y < iy + PX(26)) {
			app.focus = FOCUS_SCM_INPUT;
			input_click(&app.commit_in, FONT_UI, x + PX(18), e->x);
			return true;
		}
		if (e->y >= by && e->y < by + PX(28)) {
			scm_commit();
			return true;
		}
		if (e->y < cy)
			return true;
		scm_rows();
		u32 i = (u32)((e->y - cy + app.scm_scroll) / rh);
		if (i >= scm_n)
			return true;
		app.scm_sel = i;
		app.focus = FOCUS_SIDEBAR;
		u32 it = scm_item[i];
		const GitStatus *st = git_status(app.git);
		if (it & 0x80000000u) {
			int sec = (int)(it & 1);
			if (e->x >= x + w - PX(60) && e->x < x + w - PX(36)) {
				if (sec == 0)
					git_stage_all(app.git);
				else
					for (u32 k = 0; k < st->count; k++)
						if (st_staged(st->flags[k]))
							git_unstage(app.git, st->pool.b + st->path[k]);
				scm_refresh();
			} else {
				scm_closed[sec] = !scm_closed[sec];
			}
			return true;
		}
		bool staged = it & 0x40000000u;
		u32 si = it & 0x3FFFFFFFu;
		char path[4096];
		snprintf(path, sizeof path, "%s", st->pool.b + st->path[si]);
		char lb[2] = {'M', 0};
		int right = x + w - PX(14) - font_width(FONT_UI, lb, 1) - PX(10);
		if (e->x >= right - PX(18) && e->x < right) {
			if (staged)
				git_unstage(app.git, path);
			else
				git_stage(app.git, path);
			scm_refresh();
			return true;
		}
		if (!staged && e->x >= right - PX(38) && e->x < right - PX(18)) {
			git_discard(app.git, path);
			/* reload open buffers of that file */
			char abs[8192];
			snprintf(abs, sizeof abs, "%s%s", git_workdir(app.git), path);
			for (u32 d = 0; d < app.docs.n; d++)
				if (app.docs.path[d] && strcmp(app.docs.path[d], abs) == 0 && !text_dirty(&app.docs.text[d])) {
					Text *t = &app.docs.text[d];
					size_t n = 0;
					char *s = vx_read_file(abs, &n);
					if (s) {
						doc_replace(d, 0, t->len, s, (u32)n, 0);
						t->saved_upos = t->upos;
						free(s);
					}
				}
			scm_refresh();
			return true;
		}
		char abs[8192];
		snprintf(abs, sizeof abs, "%s%s", git_workdir(app.git), path);
		open_file(abs, e->clicks < 2, UINT32_MAX, 0);
		if (e->clicks < 2)
			app.focus = FOCUS_SIDEBAR;
		return true;
	}
	if (app.focus == FOCUS_SCM_INPUT) {
		if (e->type == EV_TEXT) {
			input_text(&app.commit_in, e->text, strlen(e->text));
			return true;
		}
		if (e->type == EV_KEY) {
			if ((e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) && (e->mods & MOD_CTRL)) {
				scm_commit();
				return true;
			}
			if (e->key == SDLK_ESCAPE) {
				app.focus = FOCUS_EDITOR;
				return true;
			}
			if (e->key == SDLK_DOWN) {
				app.focus = FOCUS_SIDEBAR;
				return true;
			}
			return input_key(&app.commit_in, e);
		}
		return false;
	}
	if (e->type == EV_KEY) {
		scm_rows();
		switch (e->key) {
		case SDLK_DOWN:
			if (scm_n)
				app.scm_sel = VX_MIN(app.scm_sel + 1, scm_n - 1);
			return true;
		case SDLK_UP:
			if (app.scm_sel)
				app.scm_sel--;
			else
				app.focus = FOCUS_SCM_INPUT;
			return true;
		case SDLK_RETURN:
		case SDLK_SPACE:
			if (app.scm_sel < scm_n && !(scm_item[app.scm_sel] & 0x80000000u)) {
				const GitStatus *st = git_status(app.git);
				u32 si = scm_item[app.scm_sel] & 0x3FFFFFFFu;
				char abs[8192];
				snprintf(abs, sizeof abs, "%s%s", git_workdir(app.git), st->pool.b + st->path[si]);
				open_file(abs, e->key == SDLK_SPACE, UINT32_MAX, 0);
				if (e->key == SDLK_SPACE)
					app.focus = FOCUS_SIDEBAR;
			}
			return true;
		default:
			return false;
		}
	}
	return false;
}

/* ---- problems ----------------------------------------------------------------- */

static u32 prow_n, prow_cap;
static u32 *prow_doc, *prow_diag; /* diag UINT32_MAX = file header */

static void problem_rows(void)
{
	prow_n = 0;
	Docs *D = &app.docs;
	for (u32 d = 0; d < D->n; d++) {
		if (!D->dn[d] || !D->path[d])
			continue;
		VX_GROW(prow_doc, prow_cap, prow_n + 1 + D->dn[d]);
		prow_diag = vx_realloc(prow_diag, prow_cap * sizeof(u32));
		prow_doc[prow_n] = d;
		prow_diag[prow_n++] = UINT32_MAX;
		/* errors first, then by line */
		for (u8 sev = 1; sev <= 4; sev++)
			for (u32 i = 0; i < D->dn[d]; i++)
				if (D->d_sev[d][i] == sev) {
					prow_doc[prow_n] = d;
					prow_diag[prow_n++] = i;
				}
	}
}

void problems_draw(int x, int y, int w, int h)
{
	problem_rows();
	int rh = app.row_h;
	if (!prow_n) {
		const char *m = "No problems have been detected in the workspace.";
		draw_text(FONT_UI, x + PX(20), y + PX(4), m, strlen(m), COL(C_FG));
		return;
	}
	int maxs = VX_MAX((int)prow_n * rh - h, 0);
	app.problems_scroll = VX_CLAMP(app.problems_scroll, 0, maxs);
	bool focused = app.focus == FOCUS_PANEL;
	Docs *D = &app.docs;
	for (u32 i = (u32)(app.problems_scroll / rh); i < prow_n; i++) {
		int ry = y + (int)i * rh - app.problems_scroll;
		if (ry > y + h)
			break;
		bool hover = in_rect(app.hover_x, app.hover_y, x, ry, w, rh);
		list_row_bg(x, ry, w, rh, i == app.problems_sel && focused, focused, hover);
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		u32 d = prow_doc[i];
		if (prow_diag[i] == UINT32_MAX) {
			const char *rel = D->rel[d];
			const char *sl = strrchr(rel, '/');
			const char *base = sl ? sl + 1 : rel;
			draw_icon(ICON_CHEVRON_DOWN, x + PX(8), ry + (rh - PX(16)) / 2, COL(C_FG));
			int tx = file_icon_draw(base, x + PX(26), ry, rh);
			tx = draw_text(FONT_UI, tx, ty, base, strlen(base), COL(C_FG));
			if (sl)
				tx = draw_text(FONT_UI, tx + PX(6), ty, rel, (size_t)(sl - rel), COL(C_DESCRIPTION_FG));
			char b[16];
			snprintf(b, sizeof b, "%u", D->dn[d]);
			int bw = VX_MAX(PX(18), font_width(FONT_SMALL, b, strlen(b)) + PX(10));
			badge(tx + PX(8) + bw, ry, rh, b, COL(C_BADGE_BG), COL(C_BADGE_FG));
			continue;
		}
		u32 k = prow_diag[i];
		u8 sev = D->d_sev[d][k];
		u32 c = sev == LSP_SEV_ERROR ? COL(C_ERROR) : sev == LSP_SEV_WARNING ? COL(C_WARNING) : COL(C_INFO);
		draw_icon(sev == LSP_SEV_ERROR ? ICON_ERROR : sev == LSP_SEV_WARNING ? ICON_WARNING : ICON_INFO, x + PX(44),
		          ry + (rh - PX(16)) / 2, c);
		const char *m = D->d_pool[d].b + D->d_msg[d][k];
		size_t mlen = strcspn(m, "\x1f");
		char pos[160];
		snprintf(pos, sizeof pos, "%s%s[Ln %u, Col %u]", m[mlen] ? m + mlen + 1 : "", m[mlen] ? " " : "",
		         D->d_line0[d][k] + 1, D->d_col0[d][k] + 1);
		int pw = font_width(FONT_UI, pos, strlen(pos));
		int tx = draw_text_fit(FONT_UI, x + PX(66), ty, m, mlen, w - PX(66) - pw - PX(20), COL(C_FG));
		draw_text(FONT_UI, tx + PX(8), ty, pos, strlen(pos), COL(C_DESCRIPTION_FG));
	}
}

static void problem_open(u32 i)
{
	if (i >= prow_n || prow_diag[i] == UINT32_MAX)
		return;
	Docs *D = &app.docs;
	u32 d = prow_doc[i], k = prow_diag[i];
	open_file(D->path[d], false, D->d_line0[d][k], D->d_col0[d][k]);
}

bool problems_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)w;
	int cy = y + PX(35);
	int rh = app.row_h;
	problem_rows();
	if (e->type == EV_WHEEL) {
		app.problems_scroll -= (int)(e->wy * rh * 3);
		return true;
	}
	if (e->type == EV_MDOWN && e->y >= cy) {
		u32 i = (u32)((e->y - cy + app.problems_scroll) / rh);
		if (i < prow_n) {
			app.problems_sel = i;
			problem_open(i);
		}
		return true;
	}
	if (e->type == EV_KEY) {
		switch (e->key) {
		case SDLK_DOWN:
			if (prow_n)
				app.problems_sel = VX_MIN(app.problems_sel + 1, prow_n - 1);
			break;
		case SDLK_UP:
			if (app.problems_sel)
				app.problems_sel--;
			break;
		case SDLK_RETURN:
			problem_open(app.problems_sel);
			break;
		default:
			return false;
		}
		int list_h = h - PX(35);
		int ry = (int)app.problems_sel * rh;
		if (ry < app.problems_scroll)
			app.problems_scroll = ry;
		if (ry + rh > app.problems_scroll + list_h)
			app.problems_scroll = ry + rh - list_h;
		return true;
	}
	return false;
}

/* ---- output ----------------------------------------------------------------- */

static bool output_follow = true;

void output_draw(int x, int y, int w, int h)
{
	int lh = app.line_h;
	int total = (int)app.out_n * lh;
	int maxs = VX_MAX(total - h + PX(8), 0);
	if (output_follow)
		app.output_scroll = maxs;
	app.output_scroll = VX_CLAMP(app.output_scroll, 0, maxs);
	draw_rect(x, y, w, h, COL(C_EDITOR_BG));
	for (u32 i = (u32)(app.output_scroll / lh); i < app.out_n; i++) {
		int ly = y + PX(4) + (int)i * lh - app.output_scroll;
		if (ly > y + h)
			break;
		const char *s = app.output.b + app.out_lines[i];
		draw_text_fit(FONT_MONO, x + PX(20), ly, s, strlen(s), w - PX(30), COL(C_EDITOR_FG));
	}
}

bool output_event(const Event *e, int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	if (e->type == EV_WHEEL) {
		app.output_scroll -= (int)(e->wy * app.line_h * 3);
		output_follow = e->wy < 0 && app.output_scroll >= (int)app.out_n * app.line_h - h;
		return true;
	}
	return false;
}
