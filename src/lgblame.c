#define _DEFAULT_SOURCE /* sysconf(_SC_NPROCESSORS_ONLN) is a BSD/Linux extension */
#include "lgblame.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

/* ---- origins: (commit, path) with pending line ranges ------------------------- */

typedef struct {
	git_repository *repo;
	/* origins (SoA) */
	u32 no, co;
	git_oid *o_commit;
	git_oid *o_blob;
	i64 *o_time;
	u32 *o_path; /* pool offset */
	u32 *o_head; /* first pending range */
	u8 *o_queued;
	Pool paths;
	/* origin lookup: open addressing, key = commit oid + path */
	u32 *ohash;
	u32 ohash_cap;
	/* ranges (SoA): final line, length, line in the origin's blob, next */
	u32 nr, cr;
	u32 *r_final, *r_len, *r_src, *r_next;
	/* heap of origins by commit time */
	u32 *heap;
	u32 nh, ch;
	/* tree-path memo: (tree oid, path offset) -> blob oid */
	u32 mcap, mn;
	git_oid *m_tree, *m_blob;
	u32 *m_path;
	u8 *m_found; /* 0 empty, 1 found, 2 absent */
	LgBlame *out;
	u32 remaining;
	/* hunks of the current diff */
	u32 nhk, chk;
	u32 *h_os, *h_ol, *h_ns, *h_nl;
} Ctx;

static u32 oid_hash(const git_oid *id, u32 salt)
{
	u32 h;
	memcpy(&h, id->id, sizeof h);
	return h ^ (salt * 2654435761u);
}

/* ---- tree path lookups, memoised by tree id ---------------------------------------- */

static int blob_at_tree(Ctx *c, const git_oid *tree_id, u32 path_off, git_oid *blob)
{
	if (!c->mcap) {
		c->mcap = 4096;
		c->m_tree = vx_alloc(c->mcap * sizeof(git_oid));
		c->m_blob = vx_alloc(c->mcap * sizeof(git_oid));
		c->m_path = vx_alloc(c->mcap * sizeof(u32));
		c->m_found = vx_alloc(c->mcap);
	}
	u32 h = oid_hash(tree_id, path_off) & (c->mcap - 1);
	while (c->m_found[h]) {
		if (c->m_path[h] == path_off && git_oid_equal(&c->m_tree[h], tree_id)) {
			if (c->m_found[h] == 1)
				*blob = c->m_blob[h];
			return c->m_found[h] == 1 ? 0 : 1;
		}
		h = (h + 1) & (c->mcap - 1);
	}
	/* miss: resolve one component, recurse into the subtree */
	const char *path = c->paths.b + path_off;
	const char *slash = strchr(path, '/');
	git_tree *t = NULL;
	int rc = 1;
	git_oid res;
	if (git_tree_lookup(&t, c->repo, tree_id) == 0) {
		c->out->trees_read++;
		const git_tree_entry *e;
		if (slash) {
			char comp[512];
			size_t n = (size_t)(slash - path);
			if (n < sizeof comp) {
				memcpy(comp, path, n);
				comp[n] = 0;
				e = git_tree_entry_byname(t, comp);
				if (e && git_tree_entry_type(e) == GIT_OBJECT_TREE) {
					git_oid sub = *git_tree_entry_id(e);
					git_tree_free(t);
					t = NULL;
					/* the rest of the path is a separate memo key */
					u32 rest = path_off + (u32)n + 1;
					rc = blob_at_tree(c, &sub, rest, &res);
				}
			}
		} else {
			e = git_tree_entry_byname(t, path);
			if (e && git_tree_entry_type(e) == GIT_OBJECT_BLOB) {
				res = *git_tree_entry_id(e);
				rc = 0;
			}
		}
		git_tree_free(t);
	}
	if (rc < 0)
		rc = 1;
	/* store (grow at 50% load) */
	if (c->mn * 2 >= c->mcap) {
		u32 oc = c->mcap;
		git_oid *ot = c->m_tree, *ob = c->m_blob;
		u32 *op = c->m_path;
		u8 *of = c->m_found;
		c->mcap *= 2;
		c->m_tree = vx_alloc(c->mcap * sizeof(git_oid));
		c->m_blob = vx_alloc(c->mcap * sizeof(git_oid));
		c->m_path = vx_alloc(c->mcap * sizeof(u32));
		c->m_found = vx_alloc(c->mcap);
		for (u32 i = 0; i < oc; i++)
			if (of[i]) {
				u32 k = oid_hash(&ot[i], op[i]) & (c->mcap - 1);
				while (c->m_found[k])
					k = (k + 1) & (c->mcap - 1);
				c->m_tree[k] = ot[i];
				c->m_blob[k] = ob[i];
				c->m_path[k] = op[i];
				c->m_found[k] = of[i];
			}
		free(ot);
		free(ob);
		free(op);
		free(of);
	}
	h = oid_hash(tree_id, path_off) & (c->mcap - 1);
	while (c->m_found[h])
		h = (h + 1) & (c->mcap - 1);
	c->m_tree[h] = *tree_id;
	c->m_path[h] = path_off;
	c->m_found[h] = rc == 0 ? 1 : 2;
	if (rc == 0)
		c->m_blob[h] = res;
	c->mn++;
	if (rc == 0)
		*blob = res;
	return rc;
}

