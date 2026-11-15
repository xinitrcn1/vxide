/* Source Control Graph on libgit2: revwalk + lane layout; commit details.
 * See gitgraph.h. */
#define _POSIX_C_SOURCE 200809L
#include "gitgraph.h"
#include "gitops.h"

#include <git2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_LANES 1024

static void graph_reserve(GitGraph *g, u32 n)
{
	g->hash = vx_realloc(g->hash, (size_t)n * sizeof g->hash[0]);
	g->subject = vx_realloc(g->subject, n * sizeof(u32));
	g->author = vx_realloc(g->author, n * sizeof(u32));
	g->email = vx_realloc(g->email, n * sizeof(u32));
	g->refs = vx_realloc(g->refs, n * sizeof(u32));
	g->time = vx_realloc(g->time, n * sizeof(i64));
	g->lane = vx_realloc(g->lane, n * sizeof(u16));
	g->color = vx_realloc(g->color, n);
	g->flags = vx_realloc(g->flags, n);
	g->edge_first = vx_realloc(g->edge_first, n * sizeof(u32));
	g->edge_count = vx_realloc(g->edge_count, n * sizeof(u32));
}

static void edge(GitGraph *g, u32 *cap, u8 half, u32 x0, u32 x1, u8 color)
{
	if (g->ne == *cap) {
		*cap = *cap ? *cap * 2 : 256;
		g->e_x0 = vx_realloc(g->e_x0, *cap * sizeof(u16));
		g->e_x1 = vx_realloc(g->e_x1, *cap * sizeof(u16));
		g->e_color = vx_realloc(g->e_color, *cap);
		g->e_half = vx_realloc(g->e_half, *cap);
	}
	g->e_x0[g->ne] = (u16)x0;
	g->e_x1[g->ne] = (u16)x1;
	g->e_color[g->ne] = color;
	g->e_half[g->ne] = half;
	g->ne++;
}

void git_graph_free(GitGraph *g)
{
	free(g->hash);
	free(g->subject);
	free(g->author);
	free(g->email);
	free(g->refs);
	free(g->time);
	free(g->lane);
	free(g->color);
	free(g->flags);
	free(g->edge_first);
	free(g->edge_count);
	free(g->e_x0);
	free(g->e_x1);
	free(g->e_color);
	free(g->e_half);
	pool_free(&g->pool);
	memset(g, 0, sizeof *g);
}

void git_commit_info_free(GitCommitInfo *ci)
{
	free(ci->parents);
	free(ci->path);
	free(ci->old_path);
	free(ci->status);
	free(ci->ins);
	free(ci->del);
	pool_free(&ci->pool);
	memset(ci, 0, sizeof *ci);
}

static void file_add(GitCommitInfo *ci, u32 *cap, char status, const char *path, const char *old, i32 ins, i32 del)
{
	if (ci->nfiles == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		ci->path = vx_realloc(ci->path, *cap * sizeof(u32));
		ci->old_path = vx_realloc(ci->old_path, *cap * sizeof(u32));
		ci->status = vx_realloc(ci->status, *cap);
		ci->ins = vx_realloc(ci->ins, *cap * sizeof(i32));
		ci->del = vx_realloc(ci->del, *cap * sizeof(i32));
	}
	u32 i = ci->nfiles++;
	ci->path[i] = pool_add(&ci->pool, path, (u32)strlen(path));
	ci->old_path[i] = pool_add(&ci->pool, old ? old : "", old ? (u32)strlen(old) : 0);
	ci->status[i] = status;
	ci->ins[i] = ins;
	ci->del[i] = del;
	if (ins > 0)
		ci->total_ins += (u32)ins;
	if (del > 0)
		ci->total_del += (u32)del;
}

/* ---- shared repository handle ----------------------------------------------------- */

/* One git_repository per work tree, reused across calls: libgit2 keeps the
 * packfile mappings, the object cache and the ref cache warm between graph
 * loads and commit details, so a slow disk is touched once. Calls are
 * serialised by a mutex (a repository handle is not for concurrent use). */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_workdir;
static git_repository *g_repo;

static git_repository *repo_lock(const char *workdir)
{
	gitops_libgit2_init();
	pthread_mutex_lock(&g_mu);
	if (g_repo && g_workdir && !strcmp(g_workdir, workdir))
		return g_repo;
	git_repository_free(g_repo);
	free(g_workdir);
	g_repo = NULL;
	g_workdir = NULL;
	if (git_repository_open_ext(&g_repo, workdir, 0, NULL) != 0) {
		g_repo = NULL;
		pthread_mutex_unlock(&g_mu);
		return NULL;
	}
	g_workdir = vx_strdup(workdir);
	return g_repo;
}

