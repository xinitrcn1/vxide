#define _POSIX_C_SOURCE 200809L
#include "font.h"
#include "gpu.h"

#include <math.h>
#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H

GlyphTable font_glyphs;
u8 *font_atlas;
FontMetrics font_m[FONT_COUNT];

#define HASH_SIZE 16384u
#define MAX_FACES 4

static FT_Library ft;
static FT_Face faces[FONT_COUNT][MAX_FACES]; /* primary + fallbacks */
static u32 hash_key[HASH_SIZE];              /* (font << 21 | cp) + 1, 0 = empty */
static u16 hash_val[HASH_SIZE];
static u16 ascii[FONT_COUNT][128];           /* glyph id + 1 */
static u8 gamma_lut[256];
static int shelf_x, shelf_y, shelf_h;
static bool was_reset;
/* Icons keep their regions across resets: they live below this row. */
static int reserved_y;

static const char *font_files[FONT_COUNT][MAX_FACES] = {
    {"NotoSans-Regular.ttf", "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"NotoSans-Regular.ttf", "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"DejaVuSansMono.ttf", "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"DejaVuSansMono-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSansSymbols2-Regular.ttf", NULL},
    {"DejaVuSansMono-Oblique.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSansMono.ttf", NULL},
    {"DejaVuSansMono-BoldOblique.ttf", "DejaVuSans-BoldOblique.ttf", "DejaVuSansMono-Bold.ttf", NULL},
};

static const char *font_dirs[] = {
    "/usr/local/share/fonts/noto/", "/usr/local/share/fonts/dejavu/",
    "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/truetype/dejavu/",
    "/usr/share/fonts/noto/", "/usr/share/fonts/dejavu/", "/usr/share/fonts/TTF/",
};

static FT_Face open_face(const char *file, int px)
{
	char path[512];
	for (size_t i = 0; i < VX_COUNT(font_dirs); i++) {
		snprintf(path, sizeof path, "%s%s", font_dirs[i], file);
		FT_Face f;
		if (FT_New_Face(ft, path, 0, &f) == 0) {
			FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px);
			return f;
		}
	}
	return NULL;
}

static void metrics(int font, int px, int line)
{
	FT_Face f = faces[font][0];
	FontMetrics *m = &font_m[font];
	m->size = px;
	int asc = (int)((f->size->metrics.ascender + 63) >> 6);
	int desc = (int)((-f->size->metrics.descender + 63) >> 6);
	m->line = line ? line : asc + desc;
	m->ascent = asc + (m->line - (asc + desc)) / 2;
	FT_UInt gi = FT_Get_Char_Index(f, '0');
	m->adv = px / 2;
	if (gi && FT_Load_Glyph(f, gi, FT_LOAD_TARGET_LIGHT) == 0)
		m->adv = (int)((f->glyph->advance.x + 32) >> 6);
}

bool font_init(int ui_px, int mono_px, int mono_line, int small_px)
{
	if (FT_Init_FreeType(&ft))
		return false;
	font_atlas = vx_alloc((size_t)ATLAS_SIZE * ATLAS_SIZE);
	for (int f = 0; f < FONT_COUNT; f++) {
		int px = f <= FONT_UI_BOLD ? ui_px : f <= FONT_SMALL_BOLD ? small_px : mono_px;
		for (int k = 0; k < MAX_FACES && font_files[f][k]; k++)
			faces[f][k] = open_face(font_files[f][k], px);
		if (!faces[f][0]) {
			/* Fall back to whatever loaded. */
			for (int k = 1; k < MAX_FACES; k++)
				if (faces[f][k]) {
					faces[f][0] = faces[f][k];
					faces[f][k] = NULL;
					break;
				}
		}
		if (!faces[f][0]) {
			fprintf(stderr, "vxide: no font for slot %d (%s)\n", f, font_files[f][0]);
			return false;
		}
		metrics(f, px, f >= FONT_MONO ? mono_line : 0);
	}
	/* Mild contrast boost, closer to how browser engines render text. */
	for (int i = 0; i < 256; i++)
		gamma_lut[i] = (u8)lrint(255.0 * pow(i / 255.0, 1.0 / 1.25));
	/* Texel (0,0)..(1,1) is solid white: handy for debug, keeps (0,0) valid. */
	font_atlas[0] = font_atlas[1] = font_atlas[ATLAS_SIZE] = font_atlas[ATLAS_SIZE + 1] = 255;
	shelf_x = 2;
	shelf_y = 0;
	shelf_h = 2;
	return true;
}

void font_shutdown(void)
{
	for (int f = 0; f < FONT_COUNT; f++)
		for (int k = 0; k < MAX_FACES; k++)
			if (faces[f][k]) {
				/* Fallback faces may be shared after the swap above. */
				FT_Done_Face(faces[f][k]);
				faces[f][k] = NULL;
			}
	FT_Done_FreeType(ft);
	free(font_atlas);
	font_atlas = NULL;
}