/* ---- origins and ranges ------------------------------------------------------------- */

static u32 origin(Ctx *c, const git_commit *cm, u32 path_off, const git_oid *blob)
{
	const git_oid *id = git_commit_id(cm);
	if (c->no * 2 >= c->ohash_cap) {
		u32 nc = c->ohash_cap ? c->ohash_cap * 2 : 1024;
		free(c->ohash);
		c->ohash = vx_alloc(nc * sizeof(u32));
		memset(c->ohash, 0xFF, nc * sizeof(u32));
		c->ohash_cap = nc;
		for (u32 i = 0; i < c->no; i++) {
			u32 h = oid_hash(&c->o_commit[i], c->o_path[i]) & (nc - 1);
			while (c->ohash[h] != UINT32_MAX)
				h = (h + 1) & (nc - 1);
			c->ohash[h] = i;
		}
	}
	u32 h = oid_hash(id, path_off) & (c->ohash_cap - 1);
	while (c->ohash[h] != UINT32_MAX) {
		u32 o = c->ohash[h];
		if (c->o_path[o] == path_off && git_oid_equal(&c->o_commit[o], id))
			return o;
		h = (h + 1) & (c->ohash_cap - 1);
	}
	if (c->no == c->co) {
		u32 n = c->co ? c->co * 2 : 256;
		c->o_commit = vx_realloc(c->o_commit, n * sizeof(git_oid));
		c->o_blob = vx_realloc(c->o_blob, n * sizeof(git_oid));
		c->o_time = vx_realloc(c->o_time, n * sizeof(i64));
		c->o_path = vx_realloc(c->o_path, n * sizeof(u32));
		c->o_head = vx_realloc(c->o_head, n * sizeof(u32));
		c->o_queued = vx_realloc(c->o_queued, n);
		c->co = n;
	}
	u32 o = c->no++;
	c->o_commit[o] = *id;
	c->o_blob[o] = *blob;
	c->o_time[o] = (i64)git_commit_time(cm);
	c->o_path[o] = path_off;
	c->o_head[o] = UINT32_MAX;
	c->o_queued[o] = 0;
	c->ohash[h] = o;
	return o;
}

static void heap_push(Ctx *c, u32 o)
{
	if (c->o_queued[o])
		return;
	c->o_queued[o] = 1;
	VX_GROW(c->heap, c->ch, c->nh + 1);
	u32 i = c->nh++;
	c->heap[i] = o;
	while (i > 0) {
		u32 p = (i - 1) / 2;
		if (c->o_time[c->heap[p]] >= c->o_time[c->heap[i]])
			break;
		u32 t = c->heap[p];
		c->heap[p] = c->heap[i];
		c->heap[i] = t;
		i = p;
	}
}

