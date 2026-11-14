/* Native git integration on top of libgit2. See git.h.
 *
 * The status table is SoA sorted by path (strcmp order), so a file is a
 * binary search and a directory "dir" is the contiguous run of entries
 * starting with "dir/". Ignored paths are not in the status list (VSCodium
 * does not list them in SCM either); git_path_flags answers GS_IGNORED for
 * them through git_ignore_path_is_ignored, memoised in a small hash table
 * that is cleared on every git_refresh (one rule walk per distinct path per
 * refresh, so the explorer can ask every visible row on every frame). */
#define _POSIX_C_SOURCE 200809L
#include "git.h"

#include <git2.h>
#include <git2/sys/errors.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

struct Git {
	git_repository *repo;
	char *workdir;
	GitStatus st;
	char branch[256];
	u32 ahead, behind;
	/* ignore memo: open addressing, key = pool offset, val = 0/1 */
	Pool ign_pool;
	u32 *ign_key; /* offset + 1, 0 = empty */
	u8 *ign_val;
	u32 ign_cap, ign_n;
};

static int g_lib_refs;
static char g_err[512];

static void set_err(const char *what)
{
	const git_error *e = git_error_last();
	snprintf(g_err, sizeof g_err, "%s%s%s", what,
	         (e && e->message) ? ": " : "", (e && e->message) ? e->message : "");
}

const char *git_last_error(void) { return g_err; }

static void status_free(GitStatus *st)
{
	free(st->path);
	free(st->flags);
	pool_free(&st->pool);
	memset(st, 0, sizeof *st);
}

static void ign_clear(Git *g)
{
	pool_clear(&g->ign_pool);
	if (g->ign_key)
		memset(g->ign_key, 0, g->ign_cap * sizeof *g->ign_key);
	g->ign_n = 0;
}

Git *git_open(const char *dir)
{
	if (g_lib_refs++ == 0)
		git_libgit2_init();
	git_repository *repo = NULL;
	if (git_repository_open_ext(&repo, dir, 0, NULL) < 0 || git_repository_is_bare(repo)) {
		set_err("open");
		git_repository_free(repo);
		if (--g_lib_refs == 0)
			git_libgit2_shutdown();
		return NULL;
	}
	Git *g = vx_alloc(sizeof *g);
	g->repo = repo;
	g->workdir = vx_strdup(git_repository_workdir(repo));
	git_refresh(g);
	return g;
}

void git_close(Git *g)
{
	if (!g)
		return;
	status_free(&g->st);
	pool_free(&g->ign_pool);
	free(g->ign_key);
	free(g->ign_val);
	free(g->workdir);
	git_repository_free(g->repo);
	free(g);
	if (--g_lib_refs == 0)
		git_libgit2_shutdown();
}

const char *git_workdir(Git *g) { return g->workdir; }

static u16 map_status(unsigned s)
{
	u16 f = 0;
	if (s & GIT_STATUS_INDEX_NEW) f |= GS_INDEX_NEW;
	if (s & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_TYPECHANGE)) f |= GS_INDEX_MODIFIED;
	if (s & GIT_STATUS_INDEX_DELETED) f |= GS_INDEX_DELETED;
	if (s & GIT_STATUS_INDEX_RENAMED) f |= GS_INDEX_RENAMED;
	if (s & GIT_STATUS_WT_NEW) f |= GS_WT_NEW;
	if (s & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_TYPECHANGE)) f |= GS_WT_MODIFIED;
	if (s & GIT_STATUS_WT_DELETED) f |= GS_WT_DELETED;
	if (s & GIT_STATUS_WT_RENAMED) f |= GS_WT_RENAMED;
	if (s & GIT_STATUS_IGNORED) f |= GS_IGNORED;
	if (s & GIT_STATUS_CONFLICTED) f |= GS_CONFLICT;
	return f;
}

static const char *g_sort_pool;
static int cmp_entry(const void *a, const void *b)
{
	const u32 *x = a, *y = b; /* pairs: path offset, flags */
	return strcmp(g_sort_pool + x[0], g_sort_pool + y[0]);
}

