#define _POSIX_C_SOURCE 200809L
#include "base.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

_Noreturn void vx_die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("vxide: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	abort();
}

void *vx_alloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);
	if (!p)
		vx_die("out of memory (%zu bytes)", n);
	return p;
}

void *vx_realloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		vx_die("out of memory (%zu bytes)", n);
	return p;
}

char *vx_strdup(const char *s) { return vx_strndup(s, strlen(s)); }

char *vx_strndup(const char *s, size_t n)
{
	char *d = vx_alloc(n + 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

void vx_grow(void **p, u32 *cap, u32 need, size_t elem)
{
	if (need <= *cap)
		return;
	u32 c = *cap ? *cap : 16;
	while (c < need)
		c = c < 0x40000000u ? c * 2 : need;
	*p = vx_realloc(*p, (size_t)c * elem);
	*cap = c;
}

u32 pool_add(Pool *p, const char *s, u32 n)
{
	VX_GROW(p->b, p->cap, p->n + n + 1);
	u32 off = p->n;
	if (n)
		memcpy(p->b + off, s, n);
	p->b[off + n] = 0;
	p->n += n + 1;
	return off;
}

void pool_clear(Pool *p) { p->n = 0; }

void pool_free(Pool *p)
{
	free(p->b);
	p->b = NULL;
	p->n = p->cap = 0;
}

u64 vx_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

char *vx_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	size_t cap = 4096, n = 0;
	char *b = vx_alloc(cap);
	for (;;) {
		if (n + 1 >= cap)
			b = vx_realloc(b, cap *= 2);
		size_t r = fread(b + n, 1, cap - n - 1, f);
		if (r == 0)
			break;
		n += r;
	}
	int err = ferror(f);
	fclose(f);
	if (err) {
		free(b);
		return NULL;
	}
	b[n] = 0;
	if (len)
		*len = n;
	return b;
}

int vx_write_file(const char *path, const char *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t w = fwrite(data, 1, len, f);
	int e = fclose(f);
	return (w == len && e == 0) ? 0 : -1;
}

u32 utf8_decode(const char *s, size_t n, u32 *cp)
{
	const u8 *u = (const u8 *)s;
	if (n == 0) {
		*cp = 0;
		return 0;
	}
	u8 c = u[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	u32 need, v;
	if ((c & 0xE0) == 0xC0) {
		need = 1;
		v = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		need = 2;
		v = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		need = 3;
		v = c & 0x07;
	} else {
		*cp = 0xFFFD;
		return 1;
	}
	if (n < need + 1) {
		*cp = 0xFFFD;
		return 1;
	}
	for (u32 i = 1; i <= need; i++) {
		if (!utf8_cont(u[i])) {
			*cp = 0xFFFD;
			return 1;
		}
		v = (v << 6) | (u[i] & 0x3F);
	}
	*cp = v;
	return need + 1;
}

u32 utf8_encode(u32 cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}