static u32 heap_pop(Ctx *c)
{
	u32 top = c->heap[0];
	c->heap[0] = c->heap[--c->nh];
	u32 i = 0;
	for (;;) {
		u32 l = 2 * i + 1, r = l + 1, m = i;
		if (l < c->nh && c->o_time[c->heap[l]] > c->o_time[c->heap[m]])
			m = l;
		if (r < c->nh && c->o_time[c->heap[r]] > c->o_time[c->heap[m]])
			m = r;
		if (m == i)
			break;
		u32 t = c->heap[m];
		c->heap[m] = c->heap[i];
		c->heap[i] = t;
		i = m;
	}
	c->o_queued[top] = 0;
	return top;
}

static void add_range(Ctx *c, u32 o, u32 final, u32 len, u32 src)
{
	if (!len)
		return;
	if (c->nr == c->cr) {
		u32 n = c->cr ? c->cr * 2 : 1024;
		c->r_final = vx_realloc(c->r_final, n * sizeof(u32));
		c->r_len = vx_realloc(c->r_len, n * sizeof(u32));
		c->r_src = vx_realloc(c->r_src, n * sizeof(u32));
		c->r_next = vx_realloc(c->r_next, n * sizeof(u32));
		c->cr = n;
	}
	u32 r = c->nr++;
	c->r_final[r] = final;
	c->r_len[r] = len;
	c->r_src[r] = src;
	c->r_next[r] = c->o_head[o];
	c->o_head[o] = r;
}

/* Commit-table index for a commit (author, summary, time). */
static u32 commit_entry(Ctx *c, const git_oid *id)
{
	LgBlame *b = c->out;
	for (u32 i = b->nc; i-- > 0;)
		if (git_oid_equal(&b->oid[i], id))
			return i;
	u32 i = b->nc++;
	b->oid = vx_realloc(b->oid, b->nc * sizeof(git_oid));
	b->author = vx_realloc(b->author, b->nc * sizeof(u32));
	b->summary = vx_realloc(b->summary, b->nc * sizeof(u32));
	b->time = vx_realloc(b->time, b->nc * sizeof(i64));
	b->oid[i] = *id;
	git_commit *cm = NULL;
	const char *name = "", *sum = "";
	i64 t = 0;
	if (git_commit_lookup(&cm, c->repo, id) == 0) {
		const git_signature *s = git_commit_author(cm);
		if (s) {
			name = s->name ? s->name : "";
			t = (i64)s->when.time;
		}
		sum = git_commit_summary(cm);
		if (!sum)
			sum = "";
	}
	b->author[i] = pool_add(&b->pool, name, (u32)strlen(name));
	b->summary[i] = pool_add(&b->pool, sum, (u32)strlen(sum));
	b->time[i] = t;
	git_commit_free(cm);
	return i;
}

static void blame_ranges_to(Ctx *c, u32 head, const git_oid *commit)
{
	u32 idx = commit_entry(c, commit);
	for (u32 r = head; r != UINT32_MAX; r = c->r_next[r]) {
		for (u32 k = 0; k < c->r_len[r]; k++)
			c->out->line_commit[c->r_final[r] + k] = idx;
		c->remaining -= c->r_len[r];
	}
}

/* ---- blob diffs ------------------------------------------------------------------------ */

