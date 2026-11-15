/* Immediate-mode quad list with a rectangular clip. Everything is square:
 * fills, 1px borders, glyphs, icons, squiggles. */
#ifndef VX_DRAW_H
#define VX_DRAW_H

#include "gpu.h"

enum {
	/* 24 px activity bar icons */
	ICON_FILES,
	ICON_SEARCH,
	ICON_SCM,
	ICON_GEAR,
	/* 16 px versions for the top-level tabs */
	ICON_FILES_S,
	ICON_SEARCH_S,
	ICON_SCM_S,
	ICON_GEAR_S,
	ICON_BOOK_S, /* Manpages */
	ICON_PULSE_S, /* Profiling */
	ICON_PLAY_S,  /* Workflow */
	ICON_SIGMA_S, /* ucas */
	ICON_CHAT_S,  /* LLM agent */
	/* 16 px */
	ICON_CHEVRON_RIGHT,
	ICON_CHEVRON_DOWN,
	ICON_CLOSE,
	ICON_DOT,
	ICON_ERROR,
	ICON_WARNING,
	ICON_INFO,
	ICON_CHECK,
	ICON_PLUS,
	ICON_MINUS,
	ICON_DISCARD,
	ICON_REFRESH,
	ICON_BRANCH,
	ICON_COLLAPSE,
	ICON_NEW_FILE,
	ICON_NEW_FOLDER,
	ICON_ARROW_UP,
	ICON_ARROW_DOWN,
	ICON_SPLIT,
	/* debugger toolbar, 16 px */
	ICON_DBG_CONTINUE,
	ICON_DBG_PAUSE,
	ICON_DBG_STEP_OVER,
	ICON_DBG_STEP_INTO,
	ICON_DBG_STEP_OUT,
	ICON_DBG_STOP,
	ICON_DBG_RESTART,
	ICON_COUNT
};

typedef struct {
	Quad *q;
	u32 n, cap;
	int x0, y0, x1, y1; /* clip */
	int w, h;
} DrawList;

extern DrawList draw;

void icons_init(void);
int icon_size(int icon);

void draw_begin(int w, int h);
void draw_clip(int x, int y, int w, int h); /* intersect with screen */
void draw_noclip(void);
void draw_rect(int x, int y, int w, int h, u32 rgba);
/* the background image (gpu_set_background) at this alpha, over x,y,w,h */
void push_image_quad(int x, int y, int w, int h, u32 alpha);
/* Picture atlas rect (u, v, iw, ih) scaled into x, y, w, h (keep the aspect:
 * one scale, iw / w); see gpu_pic_pixels. */
void draw_picture(int x, int y, int w, int h, int u, int v, int iw, int ih, u32 alpha);
void draw_frame(int x, int y, int w, int h, u32 rgba); /* 1px outline */
void draw_squiggle(int x, int y, int w, u32 rgba);
void draw_icon(int icon, int x, int y, u32 rgba);
/* Draw UTF-8 text with its line top at y. Returns the pen x after it.
 * Tabs advance to the next multiple of tab_px from x0 when tab_px > 0. */
int draw_text(int font, int x, int y, const char *s, size_t n, u32 rgba);
/* Text cut with an ellipsis to fit max_w. */
int draw_text_fit(int font, int x, int y, const char *s, size_t n, int max_w, u32 rgba);
void draw_glyph(u32 gid, int x, int y, u32 rgba);
void draw_atlas_rect(int x, int y, int w, int h, u16 u, u16 v, u32 rgba);

#endif
