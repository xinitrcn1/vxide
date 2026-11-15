/* Native git integration on top of libgit2.
 * Data is kept as flat tables, refreshed on demand (file save, focus, timer). */
#ifndef VX_GIT_H
#define VX_GIT_H

#include "base.h"

/* Per-path status bits (combined index + worktree). */
enum {
	GS_INDEX_NEW = 1 << 0,
	GS_INDEX_MODIFIED = 1 << 1,
	GS_INDEX_DELETED = 1 << 2,
	GS_INDEX_RENAMED = 1 << 3,
	GS_WT_NEW = 1 << 4, /* untracked */
	GS_WT_MODIFIED = 1 << 5,
	GS_WT_DELETED = 1 << 6,
	GS_WT_RENAMED = 1 << 7,
	GS_IGNORED = 1 << 8,
	GS_CONFLICT = 1 << 9,
};

/* Gutter marks per line (quick diff of the buffer against the index,
 * like VSCodium's SCM gutter). */
enum { GM_NONE, GM_ADDED, GM_MODIFIED, GM_DELETED_ABOVE };

typedef struct Git Git;

/* Status table, SoA. path = repo-relative, '/' separated, in pool. */
typedef struct {
	u32 count;
	u32 *path; /* offsets into pool */
	u16 *flags;
	Pool pool;
} GitStatus;

Git *git_open(const char *dir); /* NULL when dir is not inside a repo */
/* scan = false: open without computing the status (see git_refresh_async). */
Git *git_open_ex(const char *dir, bool scan);
void git_close(Git *g);
const char *git_workdir(Git *g); /* absolute, with trailing '/' */

/* Re-read status, branch, ahead/behind. Returns 0 on success. */
int git_refresh(Git *g);
/* Recompute the status on a background thread (own repository handle,
 * refreshed stat data written back to the index like `git status`). The
 * UI thread calls git_poll each tick: true when a new table was swapped
 * in. git_refreshing tells whether a refresh is pending or running. */
int git_refresh_async(Git *g);
bool git_poll(Git *g);
bool git_refreshing(Git *g);
const GitStatus *git_status(Git *g);
const char *git_branch(Git *g); /* "main", or short hash when detached */
void git_ahead_behind(Git *g, u32 *ahead, u32 *behind);

/* Status of a repo-relative path; for directories, OR of all children
 * (so the explorer can colour folders). 0 when clean. */
u16 git_path_flags(Git *g, const char *relpath);

/* Index operations; repo-relative paths. Return 0 on success. */
int git_stage(Git *g, const char *relpath);
int git_unstage(Git *g, const char *relpath);
int git_stage_all(Git *g);
int git_discard(Git *g, const char *relpath); /* checkout from index; untracked -> delete */
int git_commit_staged(Git *g, const char *message); /* uses user.name/user.email */

/* Quick diff: compare text (the live buffer) with the index version of
 * relpath. Writes nlines marks (GM_*). Returns 0 on success; when the file
 * is not tracked every line is GM_ADDED. */
int git_line_marks(Git *g, const char *relpath, const char *text, size_t n,
                   u8 *marks, u32 nlines);

/* Unified diff text (index vs worktree, or HEAD vs index when staged != 0)
 * for one path; malloc'd, NUL terminated. */
char *git_diff_text(Git *g, const char *relpath, int staged);

/* Last libgit2 error message (static storage). */
const char *git_last_error(void);

#endif
