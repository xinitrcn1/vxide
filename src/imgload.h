/* PNG / JPEG to RGBA8 (straight alpha). */
#ifndef VX_IMGLOAD_H
#define VX_IMGLOAD_H

#include "base.h"

/* NULL when the file is missing, not PNG/JPEG, or broken; free() the result. */
u8 *img_load(const char *path, int *w, int *h);

#endif