static void repo_unlock(void) { pthread_mutex_unlock(&g_mu); }

/* ---- decorations ------------------------------------------------------------------- */

typedef struct {
	u32 n, cap;
	git_oid *oid;
	u8 *rank; /* 0 HEAD, 1 local, 2 remote, 3 tag: git's %D order */
	u32 *text; /* pool offsets of "main", "origin/main", "tag: v1" */
	Pool pool;
} Decor;

static void decor_add(Decor *d, const git_oid *oid, u8 rank, const char *text)
{
	if (d->n == d->cap) {
		u32 c = d->cap ? d->cap * 2 : 64;
		d->oid = vx_realloc(d->oid, c * sizeof(git_oid));
		d->rank = vx_realloc(d->rank, c);
		d->text = vx_realloc(d->text, c * sizeof(u32));
		d->cap = c;
	}
	u32 i = d->n++;
	d->oid[i] = *oid;
	d->rank[i] = rank;
	d->text[i] = pool_add(&d->pool, text, (u32)strlen(text));
}

static void decor_free(Decor *d)
{
	free(d->oid);
	free(d->rank);
	free(d->text);
	pool_free(&d->pool);
}

/* Every ref (peeled to its commit); HEAD as "HEAD -> branch" or "HEAD". */
static void decor_load(Decor *d, git_repository *r, git_reference *head)
{
	memset(d, 0, sizeof *d);
	git_reference_iterator *it = NULL;
	if (git_reference_iterator_new(&it, r) != 0)
		return;
	const char *head_branch = head && git_reference_is_branch(head) ? git_reference_name(head) : NULL;
	git_reference *ref;
	while (git_reference_next(&ref, it) == 0) {
		const char *nm = git_reference_name(ref);
		u8 rank = 255;
		if (!strncmp(nm, "refs/heads/", 11))
			rank = 1;
		else if (!strncmp(nm, "refs/remotes/", 13) && git_reference_type(ref) == GIT_REFERENCE_DIRECT)
			rank = 2;
		else if (!strncmp(nm, "refs/tags/", 10))
			rank = 3;
		git_object *c = NULL;
		if (rank != 255 && git_reference_peel(&c, ref, GIT_OBJECT_COMMIT) == 0) {
			char text[600];
			if (rank == 3)
				snprintf(text, sizeof text, "tag: %s", nm + 10);
			else if (rank == 1 && head_branch && !strcmp(nm, head_branch))
				snprintf(text, sizeof text, "HEAD -> %s", nm + 11), rank = 0;
			else
				snprintf(text, sizeof text, "%s", git_reference_shorthand(ref));
			decor_add(d, git_object_id(c), rank, text);
		}
		git_object_free(c);
		git_reference_free(ref);
	}
	git_reference_iterator_free(it);
	if (head && !head_branch) {
		git_object *c = NULL;
		if (git_reference_peel(&c, head, GIT_OBJECT_COMMIT) == 0)
			decor_add(d, git_object_id(c), 0, "HEAD");
		git_object_free(c);
	}
}

/* "HEAD -> main, feat, origin/main, tag: v1" for one commit */
static u32 decor_text(const Decor *d, const git_oid *oid, Pool *out)
{
	char buf[4096];
	size_t n = 0;
	buf[0] = 0;
	for (u8 rank = 0; rank <= 3; rank++)
		for (u32 i = 0; i < d->n; i++) {
			if (d->rank[i] != rank || !git_oid_equal(&d->oid[i], oid))
				continue;
			const char *t = d->pool.b + d->text[i];
			int k = snprintf(buf + n, sizeof buf - n, "%s%s", n ? ", " : "", t);
			if (k > 0 && (size_t)k < sizeof buf - n)
				n += (size_t)k;
		}
	return pool_add(out, buf, (u32)n);
}

/* ---- graph scope: current branch, upstream, base (VSCodium "auto") ------------------- */

