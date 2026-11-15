#define _POSIX_C_SOURCE 200809L
#include "blame.h"
#include "lgblame.h"

#include <git2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


/* Published result (SoA): per line an index into the commit table, or
 * UINT32_MAX for uncommitted lines. */
typedef struct {
	char *path;
	u32 key;
	bool tracked;
	u32 nlines;
	u32 *line_commit;
	u32 nc;
	u32 *author, *summary; /* pool offsets */
	i64 *time;
	char (*hash)[12];
	Pool pool;
} Result;

struct Blame {
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool quit;

	/* pending request (owned by the mutex) */
	bool has_req;
	char *req_path;
	char *req_text;
	size_t req_n;
	u32 req_key;

	Result res; /* published, under the mutex */
	bool updated;

	/* worker-only state */
	git_repository *repo;
	LgBlame head_blame; /* blame of the committed file at base_head */
	bool have;
	char *base_path;
	git_oid base_head;
};

static void result_free(Result *r)
{
	free(r->path);
	free(r->line_commit);
	free(r->author);
	free(r->summary);
	free(r->time);
	free(r->hash);
	pool_free(&r->pool);
	memset(r, 0, sizeof *r);
}

static void compute(Blame *b, const char *path, const char *text, size_t n, u32 key, Result *r)
{
	memset(r, 0, sizeof *r);
	r->path = vx_strdup(path);
	r->key = key;
	r->nlines = 1;
	for (size_t i = 0; i < n; i++)
		r->nlines += text[i] == '\n';
	git_oid head;
	if (git_reference_name_to_id(&head, b->repo, "HEAD") != 0)
		return; /* unborn branch: nothing to blame */
	/* the committed file's blame is cached per (path, HEAD) */
	if (!b->have || !b->base_path || strcmp(b->base_path, path) || !git_oid_equal(&head, &b->base_head)) {
		if (b->have)
			lg_blame_free(&b->head_blame);
		b->have = false;
		free(b->base_path);
		b->base_path = vx_strdup(path);
		b->base_head = head;
		if (lg_blame_head(b->repo, path, &b->head_blame) != 0)
			return; /* untracked or not in HEAD */
		b->have = true;
	}
	LgBlame *hb = &b->head_blame;
	/* the live buffer over the HEAD blob: edited lines are uncommitted */
	u32 *map = vx_alloc(r->nlines * sizeof(u32));
	if (lg_blame_buffer_map(b->repo, &hb->head_blob, path, text, n, map, r->nlines) != 0) {
		free(map);
		return;
	}
	r->tracked = true;
	r->nc = hb->nc;
	r->author = vx_alloc((hb->nc + 1) * sizeof(u32));
	r->summary = vx_alloc((hb->nc + 1) * sizeof(u32));
	r->time = vx_alloc((hb->nc + 1) * sizeof(i64));
	r->hash = vx_alloc((hb->nc + 1) * sizeof r->hash[0]);
	for (u32 i = 0; i < hb->nc; i++) {
		const char *au = hb->pool.b + hb->author[i], *su = hb->pool.b + hb->summary[i];
		r->author[i] = pool_add(&r->pool, au, (u32)strlen(au));
		r->summary[i] = pool_add(&r->pool, su, (u32)strlen(su));
		r->time[i] = hb->time[i];
		git_oid_tostr(r->hash[i], sizeof r->hash[i], &hb->oid[i]);
	}
	r->line_commit = map;
	for (u32 l = 0; l < r->nlines; l++)
		map[l] = map[l] < hb->nlines ? hb->line_commit[map[l]] : UINT32_MAX;
}

static void *worker(void *arg)
{
	Blame *b = arg;
	pthread_mutex_lock(&b->mu);
	for (;;) {
		while (!b->has_req && !b->quit)
			pthread_cond_wait(&b->cv, &b->mu);
		if (b->quit)
			break;
		char *path = b->req_path, *text = b->req_text;
		size_t n = b->req_n;
		u32 key = b->req_key;
		b->req_path = b->req_text = NULL;
		b->has_req = false;
		pthread_mutex_unlock(&b->mu);

		Result r;
		compute(b, path, text, n, key, &r);
		free(path);
		free(text);

		pthread_mutex_lock(&b->mu);
		result_free(&b->res);
		b->res = r;
		b->updated = true;
	}
	pthread_mutex_unlock(&b->mu);
	return NULL;
}

