#define _POSIX_C_SOURCE 200809L
#include "blame.h"

#include <git2.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
	char *workdir;
	char *git_bin; /* absolute path of git, resolved once from PATH */
	bool no_cli; /* the git binary could not be run: use libgit2 */
	git_repository *repo;
	git_blame *base;
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

static u32 commit_index(Blame *b, Result *r, const git_oid *id, const git_signature *sig, u32 *cap)
{
	for (u32 i = 0; i < r->nc; i++) {
		char hx[12];
		git_oid_tostr(hx, sizeof hx, id);
		if (!strcmp(hx, r->hash[i]))
			return i;
	}
	if (r->nc == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		r->author = vx_realloc(r->author, *cap * sizeof(u32));
		r->summary = vx_realloc(r->summary, *cap * sizeof(u32));
		r->time = vx_realloc(r->time, *cap * sizeof(i64));
		r->hash = vx_realloc(r->hash, *cap * sizeof r->hash[0]);
	}
	u32 i = r->nc++;
	git_oid_tostr(r->hash[i], sizeof r->hash[i], id);
	const char *summary = "";
	git_commit *c = NULL;
	if (git_commit_lookup(&c, b->repo, id) == 0) {
		const char *s = git_commit_summary(c);
		if (s)
			summary = s;
		if (!sig)
			sig = git_commit_author(c);
	}
	const char *name = sig && sig->name ? sig->name : "";
	r->author[i] = pool_add(&r->pool, name, (u32)strlen(name));
	r->summary[i] = pool_add(&r->pool, summary, (u32)strlen(summary));
	r->time[i] = sig ? (i64)sig->when.time : 0;
	git_commit_free(c);
	return i;
}

static u32 add_commit(Result *r, u32 *cap, const char *hash, const char *author, const char *summary, i64 when)
{
	if (r->nc == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		r->author = vx_realloc(r->author, *cap * sizeof(u32));
		r->summary = vx_realloc(r->summary, *cap * sizeof(u32));
		r->time = vx_realloc(r->time, *cap * sizeof(i64));
		r->hash = vx_realloc(r->hash, *cap * sizeof r->hash[0]);
	}
	u32 i = r->nc++;
	snprintf(r->hash[i], sizeof r->hash[i], "%.*s", 10, hash);
	r->author[i] = pool_add(&r->pool, author, (u32)strlen(author));
	r->summary[i] = pool_add(&r->pool, summary, (u32)strlen(summary));
	r->time[i] = when;
	return i;
}

/* `git blame --incremental --contents <snapshot>`: what VSCodium runs. The
 * CLI uses the commit-graph's changed-path Bloom filters, which makes it
 * far faster than libgit2's blame on long histories. Returns false when
 * the git binary cannot be run (the caller falls back to libgit2). */
