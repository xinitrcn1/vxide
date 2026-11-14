/* vxide - common types and small helpers. C11. */
#ifndef VX_BASE_H
#define VX_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define VX_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define VX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define VX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define VX_CLAMP(v, lo, hi) VX_MIN(VX_MAX(v, lo), hi)

/* Abort with a message; used for allocation failure and broken invariants. */
_Noreturn void vx_die(const char *fmt, ...);
void *vx_alloc(size_t n);              /* zeroed */
void *vx_realloc(void *p, size_t n);
char *vx_strdup(const char *s);
char *vx_strndup(const char *s, size_t n);

/* Grow a dynamic array: ensures cap >= need. elem = sizeof element. */
void vx_grow(void **p, u32 *cap, u32 need, size_t elem);
#define VX_GROW(p, cap, need) vx_grow((void **)&(p), &(cap), (need), sizeof(*(p)))

/* Byte pool: append-only string storage referenced by offsets. */
typedef struct {
	char *b;
	u32 n, cap;
} Pool;
u32 pool_add(Pool *p, const char *s, u32 n); /* stores s + NUL, returns offset */
static inline const char *pool_str(const Pool *p, u32 off) { return p->b + off; }
void pool_clear(Pool *p);
void pool_free(Pool *p);

/* Monotonic clock in nanoseconds. */
u64 vx_now_ns(void);

/* Read a whole file; returns malloc'd buffer (NUL terminated) or NULL. */
char *vx_read_file(const char *path, size_t *len);
int vx_write_file(const char *path, const char *data, size_t len);

/* UTF-8 helpers. */
u32 utf8_decode(const char *s, size_t n, u32 *cp); /* returns bytes used (>=1) */
u32 utf8_encode(u32 cp, char out[4]);
static inline bool utf8_cont(u8 c) { return (c & 0xC0) == 0x80; }

#endif