/* A range node outside any origin list (linked into `keep`). */
static u32 keep_push(Ctx *c, u32 keep, u32 final, u32 len, u32 src)
{
	if (c->nr == c->cr) {
		u32 n = c->cr ? c->cr * 2 : 1024;
		c->r_final = vx_realloc(c->r_final, n * sizeof(u32));
		c->r_len = vx_realloc(c->r_len, n * sizeof(u32));
		c->r_src = vx_realloc(c->r_src, n * sizeof(u32));
		c->r_next = vx_realloc(c->r_next, n * sizeof(u32));
		c->cr = n;
	}
	u32 q = c->nr++;
	c->r_final[q] = final;
	c->r_len[q] = len;
	c->r_src[q] = src;
	c->r_next[q] = keep;
	return q;
}

/* Pass ranges (in the child's blob lines) through the current hunks:
 * lines in unchanged segments go to parent origin p (renumbered), lines
 * the child changed are returned as a new list. Hunk new_start is
 * 1-based; for a pure deletion it names the line before the gap. */
static u32 pass_through(Ctx *c, u32 list, u32 p)
{
	u32 keep = UINT32_MAX;
	for (u32 r = list; r != UINT32_MAX;) {
		u32 next = c->r_next[r];
		u32 a = c->r_src[r], end = a + c->r_len[r], fin = c->r_final[r];
		i64 delta = 0;    /* old line = new line + delta in unchanged segments */
		u32 prev_end = 0; /* end of the previous changed region */
		for (u32 h = 0; h <= c->nhk; h++) {
			u32 cs, ce;
			if (h < c->nhk) {
				u32 ns = c->h_ns[h], nl = c->h_nl[h];
				cs = nl ? ns - 1 : ns;
				ce = nl ? ns - 1 + nl : ns;
			} else {
				cs = ce = UINT32_MAX;
			}
			/* unchanged [prev_end, cs) */
			u32 x0 = VX_MAX(prev_end, a), x1 = VX_MIN(cs, end);
			if (x0 < x1)
				add_range(c, p, fin + (x0 - a), x1 - x0, (u32)((i64)x0 + delta));
			/* changed [cs, ce): stays with the child */
			x0 = VX_MAX(cs, a);
			x1 = VX_MIN(ce, end);
			if (h < c->nhk && x0 < x1)
				keep = keep_push(c, keep, fin + (x0 - a), x1 - x0, x0);
			if (h < c->nhk) {
				delta += (i64)c->h_ol[h] - (i64)c->h_nl[h];
				prev_end = ce;
			}
			if (prev_end >= end)
				break;
		}
		r = next;
	}
	return keep;
}

/* Old path of `path` in parent tree when it was renamed in this commit. */
static int find_rename(Ctx *c, git_tree *ptree, git_tree *tree, const char *path, char *old, size_t cap)
{
	git_diff *d = NULL;
	git_diff_options o = GIT_DIFF_OPTIONS_INIT;
	int found = 0;
	if (git_diff_tree_to_tree(&d, c->repo, ptree, tree, &o) != 0)
		return 0;
	git_diff_find_options fo = GIT_DIFF_FIND_OPTIONS_INIT;
	fo.flags = GIT_DIFF_FIND_RENAMES;
	if (git_diff_find_similar(d, &fo) == 0) {
		size_t n = git_diff_num_deltas(d);
		for (size_t i = 0; i < n; i++) {
			const git_diff_delta *dl = git_diff_get_delta(d, i);
			if (dl->status == GIT_DELTA_RENAMED && !strcmp(dl->new_file.path, path)) {
				snprintf(old, cap, "%s", dl->old_file.path);
				found = 1;
				break;
			}
		}
	}
	git_diff_free(d);
	return found;
}

static u32 count_lines(git_repository *repo, const git_oid *blob)
{
	git_blob *b = NULL;
	if (git_blob_lookup(&b, repo, blob) != 0)
		return 0;
	const char *s = git_blob_rawcontent(b);
	git_object_size_t n = git_blob_rawsize(b);
	u32 lines = 0;
	for (git_object_size_t i = 0; i < n; i++)
		lines += s[i] == '\n';
	if (n && s[n - 1] != '\n')
		lines++;
	git_blob_free(b);
	return lines;
}