static void refresh_status(Git *g)
{
	GitStatus *st = &g->st;
	st->count = 0;
	pool_clear(&st->pool);

	git_status_options opt = GIT_STATUS_OPTIONS_INIT;
	opt.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opt.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
	            GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
	git_status_list *list = NULL;
	if (git_status_list_new(&list, g->repo, &opt) < 0) {
		set_err("status");
		return;
	}
	size_t n = git_status_list_entrycount(list);
	u32 *pairs = vx_alloc((n ? n : 1) * 2 * sizeof(u32));
	u32 k = 0;
	for (size_t i = 0; i < n; i++) {
		const git_status_entry *e = git_status_byindex(list, i);
		const char *p = NULL;
		if (e->index_to_workdir)
			p = e->index_to_workdir->new_file.path;
		if (!p && e->head_to_index)
			p = e->head_to_index->new_file.path;
		if (!p)
			continue;
		u16 f = map_status(e->status);
		if (!f)
			continue;
		pairs[2 * k] = pool_add(&st->pool, p, (u32)strlen(p));
		pairs[2 * k + 1] = f;
		k++;
	}
	git_status_list_free(list);

	g_sort_pool = st->pool.b;
	if (k > 1)
		qsort(pairs, k, 2 * sizeof(u32), cmp_entry);
	free(st->path);
	free(st->flags);
	st->path = vx_alloc((k ? k : 1) * sizeof *st->path);
	st->flags = vx_alloc((k ? k : 1) * sizeof *st->flags);
	for (u32 i = 0; i < k; i++) {
		st->path[i] = pairs[2 * i];
		st->flags[i] = (u16)pairs[2 * i + 1];
	}
	st->count = k;
	free(pairs);
}

static void refresh_branch(Git *g)
{
	g->branch[0] = 0;
	g->ahead = g->behind = 0;
	git_reference *head = NULL;
	int r = git_repository_head(&head, g->repo);
	if (r == GIT_EUNBORNBRANCH || r == GIT_ENOTFOUND) {
		git_reference *sym = NULL;
		if (git_reference_lookup(&sym, g->repo, "HEAD") == 0) {
			const char *t = git_reference_symbolic_target(sym);
			if (t) {
				if (strncmp(t, "refs/heads/", 11) == 0)
					t += 11;
				snprintf(g->branch, sizeof g->branch, "%s", t);
			}
			git_reference_free(sym);
		}
		return;
	}
	if (r < 0) {
		set_err("head");
		return;
	}
	if (git_repository_head_detached(g->repo)) {
		const git_oid *id = git_reference_target(head);
		if (id)
			git_oid_tostr(g->branch, 8, id);
	} else {
		snprintf(g->branch, sizeof g->branch, "%s", git_reference_shorthand(head));
		git_reference *up = NULL;
		if (git_branch_upstream(&up, head) == 0) {
			const git_oid *a = git_reference_target(head), *b = git_reference_target(up);
			size_t ah = 0, bh = 0;
			if (a && b && git_graph_ahead_behind(&ah, &bh, g->repo, a, b) == 0) {
				g->ahead = (u32)ah;
				g->behind = (u32)bh;
			}
			git_reference_free(up);
		}
		git_error_clear();
	}
	git_reference_free(head);
}

int git_refresh(Git *g)
{
	g_err[0] = 0;
	refresh_status(g);
	refresh_branch(g);
	ign_clear(g);
	return g_err[0] ? -1 : 0;
}

const GitStatus *git_status(Git *g) { return &g->st; }
const char *git_branch(Git *g) { return g->branch; }

void git_ahead_behind(Git *g, u32 *ahead, u32 *behind)
{
	if (ahead) *ahead = g->ahead;
	if (behind) *behind = g->behind;
}

