#include "menu.h"
#include "ui.h"

#include <SDL3/SDL.h>

#define MAX_ITEMS 64

typedef struct {
	u32 n;
	u32 label[MAX_ITEMS]; /* pool offsets */
	u16 cmd[MAX_ITEMS];
	u8 flags[MAX_ITEMS];
	u8 sub[MAX_ITEMS];
	Pool pool;
	int x, y, w, h;
	int sel;
	int parent_item; /* item in level 0 that opened this level */
} Level;

static Level lv[2];
static int depth;  /* open levels: 0 none, 1 menu, 2 menu + submenu */
static int filling; /* level being filled */

static int item_h(u8 flags) { return (flags & MI_SEP) ? PX(9) : PX(26); }

static void layout(Level *L, int x, int y)
{
	int w = PX(200), h = PX(8);
	for (u32 i = 0; i < L->n; i++) {
		h += item_h(L->flags[i]);
		if (L->flags[i] & MI_SEP)
			continue;
		const char *s = L->pool.b + L->label[i];
		int tw = font_width(FONT_UI, s, strlen(s)) + PX(64);
		const char *k = (L->flags[i] & MI_SUB) ? NULL : command_keys(L->cmd[i]);
		if (k)
			tw += font_width(FONT_UI, k, strlen(k)) + PX(30);
		w = VX_MAX(w, tw);
	}
	L->w = w;
	L->h = h;
	/* keep on screen */
	L->x = VX_MIN(x, app.w - w - PX(2));
	L->y = VX_MIN(y, app.h - h - PX(2));
	if (L->x < 0)
		L->x = 0;
	if (L->y < 0)
		L->y = 0;
}

static void level_reset(Level *L)
{
	L->n = 0;
	pool_clear(&L->pool);
	L->sel = -1;
}

void menu_open(int x, int y)
{
	level_reset(&lv[0]);
	level_reset(&lv[1]);
	depth = 1;
	filling = 0;
	lv[0].x = x;
	lv[0].y = y;
	app.compl_open = app.hover_open = false;
	app.dirty = true;
}

static void add(const char *label, int cmd, u8 flags, int sub)
{
	Level *L = &lv[filling];
	if (L->n >= MAX_ITEMS)
		return;
	u32 i = L->n++;
	L->label[i] = pool_add(&L->pool, label ? label : "", label ? (u32)strlen(label) : 0);
	L->cmd[i] = (u16)cmd;
	L->flags[i] = flags;
	L->sub[i] = (u8)sub;
	layout(L, L->x, L->y);
}

void menu_item(const char *label, int cmd, bool enabled) { add(label, cmd, enabled ? 0 : MI_DISABLED, 0); }

void menu_sep(void)
{
	Level *L = &lv[filling];
	if (L->n && !(L->flags[L->n - 1] & MI_SEP))
		add(NULL, 0, MI_SEP, 0);
}

void menu_sub(const char *label, int sub) { add(label, 0, MI_SUB, sub); }

bool menu_is_open(void) { return depth > 0; }

void menu_close(void)
{
	depth = 0;
	app.dirty = true;
}

static int item_at(Level *L, int mx, int my)
{
	if (mx < L->x || mx >= L->x + L->w || my < L->y || my >= L->y + L->h)
		return -2; /* outside */
	int y = L->y + PX(4);
	for (u32 i = 0; i < L->n; i++) {
		int h = item_h(L->flags[i]);
		if (my >= y && my < y + h)
			return (L->flags[i] & (MI_SEP | MI_DISABLED)) ? -1 : (int)i;
		y += h;
	}
	return -1;
}

static int item_y(Level *L, int idx)
{
	int y = L->y + PX(4);
	for (int i = 0; i < idx; i++)
		y += item_h(L->flags[i]);
	return y;
}

static void open_sub(int idx)
{
	Level *P = &lv[0];
	if (depth == 2 && lv[1].parent_item == idx)
		return;
	level_reset(&lv[1]);
	lv[1].parent_item = idx;
	lv[1].x = P->x + P->w - PX(2);
	lv[1].y = item_y(P, idx) - PX(4);
	filling = 1;
	menu_fill_sub(P->sub[idx]);
	filling = 0;
	/* flip to the left when there is no room on the right */
	if (lv[1].x + lv[1].w > app.w)
		lv[1].x = VX_MAX(P->x - lv[1].w + PX(2), 0);
	layout(&lv[1], lv[1].x, lv[1].y);
	depth = 2;
}

static void activate(int level, int idx)
{
	Level *L = &lv[level];
	if (idx < 0 || (u32)idx >= L->n)
		return;
	if (L->flags[idx] & MI_SUB) {
		open_sub(idx);
		lv[1].sel = 0;
		return;
	}
	int cmd = L->cmd[idx];
	menu_close();
	run_command(cmd);
}