Blame *blame_start(const char *workdir)
{
	git_libgit2_init();
	Blame *b = vx_alloc(sizeof *b);
	/* keep blobs in libgit2's object cache: each version is the new side of
	 * one diff and the old side of the next */
	git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_BLOB, (size_t)16 << 20);
	if (git_repository_open(&b->repo, workdir) != 0) {
		free(b);
		git_libgit2_shutdown();
		return NULL;
	}
	pthread_mutex_init(&b->mu, NULL);
	pthread_cond_init(&b->cv, NULL);
	if (pthread_create(&b->th, NULL, worker, b) != 0) {
		git_repository_free(b->repo);
		free(b);
		git_libgit2_shutdown();
		return NULL;
	}
	return b;
}

void blame_stop(Blame *b)
{
	if (!b)
		return;
	pthread_mutex_lock(&b->mu);
	b->quit = true;
	pthread_cond_signal(&b->cv);
	pthread_mutex_unlock(&b->mu);
	pthread_join(b->th, NULL);
	free(b->req_path);
	free(b->req_text);
	result_free(&b->res);
	if (b->have)
		lg_blame_free(&b->head_blame);
	free(b->base_path);
	git_repository_free(b->repo);
	pthread_mutex_destroy(&b->mu);
	pthread_cond_destroy(&b->cv);
	free(b);
	git_libgit2_shutdown();
}

void blame_request(Blame *b, const char *relpath, const char *text, size_t n, u32 key)
{
	pthread_mutex_lock(&b->mu);
	free(b->req_path);
	free(b->req_text);
	b->req_path = vx_strdup(relpath);
	b->req_text = vx_strndup(text, n);
	b->req_n = n;
	b->req_key = key;
	b->has_req = true;
	pthread_cond_signal(&b->cv);
	pthread_mutex_unlock(&b->mu);
}

/* Strings handed out are copied here so they outlive the lock. */
static char g_author[128], g_summary[512];

bool blame_line(Blame *b, const char *relpath, u32 key, u32 line, BlameLine *out)
{
	bool ok = false;
	pthread_mutex_lock(&b->mu);
	Result *r = &b->res;
	if (r->path && r->tracked && r->key == key && !strcmp(r->path, relpath) && line < r->nlines) {
		u32 c = r->line_commit[line];
		memset(out, 0, sizeof *out);
		if (c == UINT32_MAX) {
			out->uncommitted = true;
			g_author[0] = g_summary[0] = 0;
		} else {
			snprintf(g_author, sizeof g_author, "%s", r->pool.b + r->author[c]);
			snprintf(g_summary, sizeof g_summary, "%s", r->pool.b + r->summary[c]);
			memcpy(out->hash, r->hash[c], sizeof out->hash);
			out->time = r->time[c];
		}
		out->author = g_author;
		out->summary = g_summary;
		ok = true;
	}
	pthread_mutex_unlock(&b->mu);
	return ok;
}

bool blame_ready(Blame *b, const char *relpath, u32 key)
{
	pthread_mutex_lock(&b->mu);
	bool ok = b->res.path && b->res.key == key && !strcmp(b->res.path, relpath);
	pthread_mutex_unlock(&b->mu);
	return ok;
}

bool blame_take_update(Blame *b)
{
	pthread_mutex_lock(&b->mu);
	bool u = b->updated;
	b->updated = false;
	pthread_mutex_unlock(&b->mu);
	return u;
}

void blame_ago(i64 when, i64 now, char *buf, size_t cap)
{
	i64 d = now - when;
	if (d < 0)
		d = 0;
	static const struct {
		i64 secs;
		const char *unit;
	} units[] = {{365 * 86400, "year"}, {30 * 86400, "month"}, {7 * 86400, "week"},
	             {86400, "day"},        {3600, "hour"},         {60, "minute"}};
	for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
		if (d >= units[i].secs) {
			i64 k = d / units[i].secs;
			snprintf(buf, cap, "%lld %s%s ago", (long long)k, units[i].unit, k == 1 ? "" : "s");
			return;
		}
	snprintf(buf, cap, "now");
}
