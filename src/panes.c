/* Side bar views (Explorer, Search, Source Control), the panel views
 * (Problems, Output) and the single-line input box they share. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "syntax.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
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
	if (!in->b)
		input_set(in, "", 0); /* the first character needs a terminated buffer */
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

static void toggle_box(int bx, int by, const char *label, bool on, bool underline)
{
	if (on) {
		draw_rect(bx, by, PX(20), PX(20), (COL(C_FOCUS_BORDER) & 0xFFFFFF00u) | 0x50u);
		draw_frame(bx, by, PX(20), PX(20), COL(C_FOCUS_BORDER));
	}
	int tw = font_width(FONT_SMALL_BOLD, label, strlen(label));
	draw_text(FONT_SMALL_BOLD, bx + (PX(20) - tw) / 2, by + (PX(20) - font_m[FONT_SMALL].line) / 2, label,
	          strlen(label), COL(C_FG));
	if (underline)
		draw_rect(bx + PX(3), by + PX(16), PX(14), PX(1), COL(C_FG));
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

static u32 hash_bytes(u32 h, const Input *in)
{
	for (u32 i = 0; i < in->n; i++)
		h = (h ^ (u8)in->b[i]) * 16777619u;
	return (h ^ 0xFF) * 16777619u;
}

static u32 search_hash(void)
{
	u32 h = 2166136261u ^ (u32)app.search_flags;
	h = hash_bytes(h, &app.search_in);
	if (app.search_show_details) {
		h = hash_bytes(h, &app.search_inc_in);
		h = hash_bytes(h, &app.search_exc_in);
	}
	return h;
}

static const char *box_text(Input *in) { return in->b ? in->b : ""; }

void search_run(void)
{
	ui_ensure_files();
	matcher_free(&app.search_m);
	matcher_init(&app.search_m, box_text(&app.search_in), app.search_in.n, app.search_flags);
	const char *inc = app.search_show_details ? box_text(&app.search_inc_in) : NULL;
	const char *exc = app.search_show_details ? box_text(&app.search_exc_in) : NULL;
	search_text(&app.results, app.root, &app.files, &app.search_m, inc, exc, 20000);
	free(app.search_collapsed);
	app.search_collapsed = vx_alloc(app.files.n + 1);
	app.search_scroll = 0;
	app.search_sel = 0;
	search_last_hash = search_hash();
	app.dirty = true;
}

void search_tick(void)
{
	if (search_due && vx_now_ns() >= search_due) {
		search_due = 0;
		if (search_hash() != search_last_hash)
			search_run();
	}
}

static void search_changed(void) { search_due = vx_now_ns() + 300000000ull; }

/* Flattened rows: file headers then matches. */
static u32 srow_n;
static u32 *srow_res; /* result index, or 0x80000000 | file for headers */
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

/* Layout of the top part (inputs) and where the list starts. */
typedef struct {
	int find_y, repl_y, inc_y, exc_y, msg_y, list_y;
	int ix, iw;
} SLayout;

static SLayout search_layout(int x, int y, int w)
{
	SLayout L;
	L.ix = x + PX(24);
	L.iw = w - PX(24) - PX(12);
	int cy = y + PX(35);
	L.find_y = cy;
	cy += PX(26) + PX(4);
	L.repl_y = cy;
	if (app.search_show_replace)
		cy += PX(26) + PX(4);
	L.inc_y = L.exc_y = cy;
	cy += PX(18); /* "..." row */
	if (app.search_show_details) {
		L.inc_y = cy + PX(18);
		cy = L.inc_y + PX(26) + PX(4);
		L.exc_y = cy + PX(18);
		cy = L.exc_y + PX(26) + PX(4);
	}
	L.msg_y = cy;
	L.list_y = cy + PX(24);
	return L;
}

static Input *search_box_input(int b)
{
	return b == 1 ? &app.search_repl_in : b == 2 ? &app.search_inc_in : b == 3 ? &app.search_exc_in : &app.search_in;
}

/* Replacement text for result i, using its preview line (the pattern is
 * re-run there; line-mode matches are within the line). */
static char *result_replacement(u32 i, u32 *len)
{
	const SearchResults *r = &app.results;
	const char *t = r->pool.b + r->text[i];
	u32 n = (u32)strlen(t), col = r->text_col[i];
	char *out = NULL;
	u32 cap = 0;
	*len = 0;
	u32 a, b;
	const char *repl = box_text(&app.search_repl_in);
	if ((app.search_flags & SEARCH_REGEX) && col <= n && matcher_next(&app.search_m, t, n, col, &a, &b) && a == col) {
		matcher_expand(&app.search_m, repl, app.search_repl_in.n, t, &out, len, &cap);
	} else {
		out = vx_strndup(repl, app.search_repl_in.n);
		*len = app.search_repl_in.n;
	}
	return out;
}

void search_draw(int x, int y, int w, int h)
{
	view_title(x, y, w, "SEARCH");
	SLayout L = search_layout(x, y, w);
	/* title actions: refresh, clear, collapse */
	if (in_rect(app.hover_x, app.hover_y, x, y, w, h)) {
		draw_icon(ICON_REFRESH, x + w - PX(76), y + PX(10), COL(C_SIDEBAR_FG));
		draw_icon(ICON_CLOSE, x + w - PX(52), y + PX(10), COL(C_SIDEBAR_FG));
		draw_icon(ICON_COLLAPSE, x + w - PX(28), y + PX(10), COL(C_SIDEBAR_FG));
	}
	/* replace toggle chevron to the left of the boxes */
	int th = app.search_show_replace ? PX(56) : PX(26);
	draw_icon(app.search_show_replace ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + PX(5), L.find_y + (th - PX(16)) / 2,
	          COL(C_SIDEBAR_FG));
	bool sf = app.focus == FOCUS_SEARCH_INPUT;
	input_draw(&app.search_in, L.ix, L.find_y, L.iw, PX(26), "Search", sf && app.search_box == 0);
	bool bad = app.search_m.err[0] != 0;
	if (bad)
		draw_frame(L.ix, L.find_y, L.iw, PX(26), COL(C_ERROR));
	int bx = L.ix + L.iw - PX(70);
	toggle_box(bx, L.find_y + PX(3), "Aa", app.search_flags & SEARCH_CASE, false);
	toggle_box(bx + PX(22), L.find_y + PX(3), "ab", app.search_flags & SEARCH_WORD, true);
	toggle_box(bx + PX(44), L.find_y + PX(3), ".*", app.search_flags & SEARCH_REGEX, false);
	if (app.search_show_replace) {
		input_draw(&app.search_repl_in, L.ix, L.repl_y, L.iw - PX(26), PX(26), "Replace", sf && app.search_box == 1);
		/* replace all button */
		int rbx = L.ix + L.iw - PX(22);
		bool on = app.results.n && app.search_repl_in.b;
		draw_text(FONT_SMALL_BOLD, rbx, L.repl_y + (PX(26) - font_m[FONT_SMALL].line) / 2, "AB", 2,
		          on ? COL(C_SIDEBAR_FG) : COL(C_FG_DIM));
	}
	/* "..." toggle for include / exclude */
	draw_text(FONT_UI_BOLD, x + w - PX(30), L.inc_y - (app.search_show_details ? PX(36) : 0) - PX(4) +
	          (app.search_show_details ? 0 : 0), "\xE2\x80\xA6", 3, COL(C_SIDEBAR_FG));
	if (app.search_show_details) {
		draw_text(FONT_SMALL, L.ix, L.inc_y - PX(16), "files to include", 16, COL(C_SIDEBAR_FG));
		input_draw(&app.search_inc_in, L.ix, L.inc_y, L.iw, PX(26), "e.g. *.c, src/**/include", sf && app.search_box == 2);
		draw_text(FONT_SMALL, L.ix, L.exc_y - PX(16), "files to exclude", 16, COL(C_SIDEBAR_FG));
		input_draw(&app.search_exc_in, L.ix, L.exc_y, L.iw, PX(26), "e.g. *.o, build/**", sf && app.search_box == 3);
	}
	const SearchResults *r = &app.results;
	char msg[200];
	u32 mc = COL(C_DESCRIPTION_FG);
	if (bad) {
		snprintf(msg, sizeof msg, "%s", app.search_m.err);
		mc = COL(C_ERROR);
	} else if (app.search_in.n && r->n)
		snprintf(msg, sizeof msg, "%u result%s in %u file%s%s", r->n, r->n == 1 ? "" : "s", r->files_hit,
		         r->files_hit == 1 ? "" : "s", r->truncated ? " - more results exist, refine your search" : "");
	else if (app.search_in.n && search_hash() == search_last_hash)
		snprintf(msg, sizeof msg, "No results found. Review your settings for configured exclusions.");
	else
		msg[0] = 0;
	draw_text_fit(FONT_UI, x + PX(20), L.msg_y + (PX(24) - font_m[FONT_UI].line) / 2, msg, strlen(msg), w - PX(28), mc);
	int cy = L.list_y;
	search_rows();
	int rh = app.row_h;
	int list_h = y + h - cy;
	int maxs = VX_MAX((int)srow_n * rh - list_h, 0);
	app.search_scroll = VX_CLAMP(app.search_scroll, 0, maxs);
	draw_clip(x, cy, w, list_h);
	bool focused = app.focus == FOCUS_SIDEBAR;
	bool replacing = app.search_show_replace && app.search_repl_in.b;
	for (u32 i = (u32)(app.search_scroll / rh); i < srow_n; i++) {
		int ry = cy + (int)i * rh - app.search_scroll;
		if (ry > y + h)
			break;
		bool hover = in_rect(app.hover_x, app.hover_y, x, ry, w, rh) && app.hover_y >= cy;
		list_row_bg(x, ry, w, rh, i == app.search_sel && focused, focused, hover);
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		u32 v = srow_res[i];
		int right = x + w - PX(12);
		if (hover) {
			/* actions: replace (when replacing), dismiss */
			draw_icon(ICON_CLOSE, right - PX(16), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			if (replacing)
				draw_text(FONT_SMALL_BOLD, right - PX(40), ty + PX(1), "AB", 2, COL(C_SIDEBAR_FG));
			right -= replacing ? PX(46) : PX(22);
		}
		if (v & 0x80000000u) {
			u32 f = v & 0x7FFFFFFFu;
			const char *p = app.files.pool.b + app.files.path[f];
			const char *base = p + app.files.base[f];
			bool col = app.search_collapsed && app.search_collapsed[f];
			draw_icon(col ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, x + PX(8), ry + (rh - PX(16)) / 2, COL(C_SIDEBAR_FG));
			int tx = file_icon_draw(base, x + PX(24), ry, rh);
			char letter;
			bool has;
			u32 gc = path_git_color(p, &letter, &has);
			tx = draw_text(FONT_UI, tx, ty, base, strlen(base), has ? gc : COL(C_SIDEBAR_FG));
			u32 cnt = 0;
			for (u32 k = 0; k < r->n; k++)
				cnt += r->file[k] == f;
			char bnum[16];
			snprintf(bnum, sizeof bnum, "%u", cnt);
			int bw = VX_MAX(PX(18), font_width(FONT_SMALL, bnum, strlen(bnum)) + PX(10));
			if (app.files.base[f] > 1)
				draw_text_fit(FONT_UI, tx + PX(6), ty, p, app.files.base[f] - 1, right - bw - tx - PX(12), COL(C_DESCRIPTION_FG));
			badge(right, ry, rh, bnum, COL(C_BADGE_BG), COL(C_BADGE_FG));
			continue;
		}
		const char *t = r->pool.b + r->text[v];
		u32 n = (u32)strlen(t);
		u32 mc0 = VX_MIN(r->text_col[v], n);
		u32 ml = VX_MIN(r->len[v], n - mc0);
		int tx = x + PX(40);
		int maxx = right - PX(4);
		tx = draw_text_fit(FONT_UI, tx, ty, t, mc0, maxx - tx, COL(C_SIDEBAR_FG));
		int ma = tx;
		tx = draw_text_fit(FONT_UI, tx, ty, t + mc0, ml, VX_MAX(maxx - tx, 0), COL(C_SIDEBAR_FG));
		if (replacing) {
			/* preview: old text struck out (removed), new text inserted */
			draw_rect(ma, ry + PX(2), tx - ma, rh - PX(4), COL(C_DIFF_REMOVED));
			draw_rect(ma, ty + font_m[FONT_UI].line / 2, tx - ma, PX(1), COL(C_SIDEBAR_FG));
			u32 rl;
			char *rp = result_replacement(v, &rl);
			int rx = tx;
			tx = draw_text_fit(FONT_UI, tx, ty, rp, rl, VX_MAX(maxx - tx, 0), COL(C_SIDEBAR_FG));
			draw_rect(rx, ry + PX(2), tx - rx, rh - PX(4), COL(C_DIFF_INSERTED));
			free(rp);
		} else {
			draw_rect(ma, ry + PX(2), tx - ma, rh - PX(4), COL(C_FIND_MATCH_OTHER));
		}
		if (tx < maxx)
			draw_text_fit(FONT_UI, tx, ty, t + mc0 + ml, n - mc0 - ml, maxx - tx, COL(C_SIDEBAR_FG));
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
		vw->head[0] = VX_MIN(vw->anchor[0] + r->len[v], app.docs.text[vw->doc].len);
	}
}

/* Drop results (dismiss): one result, or a whole file. */
static void search_dismiss(u32 file, u32 result)
{
	SearchResults *r = &app.results;
	u32 k = 0;
	for (u32 i = 0; i < r->n; i++) {
		bool drop = result != UINT32_MAX ? i == result : r->file[i] == file;
		if (drop)
			continue;
		r->file[k] = r->file[i];
		r->line[k] = r->line[i];
		r->col[k] = r->col[i];
		r->len[k] = r->len[i];
		r->text[k] = r->text[i];
		r->text_col[k] = r->text_col[i];
		k++;
	}
	r->n = k;
	app.dirty = true;
}

static int write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[8300];
	snprintf(tmp, sizeof tmp, "%s.vxide~XXXXXX", path);
	int fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	struct stat st;
	if (stat(path, &st) == 0)
		fchmod(fd, st.st_mode & 07777);
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, data + off, n - off);
		if (w <= 0) {
			close(fd);
			unlink(tmp);
			return -1;
		}
		off += (size_t)w;
	}
	if (close(fd) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* Replace in one file: every match, or only the one at (line, col). The
 * pattern is re-run on the current text so stale results cannot corrupt
 * the file. Open documents are edited (one undo step) and saved when they
 * were clean; other files are rewritten atomically. Returns replacements. */
static u32 replace_in_file(u32 f, u32 only_line, u32 only_col)
{
	char abs[8192];
	snprintf(abs, sizeof abs, "%s/%s", app.root, app.files.pool.b + app.files.path[f]);
	u32 d = doc_find(abs);
	size_t n = 0;
	char *own = NULL;
	const char *hay;
	if (d != UINT32_MAX) {
		hay = text_flat(&app.docs.text[d]);
		n = app.docs.text[d].len;
	} else {
		own = vx_read_file(abs, &n);
		if (!own)
			return 0;
		hay = own;
	}
	Matcher *m = &app.search_m;
	const char *repl = box_text(&app.search_repl_in);
	u32 cnt = 0, line = 0, ls = 0, scanned = 0;
	/* collect edits */
	u32 *ea = NULL, *eb = NULL, *eo = NULL, *el = NULL, ecap = 0;
	Pool reps = {0};
	u32 pos = 0, a, b;
	while (pos <= n && matcher_next(m, hay, (u32)n, pos, &a, &b)) {
		if (a == b) {
			pos = a + 1;
			continue;
		}
		for (u32 k = scanned; k < a; k++)
			if (hay[k] == '\n') {
				line++;
				ls = k + 1;
			}
		scanned = a;
		pos = b;
		if (only_line != UINT32_MAX && (line != only_line || a - ls != only_col))
			continue;
		char *out = NULL;
		u32 len = 0, cap = 0;
		matcher_expand(m, repl, app.search_repl_in.n, hay, &out, &len, &cap);
		VX_GROW(ea, ecap, cnt + 1);
		eb = vx_realloc(eb, ecap * sizeof(u32));
		eo = vx_realloc(eo, ecap * sizeof(u32));
		el = vx_realloc(el, ecap * sizeof(u32));
		ea[cnt] = a;
		eb[cnt] = b;
		eo[cnt] = pool_add(&reps, out ? out : "", len);
		el[cnt] = len;
		cnt++;
		free(out);
	}
	if (cnt && d != UINT32_MAX) {
		bool was_clean = !text_dirty(&app.docs.text[d]);
		u32 g = ed_new_group();
		for (u32 k = cnt; k-- > 0;)
			doc_replace(d, ea[k], eb[k] - ea[k], reps.b + eo[k], el[k], g);
		if (was_clean)
			doc_save(d);
	} else if (cnt) {
		/* build the new content */
		size_t nn = n;
		for (u32 k = 0; k < cnt; k++)
			nn = nn - (eb[k] - ea[k]) + el[k];
		char *buf = vx_alloc(nn + 1);
		size_t o = 0, prev = 0;
		for (u32 k = 0; k < cnt; k++) {
			memcpy(buf + o, hay + prev, ea[k] - prev);
			o += ea[k] - prev;
			memcpy(buf + o, reps.b + eo[k], el[k]);
			o += el[k];
			prev = eb[k];
		}
		memcpy(buf + o, hay + prev, n - prev);
		if (write_atomic(abs, buf, nn) != 0) {
			ui_status("Could not write %s", app.files.pool.b + app.files.path[f]);
			cnt = 0;
		}
		free(buf);
	}
	free(ea);
	free(eb);
	free(eo);
	free(el);
	pool_free(&reps);
	free(own);
	return cnt;
}

void search_replace(u32 file, u32 result)
{
	if (!app.search_m.nq || app.search_m.err[0])
		return;
	SearchResults *r = &app.results;
	u32 total = 0, files = 0;
	if (result != UINT32_MAX) {
		total = replace_in_file(r->file[result], r->line[result], r->col[result]);
		files = total ? 1 : 0;
	} else {
		u32 prev = UINT32_MAX;
		for (u32 i = 0; i < r->n; i++) {
			if (r->file[i] == prev || (file != UINT32_MAX && r->file[i] != file))
				continue;
			prev = r->file[i];
			u32 c = replace_in_file(r->file[i], UINT32_MAX, 0);
			total += c;
			files += c ? 1 : 0;
		}
	}
	ui_status("Replaced %u occurrence%s across %u file%s with '%s'", total, total == 1 ? "" : "s", files,
	          files == 1 ? "" : "s", box_text(&app.search_repl_in));
	app.git_ns = 0;
	search_run();
}

void search_step_result(int dir)
{
	search_rows();
	if (!srow_n)
		return;
	u32 i = app.search_sel;
	for (u32 k = 0; k < srow_n; k++) {
		i = dir > 0 ? (i + 1) % srow_n : (i + srow_n - 1) % srow_n;
		if (!(srow_res[i] & 0x80000000u))
			break;
	}
	app.search_sel = i;
	if (!(srow_res[i] & 0x80000000u))
		search_open_row(i, true);
}

bool search_event(const Event *e, int x, int y, int w, int h)
{
	SLayout L = search_layout(x, y, w);
	int cy = L.list_y;
	int rh = app.row_h;
	if (e->type == EV_WHEEL) {
		app.search_scroll -= (int)(e->wy * rh * 3);
		return true;
	}
	if (e->type == EV_MDOWN) {
		if (e->y < y + PX(35)) {
			if (e->x >= x + w - PX(80) && e->x < x + w - PX(56))
				search_run();
			else if (e->x >= x + w - PX(56) && e->x < x + w - PX(32)) {
				input_set(&app.search_in, "", 0);
				input_set(&app.search_repl_in, "", 0);
				search_run();
			} else if (e->x >= x + w - PX(32)) {
				for (u32 f = 0; f < app.files.n; f++)
					app.search_collapsed[f] = 1;
			}
			return true;
		}
		if (e->x < L.ix && e->y >= L.find_y && e->y < L.find_y + (app.search_show_replace ? PX(56) : PX(26))) {
			app.search_show_replace = !app.search_show_replace;
			return true;
		}
		struct {
			int y, box;
		} boxes[4] = {{L.find_y, 0}, {L.repl_y, 1}, {L.inc_y, 2}, {L.exc_y, 3}};
		for (int k = 0; k < 4; k++) {
			if (k == 1 && !app.search_show_replace)
				continue;
			if (k >= 2 && !app.search_show_details)
				continue;
			if (e->y < boxes[k].y || e->y >= boxes[k].y + PX(26) || e->x < L.ix)
				continue;
			if (k == 0) {
				int bx = L.ix + L.iw - PX(70);
				if (e->x >= bx && e->x < bx + PX(66)) {
					int which = (e->x - bx) / PX(22);
					app.search_flags ^= which == 0 ? SEARCH_CASE : which == 1 ? SEARCH_WORD : SEARCH_REGEX;
					search_run();
					app.focus = FOCUS_SEARCH_INPUT;
					return true;
				}
			}
			if (k == 1 && e->x >= L.ix + L.iw - PX(24)) {
				search_replace(UINT32_MAX, UINT32_MAX);
				return true;
			}
			app.search_box = k;
			app.focus = FOCUS_SEARCH_INPUT;
			input_click(search_box_input(k), FONT_UI, L.ix + PX(6), e->x);
			return true;
		}
		/* "..." */
		int dots_y = L.inc_y - (app.search_show_details ? PX(36) : 0) - PX(4);
		if (e->x >= x + w - PX(34) && e->y >= dots_y && e->y < dots_y + PX(18)) {
			app.search_show_details = !app.search_show_details;
			search_changed();
			return true;
		}
		if (e->y >= cy) {
			search_rows();
			u32 i = (u32)((e->y - cy + app.search_scroll) / rh);
			if (i < srow_n) {
				app.search_sel = i;
				app.focus = FOCUS_SIDEBAR;
				u32 v = srow_res[i];
				int right = x + w - PX(12);
				bool replacing = app.search_show_replace && app.search_repl_in.b;
				if (e->x >= right - PX(18)) {
					search_dismiss(v & 0x80000000u ? v & 0x7FFFFFFFu : UINT32_MAX,
					               v & 0x80000000u ? UINT32_MAX : v);
					return true;
				}
				if (replacing && e->x >= right - PX(42)) {
					if (v & 0x80000000u)
						search_replace(v & 0x7FFFFFFFu, UINT32_MAX);
					else
						search_replace(UINT32_MAX, v);
					return true;
				}
				search_open_row(i, e->clicks < 2);
				if (!(v & 0x80000000u))
					app.focus = e->clicks >= 2 ? FOCUS_EDITOR : FOCUS_SIDEBAR;
			}
		}
		return true;
	}
	if (app.focus == FOCUS_SEARCH_INPUT) {
		Input *in = search_box_input(app.search_box);
		if (e->type == EV_TEXT) {
			input_text(in, e->text, strlen(e->text));
			if (app.search_box != 1)
				search_changed();
			app.dirty = true;
			return true;
		}
		if (e->type == EV_KEY) {
			int mods = e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
			if ((e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) && mods == (MOD_CTRL | MOD_ALT)) {
				search_replace(UINT32_MAX, UINT32_MAX);
				return true;
			}
			if (e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) {
				search_run();
				return true;
			}
			if (e->key == SDLK_TAB) {
				/* cycle the visible boxes */
				int order[4], no = 0;
				order[no++] = 0;
				if (app.search_show_replace)
					order[no++] = 1;
				if (app.search_show_details) {
					order[no++] = 2;
					order[no++] = 3;
				}
				int at = 0;
				for (int k = 0; k < no; k++)
					if (order[k] == app.search_box)
						at = k;
				app.search_box = order[(at + ((e->mods & MOD_SHIFT) ? no - 1 : 1)) % no];
				return true;
			}
			if (e->key == SDLK_DOWN && app.search_box == 0 && !app.search_show_replace) {
				app.focus = FOCUS_SIDEBAR;
				return true;
			}
			int toggle = mods == MOD_ALT ? (e->key == SDLK_C ? SEARCH_CASE : e->key == SDLK_W ? SEARCH_WORD
			                                : e->key == SDLK_R ? SEARCH_REGEX : 0)
			                             : 0;
			if (toggle) {
				app.search_flags ^= toggle;
				search_run();
				return true;
			}
			if (e->key == SDLK_ESCAPE) {
				app.focus = FOCUS_EDITOR;
				return true;
			}
			bool r = input_key(in, e);
			if (r && app.search_box != 1)
				search_changed();
			app.dirty = true;
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
		case SDLK_DELETE:
			if (app.search_sel < srow_n) {
				u32 v = srow_res[app.search_sel];
				search_dismiss(v & 0x80000000u ? v & 0x7FFFFFFFu : UINT32_MAX, v & 0x80000000u ? UINT32_MAX : v);
			}
			break;
		case SDLK_1:
			if ((e->mods & (MOD_CTRL | MOD_SHIFT)) == (MOD_CTRL | MOD_SHIFT) && app.search_sel < srow_n &&
			    app.search_show_replace) {
				u32 v = srow_res[app.search_sel];
				if (v & 0x80000000u)
					search_replace(v & 0x7FFFFFFFu, UINT32_MAX);
				else
					search_replace(UINT32_MAX, v);
				break;
			}
			return false;
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
		app.blame_gen++;
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

extern int gitui_graph_height(int h);
extern void gitui_graph_draw(int x, int y, int w, int h);
extern bool gitui_graph_event(const Event *e, int x, int y, int w, int h);
extern void ctx_menu_scm_title(int x, int y);
extern void gitui_status_line(int x, int y, int w);

void scm_draw(int x, int y, int w, int h)
{
	int cy = view_title(x, y, w, "SOURCE CONTROL");
	if (app.git) {
		/* "..." opens the git actions menu, like VSCodium's view title */
		draw_text(FONT_UI_BOLD, x + w - PX(28), y + (PX(35) - font_m[FONT_UI_BOLD].line) / 2 - PX(3),
		          "\xE2\x80\xA6", 3, COL(C_SIDEBAR_FG));
		draw_icon(ICON_REFRESH, x + w - PX(54), y + (PX(35) - PX(16)) / 2, COL(C_SIDEBAR_FG));
		draw_icon(ICON_CHECK, x + w - PX(78), y + (PX(35) - PX(16)) / 2, COL(C_SIDEBAR_FG));
		/* the graph section takes the lower part */
		int gh = gitui_graph_height(h);
		gitui_graph_draw(x, y + h - gh, w, gh);
		h -= gh;
		draw_clip(x, y, w, h);
	}
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
	gitui_status_line(x, cy - PX(6), w);
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
	{
		int gh = gitui_graph_height(h);
		bool in_graph = (e->type == EV_MDOWN || e->type == EV_WHEEL || e->type == EV_MMOVE) && e->y >= y + h - gh;
		if (in_graph)
			return gitui_graph_event(e, x, y + h - gh, w, gh);
		if (e->type == EV_KEY && app.focus == FOCUS_SIDEBAR && gitui_graph_event(e, x, y + h - gh, w, gh))
			return true;
		h -= gh;
	}
	if (e->type == EV_MDOWN && e->y < y + PX(35)) {
		if (e->x >= x + w - PX(32))
			ctx_menu_scm_title(e->x, y + PX(30));
		else if (e->x >= x + w - PX(58))
			run_command(CMD_GIT_REFRESH);
		else if (e->x >= x + w - PX(82))
			run_command(CMD_GIT_COMMIT);
		return true;
	}
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

/* Right click in the Source Control view. */
bool scm_context(int x, int y, int w, int h, int mx, int my)
{
	if (!app.git)
		return false;
	int gh = gitui_graph_height(h);
	if (my >= y + h - gh) {
		extern bool gitui_graph_context(int x, int y, int w, int h, int mx, int my);
		return gitui_graph_context(x, y + h - gh, w, gh, mx, my);
	}
	if (my < y + PX(35)) {
		ctx_menu_scm_title(mx, my);
		return true;
	}
	int cy = scm_list_y(y);
	if (my < cy)
		return false;
	scm_rows();
	u32 i = (u32)((my - cy + app.scm_scroll) / app.row_h);
	if (i >= scm_n || (scm_item[i] & 0x80000000u))
		return false;
	app.scm_sel = i;
	const GitStatus *st = git_status(app.git);
	u32 si = scm_item[i] & 0x3FFFFFFFu;
	ctx_menu_scm_file(mx, my, st->pool.b + st->path[si], (scm_item[i] & 0x40000000u) != 0);
	return true;
}

/* I-beam over the side bar's text boxes. */
bool sidebar_over_input(int x, int y, int w, int mx, int my)
{
	if (app.side_view == SIDE_SEARCH) {
		SLayout L = search_layout(x, y, w);
		int ys[4] = {L.find_y, app.search_show_replace ? L.repl_y : -1000, app.search_show_details ? L.inc_y : -1000,
		             app.search_show_details ? L.exc_y : -1000};
		for (int k = 0; k < 4; k++)
			if (my >= ys[k] && my < ys[k] + PX(26) && mx >= L.ix && mx < L.ix + L.iw - (k == 0 ? PX(70) : 0))
				return true;
		return false;
	}
	if (app.side_view == SIDE_SCM && app.git) {
		int iy = y + PX(35) + PX(8);
		return my >= iy && my < iy + PX(26) && mx >= x + PX(12) && mx < x + w - PX(12);
	}
	return false;
}