/* ---- the origin graph (pass 1) and parallel diffs (pass 2) --------------------------- */

/* Edges child origin -> parent origin; job = blob diff to apply (or
 * UINT32_MAX when the blobs are equal: TREESAME). */
typedef struct {
	u32 n, cap;
	u32 *from, *to, *job;
} Edges;

typedef struct {
	u32 n, cap;
	git_oid *old_blob, *new_blob;
	/* results: hunks per job */
	u32 *nh;
	u32 **os, **ol, **ns, **nl;
	_Atomic u32 next;
	const char *repo_path;
} Jobs;

static void edge_add(Edges *e, u32 from, u32 to, u32 job)
{
	if (e->n == e->cap) {
		e->cap = e->cap ? e->cap * 2 : 1024;
		e->from = vx_realloc(e->from, e->cap * sizeof(u32));
		e->to = vx_realloc(e->to, e->cap * sizeof(u32));
		e->job = vx_realloc(e->job, e->cap * sizeof(u32));
	}
	e->from[e->n] = from;
	e->to[e->n] = to;
	e->job[e->n] = job;
	e->n++;
}

static u32 job_add(Jobs *j, const git_oid *old_b, const git_oid *new_b)
{
	if (j->n == j->cap) {
		j->cap = j->cap ? j->cap * 2 : 256;
		j->old_blob = vx_realloc(j->old_blob, j->cap * sizeof(git_oid));
		j->new_blob = vx_realloc(j->new_blob, j->cap * sizeof(git_oid));
	}
	j->old_blob[j->n] = *old_b;
	j->new_blob[j->n] = *new_b;
	return j->n++;
}

typedef struct {
	u32 n, cap;
	u32 *os, *ol, *ns, *nl;
} HunkBuf;

static int job_hunk(const git_diff_delta *d, const git_diff_hunk *h, void *p)
{
	(void)d;
	HunkBuf *b = p;
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 32;
		b->os = vx_realloc(b->os, b->cap * sizeof(u32));
		b->ol = vx_realloc(b->ol, b->cap * sizeof(u32));
		b->ns = vx_realloc(b->ns, b->cap * sizeof(u32));
		b->nl = vx_realloc(b->nl, b->cap * sizeof(u32));
	}
	b->os[b->n] = (u32)h->old_start;
	b->ol[b->n] = (u32)h->old_lines;
	b->ns[b->n] = (u32)h->new_start;
	b->nl[b->n] = (u32)h->new_lines;
	b->n++;
	return 0;
}

static void *job_worker(void *arg)
{
	Jobs *j = arg;
	git_repository *repo = NULL;
	if (git_repository_open(&repo, j->repo_path) != 0)
		return NULL;
	git_diff_options o = GIT_DIFF_OPTIONS_INIT;
	o.context_lines = 0;
	o.interhunk_lines = 0;
	o.flags = GIT_DIFF_FORCE_TEXT; /* git blame treats every file as text */
	for (;;) {
		u32 k = atomic_fetch_add(&j->next, 1);
		if (k >= j->n)
			break;
		git_blob *ob = NULL, *nb = NULL;
		HunkBuf hb = {0};
		if (git_blob_lookup(&ob, repo, &j->old_blob[k]) == 0 && git_blob_lookup(&nb, repo, &j->new_blob[k]) == 0)
			git_diff_blobs(ob, NULL, nb, NULL, &o, NULL, NULL, job_hunk, NULL, &hb);
		git_blob_free(ob);
		git_blob_free(nb);
		j->nh[k] = hb.n;
		j->os[k] = hb.os;
		j->ol[k] = hb.ol;
		j->ns[k] = hb.ns;
		j->nl[k] = hb.nl;
	}
	git_repository_free(repo);
	return NULL;
}

