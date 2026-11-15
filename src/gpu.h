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
#define QUAD_IMAGE 0xFFFDu /* samples the background image (gpu_set_background) */
#define ATLAS_SIZE 2048

struct SDL_Window;

/* win == NULL: headless, renders into an offscreen image of w x h. */
bool gpu_init(struct SDL_Window *win, int w, int h);
void gpu_shutdown(void);
void gpu_resize(int w, int h);
/* The CPU atlas (ATLAS_SIZE^2 bytes, R8). Mark changed rects; the next
 * frame uploads their union. */
/* The glyph atlas pixels (ATLAS_SIZE^2, R8): the persistently mapped
 * staging buffer itself, so there is no second copy on the heap. Write
 * glyphs here, then mark the rect with gpu_atlas_dirty. */
u8 *gpu_atlas_pixels(void);
bool gpu_atlas_ready(void);
/* Where the next frame's quads go: that frame's mapped instance buffer
 * (waits until the GPU is done with it). draw.c writes there directly, so
 * there is no CPU quad list and no per-frame copy. */
Quad *gpu_frame_quads(u32 *cap);
void gpu_atlas_dirty(int x, int y, int w, int h);
/* Background image (RGBA8, straight alpha), NULL to clear; copied. */
void gpu_set_background(const u8 *rgba, int w, int h);
/* Where the image lies on screen for this frame (QUAD_IMAGE quads sample
 * it by pixel position, so clipping them needs no texture coordinates). */
void gpu_background_place(float x, float y, float w, float h);
/* Draw and present. clear = 0xRRGGBBAA. Returns false when the frame was
 * skipped (minimized / out of date; it recreates the swapchain). */
bool gpu_frame(const Quad *q, u32 n, u32 clear);
/* Headless only: wait and copy the last frame as RGBA8 (w*h*4). */
bool gpu_readback(u8 *rgba, int w, int h);
const char *gpu_device_name(void);

#endif
