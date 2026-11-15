/* Source Control Graph: git log + lane layout; commit details. */
#define _POSIX_C_SOURCE 200809L
#include "gitgraph.h"
#include "gitops.h"

#include <stdio.h>
#include <stdlib.h>

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

/* Split at sep in place, up to max fields; returns the count. */
static u32 split(char *s, char sep, char **f, u32 max)
{
	u32 n = 0;
	f[n++] = s;
	for (char *p = s; *p && n < max; p++)
		if (*p == sep) {
			*p = 0;
			f[n++] = p + 1;
		}
	return n;
}

int git_graph_load(GitGraph *g, const char *workdir, u32 limit)
{
	memset(g, 0, sizeof *g);
	char lim[32];
	snprintf(lim, sizeof lim, "--max-count=%u", limit ? limit : 2000);
	const char *argv[] = {"log", "--topo-order", lim, "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%D%x1f%s%x1e",
	                      "HEAD", "--branches", "--remotes", "--", NULL};
	GitRun r;
	if (git_cli(workdir, argv, NULL, &r, NULL) != 0) {
		/* unborn HEAD or not a repository: an empty graph */
		int st = r.status;
		git_run_free(&r);
		return st < 0 ? -1 : 0;
	}
	/* count rows */
	u32 rows = 0;
	for (size_t i = 0; i < r.nout; i++)
		rows += r.out[i] == 0x1e;
	graph_reserve(g, rows + 1);

	/* active lanes: expected commit (40 hex + NUL), used flag, colour */
	char (*want)[41] = vx_alloc(MAX_LANES * sizeof *want);
	bool *used = vx_alloc(MAX_LANES * sizeof(bool));
	bool *fresh = vx_alloc(MAX_LANES * sizeof(bool)); /* lane started at this row's dot */
	u8 *col = vx_alloc(MAX_LANES);
	u32 nl = 0;
	u8 next_color = 0;
	u32 ecap = 0;

	char *p = r.out;
	while (*p && g->n < rows) {
		char *e = strchr(p, 0x1e);
		if (!e)
			break;
		*e = 0;
		while (*p == '\n')
			p++;
		char *f[7];
		if (split(p, 0x1f, f, 7) != 7) {
			p = e + 1;
			continue;
		}
		u32 i = g->n++;
		snprintf(g->hash[i], 41, "%s", f[0]);
		g->author[i] = pool_add(&g->pool, f[2], (u32)strlen(f[2]));
		g->email[i] = pool_add(&g->pool, f[3], (u32)strlen(f[3]));
		g->time[i] = strtoll(f[4], NULL, 10);
		g->refs[i] = pool_add(&g->pool, f[5], (u32)strlen(f[5]));
		g->subject[i] = pool_add(&g->pool, f[6], (u32)strlen(f[6]));
		char *par[64];
		u32 np = 0;
		for (char *q = f[1]; *q && np < 64;) {
			char *sp = strchr(q, ' ');
			if (sp)
				*sp = 0;
			if (*q)
				par[np++] = q;
			if (!sp)
				break;
			q = sp + 1;
		}
		g->flags[i] = (u8)((np > 1 ? GG_MERGE : 0) | (np == 0 ? GG_ROOT : 0) |
		                   (!strncmp(f[5], "HEAD", 4) && (f[5][4] == 0 || f[5][4] == ' ' || f[5][4] == ',') ? GG_HEAD : 0));
		g->edge_first[i] = g->ne;

		/* the commit's lane */
		u32 lc = UINT32_MAX;
		for (u32 j = 0; j < nl; j++)
			if (used[j] && !strcmp(want[j], g->hash[i])) {
				lc = j;
				break;
			}
		bool expected = lc != UINT32_MAX;
		if (!expected) {
			for (u32 j = 0; j < nl; j++)
				if (!used[j]) {
					lc = j;
					break;
				}
			if (lc == UINT32_MAX) {
				if (nl >= MAX_LANES)
					lc = nl - 1; /* degenerate: overdraw the last lane */
				else
					lc = nl++;
			}
			col[lc] = next_color;
			next_color = (u8)((next_color + 1) % 5);
		}
		u8 dot_color = col[lc];
		/* upper half: lanes expecting this commit merge into its dot */
		for (u32 j = 0; j < nl; j++) {
			if (!used[j])
				continue;
			if (!strcmp(want[j], g->hash[i])) {
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
			snprintf(want[lc], 41, "%s", par[0]);
			col[lc] = dot_color;
			fresh[lc] = true;
			edge(g, &ecap, 1, lc, lc, dot_color);
		}
		/* other parents: join a lane expecting them, else open one */
		for (u32 k = 1; k < np; k++) {
			u32 t = UINT32_MAX;
			for (u32 j = 0; j < nl; j++)
				if (used[j] && j != lc && !strcmp(want[j], par[k])) {
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
				snprintf(want[t], 41, "%s", par[k]);
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
		/* width of this row */
		u32 w = lc + 1;
		for (u32 j = 0; j < nl; j++)
			if (used[j] && j + 1 > w)
				w = j + 1;
		for (u32 k = g->edge_first[i]; k < g->ne; k++) {
			if ((u32)g->e_x0[k] + 1 > w)
				w = g->e_x0[k] + 1u;
			if ((u32)g->e_x1[k] + 1 > w)
				w = g->e_x1[k] + 1u;
		}
		if (w > g->lanes)
			g->lanes = (u16)w;
		while (nl && !used[nl - 1])
			nl--;
		p = e + 1;
	}
	free(want);
	free(used);
	free(fresh);
	free(col);
	git_run_free(&r);
	return 0;
}

/* ---- commit details --------------------------------------------------------- */

static bool safe_rev(const char *h) { return h && *h && h[0] != '-' && !strchr(h, ' '); }

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

int git_commit_details(const char *workdir, const char *hash, GitCommitInfo *ci)
{
	memset(ci, 0, sizeof *ci);
	if (!safe_rev(hash))
		return -1;
	const char *a1[] = {"show", "-s", "--format=%H%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%P%x1f%B",
	                    hash, "--", NULL};
	GitRun r;
	if (git_cli(workdir, a1, NULL, &r, NULL) != 0) {
		git_run_free(&r);
		return -1;
	}
	char *f[9];
	if (split(r.out, 0x1f, f, 9) != 9) {
		git_run_free(&r);
		return -1;
	}
	snprintf(ci->hash, sizeof ci->hash, "%s", f[0]);
	ci->author = pool_add(&ci->pool, f[1], (u32)strlen(f[1]));
	ci->author_email = pool_add(&ci->pool, f[2], (u32)strlen(f[2]));
	ci->author_time = strtoll(f[3], NULL, 10);
	ci->committer = pool_add(&ci->pool, f[4], (u32)strlen(f[4]));
	ci->committer_email = pool_add(&ci->pool, f[5], (u32)strlen(f[5]));
	ci->commit_time = strtoll(f[6], NULL, 10);
	/* trim the trailing newlines git adds to %B */
	size_t ml = strlen(f[8]);
	while (ml && (f[8][ml - 1] == '\n' || f[8][ml - 1] == '\r'))
		ml--;
	ci->message = pool_add(&ci->pool, f[8], (u32)ml);
	for (char *q = f[7]; *q;) {
		char *sp = strchr(q, ' ');
		size_t n = sp ? (size_t)(sp - q) : strlen(q);
		if (n == 40) {
			ci->parents = vx_realloc(ci->parents, (ci->nparents + 1) * sizeof ci->parents[0]);
			memcpy(ci->parents[ci->nparents], q, 40);
			ci->parents[ci->nparents][40] = 0;
			ci->nparents++;
		}
		if (!sp)
			break;
		q = sp + 1;
	}
	git_run_free(&r);

	/* changed files vs the first parent: numstat and name-status, same order */
	const char *p1 = ci->nparents ? ci->parents[0] : NULL;
	const char *an[10], *as[10];
	int k = 0;
	an[k] = "diff-tree";
	as[k++] = "diff-tree";
	an[k] = "-r";
	as[k++] = "-r";
	an[k] = "-M";
	as[k++] = "-M";
	an[k] = "-z";
	as[k++] = "-z";
	an[k] = "--no-commit-id";
	as[k++] = "--no-commit-id";
	an[k] = "--numstat";
	as[k++] = "--name-status";
	if (p1) {
		an[k] = p1;
		as[k++] = p1;
	} else {
		an[k] = "--root";
		as[k++] = "--root";
	}
	an[k] = ci->hash;
	as[k++] = ci->hash;
	an[k] = NULL;
	as[k] = NULL;
	GitRun rn, rs;
	if (git_cli(workdir, an, NULL, &rn, NULL) != 0 || git_cli(workdir, as, NULL, &rs, NULL) != 0) {
		git_run_free(&rn);
		git_run_free(&rs);
		return 0; /* metadata is still useful */
	}
	u32 cap = 0;
	char *pn = rn.out, *ps = rs.out;
	char *en = rn.out + rn.nout, *es = rs.out + rs.nout;
	while (pn < en && ps < es) {
		/* name-status: "M\0path\0" or "R086\0old\0new\0" */
		char st = ps[0];
		ps += strlen(ps) + 1;
		const char *old = NULL, *path;
		if ((st == 'R' || st == 'C') && ps < es) {
			old = ps;
			ps += strlen(ps) + 1;
		}
		if (ps >= es)
			break;
		path = ps;
		ps += strlen(ps) + 1;
		/* numstat: "ins\tdel\tpath\0" or "ins\tdel\t\0old\0new\0" */
		char *tab1 = strchr(pn, '\t');
		char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
		i32 ins = -1, del = -1;
		if (tab1 && tab2) {
			if (pn[0] != '-')
				ins = (i32)strtol(pn, NULL, 10);
			if (tab1[1] != '-')
				del = (i32)strtol(tab1 + 1, NULL, 10);
			bool renamed = tab2[1] == 0;
			pn += strlen(pn) + 1;
			if (renamed && pn < en) {
				pn += strlen(pn) + 1; /* old */
				if (pn < en)
					pn += strlen(pn) + 1; /* new */
			}
		} else {
			pn += strlen(pn) + 1;
		}
		file_add(ci, &cap, st, path, old, ins, del);
	}
	git_run_free(&rn);
	git_run_free(&rs);
	return 0;
}

char *git_show_file_diff(const char *workdir, const char *hash, const char *path)
{
	if (!safe_rev(hash) || !path)
		return NULL;
	/* first parent of the commit */
	const char *ap[] = {"show", "-s", "--format=%P", hash, "--", NULL};
	GitRun r;
	if (git_cli(workdir, ap, NULL, &r, NULL) != 0) {
		git_run_free(&r);
		return NULL;
	}
	char p1[41] = "";
	if (r.nout >= 40 && r.out[0] != '\n') {
		memcpy(p1, r.out, 40);
		p1[40] = 0;
	}
	git_run_free(&r);
	const char *argv[12];
	int k = 0;
	argv[k++] = "diff-tree";
	argv[k++] = "-p";
	argv[k++] = "-M";
	argv[k++] = "--no-commit-id";
	argv[k++] = "--no-color";
	if (p1[0])
		argv[k++] = p1;
	else
		argv[k++] = "--root";
	argv[k++] = hash;
	argv[k++] = "--";
	argv[k++] = path;
	argv[k] = NULL;
	if (git_cli(workdir, argv, NULL, &r, NULL) != 0) {
		git_run_free(&r);
		return NULL;
	}
	free(r.err);
	return r.out;
}