static void run_jobs(Jobs *j)
{
	j->nh = vx_alloc((j->n + 1) * sizeof(u32));
	j->os = vx_alloc((j->n + 1) * sizeof(u32 *));
	j->ol = vx_alloc((j->n + 1) * sizeof(u32 *));
	j->ns = vx_alloc((j->n + 1) * sizeof(u32 *));
	j->nl = vx_alloc((j->n + 1) * sizeof(u32 *));
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	u32 nt = (u32)VX_CLAMP(cpus, 1, 8);
	if (nt > j->n)
		nt = j->n ? j->n : 1;
	atomic_store(&j->next, 0);
	pthread_t th[8];
	u32 started = 0;
	for (u32 t = 1; t < nt; t++)
		if (pthread_create(&th[started], NULL, job_worker, j) == 0)
			started++;
	job_worker(j); /* this thread works too */
	for (u32 t = 0; t < started; t++)
		pthread_join(th[t], NULL);
}

static void jobs_free(Jobs *j)
{
	for (u32 k = 0; k < j->n && j->os; k++) {
		free(j->os[k]);
		free(j->ol[k]);
		free(j->ns[k]);
		free(j->nl[k]);
	}
	free(j->os);
	free(j->ol);
	free(j->ns);
	free(j->nl);
	free(j->nh);
	free(j->old_blob);
	free(j->new_blob);
}

/* Per-origin side tables, kept as long as the origin table (zero-filled). */
static void sync_tabs(Ctx *c, u8 **visited, u32 **efirst, u32 **ecount, u32 *tcap)
{
	if (*tcap >= c->co)
		return;
	u32 n = c->co;
	*visited = vx_realloc(*visited, n);
	*efirst = vx_realloc(*efirst, n * sizeof(u32));
	*ecount = vx_realloc(*ecount, n * sizeof(u32));
	memset(*visited + *tcap, 0, n - *tcap);
	memset(*efirst + *tcap, 0, (n - *tcap) * sizeof(u32));
	memset(*ecount + *tcap, 0, (n - *tcap) * sizeof(u32));
	*tcap = n;
}

