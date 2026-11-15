/* Image files for the editor background: PNG (libpng) and JPEG (libjpeg),
 * decoded to RGBA8. Big pictures are box-filtered down so the texture stays
 * at most IMG_MAX on a side (a 6000 px photo is 140 MB of RGBA otherwise). */
#include "imgload.h"

#include <libpng16/png.h> /* not src/png.h (our PNG writer) */
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>

#define IMG_MAX 4096

static u8 *load_png(const char *path, int *w, int *h)
{
	png_image im;
	memset(&im, 0, sizeof im);
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&im, path))
		return NULL;
	im.format = PNG_FORMAT_RGBA;
	u8 *px = malloc(PNG_IMAGE_SIZE(im));
	if (!px || !png_image_finish_read(&im, NULL, px, 0, NULL)) {
		free(px);
		png_image_free(&im);
		return NULL;
	}
	*w = (int)im.width;
	*h = (int)im.height;
	return px;
}

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
} JErr;

static void jpeg_fail(j_common_ptr c)
{
	JErr *e = (JErr *)c->err;
	longjmp(e->jb, 1);
}

static u8 *load_jpeg(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	struct jpeg_decompress_struct c;
	JErr err;
	u8 *volatile px = NULL;
	u8 *volatile row = NULL;
	c.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = jpeg_fail;
	if (setjmp(err.jb)) {
		jpeg_destroy_decompress(&c);
		fclose(f);
		free(px);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&c);
	jpeg_stdio_src(&c, f);
	jpeg_read_header(&c, TRUE);
	c.out_color_space = JCS_RGB;
	/* let libjpeg scale by 1/2, 1/4, 1/8 while decoding when the picture is huge */
	while (c.scale_denom < 8 && (c.image_width / (c.scale_denom * 2) >= IMG_MAX ||
	                             c.image_height / (c.scale_denom * 2) >= IMG_MAX))
		c.scale_denom *= 2;
	jpeg_start_decompress(&c);
	int W = (int)c.output_width, H = (int)c.output_height;
	px = malloc((size_t)W * H * 4);
	row = malloc((size_t)W * 3);
	if (!px || !row)
		longjmp(err.jb, 1);
	while (c.output_scanline < c.output_height) {
		JSAMPROW r = row;
		int y = (int)c.output_scanline;
		jpeg_read_scanlines(&c, &r, 1);
		u8 *o = px + (size_t)y * W * 4;
		for (int x = 0; x < W; x++) {
			o[x * 4 + 0] = row[x * 3 + 0];
			o[x * 4 + 1] = row[x * 3 + 1];
			o[x * 4 + 2] = row[x * 3 + 2];
			o[x * 4 + 3] = 255;
		}
	}
	jpeg_finish_decompress(&c);
	jpeg_destroy_decompress(&c);
	fclose(f);
	free(row);
	*w = W;
	*h = H;
	return px;
}

/* Integer box filter down to fit IMG_MAX. */
static u8 *shrink(u8 *px, int *w, int *h)
{
	int W = *w, H = *h;
	int k = 1;
	while (W / k > IMG_MAX || H / k > IMG_MAX)
		k++;
	if (k == 1)
		return px;
	int nw = W / k, nh = H / k;
	u8 *o = malloc((size_t)nw * nh * 4);
	if (!o)
		return px;
	for (int y = 0; y < nh; y++)
		for (int x = 0; x < nw; x++) {
			u32 acc[4] = {0, 0, 0, 0};
			for (int j = 0; j < k; j++)
				for (int i = 0; i < k; i++) {
					const u8 *p = px + ((size_t)(y * k + j) * W + (size_t)(x * k + i)) * 4;
					for (int c = 0; c < 4; c++)
						acc[c] += p[c];
				}
			for (int c = 0; c < 4; c++)
				o[((size_t)y * nw + x) * 4 + c] = (u8)(acc[c] / (u32)(k * k));
		}
	free(px);
	*w = nw;
	*h = nh;
	return o;
}

u8 *img_load(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	u8 sig[8] = {0};
	size_t n = fread(sig, 1, sizeof sig, f);
	fclose(f);
	u8 *px = NULL;
	if (n >= 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8))
		px = load_png(path, w, h);
	else if (n >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF)
		px = load_jpeg(path, w, h);
	return px ? shrink(px, w, h) : NULL;
}
