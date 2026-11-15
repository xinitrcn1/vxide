/* The pictures ucas answers with (GIF plots and animations, PNG) as RGBA8
 * frames: stb_image from ucas's own tree. */
#ifndef VX_UCASIMG_H
#define VX_UCASIMG_H

#include "base.h"

typedef struct {
	int w, h, nframes;
	u8 *rgba;    /* nframes * w * h * 4, malloc'd */
	int *delay;  /* ms per frame, malloc'd (NULL for a still) */
} UcasImage;

/* Decode a GIF or PNG; only the first frame when first_only. False when the
 * data is neither or broken. */
bool ucasimg_decode(const u8 *data, u32 n, bool first_only, UcasImage *out);
/* Size only, from the header (GIF / PNG); false when unknown */
bool ucasimg_size(const u8 *data, u32 n, int *w, int *h);
void ucasimg_free(UcasImage *im);

#endif
