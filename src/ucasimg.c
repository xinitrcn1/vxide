/* GIF / PNG decoding for the ucas tab (stb_image, compiled here privately). */
#include "ucasimg.h"

#include <stdlib.h>
#include <string.h>

#ifdef VXIDE_UCAS
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_GIF
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#endif

bool ucasimg_size(const u8 *d, u32 n, int *w, int *h)
{
	if (n >= 10 && !memcmp(d, "GIF8", 4)) {
		*w = d[6] | d[7] << 8;
		*h = d[8] | d[9] << 8;
		return true;
	}
	if (n >= 24 && !memcmp(d, "\x89PNG", 4)) {
		*w = d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19];
		*h = d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23];
		return true;
	}
	return false;
}

bool ucasimg_decode(const u8 *data, u32 n, bool first_only, UcasImage *out)
{
	memset(out, 0, sizeof *out);
#ifdef VXIDE_UCAS
	int comp = 0;
	if (n >= 4 && !memcmp(data, "GIF8", 4) && !first_only) {
		int *delays = NULL, z = 0;
		u8 *p = stbi_load_gif_from_memory(data, (int)n, &delays, &out->w, &out->h, &z, &comp, 4);
		if (!p)
			return false;
		out->rgba = p; /* stb allocates with malloc: ucasimg_free frees it */
		out->nframes = z;
		out->delay = malloc(sizeof(int) * (size_t)z);
		for (int i = 0; i < z; i++)
			out->delay[i] = delays && delays[i] > 0 ? delays[i] : 100;
		free(delays); /* stb allocates them with STBI_MALLOC, i.e. malloc */
		return true;
	}
	u8 *p = stbi_load_from_memory(data, (int)n, &out->w, &out->h, &comp, 4);
	if (!p)
		return false;
	out->rgba = p;
	out->nframes = 1;
	return true;
#else
	(void)data;
	(void)n;
	(void)first_only;
	return false;
#endif
}

void ucasimg_free(UcasImage *im)
{
	free(im->rgba);
	free(im->delay);
	memset(im, 0, sizeof *im);
}
