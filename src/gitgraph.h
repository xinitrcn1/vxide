/* Source Control Graph (VSCodium's scm.graph) and commit details.
 *
 * The graph lists commits reachable from HEAD, every local branch and
 * every remote-tracking branch (`git log --topo-order HEAD --branches
 * --remotes`), newest first, with lanes assigned by the usual algorithm:
 * active lanes hold the commit they expect next; a commit takes the
 * leftmost lane expecting it and closes the others (they merge into it);
 * its first parent continues its lane; other parents join a lane already
 * expecting them or take the leftmost free lane. Lane colours rotate over
 * 5 palette entries and stay fixed along a lane.
 *
 * Drawing: each row has edges in lane units, split at the commit dot:
 * e_half 0 = upper half (row top -> dot), 1 = lower half (dot -> row
 * bottom); e_x0 is the lane at the start of the half, e_x1 at its end
 * (equal = straight vertical). Lower-half end lanes of row i equal the
 * upper-half start lanes of row i + 1 (as sets). */
#ifndef VX_GITGRAPH_H
#define VX_GITGRAPH_H

#include "base.h"

enum { GG_MERGE = 1, GG_HEAD = 2, GG_ROOT = 4 };

typedef struct {
	u32 n;               /* commits (rows) */
	char (*hash)[41];
	u32 *subject, *author, *email; /* pool offsets */
	u32 *refs;           /* "HEAD -> main, origin/main, tag: v1" or "" (pool) */
	i64 *time;           /* author time, seconds */
	u16 *lane;           /* column of the dot */
	u8 *color;           /* palette index 0..4 of the dot's lane */
	u8 *flags;           /* GG_* */
	u32 *edge_first, *edge_count; /* edges of row i */
	u32 ne;
	u16 *e_x0, *e_x1;
	u8 *e_color, *e_half;
	u16 lanes;           /* widest row, in lanes */
	Pool pool;
} GitGraph;

/* 0 on success (an empty or unborn repository gives n = 0). */
int git_graph_load(GitGraph *g, const char *workdir, u32 limit);
void git_graph_free(GitGraph *g);

/* Changed file of a commit vs its first parent (SoA). */
typedef struct {
	char hash[41];
	u32 author, author_email, committer, committer_email; /* pool */
	i64 author_time, commit_time;
	u32 message;  /* full message (pool) */
	u32 nparents;
	char (*parents)[41];
	u32 nfiles;
	u32 *path, *old_path; /* pool; old_path "" unless renamed/copied */
	char *status;         /* 'A' 'M' 'D' 'R' 'C' 'T' */
	i32 *ins, *del;       /* -1 for binary files */
	u32 total_ins, total_del;
	Pool pool;
} GitCommitInfo;

int git_commit_details(const char *workdir, const char *hash, GitCommitInfo *out); /* 0 ok */
void git_commit_info_free(GitCommitInfo *ci);
/* Unified diff of one file in a commit (vs its first parent; the root
 * commit against the empty tree). malloc'd, NULL on error. */
char *git_show_file_diff(const char *workdir, const char *hash, const char *path);

#endif