/* Resolve a ref name for the base; skips HEAD, the branch and its upstream. */
static bool base_try(git_repository *r, const char *name, const char *cur, const char *up, char *out, size_t cap,
                     git_oid *oid)
{
	if (!name || !*name || !strcmp(name, "HEAD"))
		return false;
	git_reference *ref = NULL;
	if (git_reference_dwim(&ref, r, name) != 0)
		return false;
	const char *sh = git_reference_shorthand(ref);
	bool ok = strcmp(sh, cur) && (!up || strcmp(sh, up));
	git_object *c = NULL;
	if (ok && git_reference_peel(&c, ref, GIT_OBJECT_COMMIT) == 0) {
		*oid = *git_object_id(c);
		snprintf(out, cap, "%s", sh);
	} else {
		ok = false;
	}
	git_object_free(c);
	git_reference_free(ref);
	return ok;
}

static bool find_base(git_repository *r, const char *cur, const char *up, char *out, size_t cap, git_oid *oid)
{
	char key[600], refname[600];
	/* 1. VSCodium's own record */
	git_config *cfg = NULL;
	if (git_repository_config_snapshot(&cfg, r) == 0) {
		const char *v = NULL;
		snprintf(key, sizeof key, "branch.%s.vscode-merge-base", cur);
		bool ok = git_config_get_string(&v, cfg, key) == 0 && base_try(r, v, cur, up, out, cap, oid);
		git_config_free(cfg);
		if (ok)
			return true;
	}
	/* 2. the branch's oldest reflog entry: "branch: Created from <ref>" */
	snprintf(refname, sizeof refname, "refs/heads/%s", cur);
	git_reflog *log = NULL;
	if (git_reflog_read(&log, r, refname) == 0) {
		size_t n = git_reflog_entrycount(log);
		const git_reflog_entry *e = n ? git_reflog_entry_byindex(log, n - 1) : NULL;
		const char *m = e ? git_reflog_entry_message(e) : NULL;
		static const char pre[] = "branch: Created from ";
		bool ok = false;
		if (m && !strncmp(m, pre, sizeof pre - 1)) {
			char from[512];
			snprintf(from, sizeof from, "%s", m + sizeof pre - 1);
			from[strcspn(from, "\n")] = 0;
			ok = base_try(r, from, cur, up, out, cap, oid);
		}
		git_reflog_free(log);
		if (ok)
			return true;
	}
	/* 3. the default branch of the upstream's remote */
	git_buf rb = {0};
	if (git_branch_upstream_remote(&rb, r, refname) == 0) {
		char sym[600];
		snprintf(sym, sizeof sym, "refs/remotes/%s/HEAD", rb.ptr);
		git_reference *h = NULL;
		bool ok = false;
		if (git_reference_lookup(&h, r, sym) == 0 && git_reference_type(h) == GIT_REFERENCE_SYMBOLIC) {
			const char *t = git_reference_symbolic_target(h);
			ok = t && base_try(r, !strncmp(t, "refs/remotes/", 13) ? t + 13 : t, cur, up, out, cap, oid);
		}
		git_reference_free(h);
		static const char *defs[] = {"main", "master"};
		for (int i = 0; i < 2 && !ok; i++) {
			snprintf(sym, sizeof sym, "%s/%s", rb.ptr, defs[i]);
			ok = base_try(r, sym, cur, up, out, cap, oid);
		}
		git_buf_dispose(&rb);
		if (ok)
			return true;
	}
	/* 4. a local main / master */
	return base_try(r, "main", cur, up, out, cap, oid) || base_try(r, "master", cur, up, out, cap, oid);
}

/* ---- graph ------------------------------------------------------------------------- */

