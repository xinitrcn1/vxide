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

/* Coalesced struct-of-arrays. All columns of a table share one heap block
 * (one allocation, one free, columns 8-byte aligned, in the order given);
 * the first column's pointer is the block. Rows are addressed by u32
 * index, never by pointer, so growing the table moves nothing anyone
 * holds. Usage:
 *     SoaCol c[] = {SOA(t->addr), SOA(t->len)};
 *     soa_grow(c, 2, &t->cap, t->n + 1, t->n);   ...   soa_free(c, 2);
 */
typedef struct {
	void **p;
	u32 size; /* bytes per element */
} SoaCol;
#define SOA(col) ((SoaCol){(void **)&(col), (u32)sizeof(*(col))})
/* ensure cap >= need (doubling), keeping the first n rows */
void soa_grow(const SoaCol *cols, u32 ncols, u32 *cap, u32 need, u32 n);
/* exactly n rows of capacity (drop the growth slack once a table is built) */
void soa_fit(const SoaCol *cols, u32 ncols, u32 *cap, u32 n);
void soa_free(const SoaCol *cols, u32 ncols);

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

/* Heap tracing (debug builds with -DVXIDE_HEAPTRACE): our allocations are
 * tagged with their call site so heap_trace_report() can say who holds the
 * memory. Libraries keep their own allocator (they free what they allocate). */
#ifdef VXIDE_HEAPTRACE
void *ht_malloc(size_t n, const char *f, int l);
void *ht_calloc(size_t a, size_t b, const char *f, int l);
void *ht_realloc(void *p, size_t n, const char *f, int l);
void ht_free(void *p);
char *ht_strdup(const char *s, const char *f, int l);
void *vx_alloc_t(size_t n, const char *f, int l);
void *vx_realloc_t(void *p, size_t n, const char *f, int l);
char *vx_strndup_t(const char *s, size_t n, const char *f, int l);
void vx_grow_t(void **p, u32 *cap, u32 need, size_t elem, const char *f, int l);
u32 pool_add_t(Pool *p, const char *s, u32 n, const char *f, int l);
void soa_grow_t(const SoaCol *c, u32 nc, u32 *cap, u32 need, u32 n, const char *f, int l);
void soa_fit_t(const SoaCol *c, u32 nc, u32 *cap, u32 n, const char *f, int l);
void soa_free_t(const SoaCol *c, u32 nc);
#ifndef VX_HEAPTRACE_IMPL
#define malloc(n) ht_malloc((n), __FILE__, __LINE__)
#define calloc(a, b) ht_calloc((a), (b), __FILE__, __LINE__)
#define realloc(p, n) ht_realloc((p), (n), __FILE__, __LINE__)
#define free(p) ht_free(p)
#define strdup(s) ht_strdup((s), __FILE__, __LINE__)
#define vx_alloc(n) vx_alloc_t((n), __FILE__, __LINE__)
#define vx_realloc(p, n) vx_realloc_t((p), (n), __FILE__, __LINE__)
#define vx_strdup(s) vx_strndup_t((s), strlen(s), __FILE__, __LINE__)
#define vx_strndup(s, n) vx_strndup_t((s), (n), __FILE__, __LINE__)
#undef VX_GROW
#define VX_GROW(p, cap, need) vx_grow_t((void **)&(p), &(cap), (need), sizeof(*(p)), __FILE__, __LINE__)
#define pool_add(p, s, n) pool_add_t((p), (s), (n), __FILE__, __LINE__)
#define soa_grow(c, nc, cap, need, n) soa_grow_t((c), (nc), (cap), (need), (n), __FILE__, __LINE__)
#define soa_fit(c, nc, cap, n) soa_fit_t((c), (nc), (cap), (n), __FILE__, __LINE__)
#define soa_free(c, nc) soa_free_t((c), (nc))
#endif
#endif
void heap_trace_report(int top);

#endif
