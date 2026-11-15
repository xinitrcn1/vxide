/* Line hunks and word changes for a side-by-side diff editor, range
 * staging / unstaging / reverting (VSCodium's "Stage Selected Ranges",
 * gutter "Stage Change", "Revert Selected Ranges"), and merge conflicts.
 * libgit2 only; tables are SoA. Lines are 0-based; byte offsets index the
 * texts passed in. */
#ifndef VX_GITHUNK_H
#define VX_GITHUNK_H

#include "base.h"

/* ---- diff of two texts ------------------------------------------------------ */

typedef struct {
	u32 n, cap;
	/* hunks with 0 context. A pure insertion has old_lines 0 and old_start
	 * = the old line it is inserted before; a pure deletion has new_lines 0
	 * and new_start = the new line it sits before. */
	u32 *old_start, *old_lines, *new_start, *new_lines;
	/* word changes of the hunk (paired old/new lines only): [chg_first,
	 * chg_first + chg_count) into the c_* arrays */
	u32 *chg_first, *chg_count;
	u32 nc, ccap;
	u32 *c_old_off, *c_old_len; /* byte range in the old text */
	u32 *c_new_off, *c_new_len; /* byte range in the new text */
	bool binary;                /* NUL bytes: no hunks */
} GitHunks;

/* Hunks old -> new (git's xdiff through git_diff_buffers), plus intra-line
 * changes for modified line pairs (token LCS, capped per line). 0 ok. */
int githunk_diff(const char *old, size_t no, const char *new_, size_t nn, GitHunks *out);
void githunk_free(GitHunks *h);

/* ---- sources ------------------------------------------------------------------ */

/* Blob text of relpath in the index (stage 0) / in HEAD. *text is malloc'd
 * and NUL terminated. 0 ok, 1 not there (*text = "" , *n = 0), -1 error. */
int githunk_read_index(const char *workdir, const char *relpath, char **text, size_t *n);
int githunk_read_head(const char *workdir, const char *relpath, char **text, size_t *n);

/* ---- ranges ------------------------------------------------------------------- */

/* Stage the index->worktree hunks intersecting any [line_lo[i], line_hi[i]]
 * (inclusive, 0-based worktree lines); a hunk touching a range is staged
 * whole. worktree_text NULL = the file is deleted: the entry is removed.
 * A file not in the index is added (mode 100644, 100755 if the worktree
 * file is executable). 0 ok. */
int githunk_stage_ranges(const char *workdir, const char *relpath, const char *worktree_text, size_t n,
                         const u32 *line_lo, const u32 *line_hi, u32 nranges);
/* Revert in the index the HEAD->index hunks intersecting the INDEX line
 * ranges. A file absent from HEAD whose index text becomes empty is
 * removed from the index. 0 ok. */
int githunk_unstage_ranges(const char *workdir, const char *relpath, const u32 *line_lo, const u32 *line_hi,
                           u32 nranges);
/* The worktree text with the index->worktree hunks intersecting the
 * worktree line ranges reverted to the index; malloc'd, NUL terminated
 * (*out_n bytes), NULL on error. The caller applies it as an edit. */
char *githunk_revert_ranges(const char *workdir, const char *relpath, const char *worktree_text, size_t n,
                            const u32 *line_lo, const u32 *line_hi, u32 nranges, size_t *out_n);

/* ---- conflicts --------------------------------------------------------------- */

enum { GC_BASE = 1, GC_OURS = 2, GC_THEIRS = 4 };

typedef struct {
	u32 n, cap;
	u32 *path; /* pool */
	u8 *sides; /* GC_* present */
	Pool pool;
} GitConflicts;

int githunk_conflicts(const char *workdir, GitConflicts *out); /* 0 ok */
void githunk_conflicts_free(GitConflicts *c);
/* Stage 1/2/3 blob texts (malloc'd, NULL when that side is absent). */
int githunk_conflict_blobs(const char *workdir, const char *relpath, char **base, size_t *nb, char **ours,
                           size_t *no, char **theirs, size_t *nt);
/* Add the worktree file to the index (clears the conflict); a missing
 * file is removed instead. Refused while the file still has conflict
 * markers unless force. 0 ok. */
int githunk_mark_resolved(const char *workdir, const char *relpath, bool force);

/* Conflict regions in a text: <<<<<<< [label], optional ||||||| [label]
 * (diff3), =======, >>>>>>> [label]. CRLF tolerant; unterminated or
 * malformed regions are skipped; a new <<<<<<< restarts the region. */
typedef struct {
	u32 n, cap;
	u32 *start, *base, *sep, *end; /* line numbers; base UINT32_MAX when absent */
	u32 *ours_label, *base_label, *theirs_label; /* pool ("" when absent) */
	Pool pool;
} GitMarkers;

u32 githunk_find_markers(const char *text, size_t n, GitMarkers *out); /* returns out->n */
void githunk_markers_free(GitMarkers *m);

/* Last error message (thread-local storage is not used: UI thread only). */
const char *githunk_error(void);

#endif
