/* Blame of the committed file at HEAD on libgit2 primitives, built to
 * read as few objects as possible (fast, and kind to slow disks).
 *
 * libgit2's own git_blame_file diffs whole trees at every history step
 * (12 s for a 22k-line file with 1915 commits of history). Here each
 * commit only looks up the tree entries along the file's path, memoised
 * by tree id, so an unchanged directory costs one cache hit; blobs are
 * diffed only where the file changed, and the walk stops as soon as every
 * line has an owner. Ordering follows git blame: commits by time, a merge
 * passes everything to a parent with the same blob, otherwise unchanged
 * lines pass to each parent in turn; whole-file renames are followed. */
#ifndef VX_LGBLAME_H
#define VX_LGBLAME_H

#include "base.h"
#include <git2.h>

typedef struct {
	u32 nlines;
	u32 *line_commit; /* index into the commit table */
	u32 nc, cc;            /* commit table (SoA), fitted to nc */
	git_oid *oid;
	u32 *author, *summary; /* pool offsets */
	i64 *time;             /* author time */
	Pool pool;
	git_oid head_blob;
	/* statistics */
	u32 commits_visited, trees_read, diffs;
} LgBlame;

/* 0 on success; 1 when the path is not in HEAD; -1 on error. */
int lg_blame_head(git_repository *repo, const char *path, LgBlame *out);
void lg_blame_free(LgBlame *b);

/* Map the live buffer onto the HEAD blob: out[i] = HEAD line for buffer
 * line i, or UINT32_MAX for lines not in HEAD (uncommitted). */
int lg_blame_buffer_map(git_repository *repo, const git_oid *head_blob, const char *path, const char *text,
                        size_t n, u32 *out, u32 nlines);

#endif