int lg_blame_head(git_repository *repo, const char *path, LgBlame *out)
{
	memset(out, 0, sizeof *out);
	Ctx c = {0};
	c.repo = repo;
	c.out = out;
	git_commit *head = NULL;
	git_oid hid;
	if (git_reference_name_to_id(&hid, repo, "HEAD") != 0 || git_commit_lookup(&head, repo, &hid) != 0)
		return 1;
	u32 p0 = pool_add(&c.paths, path, (u32)strlen(path));
	git_oid blob;
	if (blob_at_tree(&c, git_commit_tree_id(head), p0, &blob) != 0) {
		git_commit_free(head);
		pool_free(&c.paths);
		free(c.m_tree);
		free(c.m_blob);
		free(c.m_path);
		free(c.m_found);
		return 1;
	}
	out->head_blob = blob;
	out->nlines = count_lines(repo, &blob);
	out->line_commit = vx_alloc((out->nlines + 1) * sizeof(u32));
	memset(out->line_commit, 0xFF, (out->nlines + 1) * sizeof(u32));
	u32 o0 = origin(&c, head, p0, &blob);
	git_commit_free(head);

	/* pass 1: walk the history of the path: origins, edges, diff jobs */
	Edges E = {0};
	Jobs J = {0};
	J.repo_path = git_repository_path(repo);
	u32 *efirst = NULL, *ecount = NULL, tcap = 0;
	u8 *visited = NULL;
	heap_push(&c, o0);
	while (c.nh) {
		u32 o = heap_pop(&c);
		sync_tabs(&c, &visited, &efirst, &ecount, &tcap);
		if (visited[o])
			continue;
		visited[o] = 1;
		out->commits_visited++;
		efirst[o] = E.n;
		ecount[o] = 0;
		git_commit *cm = NULL;
		if (git_commit_lookup(&cm, repo, &c.o_commit[o]) != 0)
			continue;
		u32 np = VX_MIN((u32)git_commit_parentcount(cm), 16u);
		git_commit *par[16];
		git_oid pblob[16];
		u32 ppath[16];
		bool has[16];
		int same = -1;
		bool any = false;
		for (u32 k = 0; k < np; k++) {
			par[k] = NULL;
			has[k] = false;
			if (git_commit_parent(&par[k], cm, k) != 0)
				continue;
			ppath[k] = c.o_path[o];
			if (blob_at_tree(&c, git_commit_tree_id(par[k]), ppath[k], &pblob[k]) == 0) {
				has[k] = any = true;
				if (same < 0 && git_oid_equal(&pblob[k], &c.o_blob[o]))
					same = (int)k;
			}
		}
		if (!any && np && par[0]) {
			/* followed through a whole-file rename */
			git_tree *t = NULL, *pt = NULL;
			char old[1024];
			if (git_commit_tree(&t, cm) == 0 && git_commit_tree(&pt, par[0]) == 0 &&
			    find_rename(&c, pt, t, c.paths.b + c.o_path[o], old, sizeof old)) {
				ppath[0] = pool_add(&c.paths, old, (u32)strlen(old));
				if (blob_at_tree(&c, git_commit_tree_id(par[0]), ppath[0], &pblob[0]) == 0) {
					has[0] = true;
					if (git_oid_equal(&pblob[0], &c.o_blob[o]))
						same = 0;
				}
			}
			git_tree_free(t);
			git_tree_free(pt);
		}
		for (u32 k = 0; k < np; k++) {
			if (!has[k] || (same >= 0 && (int)k != same))
				continue;
			u32 po = origin(&c, par[k], ppath[k], &pblob[k]);
			u32 job = same >= 0 ? UINT32_MAX : job_add(&J, &pblob[k], &c.o_blob[o]);
			sync_tabs(&c, &visited, &efirst, &ecount, &tcap); /* origin() may have grown */
			edge_add(&E, o, po, job);
			ecount[o]++;
			heap_push(&c, po);
		}
		for (u32 k = 0; k < np; k++)
			git_commit_free(par[k]);
		git_commit_free(cm);
	}

	/* pass 2: every blob diff, in parallel (blob loads rebuild delta chains) */
	run_jobs(&J);
	out->diffs = J.n;

	/* pass 3: pass the lines down the graph in time order */
	c.remaining = out->nlines;
	for (u32 o = 0; o < c.no; o++) {
		c.o_head[o] = UINT32_MAX;
		c.o_queued[o] = 0;
	}
	c.nh = 0;
	add_range(&c, o0, 0, out->nlines, 0);
	heap_push(&c, o0);
	while (c.nh && c.remaining) {
		u32 o = heap_pop(&c);
		u32 list = c.o_head[o];
		c.o_head[o] = UINT32_MAX;
		if (list == UINT32_MAX)
			continue;
		u32 n_e = o < tcap && visited[o] ? ecount[o] : 0;
		for (u32 e = 0; e < n_e && list != UINT32_MAX; e++) {
			u32 k = efirst[o] + e;
			u32 po = E.to[k];
			if (E.job[k] == UINT32_MAX) {
				/* same blob: everything passes on */
				for (u32 r = list; r != UINT32_MAX;) {
					u32 next = c.r_next[r];
					c.r_next[r] = c.o_head[po];
					c.o_head[po] = r;
					r = next;
				}
				list = UINT32_MAX;
				heap_push(&c, po);
				break;
			}
			u32 j = E.job[k];
			c.nhk = J.nh[j];
			c.h_os = J.os[j];
			c.h_ol = J.ol[j];
			c.h_ns = J.ns[j];
			c.h_nl = J.nl[j];
			u32 before = c.o_head[po];
			list = pass_through(&c, list, po);
			if (c.o_head[po] != before)
				heap_push(&c, po);
		}
		c.h_os = c.h_ol = c.h_ns = c.h_nl = NULL;
		if (list != UINT32_MAX)
			blame_ranges_to(&c, list, &c.o_commit[o]);
	}
	for (u32 o = 0; o < c.no && c.remaining; o++)
		if (c.o_head[o] != UINT32_MAX) {
			blame_ranges_to(&c, c.o_head[o], &c.o_commit[o]);
			c.o_head[o] = UINT32_MAX;
		}

	jobs_free(&J);
	free(E.from);
	free(E.to);
	free(E.job);
	free(efirst);
	free(ecount);
	free(visited);
	free(c.o_commit);
	free(c.o_blob);
	free(c.o_time);
	free(c.o_path);
	free(c.o_head);
	free(c.o_queued);
	free(c.ohash);
	free(c.r_final);
	free(c.r_len);
	free(c.r_src);
	free(c.r_next);
	free(c.heap);
	free(c.m_tree);
	free(c.m_blob);
	free(c.m_path);
	free(c.m_found);
	pool_free(&c.paths);
	return 0;
}