int git_graph_load(GitGraph *g, const char *workdir, u32 limit)
{
	memset(g, 0, sizeof *g);
	g->cur_ref = g->up_ref = g->base_ref = pool_add(&g->pool, "", 0);
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	git_reference *head = NULL;
	if (git_repository_head(&head, r) != 0) {
		/* unborn HEAD or empty repository: an empty graph */
		repo_unlock();
		return 0;
	}
	git_revwalk *w = NULL;
	if (git_revwalk_new(&w, r) != 0) {
		git_reference_free(head);
		repo_unlock();
		return -1;
	}
	git_revwalk_sorting(w, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
	git_object *hc = NULL;
	if (git_reference_peel(&hc, head, GIT_OBJECT_COMMIT) == 0)
		git_revwalk_push(w, git_object_id(hc));
	git_object_free(hc);
	if (git_reference_is_branch(head)) {
		const char *cur = git_reference_shorthand(head);
		g->cur_ref = pool_add(&g->pool, cur, (u32)strlen(cur));
		char upname[512] = "";
		git_reference *up = NULL;
		if (git_branch_upstream(&up, head) == 0) {
			git_object *uc = NULL;
			if (git_reference_peel(&uc, up, GIT_OBJECT_COMMIT) == 0) {
				git_revwalk_push(w, git_object_id(uc));
				snprintf(upname, sizeof upname, "%s", git_reference_shorthand(up));
				g->up_ref = pool_add(&g->pool, upname, (u32)strlen(upname));
			}
			git_object_free(uc);
		}
		git_reference_free(up);
		char base[512];
		git_oid bo;
		if (find_base(r, cur, upname[0] ? upname : NULL, base, sizeof base, &bo)) {
			git_revwalk_push(w, &bo);
			g->base_ref = pool_add(&g->pool, base, (u32)strlen(base));
		}
	}
	Decor dec;
	decor_load(&dec, r, head);
	git_reference_free(head);

	u32 cap_rows = 0, ecap = 0;
	u32 max = limit ? limit : 2000;
	/* active lanes: expected commit, used flag, colour */
	git_oid *want = vx_alloc(MAX_LANES * sizeof *want);
	bool *used = vx_alloc(MAX_LANES * sizeof(bool));
	bool *fresh = vx_alloc(MAX_LANES * sizeof(bool)); /* lane started at this row's dot */
	u8 *col = vx_alloc(MAX_LANES);
	u32 nl = 0;
	u8 next_color = 0;
	git_oid id;
	while (g->n < max && git_revwalk_next(&id, w) == 0) {
		git_commit *cm = NULL;
		if (git_commit_lookup(&cm, r, &id) != 0)
			continue;
		if (g->n == cap_rows) {
			cap_rows = cap_rows ? cap_rows * 2 : 256;
			graph_reserve(g, cap_rows);
		}
		u32 i = g->n++;
		git_oid_tostr(g->hash[i], 41, &id);
		const git_signature *a = git_commit_author(cm);
		const char *an = a && a->name ? a->name : "", *ae = a && a->email ? a->email : "";
		g->author[i] = pool_add(&g->pool, an, (u32)strlen(an));
		g->email[i] = pool_add(&g->pool, ae, (u32)strlen(ae));
		g->time[i] = a ? (i64)a->when.time : (i64)git_commit_time(cm);
		const char *sum = git_commit_summary(cm);
		g->subject[i] = pool_add(&g->pool, sum ? sum : "", sum ? (u32)strlen(sum) : 0);
		g->refs[i] = decor_text(&dec, &id, &g->pool);
		u32 np = git_commit_parentcount(cm);
		if (np > 64)
			np = 64;
		git_oid par[64];
		for (u32 k = 0; k < np; k++)
			par[k] = *git_commit_parent_id(cm, k);
		git_commit_free(cm);
		const char *refs = g->pool.b + g->refs[i];
		g->flags[i] = (u8)((np > 1 ? GG_MERGE : 0) | (np == 0 ? GG_ROOT : 0) |
		                   (!strncmp(refs, "HEAD", 4) && (refs[4] == 0 || refs[4] == ' ' || refs[4] == ',') ? GG_HEAD : 0));
		g->edge_first[i] = g->ne;

		/* the commit's lane */
		u32 lc = UINT32_MAX;
		for (u32 j = 0; j < nl; j++)
			if (used[j] && git_oid_equal(&want[j], &id)) {
				lc = j;
				break;
			}
		if (lc == UINT32_MAX) {
			for (u32 j = 0; j < nl; j++)
				if (!used[j]) {
					lc = j;
					break;
				}
			if (lc == UINT32_MAX)
				lc = nl < MAX_LANES ? nl++ : nl - 1; /* degenerate: overdraw the last lane */
			col[lc] = next_color;
			next_color = (u8)((next_color + 1) % 5);
		}
		u8 dot_color = col[lc];
		/* upper half: lanes expecting this commit merge into its dot */
		for (u32 j = 0; j < nl; j++) {
			if (!used[j])
				continue;
			if (git_oid_equal(&want[j], &id)) {
				edge(g, &ecap, 0, j, lc, col[j]);
				used[j] = false;
			} else {
				edge(g, &ecap, 0, j, j, col[j]);
			}
		}
		g->lane[i] = (u16)lc;
		g->color[i] = dot_color;
		memset(fresh, 0, nl * sizeof fresh[0]);
		/* first parent continues the commit's lane */
		if (np >= 1) {
			used[lc] = true;
			want[lc] = par[0];
			col[lc] = dot_color;
			fresh[lc] = true;
			edge(g, &ecap, 1, lc, lc, dot_color);
		}
		/* other parents: join a lane expecting them, else open one */
		for (u32 k = 1; k < np; k++) {
			u32 t = UINT32_MAX;
			for (u32 j = 0; j < nl; j++)
				if (used[j] && j != lc && git_oid_equal(&want[j], &par[k])) {
					t = j;
					break;
				}
			if (t == UINT32_MAX) {
				for (u32 j = 0; j < nl; j++)
					if (!used[j] && j != lc) {
						t = j;
						break;
					}
				if (t == UINT32_MAX)
					t = nl < MAX_LANES ? nl++ : nl - 1;
				used[t] = true;
				fresh[t] = true;
				want[t] = par[k];
				col[t] = next_color;
				next_color = (u8)((next_color + 1) % 5);
			}
			edge(g, &ecap, 1, lc, t, col[t]);
		}
		/* lower half pass-through for the other lanes */
		for (u32 j = 0; j < nl; j++)
			if (used[j] && !fresh[j])
				edge(g, &ecap, 1, j, j, col[j]);
		g->edge_count[i] = g->ne - g->edge_first[i];
		u32 wdt = lc + 1;
		for (u32 j = 0; j < nl; j++)
			if (used[j] && j + 1 > wdt)
				wdt = j + 1;
		for (u32 k = g->edge_first[i]; k < g->ne; k++) {
			if ((u32)g->e_x0[k] + 1 > wdt)
				wdt = g->e_x0[k] + 1u;
			if ((u32)g->e_x1[k] + 1 > wdt)
				wdt = g->e_x1[k] + 1u;
		}
		if (wdt > g->lanes)
			g->lanes = (u16)wdt;
		while (nl && !used[nl - 1])
			nl--;
	}
	free(want);
	free(used);
	free(fresh);
	free(col);
	decor_free(&dec);
	git_revwalk_free(w);
	repo_unlock();
	return 0;
}

/* ---- commit details --------------------------------------------------------------- */

static bool safe_rev(const char *h) { return h && *h && h[0] != '-' && !strchr(h, ' '); }

static git_commit *lookup_commit(git_repository *r, const char *hash)
{
	git_object *o = NULL, *c = NULL;
	if (git_revparse_single(&o, r, hash) != 0)
		return NULL;
	if (git_object_peel(&c, o, GIT_OBJECT_COMMIT) != 0)
		c = NULL;
	git_object_free(o);
	return (git_commit *)c;
}

static char status_letter(git_delta_t s)
{
	switch (s) {
	case GIT_DELTA_ADDED:
		return 'A';
	case GIT_DELTA_DELETED:
		return 'D';
	case GIT_DELTA_RENAMED:
		return 'R';
	case GIT_DELTA_COPIED:
		return 'C';
	case GIT_DELTA_TYPECHANGE:
		return 'T';
	default:
		return 'M';
	}
}

int git_commit_details(const char *workdir, const char *hash, GitCommitInfo *ci)
{
	memset(ci, 0, sizeof *ci);
	if (!safe_rev(hash))
		return -1;
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	git_commit *cm = lookup_commit(r, hash);
	if (!cm) {
		repo_unlock();
		return -1;
	}
	git_oid_tostr(ci->hash, sizeof ci->hash, git_commit_id(cm));
	const git_signature *a = git_commit_author(cm), *c = git_commit_committer(cm);
	ci->author = pool_add(&ci->pool, a->name ? a->name : "", a->name ? (u32)strlen(a->name) : 0);
	ci->author_email = pool_add(&ci->pool, a->email ? a->email : "", a->email ? (u32)strlen(a->email) : 0);
	ci->author_time = (i64)a->when.time;
	ci->committer = pool_add(&ci->pool, c->name ? c->name : "", c->name ? (u32)strlen(c->name) : 0);
	ci->committer_email = pool_add(&ci->pool, c->email ? c->email : "", c->email ? (u32)strlen(c->email) : 0);
	ci->commit_time = (i64)c->when.time;
	const char *msg = git_commit_message(cm);
	size_t ml = msg ? strlen(msg) : 0;
	while (ml && (msg[ml - 1] == '\n' || msg[ml - 1] == '\r'))
		ml--;
	ci->message = pool_add(&ci->pool, msg ? msg : "", (u32)ml);
	ci->nparents = git_commit_parentcount(cm);
	if (ci->nparents) {
		ci->parents = vx_alloc(ci->nparents * sizeof ci->parents[0]);
		for (u32 k = 0; k < ci->nparents; k++)
			git_oid_tostr(ci->parents[k], 41, git_commit_parent_id(cm, k));
	}
	/* changed files vs the first parent (the root against the empty tree) */
	git_tree *tree = NULL, *ptree = NULL;
	git_commit *p1 = NULL;
	git_diff *diff = NULL;
	if (git_commit_tree(&tree, cm) == 0 && (!ci->nparents || (git_commit_parent(&p1, cm, 0) == 0 &&
	                                                             git_commit_tree(&ptree, p1) == 0))) {
		git_diff_options dopt;
		git_diff_options_init(&dopt, GIT_DIFF_OPTIONS_VERSION);
		if (git_diff_tree_to_tree(&diff, r, ptree, tree, &dopt) == 0) {
			git_diff_find_options fo;
			git_diff_find_options_init(&fo, GIT_DIFF_FIND_OPTIONS_VERSION);
			fo.flags = GIT_DIFF_FIND_RENAMES;
			git_diff_find_similar(diff, &fo);
			u32 cap = 0;
			size_t nd = git_diff_num_deltas(diff);
			for (size_t k = 0; k < nd; k++) {
				git_patch *pt = NULL;
				i32 ins = -1, del = -1;
				const git_diff_delta *d = git_diff_get_delta(diff, k);
				if (git_patch_from_diff(&pt, diff, k) == 0 && pt) {
					d = git_patch_get_delta(pt);
					if (!(d->flags & GIT_DIFF_FLAG_BINARY)) {
						size_t ad = 0, de = 0;
						git_patch_line_stats(NULL, &ad, &de, pt);
						ins = (i32)ad;
						del = (i32)de;
					}
				}
				char st = status_letter(d->status);
				const char *path = d->new_file.path ? d->new_file.path : d->old_file.path;
				const char *old = (st == 'R' || st == 'C') ? d->old_file.path : NULL;
				file_add(ci, &cap, st, path, old, ins, del);
				git_patch_free(pt);
			}
		}
	}
	git_diff_free(diff);
	git_tree_free(tree);
	git_tree_free(ptree);
	git_commit_free(p1);
	git_commit_free(cm);
	repo_unlock();
	return 0;
}

char *git_show_file_diff(const char *workdir, const char *hash, const char *path)
{
	if (!safe_rev(hash) || !path)
		return NULL;
	git_repository *r = repo_lock(workdir);
	if (!r)
		return NULL;
	git_commit *cm = lookup_commit(r, hash), *p1 = NULL;
	git_tree *tree = NULL, *ptree = NULL;
	git_diff *diff = NULL;
	char *out = NULL;
	if (cm && git_commit_tree(&tree, cm) == 0 &&
	    (git_commit_parentcount(cm) == 0 || (git_commit_parent(&p1, cm, 0) == 0 && git_commit_tree(&ptree, p1) == 0))) {
		/* a pathspec-limited diff: only the trees along the path are read */
		git_diff_options dopt;
		git_diff_options_init(&dopt, GIT_DIFF_OPTIONS_VERSION);
		char *ps = (char *)path;
		dopt.pathspec.strings = &ps;
		dopt.pathspec.count = 1;
		dopt.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
		git_buf b = {0};
		if (git_diff_tree_to_tree(&diff, r, ptree, tree, &dopt) == 0 &&
		    git_diff_to_buf(&b, diff, GIT_DIFF_FORMAT_PATCH) == 0)
			out = vx_strndup(b.ptr ? b.ptr : "", b.size);
		git_buf_dispose(&b);
	}
	git_diff_free(diff);
	git_tree_free(tree);
	git_tree_free(ptree);
	git_commit_free(p1);
	git_commit_free(cm);
	repo_unlock();
	return out;
}

/* "Sat Nov 15 00:25:00 2026 +0100", git's default date format */
static void fmt_date(char *out, size_t cap, const git_time *t)
{
	time_t tt = (time_t)t->time + (time_t)t->offset * 60;
	struct tm tmv;
	gmtime_r(&tt, &tmv);
	char d[64];
	strftime(d, sizeof d, "%a %b %e %H:%M:%S %Y", &tmv);
	int off = t->offset < 0 ? -t->offset : t->offset;
	snprintf(out, cap, "%s %c%02d%02d", d, t->offset < 0 ? '-' : '+', off / 60, off % 60);
}

char *git_show_commit(const char *workdir, const char *hash)
{
	if (!safe_rev(hash))
		return NULL;
	git_repository *r = repo_lock(workdir);
	if (!r)
		return NULL;
	git_commit *cm = lookup_commit(r, hash), *p1 = NULL;
	git_tree *tree = NULL, *ptree = NULL;
	git_diff *diff = NULL;
	git_buf b = {0};
	char *res = NULL;
	if (!cm || git_commit_tree(&tree, cm) != 0 ||
	    (git_commit_parentcount(cm) && (git_commit_parent(&p1, cm, 0) != 0 || git_commit_tree(&ptree, p1) != 0)))
		goto done;
	const git_signature *a = git_commit_author(cm), *c = git_commit_committer(cm);
	char ad[96], cd[96];
	fmt_date(ad, sizeof ad, &a->when);
	fmt_date(cd, sizeof cd, &c->when);
	Pool txt = {0};
	char line[2048];
	int n = 0;
	txt.n = 0;
	VX_GROW(txt.b, txt.cap, 1);
	txt.b[0] = 0;
#define PUT(...)                                                        \
	do {                                                                \
		n = snprintf(line, sizeof line, __VA_ARGS__);                   \
		if (n > 0) {                                                    \
			u32 k = (u32)VX_MIN(n, (int)sizeof line - 1);              \
			VX_GROW(txt.b, txt.cap, txt.n + k + 1);                     \
			memcpy(txt.b + txt.n, line, k);                             \
			txt.n += k;                                                 \
			txt.b[txt.n] = 0;                                           \
		}                                                               \
	} while (0)
	PUT("commit %s\n", git_oid_tostr_s(git_commit_id(cm)));
	if (git_commit_parentcount(cm) > 1) {
		PUT("Merge:");
		for (u32 k = 0; k < git_commit_parentcount(cm); k++) {
			char h[12];
			git_oid_tostr(h, sizeof h, git_commit_parent_id(cm, k));
			PUT(" %.7s", h);
		}
		PUT("\n");
	}
	PUT("Author:     %s <%s>\nAuthorDate: %s\n", a->name, a->email, ad);
	PUT("Commit:     %s <%s>\nCommitDate: %s\n\n", c->name, c->email, cd);
	const char *msg = git_commit_message(cm);
	for (const char *m = msg; m && *m;) {
		const char *e = strchr(m, '\n');
		size_t k = e ? (size_t)(e - m) : strlen(m);
		PUT("    %.*s\n", (int)k, m);
		m = e ? e + 1 : m + k;
	}
	PUT("\n");
#undef PUT
	if (git_diff_tree_to_tree(&diff, r, ptree, tree, NULL) == 0) {
		git_diff_find_options fo;
		git_diff_find_options_init(&fo, GIT_DIFF_FIND_OPTIONS_VERSION);
		fo.flags = GIT_DIFF_FIND_RENAMES;
		git_diff_find_similar(diff, &fo);
		git_diff_stats *st = NULL;
		if (git_diff_get_stats(&st, diff) == 0 && git_diff_stats_to_buf(&b, st, GIT_DIFF_STATS_FULL, 80) == 0) {
			VX_GROW(txt.b, txt.cap, txt.n + (u32)b.size + 2);
			memcpy(txt.b + txt.n, b.ptr, b.size);
			txt.n += (u32)b.size;
			txt.b[txt.n++] = '\n';
			txt.b[txt.n] = 0;
		}
		git_diff_stats_free(st);
		git_buf_dispose(&b);
		if (git_diff_to_buf(&b, diff, GIT_DIFF_FORMAT_PATCH) == 0) {
			VX_GROW(txt.b, txt.cap, txt.n + (u32)b.size + 1);
			memcpy(txt.b + txt.n, b.ptr, b.size);
			txt.n += (u32)b.size;
			txt.b[txt.n] = 0;
		}
	}
	res = txt.b;
done:
	git_buf_dispose(&b);
	git_diff_free(diff);
	git_tree_free(tree);
	git_tree_free(ptree);
	git_commit_free(p1);
	git_commit_free(cm);
	repo_unlock();
	return res;
}
