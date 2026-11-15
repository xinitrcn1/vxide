/* Git actions through the git CLI (what VSCodium does): an async runner
 * with a FIFO queue on a worker thread for the SCM actions (stash, rebase,
 * merge, branch, fetch/pull/push, amend, ...), plus fast synchronous
 * queries (branches, stashes, tags, remotes, in-progress operation).
 *
 * Children run from an absolute git path (no PATH search in libc), no
 * shell, stdin from /dev/null unless text is given, and the environment
 * GIT_TERMINAL_PROMPT=0 GIT_EDITOR=true GIT_SEQUENCE_EDITOR=true LC_ALL=C
 * so nothing ever waits for a terminal or an editor. */
#ifndef VX_GITOPS_H
#define VX_GITOPS_H

#include "base.h"

/* ---- low level: run git once, synchronously ------------------------------ */

typedef struct {
	int status;   /* exit code; -1 when git could not be run, 128+sig on signal */
	char *out;    /* stdout, NUL terminated (malloc'd, never NULL on success) */
	size_t nout;
	char *err;    /* stderr, NUL terminated */
	size_t nerr;
} GitRun;

/* Absolute path of the git binary (resolved from PATH once), or NULL. */
const char *git_cli_path(void);
/* Run `git -C workdir argv...` with optional stdin text. pid_slot, when
 * not NULL, holds the child pid while it runs (0 otherwise) so another
 * thread can kill it. Returns r->status. */
int git_cli(const char *workdir, const char *const *argv, const char *stdin_text, GitRun *r,
            volatile int *pid_slot);
void git_run_free(GitRun *r);

/* ---- async runner ------------------------------------------------------- */

typedef struct GitOps GitOps;

typedef struct {
	u32 id;
	char *label; /* "Pulling..." */
	int status;  /* exit code (0 = success) */
	char *out, *err;
	u64 ns; /* duration */
} GitOpResult;

GitOps *gitops_start(const char *workdir); /* NULL without a git binary */
/* Stops the worker: queued ops are dropped, a running child is killed
 * (SIGTERM, then SIGKILL after 500 ms), then the thread is joined. */
void gitops_stop(GitOps *o);
const char *gitops_workdir(const GitOps *o);
/* Queue `git argv...` (argv NULL terminated, without "git"); returns the
 * op id (> 0). stdin_text may be NULL. */
u32 gitops_run(GitOps *o, const char *label, const char *const *argv, const char *stdin_text);
/* True while an op runs or is queued; label of the running one. */
bool gitops_busy(GitOps *o, char *label, size_t cap);
/* Next finished op, FIFO; false when none. Free with gitops_result_free. */
bool gitops_poll(GitOps *o, GitOpResult *out);
void gitops_result_free(GitOpResult *r);

/* ---- action helpers (VSCodium's Git commands); each returns an op id ---- */

u32 gitops_stash_push(GitOps *o, bool include_untracked, const char *message);
u32 gitops_stash_pop(GitOps *o, u32 index);
u32 gitops_stash_apply(GitOps *o, u32 index);
u32 gitops_stash_drop(GitOps *o, u32 index);
u32 gitops_checkout(GitOps *o, const char *branch, bool remote); /* remote: create a tracking branch */
u32 gitops_branch_create(GitOps *o, const char *name, const char *from /* NULL = HEAD */, bool checkout);
u32 gitops_branch_delete(GitOps *o, const char *name, bool force);
u32 gitops_branch_rename(GitOps *o, const char *new_name);
u32 gitops_merge(GitOps *o, const char *branch);
u32 gitops_merge_abort(GitOps *o);
u32 gitops_rebase(GitOps *o, const char *onto);
u32 gitops_rebase_continue(GitOps *o);
u32 gitops_rebase_abort(GitOps *o);
u32 gitops_rebase_skip(GitOps *o);
u32 gitops_cherry_pick(GitOps *o, const char *sha);
u32 gitops_cherry_pick_abort(GitOps *o);
u32 gitops_revert(GitOps *o, const char *sha);
u32 gitops_commit(GitOps *o, const char *message, bool amend, bool all, bool allow_empty);
u32 gitops_commit_amend_no_edit(GitOps *o);
u32 gitops_undo_last_commit(GitOps *o); /* reset --soft HEAD~1 */
u32 gitops_fetch(GitOps *o, bool all, bool prune);
u32 gitops_pull(GitOps *o, bool rebase);
u32 gitops_push(GitOps *o, bool force_with_lease);
u32 gitops_publish(GitOps *o, const char *remote, const char *branch); /* push -u */
u32 gitops_tag_create(GitOps *o, const char *name, const char *message /* NULL: lightweight */,
                      const char *sha /* NULL = HEAD */);
u32 gitops_tag_delete(GitOps *o, const char *name);

/* ---- synchronous queries ------------------------------------------------- */

enum { GB_LOCAL = 1, GB_REMOTE = 2, GB_CURRENT = 4 };

typedef struct {
	u32 n, cap;
	u32 *name;     /* short name: "main", "origin/main" (pool) */
	u32 *upstream; /* "origin/main" or "" (pool) */
	u32 *hash;     /* full object id (pool) */
	u8 *flags;     /* GB_* */
	u32 *ahead, *behind;
	i64 *time;     /* committer date of the tip */
	Pool pool;
} GitBranchList;

typedef struct {
	u32 n, cap;
	u32 *ref;     /* "stash@{0}" (pool) */
	u32 *message; /* "WIP on main: abc123 subject" (pool) */
	Pool pool;
} GitStashList;

typedef struct {
	u32 n, cap;
	u32 *name, *hash; /* pool */
	Pool pool;
} GitNameList; /* tags (name, target) and remotes (name, fetch url) */

enum {
	GST_REBASE = 1,      /* rebase-merge or rebase-apply */
	GST_MERGE = 2,       /* MERGE_HEAD */
	GST_CHERRY_PICK = 4, /* CHERRY_PICK_HEAD */
	GST_REVERT = 8,      /* REVERT_HEAD */
	GST_BISECT = 16,     /* BISECT_LOG */
};

int gitops_branches(GitOps *o, GitBranchList *out); /* 0 on success */
int gitops_stashes(GitOps *o, GitStashList *out);
int gitops_tags(GitOps *o, GitNameList *out);
int gitops_remotes(GitOps *o, GitNameList *out);
u32 gitops_state(GitOps *o); /* GST_* bits */
void gitops_branches_free(GitBranchList *l);
void gitops_stashes_free(GitStashList *l);
void gitops_names_free(GitNameList *l);

#endif
