/* Modal dialog: a question with up to three buttons, VSCodium's
 * "Do you want to save the changes you made to x?" being the main user.
 *
 * It takes every event while open (the rest of the window is dimmed, so it
 * reads as modal). Keys: Enter = the focused button (the primary one at
 * first), Escape = the last (Cancel), Tab / arrows move the focus, and each
 * button's underlined letter (S, N, C) picks it directly. */
#include "ui.h"

#include <stdio.h>
#include <SDL3/SDL.h>

static struct {
	bool open;
	int kind;
	u32 arg;
	char title[256];
	char detail[1024];
	const char *btn[3];
	int mnemonic[3]; /* index of the underlined letter */
	int nb, focus;
	int bx[3], by, bw[3], bh; /* button rects from the last draw */
} D;

void dialog_open(int kind, u32 arg, const char *title, const char *detail)
{
	memset(&D, 0, sizeof D);
	D.open = true;
	D.kind = kind;
	D.arg = arg;
	snprintf(D.title, sizeof D.title, "%s", title);
	snprintf(D.detail, sizeof D.detail, "%s", detail ? detail : "");
	D.btn[0] = "Save";
	D.btn[1] = "Don't Save";
	D.btn[2] = "Cancel";
	D.mnemonic[0] = 0; /* S */
	D.mnemonic[1] = 2; /* N */
	D.mnemonic[2] = 0; /* C */
	D.nb = 3;
	D.focus = 0;
	app.dirty = true;
}

bool dialog_active(void) { return D.open; }

static void finish(int choice)
{
	D.open = false;
	app.dirty = true;
	dialog_result(D.kind, D.arg, choice);
}

void dialog_draw(void)
{
	if (!D.open)
		return;
	/* dim everything behind */
	draw_rect(0, 0, app.w, app.h, 0x00000066u);
	int w = VX_MIN(PX(460), app.w - PX(40));
	int pad = PX(20);
	/* detail lines (\n separated) */
	int lines = 1;
	for (const char *p = D.detail; *p; p++)
		lines += *p == '\n';
	int lh = font_m[FONT_UI].line + PX(2);
	D.bh = PX(28);
	int h = pad + font_m[FONT_UI_BOLD].line + PX(10) + (D.detail[0] ? lines * lh + PX(10) : 0) + PX(10) + D.bh + pad;
	int x = (app.w - w) / 2, y = VX_MAX((app.h - h) / 3, PX(40));
	draw_rect(x + PX(4), y + PX(4), w, h, COL(C_WIDGET_SHADOW)); /* square drop shadow */
	draw_rect(x, y, w, h, COL(C_WIDGET_BG));
	draw_frame(x, y, w, h, COL(C_WIDGET_BORDER));
	int cy = y + pad;
	draw_text_fit(FONT_UI_BOLD, x + pad, cy, D.title, strlen(D.title), w - 2 * pad, COL(C_FG));
	cy += font_m[FONT_UI_BOLD].line + PX(10);
	if (D.detail[0]) {
		const char *p = D.detail;
		for (int i = 0; i < lines; i++) {
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			draw_text_fit(FONT_UI, x + pad, cy, p, n, w - 2 * pad, COL(C_DESCRIPTION_FG));
			cy += lh;
			p = e ? e + 1 : p + n;
		}
		cy += PX(10);
	}
	cy += PX(10);
	/* buttons, right-aligned, primary first (VSCodium's order on Linux) */
	int bx = x + w - pad;
	for (int i = D.nb; i-- > 0;) {
		int bw = font_width(FONT_UI, D.btn[i], strlen(D.btn[i])) + PX(28);
		bx -= bw;
		D.bx[i] = bx;
		D.bw[i] = bw;
		bx -= PX(8);
	}
	D.by = cy;
	for (int i = 0; i < D.nb; i++) {
		bool hover = app.hover_x >= D.bx[i] && app.hover_x < D.bx[i] + D.bw[i] && app.hover_y >= D.by &&
		             app.hover_y < D.by + D.bh;
		bool primary = i == 0;
		u32 bg = primary ? (hover ? COL(C_BUTTON_HOVER_BG) : COL(C_BUTTON_BG))
		                 : (hover ? COL(C_LIST_HOVER_BG) : COL(C_WIDGET_BG));
		u32 fg = primary ? COL(C_BUTTON_FG) : COL(C_FG);
		draw_rect(D.bx[i], D.by, D.bw[i], D.bh, bg);
		if (!primary)
			draw_frame(D.bx[i], D.by, D.bw[i], D.bh, COL(C_WIDGET_BORDER));
		if (i == D.focus)
			draw_frame(D.bx[i] - PX(2), D.by - PX(2), D.bw[i] + PX(4), D.bh + PX(4), COL(C_FOCUS_BORDER));
		int tw = font_width(FONT_UI, D.btn[i], strlen(D.btn[i]));
		int tx = D.bx[i] + (D.bw[i] - tw) / 2, ty = D.by + (D.bh - font_m[FONT_UI].line) / 2;
		draw_text(FONT_UI, tx, ty, D.btn[i], strlen(D.btn[i]), fg);
		/* the mnemonic, underlined */
		int m = D.mnemonic[i];
		int ux = tx + font_width(FONT_UI, D.btn[i], (size_t)m);
		int uw = font_width(FONT_UI, D.btn[i] + m, 1);
		draw_rect(ux, ty + font_m[FONT_UI].ascent + PX(2), uw, PX(1), fg);
	}
}

void dialog_event(const Event *e)
{
	if (!D.open)
		return;
	app.dirty = true;
	if (e->type == EV_KEY) {
		switch (e->key) {
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
		case SDLK_SPACE:
			finish(D.focus);
			return;
		case SDLK_ESCAPE:
			finish(D.nb - 1);
			return;
		case SDLK_TAB:
			D.focus = (e->mods & MOD_SHIFT) ? (D.focus + D.nb - 1) % D.nb : (D.focus + 1) % D.nb;
			return;
		case SDLK_LEFT:
			D.focus = (D.focus + D.nb - 1) % D.nb;
			return;
		case SDLK_RIGHT:
			D.focus = (D.focus + 1) % D.nb;
			return;
		default:
			break;
		}
		/* mnemonics, with or without Alt */
		for (int i = 0; i < D.nb; i++) {
			char c = D.btn[i][D.mnemonic[i]];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c + 32);
			if ((int)e->key == c) {
				finish(i);
				return;
			}
		}
		return;
	}
	if (e->type == EV_MDOWN && e->button == 1) {
		for (int i = 0; i < D.nb; i++)
			if (e->x >= D.bx[i] && e->x < D.bx[i] + D.bw[i] && e->y >= D.by && e->y < D.by + D.bh) {
				finish(i);
				return;
			}
	}
	/* other events (text, moves, wheel, clicks outside) are swallowed */
}
