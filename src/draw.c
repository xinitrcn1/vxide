#include "draw.h"
#include "font.h"

DrawList draw;

void draw_begin(int w, int h)
{
	draw.q = gpu_frame_quads(&draw.cap); /* straight into the GPU's buffer */
	draw.n = 0;
	draw.w = w;
	draw.h = h;
	draw_noclip();
}

void draw_noclip(void)
{
	draw.x0 = draw.y0 = 0;
	draw.x1 = draw.w;
	draw.y1 = draw.h;
}

void draw_clip(int x, int y, int w, int h)
{
	draw.x0 = VX_MAX(x, 0);
	draw.y0 = VX_MAX(y, 0);
	draw.x1 = VX_MIN(x + w, draw.w);
	draw.y1 = VX_MIN(y + h, draw.h);
}

static void push(int x, int y, int w, int h, u16 u, u16 v, u32 c)
{
	/* Clip on the CPU: the atlas origin moves with the cut edge. */
	int x1 = x + w, y1 = y + h;
	int cx0 = VX_MAX(x, draw.x0), cy0 = VX_MAX(y, draw.y0);
	int cx1 = VX_MIN(x1, draw.x1), cy1 = VX_MIN(y1, draw.y1);
	if (cx0 >= cx1 || cy0 >= cy1 || !(c & 0xFF))
		return;
	if (u < QUAD_PIC) {
		u = (u16)(u + (cx0 - x));
		v = (u16)(v + (cy0 - y));
	} else if (u == QUAD_SQUIGGLE && (cx0 != x || cy0 != y)) {
		return; /* squiggles are cut whole */
	}
	if (draw.n == draw.cap)
		return; /* the frame is full (262144 quads): nothing more fits on screen anyway */
	Quad *q = &draw.q[draw.n++];
	q->x = (i16)cx0;
	q->y = (i16)cy0;
	q->w = (i16)(cx1 - cx0);
	q->h = (i16)(cy1 - cy0);
	q->u = u;
	q->v = v;
	q->rgba = c;
}

void draw_rect(int x, int y, int w, int h, u32 c) { push(x, y, w, h, QUAD_SOLID, 0, c); }
void push_image_quad(int x, int y, int w, int h, u32 alpha) { push(x, y, w, h, QUAD_IMAGE, 0, 0xFFFFFF00u | (alpha & 255)); }

void draw_picture(int x, int y, int w, int h, int u, int v, int iw, int ih, u32 alpha)
{
	if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0)
		return;
	/* off screen or clipped away: no placement spent */
	if (x >= draw.x1 || y >= draw.y1 || x + w <= draw.x0 || y + h <= draw.y0)
		return;
	u16 q = gpu_pic_place((float)x, (float)y, (float)iw / (float)w, u, v, iw, ih);
	if (q)
		push(x, y, w, h, q, 0, 0xFFFFFF00u | (alpha & 255));
}

void draw_atlas_rect(int x, int y, int w, int h, u16 u, u16 v, u32 c) { push(x, y, w, h, u, v, c); }

void draw_frame(int x, int y, int w, int h, u32 c)
{
	draw_rect(x, y, w, 1, c);
	draw_rect(x, y + h - 1, w, 1, c);
	draw_rect(x, y + 1, 1, h - 2, c);
	draw_rect(x + w - 1, y + 1, 1, h - 2, c);
}

void draw_squiggle(int x, int y, int w, u32 c)
{
	if (y < draw.y0 || y + 4 > draw.y1)
		return;
	int x0 = VX_MAX(x, draw.x0), x1 = VX_MIN(x + w, draw.x1);
	if (x0 < x1)
		push(x0, y, x1 - x0, 4, QUAD_SQUIGGLE, 0, c);
}

void draw_glyph(u32 g, int x, int y, u32 c)
{
	const GlyphTable *t = &font_glyphs;
	if (t->w[g])
		push(x + t->bx[g], y + t->by[g], t->w[g], t->h[g], t->u[g], t->v[g], c);
}

int draw_text(int font, int x, int y, const char *s, size_t n, u32 c)
{
	size_t i = 0;
	if (y >= draw.y1 || y + font_m[font].line <= draw.y0)
		return x + font_width(font, s, n);
	while (i < n) {
		u32 cp;
		u8 b = (u8)s[i];
		if (b < 0x80) {
			cp = b;
			i++;
		} else {
			i += utf8_decode(s + i, n - i, &cp);
		}
		u32 g = font_glyph(font, cp);
		if (cp > ' ' && x < draw.x1)
			draw_glyph(g, x, y, c);
		x += font_glyphs.adv[g];
	}
	return x;
}

int draw_text_fit(int font, int x, int y, const char *s, size_t n, int max_w, u32 c)
{
	int w = font_width(font, s, n);
	if (w <= max_w)
		return draw_text(font, x, y, s, n, c);
	int ell = font_width(font, "\xE2\x80\xA6", 3);
	int pen = 0;
	size_t i = 0;
	while (i < n) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		int a = font_glyphs.adv[font_glyph(font, cp)];
		if (pen + a + ell > max_w)
			break;
		pen += a;
		i += k;
	}
	x = draw_text(font, x, y, s, i, c);
	return draw_text(font, x, y, "\xE2\x80\xA6", 3, c);
}