void lg_blame_free(LgBlame *b)
{
	free(b->line_commit);
	free(b->oid);
	free(b->author);
	free(b->summary);
	free(b->time);
	pool_free(&b->pool);
	memset(b, 0, sizeof *b);
}

/* ---- live buffer layer ------------------------------------------------------------- */

typedef struct {
	u32 *map;
	u32 nlines;
	u32 *h_os, *h_ol, *h_ns, *h_nl;
	u32 n, cap;
} BufCtx;

static int buf_hunk(const git_diff_delta *d, const git_diff_hunk *h, void *p)
{
	(void)d;
	BufCtx *b = p;
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 64;
		b->h_os = vx_realloc(b->h_os, b->cap * sizeof(u32));
		b->h_ol = vx_realloc(b->h_ol, b->cap * sizeof(u32));
		b->h_ns = vx_realloc(b->h_ns, b->cap * sizeof(u32));
		b->h_nl = vx_realloc(b->h_nl, b->cap * sizeof(u32));
	}
	b->h_os[b->n] = (u32)h->old_start;
	b->h_ol[b->n] = (u32)h->old_lines;
	b->h_ns[b->n] = (u32)h->new_start;
	b->h_nl[b->n] = (u32)h->new_lines;
	b->n++;
	return 0;
}

int lg_blame_buffer_map(git_repository *repo, const git_oid *head_blob, const char *path, const char *text,
                        size_t n, u32 *out, u32 nlines)
{
	git_blob *ob = NULL;
	if (git_blob_lookup(&ob, repo, head_blob) != 0)
		return -1;
	BufCtx b = {0};
	git_diff_options o = GIT_DIFF_OPTIONS_INIT;
	o.context_lines = 0;
	o.interhunk_lines = 0;
	o.flags = GIT_DIFF_FORCE_TEXT;
	int rc = git_diff_blob_to_buffer(ob, path, text, n, path, &o, NULL, NULL, buf_hunk, NULL, &b);
	git_blob_free(ob);
	if (rc != 0)
		return -1;
	/* walk buffer lines: inside a hunk's new range -> uncommitted, else old line */
	i64 delta = 0;
	u32 h = 0;
	for (u32 l = 0; l < nlines; l++) {
		while (h < b.n) {
			u32 ns = b.h_ns[h], nl = b.h_nl[h];
			u32 cs = nl ? ns - 1 : ns, ce = nl ? ns - 1 + nl : ns;
			if (l >= ce) {
				delta += (i64)b.h_ol[h] - (i64)b.h_nl[h];
				h++;
				continue;
			}
			if (l >= cs) {
				out[l] = UINT32_MAX;
				goto next;
			}
			break;
		}
		out[l] = (u32)((i64)l + delta);
	next:;
	}
	free(b.h_os);
	free(b.h_ol);
	free(b.h_ns);
	free(b.h_nl);
	return 0;
}
