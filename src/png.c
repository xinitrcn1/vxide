/* Minimal PNG writer (RGBA8, stored deflate blocks) for screenshots. */
#include "base.h"
#include "png.h"

#include <stdio.h>

static u32 crc_table[256];

static void crc_init(void)
{
	for (u32 n = 0; n < 256; n++) {
		u32 c = n;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static u32 crc(u32 c, const u8 *b, size_t n)
{
	c = ~c;
	for (size_t i = 0; i < n; i++)
		c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}

static void be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static void chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
	u8 h[8];
	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, f);
	if (n)
		fwrite(data, 1, n, f);
	u32 c = crc(0, (const u8 *)type, 4);
	c = crc(c, data, n);
	be32(h, c);
	fwrite(h, 1, 4, f);
}

int png_write(const char *path, const u8 *rgba, int w, int h)
{
	if (!crc_table[1])
		crc_init();
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	fwrite(sig, 1, 8, f);
	u8 ihdr[13];
	be32(ihdr, (u32)w);
	be32(ihdr + 4, (u32)h);
	ihdr[8] = 8;  /* depth */
	ihdr[9] = 6;  /* RGBA */
	ihdr[10] = 0; /* deflate */
	ihdr[11] = 0;
	ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);

	size_t row = (size_t)w * 4 + 1;
	size_t raw_n = row * (size_t)h;
	u8 *raw = vx_alloc(raw_n);
	for (int y = 0; y < h; y++) {
		raw[y * row] = 0;
		memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
	}
	size_t nblocks = (raw_n + 65534) / 65535;
	size_t z_n = 2 + raw_n + nblocks * 5 + 4;
	u8 *z = vx_alloc(z_n);
	size_t o = 0;
	z[o++] = 0x78;
	z[o++] = 0x01;
	u32 a = 1, b = 0;
	for (size_t i = 0; i < raw_n; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t i = 0; i < raw_n; i += 65535) {
		size_t n = VX_MIN(raw_n - i, (size_t)65535);
		z[o++] = i + n == raw_n;
		z[o++] = (u8)n;
		z[o++] = (u8)(n >> 8);
		z[o++] = (u8)~n;
		z[o++] = (u8)(~n >> 8);
		memcpy(z + o, raw + i, n);
		o += n;
	}
	be32(z + o, (b << 16) | a);
	o += 4;
	chunk(f, "IDAT", z, (u32)o);
	chunk(f, "IEND", NULL, 0);
	free(raw);
	free(z);
	return fclose(f) == 0 ? 0 : -1;
}