static bool compute_cli(Blame *b, const char *path, const char *text, size_t n, Result *r)
{
	char tmp[] = "/tmp/vxide-blame-XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, text + off, n - off);
		if (w <= 0)
			break;
		off += (size_t)w;
	}
	close(fd);
	int out[2];
	if (pipe(out) != 0) {
		unlink(tmp);
		return false;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_addclose(&fa, out[0]);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	char *argv[] = {b->git_bin, "-C", b->workdir, "blame", "--root", "--incremental", "--contents", tmp, "--",
	                (char *)path, NULL};
	pid_t pid;
	/* posix_spawn with an absolute path: no PATH search inside the
	 * vfork'd child (which also upsets ASan's allocator on FreeBSD) */
	int rc = posix_spawn(&pid, b->git_bin, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	if (rc != 0) {
		close(out[0]);
		unlink(tmp);
		return false;
	}
	Pool o = {0};
	char buf[65536];
	ssize_t k;
	while ((k = read(out[0], buf, sizeof buf)) > 0) {
		VX_GROW(o.b, o.cap, o.n + (u32)k + 1);
		memcpy(o.b + o.n, buf, (size_t)k);
		o.n += (u32)k;
	}
	close(out[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	unlink(tmp);
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) {
		pool_free(&o);
		return false;
	}
	if (WEXITSTATUS(status) != 0) {
		/* untracked file or not a repo path: nothing to show */
		pool_free(&o);
		return true;
	}
	VX_GROW(o.b, o.cap, o.n + 1);
	o.b[o.n] = 0;
	r->tracked = true;
	r->line_commit = vx_alloc(r->nlines * sizeof(u32));
	memset(r->line_commit, 0xFF, r->nlines * sizeof(u32));
	u32 cap = 0;
	/* entries: "<sha> <orig> <final> <count>" then headers until "filename" */
	char *p = o.b;
	char author[256] = "", summary[512] = "";
	i64 when = 0;
	while (*p) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		char sha[41];
		unsigned orig, fin, cnt;
		if (strlen(p) >= 40 && sscanf(p, "%40s %u %u %u", sha, &orig, &fin, &cnt) == 4) {
			bool zero = strspn(sha, "0") == 40;
			/* find a known commit or read its headers */
			u32 idx = UINT32_MAX;
			for (u32 i = 0; i < r->nc && !zero; i++)
				if (!strncmp(r->hash[i], sha, 10))
					idx = i;
			author[0] = summary[0] = 0;
			when = 0;
			char *q = e ? e + 1 : p + strlen(p);
			while (*q) {
				char *qe = strchr(q, '\n');
				if (qe)
					*qe = 0;
				if (!strncmp(q, "author ", 7))
					snprintf(author, sizeof author, "%s", q + 7);
				else if (!strncmp(q, "author-time ", 12))
					when = strtoll(q + 12, NULL, 10);
				else if (!strncmp(q, "summary ", 8))
					snprintf(summary, sizeof summary, "%s", q + 8);
				bool last = !strncmp(q, "filename ", 9);
				q = qe ? qe + 1 : q + strlen(q);
				if (last)
					break;
			}
			if (!zero && idx == UINT32_MAX)
				idx = add_commit(r, &cap, sha, author, summary, when);
			for (u32 l = fin - 1; l < fin - 1 + cnt && l < r->nlines; l++)
				r->line_commit[l] = zero ? UINT32_MAX : idx;
			p = q;
			continue;
		}
		p = e ? e + 1 : p + strlen(p);
	}
	pool_free(&o);
	return true;
}

static void compute(Blame *b, const char *path, const char *text, size_t n, u32 key, Result *r)
{
	memset(r, 0, sizeof *r);
	r->path = vx_strdup(path);
	r->key = key;
	r->nlines = 1;
	for (size_t i = 0; i < n; i++)
		r->nlines += text[i] == '\n';
	if (!b->no_cli && b->git_bin) {
		if (compute_cli(b, path, text, n, r))
			return;
		b->no_cli = true;
	}
	/* (re)blame the committed file when the path or HEAD changed */
	git_oid head;
	bool have_head = git_reference_name_to_id(&head, b->repo, "HEAD") == 0;
	if (!have_head)
		return; /* unborn branch: nothing to blame */
	if (!b->base || !b->base_path || strcmp(b->base_path, path) || !git_oid_equal(&head, &b->base_head)) {
		git_blame_free(b->base);
		b->base = NULL;
		free(b->base_path);
		b->base_path = vx_strdup(path);
		b->base_head = head;
		git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
		if (git_blame_file(&b->base, b->repo, path, &opts) != 0) {
			b->base = NULL;
			return; /* untracked or not in HEAD */
		}
	}
	git_blame *buf = NULL;
	if (git_blame_buffer(&buf, b->base, text, n) != 0)
		return;
	r->tracked = true;
	r->line_commit = vx_alloc(r->nlines * sizeof(u32));
	memset(r->line_commit, 0xFF, r->nlines * sizeof(u32));
	u32 cap = 0;
	u32 nh = git_blame_get_hunk_count(buf);
	for (u32 h = 0; h < nh; h++) {
		const git_blame_hunk *hk = git_blame_get_hunk_byindex(buf, h);
		if (!hk)
			continue;
		u32 idx = UINT32_MAX;
		if (!git_oid_is_zero(&hk->final_commit_id))
			idx = commit_index(b, r, &hk->final_commit_id, hk->final_signature, &cap);
		size_t first = hk->final_start_line_number ? hk->final_start_line_number - 1 : 0;
		for (size_t l = first; l < first + hk->lines_in_hunk && l < r->nlines; l++)
			r->line_commit[l] = idx;
	}
	git_blame_free(buf);
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

static char *find_in_path(const char *name)
{
	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/local/bin:/usr/bin:/bin";
	char buf[4096];
	while (*path) {
		const char *e = strchr(path, ':');
		size_t n = e ? (size_t)(e - path) : strlen(path);
		snprintf(buf, sizeof buf, "%.*s/%s", (int)(n ? n : 1), n ? path : ".", name);
		if (access(buf, X_OK) == 0)
			return vx_strdup(buf);
		path += n + (e ? 1 : 0);
	}
	return NULL;
}

Blame *blame_start(const char *workdir)
{
	git_libgit2_init();
	Blame *b = vx_alloc(sizeof *b);
	b->workdir = vx_strdup(workdir);
	b->git_bin = find_in_path("git");
	b->no_cli = getenv("VXIDE_BLAME_LIBGIT2") != NULL;
	if (git_repository_open(&b->repo, workdir) != 0) {
		free(b->workdir);
		free(b->git_bin);
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
	git_blame_free(b->base);
	free(b->base_path);
	git_repository_free(b->repo);
	free(b->workdir);
	free(b->git_bin);
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