static void step(Level *L, int dir)
{
	if (!L->n)
		return;
	int i = L->sel;
	for (u32 k = 0; k < L->n; k++) {
		i = (i + dir + (int)L->n) % (int)L->n;
		if (!(L->flags[i] & (MI_SEP | MI_DISABLED))) {
			L->sel = i;
			return;
		}
	}
}

bool menu_event(const Event *e)
{
	if (!depth)
		return false;
	app.dirty = true;
	if (e->type == EV_KEY) {
		Level *L = &lv[depth - 1];
		switch (e->key) {
		case SDLK_ESCAPE:
			if (depth == 2)
				depth = 1;
			else
				menu_close();
			return true;
		case SDLK_DOWN:
			step(L, 1);
			return true;
		case SDLK_UP:
			step(L, -1);
			return true;
		case SDLK_RIGHT:
			if (depth == 1 && L->sel >= 0 && (L->flags[L->sel] & MI_SUB)) {
				open_sub(L->sel);
				step(&lv[1], 1);
			}
			return true;
		case SDLK_LEFT:
			if (depth == 2)
				depth = 1;
			return true;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
		case SDLK_SPACE:
			activate(depth - 1, L->sel);
			return true;
		default:
			return true; /* the menu is modal for keys */
		}
	}
	if (e->type == EV_TEXT)
		return true;
	if (e->type == EV_MMOVE) {
		for (int l = depth - 1; l >= 0; l--) {
			int i = item_at(&lv[l], e->x, e->y);
			if (i == -2)
				continue;
			lv[l].sel = i;
			if (l == 0 && i >= 0 && (lv[0].flags[i] & MI_SUB))
				open_sub(i);
			else if (l == 0 && i >= 0 && depth == 2)
				depth = 1;
			return true;
		}
		return true;
	}
	if (e->type == EV_MDOWN || e->type == EV_MUP) {
		for (int l = depth - 1; l >= 0; l--) {
			int i = item_at(&lv[l], e->x, e->y);
			if (i == -2)
				continue;
			/* VSCodium activates on mouse up, so a right-press-drag-release works */
			if (e->type == EV_MUP && i >= 0)
				activate(l, i);
			else if (e->type == EV_MDOWN && i >= 0 && (lv[l].flags[i] & MI_SUB))
				open_sub(i);
			return true;
		}
		if (e->type == EV_MDOWN)
			menu_close();
		return e->type == EV_MDOWN;
	}
	if (e->type == EV_WHEEL)
		return true;
	return false;
}

static void draw_level(Level *L)
{
	draw_rect(L->x - PX(1), L->y + PX(1), L->w + PX(3), L->h + PX(3), COL(C_WIDGET_SHADOW));
	draw_rect(L->x, L->y, L->w, L->h, COL(C_MENU_BG));
	draw_frame(L->x, L->y, L->w, L->h, COL(C_MENU_BORDER));
	int y = L->y + PX(4);
	for (u32 i = 0; i < L->n; i++) {
		int h = item_h(L->flags[i]);
		if (L->flags[i] & MI_SEP) {
			draw_rect(L->x + PX(8), y + h / 2, L->w - PX(16), PX(1), COL(C_MENU_SEP));
			y += h;
			continue;
		}
		bool sel = (int)i == L->sel;
		bool dis = L->flags[i] & MI_DISABLED;
		u32 fg = dis ? (COL(C_MENU_FG) & 0xFFFFFF00u) | 0x66u : sel ? COL(C_MENU_SEL_FG) : COL(C_MENU_FG);
		if (sel)
			draw_rect(L->x + PX(4), y, L->w - PX(8), h, COL(C_MENU_SEL_BG));
		int ty = y + (h - font_m[FONT_UI].line) / 2;
		const char *s = L->pool.b + L->label[i];
		draw_text(FONT_UI, L->x + PX(26), ty, s, strlen(s), fg);
		if (L->flags[i] & MI_SUB) {
			draw_icon(ICON_CHEVRON_RIGHT, L->x + L->w - PX(24), y + (h - PX(16)) / 2, fg);
		} else {
			const char *k = command_keys(L->cmd[i]);
			if (k) {
				int kw = font_width(FONT_UI, k, strlen(k));
				draw_text(FONT_UI, L->x + L->w - PX(26) - kw, ty, k, strlen(k), fg);
			}
		}
		y += h;
	}
}

void menu_draw(void)
{
	if (!depth)
		return;
	draw_noclip();
	for (int l = 0; l < depth; l++)
		draw_level(&lv[l]);
}
