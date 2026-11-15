/* Allocation-site accounting for -DVXIDE_HEAPTRACE builds (see base.h). */
#define VX_HEAPTRACE_IMPL
#include "base.h"

#include <stdio.h>

#ifdef VXIDE_HEAPTRACE
#include <malloc_np.h>
#include <pthread.h>

#define SITES 4096
static struct {
	const char *f;
	int l;
	i64 bytes, count;
} site[SITES];
static u32 nsite;

/* pointer -> site (open addressing) */
#define PT (1u << 22)
static uintptr_t *pk;
static u16 *pv;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static u32 site_id(const char *f, int l)
{
	for (u32 i = 0; i < nsite; i++)
		if (site[i].l == l && site[i].f == f)
			return i;
	if (nsite == SITES)
		return 0;
	site[nsite].f = f;
	site[nsite].l = l;
	return nsite++;
}

static u32 slot(uintptr_t k)
{
	return (u32)((k >> 4) * 2654435761u) & (PT - 1);
}

static void track(void *p, const char *f, int l)
{
	if (!p)
		return;
	pthread_mutex_lock(&mu);
	if (!pk) {
		pk = calloc(PT, sizeof *pk);
		pv = calloc(PT, sizeof *pv);
	}
	u32 id = site_id(f, l);
	site[id].bytes += (i64)malloc_usable_size(p);
	site[id].count++;
	uintptr_t k = (uintptr_t)p;
	u32 i = slot(k);
	while (pk[i] && pk[i] != 1)
		i = (i + 1) & (PT - 1);
	pk[i] = k;
	pv[i] = (u16)id;
	pthread_mutex_unlock(&mu);
}

static void untrack(void *p)
{
	if (!p || !pk)
		return;
	pthread_mutex_lock(&mu);
	uintptr_t k = (uintptr_t)p;
	u32 i = slot(k);
	while (pk[i]) {
		if (pk[i] == k) {
			site[pv[i]].bytes -= (i64)malloc_usable_size(p);
			site[pv[i]].count--;
			pk[i] = 1; /* tombstone */
			break;
		}
		i = (i + 1) & (PT - 1);
	}
	pthread_mutex_unlock(&mu);
}

void *ht_malloc(size_t n, const char *f, int l)
{
	void *p = malloc(n);
	track(p, f, l);
	return p;
}
void *ht_calloc(size_t a, size_t b, const char *f, int l)
{
	void *p = calloc(a, b);
	track(p, f, l);
	return p;
}
void *ht_realloc(void *p, size_t n, const char *f, int l)
{
	untrack(p);
	void *q = realloc(p, n);
	track(q ? q : p, f, l);
	return q;
}
void ht_free(void *p)
{
	untrack(p);
	free(p);
}
char *ht_strdup(const char *s, const char *f, int l)
{
	char *d = strdup(s);
	track(d, f, l);
	return d;
}
void *vx_alloc_t(size_t n, const char *f, int l)
{
	void *p = vx_alloc(n);
	track(p, f, l);
	return p;
}
void *vx_realloc_t(void *p, size_t n, const char *f, int l)
{
	untrack(p);
	void *q = vx_realloc(p, n);
	track(q, f, l);
	return q;
}
char *vx_strndup_t(const char *s, size_t n, const char *f, int l)
{
	char *d = vx_strndup(s, n);
	track(d, f, l);
	return d;
}
void vx_grow_t(void **p, u32 *cap, u32 need, size_t elem, const char *f, int l)
{
	if (need <= *cap)
		return;
	untrack(*p);
	vx_grow(p, cap, need, elem);
	track(*p, f, l);
}

u32 pool_add_t(Pool *p, const char *s, u32 n, const char *f, int l)
{
	char *old = p->b;
	u32 oldcap = p->cap;
	if (p->n + n + 1 > p->cap)
		untrack(old);
	u32 off = pool_add(p, s, n);
	if (p->cap != oldcap || p->b != old)
		track(p->b, f, l);
	return off;
}

void soa_grow_t(const SoaCol *c, u32 nc, u32 *cap, u32 need, u32 n, const char *f, int l)
{
	bool moves = nc && (need > *cap || !*c[0].p);
	if (moves)
		untrack(*c[0].p); /* before the old block is freed */
	soa_grow(c, nc, cap, need, n);
	if (moves)
		track(*c[0].p, f, l);
}

void soa_fit_t(const SoaCol *c, u32 nc, u32 *cap, u32 n, const char *f, int l)
{
	bool moves = nc && *c[0].p && *cap != n;
	if (moves)
		untrack(*c[0].p);
	soa_fit(c, nc, cap, n);
	if (moves)
		track(*c[0].p, f, l);
}

void soa_free_t(const SoaCol *c, u32 nc)
{
	if (nc)
		untrack(*c[0].p);
	soa_free(c, nc);
}

static int cmp(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	return site[y].bytes > site[x].bytes ? 1 : site[y].bytes < site[x].bytes ? -1 : 0;
}

void heap_trace_report(int top)
{
	pthread_mutex_lock(&mu);
	static u32 idx[SITES];
	i64 total = 0;
	for (u32 i = 0; i < nsite; i++) {
		idx[i] = i;
		total += site[i].bytes;
	}
	qsort(idx, nsite, sizeof(u32), cmp);
	fprintf(stderr, "  ours: %.2f MB live\n", (double)total / 1048576.0);
	for (int k = 0; k < top && (u32)k < nsite; k++) {
		u32 i = idx[k];
		if (site[i].bytes < 16384)
			break;
		const char *b = strrchr(site[i].f, '/');
		fprintf(stderr, "  %9.1f KB %7lld blocks  %s:%d\n", (double)site[i].bytes / 1024.0,
		        (long long)site[i].count, b ? b + 1 : site[i].f, site[i].l);
	}
	pthread_mutex_unlock(&mu);
}
#else
void heap_trace_report(int top) { (void)top; }
#endif