bool font_atlas_alloc(int w, int h, u16 *u, u16 *v)
{
	if (shelf_x + w + 1 > ATLAS_SIZE) {
		shelf_y += shelf_h + 1;
		shelf_x = 0;
		shelf_h = 0;
	}
	if (shelf_y + h + 1 > ATLAS_SIZE)
		return false;
	*u = (u16)shelf_x;
	*v = (u16)shelf_y;
	shelf_x += w + 1;
	if (h > shelf_h)
		shelf_h = h;
	return true;
}

static void reset_atlas(void)
{
	memset(hash_key, 0, sizeof hash_key);
	memset(ascii, 0, sizeof ascii);
	font_glyphs.count = 0;
	memset(font_atlas + (size_t)reserved_y * ATLAS_SIZE, 0,
	       (size_t)(ATLAS_SIZE - reserved_y) * ATLAS_SIZE);
	shelf_x = 0;
	shelf_y = reserved_y;
	shelf_h = 0;
	was_reset = true;
	gpu_atlas_dirty(0, 0, ATLAS_SIZE, ATLAS_SIZE);
}

bool font_atlas_was_reset(void)
{
	bool r = was_reset;
	was_reset = false;
	/* Everything allocated so far (icons) is kept by later resets. */
	if (!reserved_y && font_glyphs.count == 0)
		reserved_y = shelf_y + shelf_h + 1;
	return r;
}

static u32 rasterize(int font, u32 cp)
{
	FT_Face face = NULL;
	FT_UInt gi = 0;
	for (int k = 0; k < MAX_FACES; k++) {
		if (!faces[font][k])
			continue;
		gi = FT_Get_Char_Index(faces[font][k], cp);
		if (gi) {
			face = faces[font][k];
			break;
		}
	}
	if (!face) {
		face = faces[font][0];
		gi = 0; /* .notdef box */
	}
	GlyphTable *g = &font_glyphs;
	if (g->count >= VX_COUNT(g->u))
		reset_atlas();
	u32 id = g->count;
	if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
		g->w[id] = g->h[id] = 0;
		g->adv[id] = (i16)font_m[font].adv;
		g->count++;
		return id;
	}
	FT_GlyphSlot s = face->glyph;
	int w = (int)s->bitmap.width, h = (int)s->bitmap.rows;
	u16 u = 0, v = 0;
	if (w && h && !font_atlas_alloc(w, h, &u, &v)) {
		reset_atlas();
		id = g->count;
		if (!font_atlas_alloc(w, h, &u, &v))
			w = h = 0;
	}
	for (int y = 0; y < h; y++) {
		const u8 *src = s->bitmap.buffer + (ptrdiff_t)y * s->bitmap.pitch;
		u8 *dst = font_atlas + (size_t)(v + y) * ATLAS_SIZE + u;
		for (int x = 0; x < w; x++)
			dst[x] = gamma_lut[src[x]];
	}
	if (w && h)
		gpu_atlas_dirty(u, v, w, h);
	g->u[id] = u;
	g->v[id] = v;
	g->w[id] = (i16)w;
	g->h[id] = (i16)h;
	g->bx[id] = (i16)s->bitmap_left;
	g->by[id] = (i16)(font_m[font].ascent - s->bitmap_top);
	int adv = (int)((s->advance.x + 32) >> 6);
	if (font >= FONT_MONO && cp < 0x2E80)
		adv = font_m[font].adv; /* keep the grid even for fallback glyphs */
	g->adv[id] = (i16)adv;
	g->count++;
	return id;
}

u32 font_glyph(int font, u32 cp)
{
	if (cp < 128 && ascii[font][cp])
		return ascii[font][cp] - 1u;
	u32 key = (((u32)font << 21) | cp) + 1;
	u32 h = (key * 2654435761u) & (HASH_SIZE - 1);
	while (hash_key[h]) {
		if (hash_key[h] == key)
			return hash_val[h];
		h = (h + 1) & (HASH_SIZE - 1);
	}
	u32 before = font_glyphs.count;
	u32 id = rasterize(font, cp);
	if (font_glyphs.count <= before) {
		/* atlas was reset during rasterize; slot h may be stale */
		h = (key * 2654435761u) & (HASH_SIZE - 1);
		while (hash_key[h])
			h = (h + 1) & (HASH_SIZE - 1);
	}
	hash_key[h] = key;
	hash_val[h] = (u16)id;
	if (cp < 128)
		ascii[font][cp] = (u16)(id + 1);
	return id;
}

int font_width(int font, const char *s, size_t n)
{
	int w = 0;
	size_t i = 0;
	while (i < n) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		i += k ? k : 1;
		w += font_glyphs.adv[font_glyph(font, cp)];
	}
	return w;
}
