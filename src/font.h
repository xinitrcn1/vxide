/* Glyph cache: FreeType rasterizes on demand into the R8 atlas that the
 * GPU samples. Glyph metrics are SoA tables indexed by glyph id. */
#ifndef VX_FONT_H
#define VX_FONT_H

#include "base.h"

enum { FONT_UI, FONT_UI_BOLD, FONT_SMALL, FONT_SMALL_BOLD, FONT_MONO, FONT_MONO_BOLD, FONT_COUNT };

typedef struct {
	u32 count;
	u16 u[8192], v[8192]; /* atlas origin            */
	i16 w[8192], h[8192]; /* bitmap size              */
	i16 bx[8192], by[8192]; /* left bearing, top offset from the line top */
	i16 adv[8192];        /* advance, whole pixels    */
} GlyphTable;

extern GlyphTable font_glyphs;
extern u8 *font_atlas; /* ATLAS_SIZE^2 */

typedef struct {
	int size;       /* pixel size                    */
	int line;       /* line height                   */
	int ascent;     /* baseline offset from the line top (with centring) */
	int adv;        /* advance of '0' (monospace cell width) */
} FontMetrics;
extern FontMetrics font_m[FONT_COUNT];

bool font_init(int ui_px, int mono_px, int mono_line, int small_px);
void font_shutdown(void);
/* Glyph id for a code point (rasterized on first use). */
u32 font_glyph(int font, u32 cp);
/* Width of UTF-8 text in pixels. */
int font_width(int font, const char *s, size_t n);
/* Reserve an atlas region (for icons); returns false when full. */
bool font_atlas_alloc(int w, int h, u16 *u, u16 *v);
/* True once after the atlas was wiped because it filled up. */
bool font_atlas_was_reset(void);

#endif
