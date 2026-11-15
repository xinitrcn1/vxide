/* Background disk work, so a slow disk never stalls the UI thread:
 *  - the workspace file list (Quick Open, Search) is scanned on a thread
 *    with its own libgit2 handle for .gitignore checks;
 *  - text searches run on a thread over a snapshot of that list, files in
 *    inode order, cancelled when the query changes.
 * The UI polls once per tick and swaps finished results in. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"

#include <git2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

/* ---- file list -------------------------------------------------------------- */

static pthread_t scan_th;
static _Atomic int scan_state; /* 0 idle, 1 running, 2 done (to swap) */
static FileList scan_out;
static char *scan_root;
static char *scan_workdir;

static bool scan_skip(void *ctx, const char *rel, bool dir)
{
	git_repository *repo = ctx;
	if (!repo || !scan_workdir)
		return false;
	char abs[8192];
	snprintf(abs, sizeof abs, "%s/%s%s", scan_root, rel, dir ? "/" : "");
	size_t wl = strlen(scan_workdir);
	if (strncmp(abs, scan_workdir, wl) != 0)
		return false;
	int ignored = 0;
	git_ignore_path_is_ignored(&ignored, repo, abs + wl);
	return ignored != 0;
}

static void *scan_main(void *arg)
{
	(void)arg;
	git_repository *repo = NULL;
	if (scan_workdir)
		git_repository_open(&repo, scan_workdir);
	FileList fl = {0};
	files_scan(&fl, scan_root, scan_skip, repo, 200000);
	git_repository_free(repo);
	scan_out = fl;
	atomic_store(&scan_state, 2);
	return NULL;
}

void bg_files_request(void)
{
	if (atomic_load(&scan_state) != 0)
		return;
	free(scan_root);
	free(scan_workdir);
	scan_root = vx_strdup(app.root);
	scan_workdir = app.git ? vx_strdup(git_workdir(app.git)) : NULL;
	atomic_store(&scan_state, 1);
	if (pthread_create(&scan_th, NULL, scan_main, NULL) != 0) {
		atomic_store(&scan_state, 0);
		return;
	}
}

bool bg_files_busy(void) { return atomic_load(&scan_state) == 1; }

/* Swap a finished scan in; true when app.files changed. */
bool bg_files_poll(void)
{
	if (atomic_load(&scan_state) != 2)
		return false;
	pthread_join(scan_th, NULL);
	files_free(&app.files);
	app.files = scan_out;
	memset(&scan_out, 0, sizeof scan_out);
	app.files_ok = true;
	free(app.search_collapsed);
	app.search_collapsed = vx_alloc(app.files.n + 1);
	atomic_store(&scan_state, 0);
	return true;
}

/* ---- text search -------------------------------------------------------------- */

typedef struct {
	char *root, *query, *inc, *exc;
	int flags;
	FileList fl; /* snapshot */
	SearchResults res;
	_Atomic int cancel;
	_Atomic u32 done;
	u32 total;
} SearchJob;

static pthread_t search_th;
static _Atomic int search_state; /* 0 idle, 1 running, 2 done */
static SearchJob *job;

static FileList fl_copy(const FileList *s)
{
	FileList d = {0};
	d.n = s->n;
	d.cap = VX_MAX(s->n, 1u); /* no block yet: soa_grow allocates exactly cap rows */
	SoaCol c[] = {FL_COLS(&d)};
	soa_grow(c, 3, &d.cap, d.cap, 0);
	if (s->n) {
		memcpy(d.path, s->path, s->n * sizeof(u32));
		memcpy(d.base, s->base, s->n * sizeof(u32));
		if (s->ino)
			memcpy(d.ino, s->ino, s->n * sizeof(u64));
	}
	pool_add(&d.pool, "", 0);
	d.pool.n = 0;
	VX_GROW(d.pool.b, d.pool.cap, s->pool.n + 1);
	memcpy(d.pool.b, s->pool.b, s->pool.n);
	d.pool.n = s->pool.n;
	return d;
}

static void *search_main(void *arg)
{
	SearchJob *j = arg;
	Matcher m;
	matcher_init(&m, j->query, strlen(j->query), j->flags);
	search_text_ex(&j->res, j->root, &j->fl, &m, j->inc, j->exc, 20000, &j->cancel, &j->done);
	matcher_free(&m);
	atomic_store(&search_state, 2);
	return NULL;
}

static void job_free(SearchJob *j)
{
	if (!j)
		return;
	free(j->root);
	free(j->query);
	free(j->inc);
	free(j->exc);
	files_free(&j->fl);
	search_free(&j->res);
	free(j);
}

static void search_stop(void)
{
	if (atomic_load(&search_state) == 0)
		return;
	atomic_store(&job->cancel, 1);
	pthread_join(search_th, NULL);
	job_free(job);
	job = NULL;
	atomic_store(&search_state, 0);
}

void bg_search_start(const char *query, int flags, const char *inc, const char *exc)
{
	search_stop();
	SearchJob *j = vx_alloc(sizeof *j);
	j->root = vx_strdup(app.root);
	j->query = vx_strdup(query);
	j->inc = inc ? vx_strdup(inc) : NULL;
	j->exc = exc ? vx_strdup(exc) : NULL;
	j->flags = flags;
	j->fl = fl_copy(&app.files);
	j->total = app.files.n;
	job = j;
	atomic_store(&search_state, 1);
	if (pthread_create(&search_th, NULL, search_main, j) != 0) {
		atomic_store(&search_state, 0);
		job_free(j);
		job = NULL;
	}
}

bool bg_search_busy(u32 *done, u32 *total)
{
	if (atomic_load(&search_state) != 1)
		return false;
	if (done)
		*done = atomic_load(&job->done);
	if (total)
		*total = job->total;
	return true;
}

/* Swap finished results in; true when app.results changed. */
bool bg_search_poll(void)
{
	if (atomic_load(&search_state) != 2)
		return false;
	pthread_join(search_th, NULL);
	search_free(&app.results);
	app.results = job->res;
	memset(&job->res, 0, sizeof job->res);
	job_free(job);
	job = NULL;
	atomic_store(&search_state, 0);
	return true;
}

void bg_shutdown(void)
{
	search_stop();
	if (atomic_load(&scan_state) != 0) {
		pthread_join(scan_th, NULL);
		files_free(&scan_out);
		atomic_store(&scan_state, 0);
	}
	free(scan_root);
	free(scan_workdir);
}