/* First index whose path is >= key (strcmp order). */
static u32 lower_bound(const GitStatus *st, const char *key)
{
	u32 lo = 0, hi = st->count;
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		if (strcmp(st->pool.b + st->path[mid], key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static u32 hash_str(const char *s, size_t n)
{
	u32 h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (u8)s[i]) * 16777619u;
	return h;
}

static bool is_ignored(Git *g, const char *rel, size_t n)
{
	if (g->ign_n * 2 + 2 > g->ign_cap) {
		u32 ncap = g->ign_cap ? g->ign_cap * 2 : 256;
		u32 *ok = g->ign_key;
		u8 *ov = g->ign_val;
		u32 ocap = g->ign_cap;
		g->ign_key = vx_alloc(ncap * sizeof *g->ign_key);
		g->ign_val = vx_alloc(ncap);
		g->ign_cap = ncap;
		for (u32 i = 0; i < ocap; i++) {
			if (!ok[i])
				continue;
			const char *s = g->ign_pool.b + ok[i] - 1;
			u32 j = hash_str(s, strlen(s)) & (ncap - 1);
			while (g->ign_key[j])
				j = (j + 1) & (ncap - 1);
			g->ign_key[j] = ok[i];
			g->ign_val[j] = ov[i];
		}
		free(ok);
		free(ov);
	}
	u32 mask = g->ign_cap - 1, j = hash_str(rel, n) & mask;
	while (g->ign_key[j]) {
		const char *s = g->ign_pool.b + g->ign_key[j] - 1;
		if (strlen(s) == n && memcmp(s, rel, n) == 0)
			return g->ign_val[j];
		j = (j + 1) & mask;
	}
	char *tmp = vx_strndup(rel, n);
	int ig = 0;
	if (git_ignore_path_is_ignored(&ig, g->repo, tmp) < 0)
		ig = 0;
	/* Directory rules ("build/") only match when libgit2 knows it is a
	 * directory: retry with a trailing slash when the path is one. */
	if (!ig) {
		size_t wl = strlen(g->workdir);
		char *abs = vx_alloc(wl + n + 1);
		memcpy(abs, g->workdir, wl);
		memcpy(abs + wl, rel, n);
		struct stat sb;
		if (stat(abs, &sb) == 0 && S_ISDIR(sb.st_mode)) {
			char *d = vx_alloc(n + 2);
			memcpy(d, rel, n);
			d[n] = '/';
			if (git_ignore_path_is_ignored(&ig, g->repo, d) < 0)
				ig = 0;
			free(d);
		}
		free(abs);
	}
	git_error_clear();
	free(tmp);
	g->ign_key[j] = pool_add(&g->ign_pool, rel, (u32)n) + 1;
	g->ign_val[j] = ig ? 1 : 0;
	g->ign_n++;
	return ig != 0;
}

u16 git_path_flags(Git *g, const char *relpath)
{
	const GitStatus *st = &g->st;
	size_t n = strlen(relpath);
	while (n && relpath[n - 1] == '/')
		n--;
	if (n == 0) {
		u16 f = 0;
		for (u32 i = 0; i < st->count; i++)
			f |= st->flags[i];
		return f;
	}
	/* Key buffer: path + "/" for the directory range. */
	char stackbuf[512];
	char *key = n + 2 <= sizeof stackbuf ? stackbuf : vx_alloc(n + 2);
	memcpy(key, relpath, n);
	key[n] = 0;
	u16 f = 0;
	u32 i = lower_bound(st, key);
	if (i < st->count && strcmp(st->pool.b + st->path[i], key) == 0)
		f |= st->flags[i];
	key[n] = '/';
	key[n + 1] = 0;
	for (i = lower_bound(st, key); i < st->count; i++) {
		if (strncmp(st->pool.b + st->path[i], key, n + 1) != 0)
			break;
		f |= st->flags[i];
	}
	if (!f && is_ignored(g, relpath, n))
		f = GS_IGNORED;
	if (key != stackbuf)
		free(key);
	return f;
}

/* ---- index operations ---- */

static git_index *open_index(Git *g)
{
	git_index *idx = NULL;
	if (git_repository_index(&idx, g->repo) < 0) {
		set_err("index");
		return NULL;
	}
	if (git_index_read(idx, 0) < 0) {
		set_err("index read");
		git_index_free(idx);
		return NULL;
	}
	return idx;
}

static bool wt_exists(Git *g, const char *rel)
{
	size_t wl = strlen(g->workdir), n = strlen(rel);
	char *abs = vx_alloc(wl + n + 1);
	memcpy(abs, g->workdir, wl);
	memcpy(abs + wl, rel, n);
	struct stat sb;
	bool ok = lstat(abs, &sb) == 0;
	free(abs);
	return ok;
}

int git_stage(Git *g, const char *relpath)
{
	git_index *idx = open_index(g);
	if (!idx)
		return -1;
	int r = wt_exists(g, relpath) ? git_index_add_bypath(idx, relpath)
	                              : git_index_remove_bypath(idx, relpath);
	if (r == 0)
		r = git_index_write(idx);
	if (r < 0)
		set_err("stage");
	git_index_free(idx);
	return r < 0 ? -1 : 0;
}

int git_stage_all(Git *g)
{
	git_index *idx = open_index(g);
	if (!idx)
		return -1;
	char *star = "*";
	git_strarray ps = { &star, 1 };
	int r = git_index_add_all(idx, &ps, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
	if (r == 0)
		r = git_index_update_all(idx, &ps, NULL, NULL);
	if (r == 0)
		r = git_index_write(idx);
	if (r < 0)
		set_err("stage all");
	git_index_free(idx);
	return r < 0 ? -1 : 0;
}

/* HEAD commit object, or NULL when unborn. */
static git_object *head_commit(Git *g)
{
	git_object *obj = NULL;
	if (git_revparse_single(&obj, g->repo, "HEAD^{commit}") < 0) {
		git_error_clear();
		return NULL;
	}
	return obj;
}

int git_unstage(Git *g, const char *relpath)
{
	git_object *head = head_commit(g);
	int r;
	if (head) {
		char *p = (char *)relpath;
		git_strarray ps = { &p, 1 };
		r = git_reset_default(g->repo, head, &ps);
		git_object_free(head);
	} else {
		git_index *idx = open_index(g);
		if (!idx)
			return -1;
		r = git_index_remove_bypath(idx, relpath);
		if (r == 0)
			r = git_index_write(idx);
		git_index_free(idx);
	}
	if (r < 0)
		set_err("unstage");
	return r < 0 ? -1 : 0;
}

int git_discard(Git *g, const char *relpath)
{
	git_index *idx = open_index(g);
	if (!idx)
		return -1;
	bool tracked = git_index_get_bypath(idx, relpath, 0) != NULL;
	git_index_free(idx);
	if (!tracked) {
		size_t wl = strlen(g->workdir), n = strlen(relpath);
		char *abs = vx_alloc(wl + n + 1);
		memcpy(abs, g->workdir, wl);
		memcpy(abs + wl, relpath, n);
		int r = unlink(abs);
		free(abs);
		if (r < 0)
			snprintf(g_err, sizeof g_err, "discard: cannot delete %s", relpath);
		return r < 0 ? -1 : 0;
	}
	git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
	co.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
	char *p = (char *)relpath;
	co.paths.strings = &p;
	co.paths.count = 1;
	int r = git_checkout_index(g->repo, NULL, &co);
	if (r < 0)
		set_err("discard");
	return r < 0 ? -1 : 0;
}

int git_commit_staged(Git *g, const char *message)
{
	int ret = -1;
	git_signature *sig = NULL;
	git_index *idx = NULL;
	git_tree *tree = NULL;
	git_object *head = NULL;
	git_buf msg = GIT_BUF_INIT;
	git_oid tree_id, commit_id;

	if (git_message_prettify(&msg, message ? message : "", 1, '#') < 0 || msg.size == 0) {
		snprintf(g_err, sizeof g_err, "commit: empty commit message");
		goto out;
	}
	if (git_signature_default(&sig, g->repo) < 0) {
		set_err("commit: no user.name/user.email configured");
		goto out;
	}
	if (!(idx = open_index(g)))
		goto out;
	if (git_index_write_tree(&tree_id, idx) < 0) {
		set_err("commit: write tree");
		goto out;
	}
	head = head_commit(g);
	if (head) {
		const git_oid *ht = git_commit_tree_id((const git_commit *)head);
		if (git_oid_equal(ht, &tree_id)) {
			snprintf(g_err, sizeof g_err, "commit: nothing staged");
			goto out;
		}
	} else if (git_index_entrycount(idx) == 0) {
		snprintf(g_err, sizeof g_err, "commit: nothing staged");
		goto out;
	}
	if (git_tree_lookup(&tree, g->repo, &tree_id) < 0) {
		set_err("commit: tree lookup");
		goto out;
	}
	const git_commit *parents[1] = { (const git_commit *)head };
	if (git_commit_create(&commit_id, g->repo, "HEAD", sig, sig, NULL, msg.ptr, tree,
	                      head ? 1 : 0, head ? parents : NULL) < 0) {
		set_err("commit");
		goto out;
	}
	ret = 0;
out:
	git_buf_dispose(&msg);
	git_tree_free(tree);
	git_object_free(head);
	git_index_free(idx);
	git_signature_free(sig);
	return ret;
}

/* ---- quick diff ---- */

typedef struct {
	u8 *marks;
	u32 nlines;
} MarkCtx;

static int hunk_cb(const git_diff_delta *d, const git_diff_hunk *h, void *payload)
{
	(void)d;
	MarkCtx *c = payload;
	if (c->nlines == 0)
		return 0;
	if (h->old_lines == 0 || h->new_lines == 0) {
		if (h->old_lines == 0) {
			/* pure addition: new_start is 1-based first added line */
			for (int i = 0; i < h->new_lines; i++) {
				u32 l = (u32)(h->new_start - 1 + i);
				if (l < c->nlines)
					c->marks[l] = GM_ADDED;
			}
		} else {
			/* pure deletion: new_start = 1-based line after which lines
			 * were removed, i.e. 0-based index of the line below them. */
			u32 l = h->new_start < 0 ? 0 : (u32)h->new_start;
			if (l >= c->nlines)
				l = c->nlines - 1;
			if (c->marks[l] == GM_NONE)
				c->marks[l] = GM_DELETED_ABOVE;
		}
		return 0;
	}
	for (int i = 0; i < h->new_lines; i++) {
		u32 l = (u32)(h->new_start - 1 + i);
		if (l < c->nlines)
			c->marks[l] = GM_MODIFIED;
	}
	return 0;
}

int git_line_marks(Git *g, const char *relpath, const char *text, size_t n, u8 *marks,
                   u32 nlines)
{
	memset(marks, GM_NONE, nlines);
	git_index *idx = open_index(g);
	if (!idx)
		return -1;
	const git_index_entry *e = git_index_get_bypath(idx, relpath, 0);
	if (!e) {
		git_index_free(idx);
		memset(marks, GM_ADDED, nlines);
		return 0;
	}
	git_blob *blob = NULL;
	int r = git_blob_lookup(&blob, g->repo, &e->id);
	git_index_free(idx);
	if (r < 0) {
		set_err("blob");
		return -1;
	}
	git_diff_options opt = GIT_DIFF_OPTIONS_INIT;
	opt.context_lines = 0;
	opt.flags = GIT_DIFF_FORCE_TEXT;
	MarkCtx c = { marks, nlines };
	r = git_diff_blob_to_buffer(blob, relpath, text, n, relpath, &opt, NULL, NULL, hunk_cb,
	                            NULL, &c);
	git_blob_free(blob);
	if (r < 0) {
		set_err("diff");
		return -1;
	}
	return 0;
}

char *git_diff_text(Git *g, const char *relpath, int staged)
{
	git_diff_options opt = GIT_DIFF_OPTIONS_INIT;
	char *p = (char *)relpath;
	opt.pathspec.strings = &p;
	opt.pathspec.count = 1;
	opt.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED |
	            GIT_DIFF_SHOW_UNTRACKED_CONTENT | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
	git_diff *diff = NULL;
	int r;
	if (staged) {
		git_object *head = head_commit(g);
		git_tree *tree = NULL;
		if (head && git_commit_tree(&tree, (const git_commit *)head) < 0)
			tree = NULL;
		r = git_diff_tree_to_index(&diff, g->repo, tree, NULL, &opt);
		git_tree_free(tree);
		git_object_free(head);
	} else {
		r = git_diff_index_to_workdir(&diff, g->repo, NULL, &opt);
	}
	if (r < 0) {
		set_err("diff");
		return NULL;
	}
	git_buf buf = GIT_BUF_INIT;
	char *out = NULL;
	if (git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH) == 0)
		out = vx_strndup(buf.ptr ? buf.ptr : "", buf.size);
	else
		set_err("diff format");
	git_buf_dispose(&buf);
	git_diff_free(diff);
	return out;
}
