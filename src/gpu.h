/* Vulkan renderer: draws one list of quads per frame with one pipeline.
 * Everything on screen is a solid rect, an atlas coverage rect (glyphs,
 * icons) or a squiggle. Works on a swapchain or headless (offscreen). */
#ifndef VX_GPU_H
#define VX_GPU_H

#include "base.h"

typedef struct {
	i16 x, y, w, h; /* pixels */
	u16 u, v;       /* atlas texel origin; QUAD_SOLID / QUAD_SQUIGGLE */
	u32 rgba;       /* 0xRRGGBBAA */
} Quad;

#define QUAD_SOLID 0xFFFFu
#define QUAD_SQUIGGLE 0xFFFEu
#define ATLAS_SIZE 2048

struct SDL_Window;

/* win == NULL: headless, renders into an offscreen image of w x h. */
bool gpu_init(struct SDL_Window *win, int w, int h);
void gpu_shutdown(void);
void gpu_resize(int w, int h);
/* The CPU atlas (ATLAS_SIZE^2 bytes, R8). Mark changed rects; the next
 * frame uploads their union. */
void gpu_set_atlas(const u8 *pixels);
void gpu_atlas_dirty(int x, int y, int w, int h);
/* Draw and present. clear = 0xRRGGBBAA. Returns false when the frame was
 * skipped (minimized / out of date; it recreates the swapchain). */
bool gpu_frame(const Quad *q, u32 n, u32 clear);
/* Headless only: wait and copy the last frame as RGBA8 (w*h*4). */
bool gpu_readback(u8 *rgba, int w, int h);
const char *gpu_device_name(void);

#endif
