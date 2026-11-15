/* Git actions and queries on libgit2 (see gitops.h). The git CLI runner
 * at the top is only a fallback: network operations libgit2 cannot
 * authenticate (https credential helpers) and raw gitops_run() argv. */
#define _POSIX_C_SOURCE 200809L
#include "gitops.h"

#include <git2.h>

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- git binary and environment ------------------------------------------- */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char *g_git;   /* absolute path or NULL */
static char **g_envp; /* environ + our overrides, built once */

static bool env_skip(const char *e)
{
	static const char *drop[] = {"GIT_TERMINAL_PROMPT=", "GIT_EDITOR=", "GIT_SEQUENCE_EDITOR=", "LC_ALL=",
	                             "GIT_PAGER=", "PAGER=", "GIT_ASKPASS=", "SSH_ASKPASS="};
	for (size_t i = 0; i < VX_COUNT(drop); i++)
		if (!strncmp(e, drop[i], strlen(drop[i])))
			return true;
	return false;
}

static void init_once(void)
{
	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/local/bin:/usr/bin:/bin";
	char buf[4096];
	while (*path) {
		const char *e = strchr(path, ':');
		size_t n = e ? (size_t)(e - path) : strlen(path);
		if (n) {
			snprintf(buf, sizeof buf, "%.*s/git", (int)n, path);
			struct stat st;
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0) {
				g_git = vx_strdup(buf);
				break;
			}
		}
		path += n + (e ? 1 : 0);
	}
	size_t ne = 0;
	for (char **e = environ; e && *e; e++)
		ne++;
	static const char *add[] = {"GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=true", "GIT_SEQUENCE_EDITOR=true",
	                            "LC_ALL=C", "GIT_PAGER=cat", "PAGER=cat"};
	g_envp = vx_alloc((ne + VX_COUNT(add) + 1) * sizeof(char *));
	size_t k = 0;
	for (char **e = environ; e && *e; e++)
		if (!env_skip(*e))
			g_envp[k++] = vx_strdup(*e);
	for (size_t i = 0; i < VX_COUNT(add); i++)
		g_envp[k++] = vx_strdup(add[i]);
	g_envp[k] = NULL;
	/* writing to a child that exited must not kill us */
	struct sigaction sa;
	if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGPIPE, &sa, NULL);
	}
}

const char *git_cli_path(void)
{
	pthread_once(&g_once, init_once);
	return g_git;
}

/* ---- one synchronous run ------------------------------------------------------ */

static void buf_add(char **b, size_t *n, size_t *cap, const char *s, size_t k)
{
	if (*n + k + 1 > *cap) {
		size_t c = *cap ? *cap : 4096;
		while (c < *n + k + 1)
			c *= 2;
		*b = vx_realloc(*b, c);
		*cap = c;
	}
	memcpy(*b + *n, s, k);
	*n += k;
	(*b)[*n] = 0;
}

int git_cli(const char *workdir, const char *const *argv, const char *stdin_text, GitRun *r,
            volatile int *pid_slot)
{
	memset(r, 0, sizeof *r);
	r->status = -1;
	const char *git = git_cli_path();
	if (!git)
		return -1;
	size_t na = 0;
	while (argv[na])
		na++;
	char **av = vx_alloc((na + 4) * sizeof(char *));
	av[0] = (char *)git;
	av[1] = "-C";
	av[2] = (char *)workdir;
	for (size_t i = 0; i < na; i++)
		av[3 + i] = (char *)argv[i];
	av[3 + na] = NULL;

	int pin[2] = {-1, -1}, pout[2], perr[2];
	if (pipe(pout) != 0) {
		free(av);
		return -1;
	}
	if (pipe(perr) != 0) {
		close(pout[0]);
		close(pout[1]);
		free(av);
		return -1;
	}
	if (stdin_text && pipe(pin) != 0) {
		close(pout[0]);
		close(pout[1]);
		close(perr[0]);
		close(perr[1]);
		free(av);
		return -1;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	if (stdin_text) {
		posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
		posix_spawn_file_actions_addclose(&fa, pin[1]);
	} else {
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	}
	posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
	posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
	posix_spawn_file_actions_addclose(&fa, pout[0]);
	posix_spawn_file_actions_addclose(&fa, perr[0]);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	/* own process group: no terminal signals, and stop can kill the group */
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	posix_spawnattr_setpgroup(&at, 0);
	sigset_t none, def;
	sigemptyset(&none);
	posix_spawnattr_setsigmask(&at, &none);
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	sigaddset(&def, SIGTERM);
	sigaddset(&def, SIGINT);
	posix_spawnattr_setsigdefault(&at, &def);
	pid_t pid;
	int rc = posix_spawn(&pid, git, &fa, &at, av, g_envp);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	free(av);
	close(pout[1]);
	close(perr[1]);
	if (stdin_text)
		close(pin[0]);
	if (rc != 0) {
		close(pout[0]);
		close(perr[0]);
		if (stdin_text)
			close(pin[1]);
		return -1;
	}
	if (pid_slot)
		*pid_slot = (int)pid;

	size_t in_off = 0, in_len = stdin_text ? strlen(stdin_text) : 0;
	int in_fd = stdin_text ? pin[1] : -1;
	if (in_fd >= 0) {
		fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL) | O_NONBLOCK);
		if (in_len == 0) {
			close(in_fd);
			in_fd = -1;
		}
	}
	size_t ocap = 0, ecap = 0;
	buf_add(&r->out, &r->nout, &ocap, "", 0);
	buf_add(&r->err, &r->nerr, &ecap, "", 0);
	int out_fd = pout[0], err_fd = perr[0];
	char buf[65536];
	while (out_fd >= 0 || err_fd >= 0) {
		struct pollfd pf[3];
		int np = 0, io = -1, ie = -1, ii = -1;
		if (out_fd >= 0) {
			pf[np].fd = out_fd;
			pf[np].events = POLLIN;
			io = np++;
		}
		if (err_fd >= 0) {
			pf[np].fd = err_fd;
			pf[np].events = POLLIN;
			ie = np++;
		}
		if (in_fd >= 0) {
			pf[np].fd = in_fd;
			pf[np].events = POLLOUT;
			ii = np++;
		}
		if (poll(pf, (nfds_t)np, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ii >= 0 && (pf[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
			ssize_t w = write(in_fd, stdin_text + in_off, in_len - in_off);
			if (w > 0)
				in_off += (size_t)w;
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				in_off = in_len;
			if (in_off >= in_len) {
				close(in_fd);
				in_fd = -1;
			}
		}
		if (io >= 0 && (pf[io].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t k = read(out_fd, buf, sizeof buf);
			if (k > 0)
				buf_add(&r->out, &r->nout, &ocap, buf, (size_t)k);
			else if (k == 0 || (errno != EINTR && errno != EAGAIN)) {
				close(out_fd);
				out_fd = -1;
			}
		}
		if (ie >= 0 && (pf[ie].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t k = read(err_fd, buf, sizeof buf);
			if (k > 0)
				buf_add(&r->err, &r->nerr, &ecap, buf, (size_t)k);
			else if (k == 0 || (errno != EINTR && errno != EAGAIN)) {
				close(err_fd);
				err_fd = -1;
			}
		}
	}
	if (in_fd >= 0)
		close(in_fd);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (pid_slot)
		*pid_slot = 0;
	if (WIFEXITED(st))
		r->status = WEXITSTATUS(st);
	else if (WIFSIGNALED(st))
		r->status = 128 + WTERMSIG(st);
	return r->status;
}

void git_run_free(GitRun *r)
{
	free(r->out);
	free(r->err);
	memset(r, 0, sizeof *r);
}

/* ---- libgit2 setup --------------------------------------------------------------- */

/* One-time libgit2 settings tuned for a slow disk: keep more commits and
 * trees in the object cache so repeated graph/blame/details queries do not
 * go back to the packfiles. */
static pthread_once_t g_lg_once = PTHREAD_ONCE_INIT;

static void lg_init_once(void)
{
	git_libgit2_init();
	git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_COMMIT, (size_t)16384);
	git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_TREE, (size_t)65536);
	/* blobs up to 4 MiB too (libgit2 caches none by default): re-showing a
	 * diff or re-blaming a file then skips inflating its delta chain again */
	git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT, GIT_OBJECT_BLOB, (size_t)(4 << 20));
	git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, (ssize_t)(128 << 20));
}

void gitops_libgit2_init(void) { pthread_once(&g_lg_once, lg_init_once); }

/* ---- op queue --------------------------------------------------------------------- */

enum {
	OP_CLI,
	OP_STASH_PUSH,
	OP_STASH_POP,
	OP_STASH_APPLY,
	OP_STASH_DROP,
	OP_CHECKOUT,
	OP_DETACH,
	OP_BRANCH_CREATE,
	OP_BRANCH_DELETE,
	OP_BRANCH_RENAME,
	OP_MERGE,
	OP_MERGE_ABORT,
	OP_REBASE,
	OP_REBASE_CONTINUE,
	OP_REBASE_ABORT,
	OP_REBASE_SKIP,
	OP_CHERRY_PICK,
	OP_CHERRY_PICK_ABORT,
	OP_REVERT,
	OP_COMMIT,
	OP_AMEND_NO_EDIT,
	OP_UNDO_COMMIT,
	OP_UNSTAGE_ALL,
	OP_DISCARD_ALL,
	OP_FETCH,
	OP_PULL,
	OP_PUSH,
	OP_PUBLISH,
	OP_TAG_CREATE,
	OP_TAG_DELETE,
};

enum { F_A = 1, F_B = 2, F_C = 4, F_D = 8 }; /* per-op flags */

/* Queue entries (SoA). String arguments are packed NUL-separated; for
 * OP_CLI they are the argv. */
typedef struct {
	u32 n, cap;
	u32 *id;
	u8 *kind;
	u32 *flags, *index;
	char **label;
	char **args; /* "a\0b\0\0" */
	char **in;   /* stdin text (CLI) or message */
} OpQueue;

typedef struct {
	u32 n, cap;
	u32 *id;
	char **label;
	int *status;
	char **out, **err;
	u64 *ns;
} DoneQueue;

#define OPQ_COLS(q) SOA((q)->label), SOA((q)->args), SOA((q)->in), SOA((q)->id), SOA((q)->flags), SOA((q)->index), SOA((q)->kind)
#define DONEQ_COLS(d) SOA((d)->label), SOA((d)->out), SOA((d)->err), SOA((d)->ns), SOA((d)->id), SOA((d)->status)

struct GitOps {
	char *workdir;
	git_repository *wrepo; /* worker thread only */
	git_repository *qrepo; /* caller thread (queries) only */
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool quit;
	bool running;
	char *running_label;
	volatile int pid;    /* running CLI child */
	volatile int cancel; /* set by stop: network transfers abort */
	u32 next_id;
	OpQueue q;
	DoneQueue d;
};

static void q_push(OpQueue *q, u32 id, u8 kind, u32 flags, u32 index, char *label, char *args, char *in)
{
	if (q->n == q->cap) {
		SoaCol c[] = {OPQ_COLS(q)};
		soa_grow(c, 7, &q->cap, q->n + 1, q->n);
	}
	u32 i = q->n++;
	q->id[i] = id;
	q->kind[i] = kind;
	q->flags[i] = flags;
	q->index[i] = index;
	q->label[i] = label;
	q->args[i] = args;
	q->in[i] = in;
}

static void q_pop_front(OpQueue *q)
{
	q->n--;
	memmove(q->id, q->id + 1, q->n * sizeof(u32));
	memmove(q->kind, q->kind + 1, q->n);
	memmove(q->flags, q->flags + 1, q->n * sizeof(u32));
	memmove(q->index, q->index + 1, q->n * sizeof(u32));
	memmove(q->label, q->label + 1, q->n * sizeof(char *));
	memmove(q->args, q->args + 1, q->n * sizeof(char *));
	memmove(q->in, q->in + 1, q->n * sizeof(char *));
}

static void d_push(DoneQueue *d, u32 id, char *label, int status, char *out, char *err, u64 ns)
{
	if (d->n == d->cap) {
		SoaCol c[] = {DONEQ_COLS(d)};
		soa_grow(c, 6, &d->cap, d->n + 1, d->n);
	}
	u32 i = d->n++;
	d->id[i] = id;
	d->label[i] = label;
	d->status[i] = status;
	d->out[i] = out;
	d->err[i] = err;
	d->ns[i] = ns;
}

/* ---- op context: output text ------------------------------------------------------ */

typedef struct {
	GitOps *o;
	git_repository *r;
	char *out, *err;
	size_t nout, nerr, cout, cerr;
	int cred_tries;
	int cred_step;
	char push_err[512];
} Ctx;

static void say(Ctx *c, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	buf_add(&c->out, &c->nout, &c->cout, buf, (size_t)VX_MIN(n, (int)sizeof buf - 1));
	buf_add(&c->out, &c->nout, &c->cout, "\n", 1);
}

static int complain(Ctx *c, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 1;
	buf_add(&c->err, &c->nerr, &c->cerr, buf, (size_t)VX_MIN(n, (int)sizeof buf - 1));
	buf_add(&c->err, &c->nerr, &c->cerr, "\n", 1);
	return 1;
}

/* Report the last libgit2 error with context; returns a nonzero status. */
static int lgfail(Ctx *c, const char *what)
{
	const git_error *e = git_error_last();
	return complain(c, "error: %s%s%s", what, e && e->message ? ": " : "", e && e->message ? e->message : "");
}

static int sig_default(Ctx *c, git_signature **sig)
{
	if (git_signature_default(sig, c->r) != 0)
		return complain(c, "error: no user identity (set user.name and user.email in your git config)");
	return 0;
}

static int head_commit(Ctx *c, git_commit **out)
{
	git_object *o = NULL;
	if (git_revparse_single(&o, c->r, "HEAD^{commit}") != 0)
		return -1;
	*out = (git_commit *)o;
	return 0;
}

static const char *current_branch_name(git_repository *r, char *buf, size_t cap)
{
	git_reference *h = NULL;
	buf[0] = 0;
	if (git_repository_head(&h, r) == 0 && git_reference_is_branch(h))
		snprintf(buf, cap, "%s", git_reference_shorthand(h));
	git_reference_free(h);
	return buf;
}

static void checkout_opts(git_checkout_options *co, bool force)
{
	git_checkout_options_init(co, GIT_CHECKOUT_OPTIONS_VERSION);
	co->checkout_strategy = force ? GIT_CHECKOUT_FORCE : GIT_CHECKOUT_SAFE;
}

/* List the conflicted paths into err; returns how many there are. */
static u32 report_conflicts(Ctx *c, const char *kind)
{
	git_index *idx = NULL;
	if (git_repository_index(&idx, c->r) != 0)
		return 0;
	u32 n = 0;
	if (git_index_has_conflicts(idx)) {
		git_index_conflict_iterator *it = NULL;
		const git_index_entry *a, *o, *t;
		if (git_index_conflict_iterator_new(&it, idx) == 0) {
			while (git_index_conflict_next(&a, &o, &t, it) == 0) {
				const char *p = o ? o->path : t ? t->path : a ? a->path : "?";
				complain(c, "CONFLICT (content): %s conflict in %s", kind, p);
				n++;
			}
			git_index_conflict_iterator_free(it);
		}
	}
	git_index_free(idx);
	return n;
}

/* Create a commit on HEAD from the index with the given parents. */
static int commit_index(Ctx *c, const char *msg, git_commit **parents, size_t np, const git_signature *author,
                        git_oid *out)
{
	git_index *idx = NULL;
	git_oid tree_id;
	git_tree *tree = NULL;
	git_signature *me = NULL;
	int rc = 1;
	if (git_repository_index(&idx, c->r) != 0 || git_index_write_tree(&tree_id, idx) != 0 ||
	    git_tree_lookup(&tree, c->r, &tree_id) != 0) {
		rc = lgfail(c, "cannot write the index tree");
		goto done;
	}
	if (sig_default(c, &me) != 0)
		goto done;
	if (git_commit_create(out, c->r, "HEAD", author ? author : me, me, NULL, msg, tree, np,
	                      (const git_commit **)parents) != 0) {
		rc = lgfail(c, "cannot create the commit");
		goto done;
	}
	rc = 0;
done:
	git_signature_free(me);
	git_tree_free(tree);
	git_index_free(idx);
	return rc;
}

/* Resolve a branch-ish name to an annotated commit (local, remote, tag, sha). */
static int annotated(Ctx *c, const char *name, git_annotated_commit **out)
{
	git_reference *ref = NULL;
	if (git_reference_dwim(&ref, c->r, name) == 0) {
		int rc = git_annotated_commit_from_ref(out, c->r, ref);
		git_reference_free(ref);
		if (rc == 0)
			return 0;
	}
	if (git_annotated_commit_from_revspec(out, c->r, name) == 0)
		return 0;
	return lgfail(c, name);
}

/* ---- stash ------------------------------------------------------------------------- */

static int stash_msg_cb(size_t index, const char *message, const git_oid *id, void *payload)
{
	(void)id;
	if (index == 0)
		snprintf(payload, 512, "%s", message);
	return 1; /* only the newest */
}

static int op_stash_push(Ctx *c, bool untracked, const char *msg)
{
	git_signature *sig = NULL;
	if (sig_default(c, &sig) != 0)
		return 1;
	git_oid id;
	int rc = git_stash_save(&id, c->r, sig, msg && *msg ? msg : NULL,
	                        untracked ? GIT_STASH_INCLUDE_UNTRACKED : GIT_STASH_DEFAULT);
	git_signature_free(sig);
	if (rc == GIT_ENOTFOUND) {
		say(c, "No local changes to save");
		return 0;
	}
	if (rc != 0)
		return lgfail(c, "stash failed");
	char m[512] = "";
	git_stash_foreach(c->r, stash_msg_cb, m);
	say(c, "Saved working directory and index state %s", m);
	return 0;
}

static int op_stash_apply(Ctx *c, u32 index, bool pop)
{
	git_stash_apply_options so;
	git_stash_apply_options_init(&so, GIT_STASH_APPLY_OPTIONS_VERSION);
	checkout_opts(&so.checkout_options, false);
	int rc = pop ? git_stash_pop(c->r, index, &so) : git_stash_apply(c->r, index, &so);
	if (rc != 0) {
		if (report_conflicts(c, "stash"))
			return complain(c, "error: the stash entry is kept in case you need it again");
		return lgfail(c, pop ? "stash pop failed" : "stash apply failed");
	}
	say(c, pop ? "Dropped stash@{%u} after applying it" : "Applied stash@{%u}", index);
	return 0;
}

/* ---- branches ---------------------------------------------------------------------- */

static int set_head_branch(Ctx *c, const char *refname, git_object *target)
{
	git_checkout_options co;
	checkout_opts(&co, false);
	if (git_checkout_tree(c->r, target, &co) != 0)
		return lgfail(c, "checkout failed (local changes would be overwritten?)");
	if (git_repository_set_head(c->r, refname) != 0)
		return lgfail(c, "cannot move HEAD");
	return 0;
}

static bool is_remote_name(git_repository *r, const char *name, size_t len)
{
	git_strarray rl = {0};
	bool ok = false;
	if (git_remote_list(&rl, r) == 0)
		for (size_t i = 0; i < rl.count; i++)
			if (strlen(rl.strings[i]) == len && !strncmp(rl.strings[i], name, len))
				ok = true;
	git_strarray_dispose(&rl);
	return ok;
}

static int op_checkout(Ctx *c, const char *name, bool remote)
{
	char local[512], refname[600];
	snprintf(local, sizeof local, "%s", name);
	git_reference *rb = NULL;
	if (remote) {
		/* origin/topic -> topic tracking origin/topic (git switch --track) */
		const char *sl = strchr(name, '/');
		while (sl && !is_remote_name(c->r, name, (size_t)(sl - name)))
			sl = strchr(sl + 1, '/');
		if (!sl)
			return complain(c, "error: '%s' is not a remote-tracking branch", name);
		snprintf(local, sizeof local, "%s", sl + 1);
		if (git_branch_lookup(&rb, c->r, name, GIT_BRANCH_REMOTE) != 0)
			return lgfail(c, name);
	}
	git_reference *lb = NULL;
	if (git_branch_lookup(&lb, c->r, local, GIT_BRANCH_LOCAL) != 0) {
		if (!remote) {
			git_reference_free(rb);
			return lgfail(c, local);
		}
		git_commit *tip = NULL;
		if (git_commit_lookup(&tip, c->r, git_reference_target(rb)) != 0 ||
		    git_branch_create(&lb, c->r, local, tip, 0) != 0) {
			git_commit_free(tip);
			git_reference_free(rb);
			return lgfail(c, "cannot create the tracking branch");
		}
		git_commit_free(tip);
		if (git_branch_set_upstream(lb, name) != 0) {
			git_reference_free(rb);
			git_reference_free(lb);
			return lgfail(c, "cannot set the upstream");
		}
		say(c, "branch '%s' set up to track '%s'.", local, name);
	}
	git_reference_free(rb);
	git_object *target = NULL;
	snprintf(refname, sizeof refname, "%s", git_reference_name(lb));
	int rc = git_reference_peel(&target, lb, GIT_OBJECT_COMMIT) != 0 ? lgfail(c, local)
	                                                                  : set_head_branch(c, refname, target);
	git_object_free(target);
	git_reference_free(lb);
	if (rc == 0)
		say(c, "Switched to branch '%s'", local);
	return rc;
}

static int op_detach(Ctx *c, const char *rev)
{
	git_object *obj = NULL;
	if (git_revparse_single(&obj, c->r, rev) != 0)
		return lgfail(c, rev);
	git_object *commit = NULL;
	int rc = 1;
	if (git_object_peel(&commit, obj, GIT_OBJECT_COMMIT) != 0) {
		rc = lgfail(c, rev);
	} else {
		git_checkout_options co;
		checkout_opts(&co, false);
		if (git_checkout_tree(c->r, commit, &co) != 0)
			rc = lgfail(c, "checkout failed");
		else if (git_repository_set_head_detached(c->r, git_object_id(commit)) != 0)
			rc = lgfail(c, "cannot detach HEAD");
		else {
			char h[12];
			git_oid_tostr(h, sizeof h, git_object_id(commit));
			say(c, "HEAD is now at %s", h);
			rc = 0;
		}
	}
	git_object_free(commit);
	git_object_free(obj);
	return rc;
}

static int op_branch_create(Ctx *c, const char *name, const char *from, bool checkout)
{
	/* the reflog says "branch: Created from <from>" and VSCodium's
	 * branch.<name>.vscode-merge-base records the base for the graph */
	char cur[256];
	const char *src = from && *from ? from : current_branch_name(c->r, cur, sizeof cur);
	if (!*src)
		src = "HEAD";
	git_annotated_commit *ac = NULL;
	if (annotated(c, src, &ac) != 0)
		return 1;
	git_reference *ref = NULL;
	int rc = git_branch_create_from_annotated(&ref, c->r, name, ac, 0);
	git_annotated_commit_free(ac);
	if (rc != 0)
		return lgfail(c, "cannot create the branch");
	git_reference *src_ref = NULL;
	if (strcmp(src, "HEAD") && git_reference_dwim(&src_ref, c->r, src) == 0 &&
	    (git_reference_is_branch(src_ref) || git_reference_is_remote(src_ref))) {
		git_config *cfg = NULL;
		char key[600];
		snprintf(key, sizeof key, "branch.%s.vscode-merge-base", name);
		if (git_repository_config(&cfg, c->r) == 0)
			git_config_set_string(cfg, key, git_reference_shorthand(src_ref));
		git_config_free(cfg);
	}
	git_reference_free(src_ref);
	if (checkout) {
		git_object *t = NULL;
		rc = git_reference_peel(&t, ref, GIT_OBJECT_COMMIT) != 0 ? lgfail(c, name)
		                                                          : set_head_branch(c, git_reference_name(ref), t);
		git_object_free(t);
		if (rc == 0)
			say(c, "Switched to a new branch '%s'", name);
	} else {
		say(c, "Created branch '%s'", name);
	}
	git_reference_free(ref);
	return rc;
}

static int op_branch_delete(Ctx *c, const char *name, bool force)
{
	git_reference *b = NULL;
	if (git_branch_lookup(&b, c->r, name, GIT_BRANCH_LOCAL) != 0)
		return complain(c, "error: branch '%s' not found", name);
	if (git_branch_is_head(b) == 1) {
		git_reference_free(b);
		return complain(c, "error: cannot delete branch '%s' used by worktree", name);
	}
	if (!force) {
		/* merged into HEAD (or into its upstream, like git branch -d) */
		const git_oid *tip = git_reference_target(b);
		git_oid head;
		bool merged = false;
		if (git_reference_name_to_id(&head, c->r, "HEAD") == 0)
			merged = git_oid_equal(&head, tip) || git_graph_descendant_of(c->r, &head, tip) == 1;
		git_reference *up = NULL;
		if (!merged && git_branch_upstream(&up, b) == 0) {
			const git_oid *u = git_reference_target(up);
			merged = u && (git_oid_equal(u, tip) || git_graph_descendant_of(c->r, u, tip) == 1);
		}
		git_reference_free(up);
		if (!merged) {
			git_reference_free(b);
			complain(c, "error: The branch '%s' is not fully merged.", name);
			return complain(c, "If you are sure you want to delete it, run 'git branch -D %s'.", name);
		}
	}
	char h[12];
	git_oid_tostr(h, sizeof h, git_reference_target(b));
	int rc = git_branch_delete(b);
	git_reference_free(b);
	if (rc != 0)
		return lgfail(c, "cannot delete the branch");
	say(c, "Deleted branch %s (was %s).", name, h);
	return 0;
}

static int op_branch_rename(Ctx *c, const char *nn)
{
	git_reference *h = NULL, *out = NULL;
	if (git_repository_head(&h, c->r) != 0 || !git_reference_is_branch(h)) {
		git_reference_free(h);
		return complain(c, "error: not on a branch");
	}
	int rc = git_branch_move(&out, h, nn, 0);
	git_reference_free(h);
	git_reference_free(out);
	if (rc != 0)
		return lgfail(c, "cannot rename the branch");
	say(c, "Renamed the branch to '%s'", nn);
	return 0;
}

/* ---- merge ------------------------------------------------------------------------- */

static int fast_forward(Ctx *c, const git_oid *to, const char *what)
{
	git_reference *head = NULL, *target_ref = NULL, *nw = NULL;
	git_object *target = NULL;
	int rc = 1;
	if (git_object_lookup(&target, c->r, to, GIT_OBJECT_COMMIT) != 0) {
		rc = lgfail(c, "fast-forward");
		goto done;
	}
	git_checkout_options co;
	checkout_opts(&co, false);
	if (git_checkout_tree(c->r, target, &co) != 0) {
		rc = lgfail(c, "fast-forward checkout failed");
		goto done;
	}
	if (git_repository_head(&head, c->r) == 0) {
		char msg[600];
		snprintf(msg, sizeof msg, "%s: Fast-forward", what);
		if (git_reference_set_target(&nw, head, to, msg) != 0) {
			rc = lgfail(c, "cannot update the branch");
			goto done;
		}
	} else if (git_repository_set_head_detached(c->r, to) != 0) {
		rc = lgfail(c, "cannot update HEAD");
		goto done;
	}
	say(c, "Fast-forward");
	rc = 0;
done:
	git_reference_free(nw);
	git_reference_free(target_ref);
	git_reference_free(head);
	git_object_free(target);
	return rc;
}

static int merge_annotated(Ctx *c, git_annotated_commit *ac, const char *name)
{
	git_merge_analysis_t an;
	git_merge_preference_t pref;
	const git_annotated_commit *heads[1] = {ac};
	if (git_merge_analysis(&an, &pref, c->r, heads, 1) != 0)
		return lgfail(c, "merge analysis failed");
	if (an & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
		say(c, "Already up to date.");
		return 0;
	}
	char what[600];
	snprintf(what, sizeof what, "merge %s", name);
	if ((an & (GIT_MERGE_ANALYSIS_FASTFORWARD | GIT_MERGE_ANALYSIS_UNBORN)) &&
	    !(pref & GIT_MERGE_PREFERENCE_NO_FASTFORWARD))
		return fast_forward(c, git_annotated_commit_id(ac), what);
	git_merge_options mo;
	git_merge_options_init(&mo, GIT_MERGE_OPTIONS_VERSION);
	git_checkout_options co;
	checkout_opts(&co, false);
	co.checkout_strategy |= GIT_CHECKOUT_ALLOW_CONFLICTS;
	if (git_merge(c->r, heads, 1, &mo, &co) != 0)
		return lgfail(c, "merge failed");
	if (report_conflicts(c, "merge"))
		return complain(c, "Automatic merge failed; fix conflicts and then commit the result.");
	git_commit *parents[2] = {NULL, NULL};
	int rc = 1;
	if (head_commit(c, &parents[0]) != 0 || git_commit_lookup(&parents[1], c->r, git_annotated_commit_id(ac)) != 0) {
		rc = lgfail(c, "merge");
	} else {
		git_reference *rr = NULL;
		bool remote = git_reference_dwim(&rr, c->r, name) == 0 && git_reference_is_remote(rr);
		git_reference_free(rr);
		char msg[700];
		snprintf(msg, sizeof msg, "Merge %s '%s'\n", remote ? "remote-tracking branch" : "branch", name);
		git_oid id;
		rc = commit_index(c, msg, parents, 2, NULL, &id);
		if (rc == 0) {
			git_repository_state_cleanup(c->r);
			say(c, "Merge made by the 'ort' strategy.");
		}
	}
	git_commit_free(parents[0]);
	git_commit_free(parents[1]);
	return rc;
}

static int op_merge(Ctx *c, const char *name)
{
	git_annotated_commit *ac = NULL;
	if (annotated(c, name, &ac) != 0)
		return 1;
	int rc = merge_annotated(c, ac, name);
	git_annotated_commit_free(ac);
	return rc;
}

/* merge --abort / cherry-pick --abort: back to HEAD and clear the state */
static int op_reset_state(Ctx *c, const char *what)
{
	git_commit *h = NULL;
	if (head_commit(c, &h) != 0)
		return lgfail(c, what);
	git_checkout_options co;
	checkout_opts(&co, true);
	int rc = git_reset(c->r, (git_object *)h, GIT_RESET_HARD, &co);
	git_commit_free(h);
	if (rc != 0)
		return lgfail(c, what);
	git_repository_state_cleanup(c->r);
	say(c, "%s", what);
	return 0;
}

/* ---- rebase ------------------------------------------------------------------------ */

static void rebase_opts(git_rebase_options *ro)
{
	git_rebase_options_init(ro, GIT_REBASE_OPTIONS_VERSION);
	checkout_opts(&ro->checkout_options, false);
}

/* Commit the current operation (if any) and apply the rest. */
static int rebase_run(Ctx *c, git_rebase *rb, bool commit_current)
{
	git_signature *me = NULL;
	if (sig_default(c, &me) != 0)
		return 1;
	int rc = 0;
	u32 applied = 0;
	if (commit_current) {
		if (report_conflicts(c, "rebase")) {
			git_signature_free(me);
			return complain(c, "error: resolve the conflicts, stage the files, then continue");
		}
		git_oid id;
		int k = git_rebase_commit(&id, rb, NULL, me, NULL, NULL);
		if (k != 0 && k != GIT_EAPPLIED) {
			git_signature_free(me);
			return lgfail(c, "cannot commit the rebased change");
		}
		applied++;
	}
	git_rebase_operation *op;
	for (;;) {
		int k = git_rebase_next(&op, rb);
		if (k == GIT_ITEROVER)
			break;
		if (k != 0) {
			if (report_conflicts(c, "rebase")) {
				char h[12];
				git_oid_tostr(h, sizeof h, &op->id);
				complain(c, "error: could not apply %s", h);
				rc = complain(c, "Resolve all conflicts, stage them, then Continue Rebase (or Abort Rebase).");
			} else {
				rc = lgfail(c, "rebase stopped");
			}
			git_signature_free(me);
			return rc;
		}
		if (report_conflicts(c, "rebase")) {
			char h[12];
			git_oid_tostr(h, sizeof h, &op->id);
			complain(c, "error: could not apply %s", h);
			git_signature_free(me);
			return complain(c, "Resolve all conflicts, stage them, then Continue Rebase (or Abort Rebase).");
		}
		git_oid id;
		k = git_rebase_commit(&id, rb, NULL, me, NULL, NULL);
		if (k != 0 && k != GIT_EAPPLIED) {
			git_signature_free(me);
			return lgfail(c, "cannot commit the rebased change");
		}
		applied++;
	}
	if (git_rebase_finish(rb, me) != 0)
		rc = lgfail(c, "cannot finish the rebase");
	else
		say(c, "Successfully rebased and updated %s (%u commit%s).", "HEAD", applied, applied == 1 ? "" : "s");
	git_signature_free(me);
	return rc;
}

static int op_rebase(Ctx *c, const char *onto)
{
	git_annotated_commit *up = NULL;
	if (annotated(c, onto, &up) != 0)
		return 1;
	/* already contains onto: nothing to do (git: "Current branch is up to date") */
	git_oid head;
	if (git_reference_name_to_id(&head, c->r, "HEAD") == 0) {
		const git_oid *u = git_annotated_commit_id(up);
		if (git_oid_equal(&head, u) || git_graph_descendant_of(c->r, &head, u) == 1) {
			git_annotated_commit_free(up);
			say(c, "Current branch is up to date.");
			return 0;
		}
		if (git_graph_descendant_of(c->r, u, &head) == 1) {
			/* HEAD is behind onto: a fast-forward, like git rebase */
			char what[600];
			snprintf(what, sizeof what, "rebase (finish): %s onto", onto);
			int rc = fast_forward(c, u, what);
			git_annotated_commit_free(up);
			return rc;
		}
	}
	git_rebase_options ro;
	rebase_opts(&ro);
	git_rebase *rb = NULL;
	int rc = git_rebase_init(&rb, c->r, NULL, up, NULL, &ro);
	git_annotated_commit_free(up);
	if (rc != 0)
		return lgfail(c, "cannot start the rebase");
	rc = rebase_run(c, rb, false);
	git_rebase_free(rb);
	return rc;
}

static int op_rebase_resume(Ctx *c, int how) /* 0 continue, 1 abort, 2 skip */
{
	git_rebase_options ro;
	rebase_opts(&ro);
	git_rebase *rb = NULL;
	if (git_rebase_open(&rb, c->r, &ro) != 0)
		return complain(c, "error: no rebase in progress");
	int rc;
	if (how == 1) {
		rc = git_rebase_abort(rb) != 0 ? lgfail(c, "cannot abort the rebase") : 0;
		if (rc == 0)
			say(c, "Rebase aborted.");
	} else if (how == 2) {
		git_commit *h = NULL;
		git_checkout_options co;
		checkout_opts(&co, true);
		if (head_commit(c, &h) == 0)
			git_reset(c->r, (git_object *)h, GIT_RESET_HARD, &co);
		git_commit_free(h);
		rc = rebase_run(c, rb, false);
	} else {
		rc = rebase_run(c, rb, git_rebase_operation_entrycount(rb) > 0 &&
		                           git_rebase_operation_current(rb) != GIT_REBASE_NO_OPERATION);
	}
	git_rebase_free(rb);
	return rc;
}

/* ---- cherry-pick / revert ----------------------------------------------------------- */

static int op_pick(Ctx *c, const char *sha, bool revert)
{
	git_object *obj = NULL;
	if (sha[0] == '-' || git_revparse_single(&obj, c->r, sha) != 0)
		return lgfail(c, sha);
	git_commit *cm = NULL;
	if (git_object_peel((git_object **)&cm, obj, GIT_OBJECT_COMMIT) != 0) {
		git_object_free(obj);
		return lgfail(c, sha);
	}
	git_object_free(obj);
	int rc;
	if (revert) {
		git_revert_options ro;
		git_revert_options_init(&ro, GIT_REVERT_OPTIONS_VERSION);
		checkout_opts(&ro.checkout_opts, false);
		ro.checkout_opts.checkout_strategy |= GIT_CHECKOUT_ALLOW_CONFLICTS;
		rc = git_revert(c->r, cm, &ro);
	} else {
		git_cherrypick_options po;
		git_cherrypick_options_init(&po, GIT_CHERRYPICK_OPTIONS_VERSION);
		checkout_opts(&po.checkout_opts, false);
		po.checkout_opts.checkout_strategy |= GIT_CHECKOUT_ALLOW_CONFLICTS;
		rc = git_cherrypick(c->r, cm, &po);
	}
	if (rc != 0) {
		git_commit_free(cm);
		return lgfail(c, revert ? "revert failed" : "cherry-pick failed");
	}
	char h[12];
	git_oid_tostr(h, sizeof h, git_commit_id(cm));
	if (report_conflicts(c, revert ? "revert" : "cherry-pick")) {
		git_commit_free(cm);
		return complain(c, "error: could not %s %s; resolve the conflicts and commit", revert ? "revert" : "apply", h);
	}
	git_commit *head = NULL;
	rc = head_commit(c, &head) != 0 ? lgfail(c, "HEAD") : 0;
	if (rc == 0) {
		git_oid id;
		if (revert) {
			char msg[1200];
			snprintf(msg, sizeof msg, "Revert \"%s\"\n\nThis reverts commit %s.\n", git_commit_summary(cm),
			         git_oid_tostr_s(git_commit_id(cm)));
			rc = commit_index(c, msg, &head, 1, NULL, &id);
		} else {
			rc = commit_index(c, git_commit_message(cm), &head, 1, git_commit_author(cm), &id);
		}
		if (rc == 0) {
			git_repository_state_cleanup(c->r);
			char nh[12];
			git_oid_tostr(nh, sizeof nh, &id);
			say(c, "[%s] %s", nh, revert ? "Revert" : git_commit_summary(cm));
		}
	}
	git_commit_free(head);
	git_commit_free(cm);
	return rc;
}

/* ---- commit -------------------------------------------------------------------------- */

static int mergehead_cb(const git_oid *oid, void *payload)
{
	git_oid *v = payload;
	for (int i = 0; i < 15; i++)
		if (git_oid_is_zero(&v[i])) {
			v[i] = *oid;
			break;
		}
	return 0;
}

static int op_commit(Ctx *c, const char *message, bool amend, bool all, bool allow_empty)
{
	git_index *idx = NULL;
	if (git_repository_index(&idx, c->r) != 0)
		return lgfail(c, "index");
	if (all) {
		/* git commit -a: stage modified and deleted tracked files */
		if (git_index_update_all(idx, NULL, NULL, NULL) != 0 || git_index_write(idx) != 0) {
			git_index_free(idx);
			return lgfail(c, "cannot stage the changes");
		}
	}
	if (git_index_has_conflicts(idx)) {
		git_index_free(idx);
		report_conflicts(c, "commit");
		return complain(c, "error: Committing is not possible because you have unmerged files.");
	}
	git_oid tree_id;
	if (git_index_write_tree(&tree_id, idx) != 0) {
		git_index_free(idx);
		return lgfail(c, "cannot write the tree");
	}
	git_index_free(idx);
	git_tree *tree = NULL;
	if (git_tree_lookup(&tree, c->r, &tree_id) != 0)
		return lgfail(c, "tree");
	/* message: given, or MERGE_MSG while merging; comments stripped */
	git_buf merge_msg = {0};
	const char *raw = message ? message : "";
	bool merging = git_repository_state(c->r) == GIT_REPOSITORY_STATE_MERGE;
	if (!*raw && merging && git_repository_message(&merge_msg, c->r) == 0)
		raw = merge_msg.ptr;
	git_buf msg = {0};
	if (raw && *raw)
		git_message_prettify(&msg, raw, 1, '#');
	git_buf_dispose(&merge_msg);
	git_commit *head = NULL;
	bool have_head = head_commit(c, &head) == 0;
	int rc = 1;
	git_signature *me = NULL;
	if (sig_default(c, &me) != 0)
		goto done;
	if (amend) {
		if (!have_head) {
			rc = complain(c, "error: nothing to amend");
			goto done;
		}
		git_oid id;
		if (git_commit_amend(&id, head, "HEAD", NULL, me, NULL, msg.ptr && *msg.ptr ? msg.ptr : NULL, tree) != 0) {
			rc = lgfail(c, "amend failed");
			goto done;
		}
		char h[12];
		git_oid_tostr(h, sizeof h, &id);
		say(c, "[%s] %s (amended)", h, msg.ptr && *msg.ptr ? msg.ptr : git_commit_summary(head));
		rc = 0;
		goto done;
	}
	if (!msg.ptr || !*msg.ptr) {
		rc = complain(c, "Aborting commit due to empty commit message.");
		goto done;
	}
	if (!allow_empty && !merging && have_head && git_oid_equal(git_commit_tree_id(head), &tree_id)) {
		rc = complain(c, "nothing to commit, working tree clean (nothing added to commit)");
		goto done;
	}
	git_commit *parents[16] = {0};
	size_t np = 0;
	if (have_head)
		parents[np++] = head;
	if (merging) {
		git_oid mh[16];
		memset(mh, 0, sizeof mh);
		git_repository_mergehead_foreach(c->r, mergehead_cb, mh);
		for (int i = 0; i < 15 && !git_oid_is_zero(&mh[i]) && np < 16; i++)
			if (git_commit_lookup(&parents[np], c->r, &mh[i]) == 0)
				np++;
	}
	git_oid id;
	if (git_commit_create(&id, c->r, "HEAD", me, me, NULL, msg.ptr, tree, np, (const git_commit **)parents) != 0) {
		rc = lgfail(c, "commit failed");
	} else {
		if (merging)
			git_repository_state_cleanup(c->r);
		char h[12], cur[256];
		git_oid_tostr(h, sizeof h, &id);
		say(c, "[%s %s] %.*s", current_branch_name(c->r, cur, sizeof cur), h, (int)strcspn(msg.ptr, "\n"), msg.ptr);
		rc = 0;
	}
	for (size_t i = have_head ? 1 : 0; i < np; i++)
		git_commit_free(parents[i]);
done:
	git_signature_free(me);
	git_commit_free(head);
	git_buf_dispose(&msg);
	git_tree_free(tree);
	return rc;
}

static int op_undo_commit(Ctx *c)
{
	git_object *p = NULL;
	if (git_revparse_single(&p, c->r, "HEAD~1") != 0)
		return complain(c, "error: there is no commit before HEAD to go back to");
	int rc = git_reset(c->r, p, GIT_RESET_SOFT, NULL) != 0 ? lgfail(c, "reset failed") : 0;
	git_object_free(p);
	if (rc == 0)
		say(c, "Undid the last commit (its changes are staged)");
	return rc;
}

static int op_unstage_all(Ctx *c)
{
	git_commit *h = NULL;
	int rc;
	if (head_commit(c, &h) == 0) {
		/* git reset -q: a mixed reset (index = HEAD, work tree kept) */
		rc = git_reset(c->r, (git_object *)h, GIT_RESET_MIXED, NULL) != 0 ? lgfail(c, "unstage failed") : 0;
		git_commit_free(h);
	} else {
		/* unborn HEAD: empty the index */
		git_index *idx = NULL;
		rc = git_repository_index(&idx, c->r) != 0 || git_index_clear(idx) != 0 || git_index_write(idx) != 0
		         ? lgfail(c, "unstage failed")
		         : 0;
		git_index_free(idx);
	}
	if (rc == 0)
		say(c, "Unstaged all changes");
	return rc;
}

static int op_discard_all(Ctx *c)
{
	/* git restore --worktree -- . : tracked files back to the index */
	git_checkout_options co;
	checkout_opts(&co, true);
	if (git_checkout_index(c->r, NULL, &co) != 0)
		return lgfail(c, "discard failed");
	say(c, "Discarded the changes in tracked files");
	return 0;
}

/* ---- tags --------------------------------------------------------------------------- */

static int op_tag_create(Ctx *c, const char *name, const char *msg, const char *sha)
{
	git_object *t = NULL;
	if (git_revparse_single(&t, c->r, sha && *sha ? sha : "HEAD") != 0)
		return lgfail(c, "tag target");
	git_oid id;
	int rc;
	if (msg && *msg) {
		git_signature *me = NULL;
		if (sig_default(c, &me) != 0) {
			git_object_free(t);
			return 1;
		}
		rc = git_tag_create(&id, c->r, name, t, me, msg, 0);
		git_signature_free(me);
	} else {
		rc = git_tag_create_lightweight(&id, c->r, name, t, 0);
	}
	git_object_free(t);
	if (rc != 0)
		return lgfail(c, "cannot create the tag");
	say(c, "Created tag '%s'", name);
	return 0;
}

/* ---- network ------------------------------------------------------------------------- */

static int cred_cb(git_credential **out, const char *url, const char *user_from_url, unsigned int allowed,
                   void *payload)
{
	Ctx *c = payload;
	(void)url;
	if (++c->cred_tries > 6)
		return GIT_EUSER; /* bounded: libgit2 keeps asking on failure */
	const char *user = user_from_url && *user_from_url ? user_from_url : "git";
	if (allowed & GIT_CREDENTIAL_USERNAME)
		return git_credential_username_new(out, user);
	if (allowed & GIT_CREDENTIAL_SSH_KEY) {
		/* the agent first, then the default keys without a passphrase */
		static const char *keys[] = {"id_ed25519", "id_ecdsa", "id_rsa"};
		const char *home = getenv("HOME");
		while (c->cred_step <= (int)VX_COUNT(keys)) {
			int s = c->cred_step++;
			if (s == 0) {
				if (getenv("SSH_AUTH_SOCK"))
					return git_credential_ssh_key_from_agent(out, user);
				continue;
			}
			char priv[1024], pub[1100];
			snprintf(priv, sizeof priv, "%s/.ssh/%s", home ? home : "", keys[s - 1]);
			snprintf(pub, sizeof pub, "%s.pub", priv);
			if (access(priv, R_OK) != 0)
				continue;
			return git_credential_ssh_key_new(out, user, access(pub, R_OK) == 0 ? pub : NULL, priv, NULL);
		}
	}
	return GIT_PASSTHROUGH; /* nothing to offer (e.g. https password): fail, CLI fallback */
}

static int transfer_cb(const git_indexer_progress *p, void *payload)
{
	(void)p;
	Ctx *c = payload;
	return c->o->cancel ? -1 : 0;
}

static int push_ref_cb(const char *refname, const char *status, void *payload)
{
	Ctx *c = payload;
	if (status)
		snprintf(c->push_err, sizeof c->push_err, "! [rejected] %s (%s)", refname, status);
	return 0;
}

static void callbacks(Ctx *c, git_remote_callbacks *cb)
{
	cb->credentials = cred_cb;
	cb->transfer_progress = transfer_cb;
	cb->push_update_reference = push_ref_cb;
	cb->payload = c;
	c->cred_tries = 0;
	c->cred_step = 0;
}

/* Errors where the git CLI may do better: authentication, transport, TLS. */
static bool net_error(int rc)
{
	if (rc == GIT_EAUTH || rc == GIT_ECERTIFICATE || rc == GIT_PASSTHROUGH || rc == GIT_EUSER)
		return true;
	const git_error *e = git_error_last();
	return e && (e->klass == GIT_ERROR_NET || e->klass == GIT_ERROR_SSH || e->klass == GIT_ERROR_HTTP ||
	             e->klass == GIT_ERROR_SSL || e->klass == GIT_ERROR_CALLBACK);
}

/* After a libgit2 network failure: run the same thing with the git CLI,
 * which can use credential helpers and askpass-free https tokens. */
static int cli_fallback(Ctx *c, const char *const *argv)
{
	const git_error *e = git_error_last();
	if (!git_cli_path())
		return complain(c, "error: %s", e && e->message ? e->message : "network operation failed");
	if (e && e->message)
		say(c, "(libgit2: %s; retrying with git)", e->message);
	GitRun r;
	git_cli(c->o->workdir, argv, NULL, &r, &c->o->pid);
	if (r.out && r.nout)
		buf_add(&c->out, &c->nout, &c->cout, r.out, r.nout);
	if (r.err && r.nerr)
		buf_add(&c->err, &c->nerr, &c->cerr, r.err, r.nerr);
	int st = r.status;
	git_run_free(&r);
	return st;
}

/* The upstream of the current branch: remote name and "refs/heads/x". */
static int upstream_of_head(Ctx *c, char *remote, size_t rcap, char *merge, size_t mcap, char *branch, size_t bcap)
{
	git_reference *h = NULL;
	remote[0] = merge[0] = branch[0] = 0;
	if (git_repository_head(&h, c->r) != 0 || !git_reference_is_branch(h)) {
		git_reference_free(h);
		return -1;
	}
	snprintf(branch, bcap, "%s", git_reference_shorthand(h));
	git_buf b = {0};
	int ok = git_branch_upstream_remote(&b, c->r, git_reference_name(h)) == 0;
	if (ok)
		snprintf(remote, rcap, "%s", b.ptr);
	git_buf_dispose(&b);
	if (ok && git_branch_upstream_merge(&b, c->r, git_reference_name(h)) == 0) {
		snprintf(merge, mcap, "%s", b.ptr);
		git_buf_dispose(&b);
	} else {
		ok = 0;
	}
	git_reference_free(h);
	return ok ? 0 : -1;
}

static int fetch_remote(Ctx *c, const char *name, bool prune)
{
	git_remote *rm = NULL;
	if (git_remote_lookup(&rm, c->r, name) != 0)
		return -1;
	git_fetch_options fo;
	git_fetch_options_init(&fo, GIT_FETCH_OPTIONS_VERSION);
	callbacks(c, &fo.callbacks);
	fo.prune = prune ? GIT_FETCH_PRUNE : GIT_FETCH_PRUNE_UNSPECIFIED;
	int rc = git_remote_fetch(rm, NULL, &fo, "fetch");
	if (rc != 0 && !net_error(rc)) {
		lgfail(c, "fetch failed");
		rc = -3; /* reported, no fallback */
	}
	if (rc == 0) {
		const git_indexer_progress *st = git_remote_stats(rm);
		say(c, "Fetched %s (%u objects)", name, st ? st->received_objects : 0u);
	}
	git_remote_free(rm);
	return rc;
}

static int op_fetch(Ctx *c, bool all, bool prune)
{
	char remote[256], merge[512], branch[256];
	git_strarray rl = {0};
	if (git_remote_list(&rl, c->r) != 0 || rl.count == 0) {
		git_strarray_dispose(&rl);
		return complain(c, "error: this repository has no remotes");
	}
	int rc = 0;
	if (all) {
		for (size_t i = 0; i < rl.count && rc == 0; i++)
			rc = fetch_remote(c, rl.strings[i], prune);
	} else {
		const char *name = upstream_of_head(c, remote, sizeof remote, merge, sizeof merge, branch, sizeof branch) == 0
		                       ? remote
		                       : rl.count == 1 ? rl.strings[0] : "origin";
		rc = fetch_remote(c, name, prune);
	}
	git_strarray_dispose(&rl);
	if (rc == -3)
		return 1;
	if (rc != 0) {
		const char *a1[] = {"fetch", all ? "--all" : "--quiet", prune ? "--prune" : "--quiet", NULL};
		return cli_fallback(c, a1);
	}
	return 0;
}

static int op_pull(Ctx *c, bool rebase)
{
	char remote[256], merge[512], branch[256];
	if (upstream_of_head(c, remote, sizeof remote, merge, sizeof merge, branch, sizeof branch) != 0)
		return complain(c, "There is no tracking information for the current branch.");
	int fr = fetch_remote(c, remote, false);
	if (fr == -3)
		return 1;
	if (fr != 0) {
		const char *a1[] = {"pull", rebase ? "--rebase" : "--no-rebase", NULL};
		return cli_fallback(c, a1);
	}
	char up[800];
	snprintf(up, sizeof up, "%s/%s", remote, !strncmp(merge, "refs/heads/", 11) ? merge + 11 : merge);
	return rebase ? op_rebase(c, up) : op_merge(c, up);
}

static int push_refspec(Ctx *c, const char *remote, const char *spec, bool force_lease, const char *dst)
{
	git_remote *rm = NULL;
	if (git_remote_lookup(&rm, c->r, remote) != 0)
		return lgfail(c, remote);
	git_push_options po;
	git_push_options_init(&po, GIT_PUSH_OPTIONS_VERSION);
	callbacks(c, &po.callbacks);
	c->push_err[0] = 0;
	int rc = 0;
	if (force_lease) {
		/* refuse when the remote ref moved since our last fetch */
		git_remote_callbacks cb;
		git_remote_init_callbacks(&cb, GIT_REMOTE_CALLBACKS_VERSION);
		callbacks(c, &cb);
		int k = git_remote_connect(rm, GIT_DIRECTION_PUSH, &cb, NULL, NULL);
		if (k != 0) {
			git_remote_free(rm);
			return net_error(k) ? -2 : lgfail(c, "cannot connect"); /* -2: caller falls back */
		}
		const git_remote_head **heads;
		size_t nh = 0;
		git_oid remote_now;
		bool have = false;
		if (git_remote_ls(&heads, &nh, rm) == 0)
			for (size_t i = 0; i < nh; i++)
				if (!strcmp(heads[i]->name, dst)) {
					remote_now = heads[i]->oid;
					have = true;
				}
		git_remote_disconnect(rm);
		char track[900];
		snprintf(track, sizeof track, "refs/remotes/%s/%s", remote, !strncmp(dst, "refs/heads/", 11) ? dst + 11 : dst);
		git_oid known;
		bool have_known = git_reference_name_to_id(&known, c->r, track) == 0;
		if (have && (!have_known || !git_oid_equal(&known, &remote_now))) {
			git_remote_free(rm);
			return complain(c, "! [rejected] %s (stale info): the remote changed since the last fetch", dst);
		}
	}
	char *s = (char *)spec;
	git_strarray refs = {&s, 1};
	int k = git_remote_push(rm, &refs, &po);
	if (k != 0 && net_error(k) && !c->push_err[0])
		rc = -2;
	else if (k != 0 && !c->push_err[0])
		rc = lgfail(c, "push failed");
	else if (c->push_err[0])
		rc = complain(c, "%s", c->push_err);
	else
		say(c, "To %s\n   %s", git_remote_url(rm), spec);
	git_remote_free(rm);
	return rc;
}

static int op_push(Ctx *c, bool force_lease)
{
	char remote[256], merge[512], branch[256];
	if (upstream_of_head(c, remote, sizeof remote, merge, sizeof merge, branch, sizeof branch) != 0)
		return complain(c, "fatal: The current branch has no upstream branch (use Publish Branch).");
	char spec[1200];
	snprintf(spec, sizeof spec, "%srefs/heads/%s:%s", force_lease ? "+" : "", branch, merge);
	int rc = push_refspec(c, remote, spec, force_lease, merge);
	if (rc == -2) {
		const char *a1[] = {"push", force_lease ? "--force-with-lease" : "--quiet", NULL};
		return cli_fallback(c, a1);
	}
	return rc;
}

static int op_publish(Ctx *c, const char *remote, const char *branch)
{
	const char *rem = remote && *remote ? remote : "origin";
	char spec[1200], dst[600], up[800];
	snprintf(dst, sizeof dst, "refs/heads/%s", branch);
	snprintf(spec, sizeof spec, "refs/heads/%s:%s", branch, dst);
	int rc = push_refspec(c, rem, spec, false, dst);
	if (rc == -2) {
		const char *a1[] = {"push", "-u", rem, branch, NULL};
		return cli_fallback(c, a1);
	}
	if (rc != 0)
		return rc;
	git_reference *b = NULL;
	snprintf(up, sizeof up, "%s/%s", rem, branch);
	if (git_branch_lookup(&b, c->r, branch, GIT_BRANCH_LOCAL) != 0 || git_branch_set_upstream(b, up) != 0)
		rc = lgfail(c, "cannot set the upstream");
	else
		say(c, "branch '%s' set up to track '%s'.", branch, up);
	git_reference_free(b);
	return rc;
}

/* ---- dispatch ------------------------------------------------------------------------ */

/* nth packed string */
static const char *arg(const char *args, u32 n)
{
	const char *p = args;
	for (u32 i = 0; i < n && *p; i++)
		p += strlen(p) + 1;
	return p;
}

static int run_cli(Ctx *c, char *args, const char *in)
{
	if (!git_cli_path())
		return complain(c, "error: this action needs the git command, which was not found");
	u32 na = 0;
	for (char *p = args; *p; p += strlen(p) + 1)
		na++;
	const char **av = vx_alloc((na + 1) * sizeof(char *));
	na = 0;
	for (char *p = args; *p; p += strlen(p) + 1)
		av[na++] = p;
	av[na] = NULL;
	GitRun r;
	git_cli(c->o->workdir, av, in, &r, &c->o->pid);
	free(av);
	if (r.out && r.nout)
		buf_add(&c->out, &c->nout, &c->cout, r.out, r.nout);
	if (r.err && r.nerr)
		buf_add(&c->err, &c->nerr, &c->cerr, r.err, r.nerr);
	int st = r.status;
	git_run_free(&r);
	return st;
}

static int dispatch(Ctx *c, u8 kind, u32 fl, u32 index, char *args, const char *in)
{
	const char *a0 = arg(args, 0), *a1 = arg(args, 1);
	switch (kind) {
	case OP_CLI:
		return run_cli(c, args, in);
	case OP_STASH_PUSH:
		return op_stash_push(c, fl & F_A, in);
	case OP_STASH_POP:
		return op_stash_apply(c, index, true);
	case OP_STASH_APPLY:
		return op_stash_apply(c, index, false);
	case OP_STASH_DROP:
		if (git_stash_drop(c->r, index) != 0)
			return lgfail(c, "stash drop failed");
		say(c, "Dropped stash@{%u}", index);
		return 0;
	case OP_CHECKOUT:
		return op_checkout(c, a0, fl & F_A);
	case OP_DETACH:
		return op_detach(c, a0);
	case OP_BRANCH_CREATE:
		return op_branch_create(c, a0, *a1 ? a1 : NULL, fl & F_A);
	case OP_BRANCH_DELETE:
		return op_branch_delete(c, a0, fl & F_A);
	case OP_BRANCH_RENAME:
		return op_branch_rename(c, a0);
	case OP_MERGE:
		return op_merge(c, a0);
	case OP_MERGE_ABORT:
		if (git_repository_state(c->r) != GIT_REPOSITORY_STATE_MERGE)
			return complain(c, "fatal: There is no merge to abort (MERGE_HEAD missing).");
		return op_reset_state(c, "Merge aborted");
	case OP_REBASE:
		return op_rebase(c, a0);
	case OP_REBASE_CONTINUE:
		return op_rebase_resume(c, 0);
	case OP_REBASE_ABORT:
		return op_rebase_resume(c, 1);
	case OP_REBASE_SKIP:
		return op_rebase_resume(c, 2);
	case OP_CHERRY_PICK:
		return op_pick(c, a0, false);
	case OP_CHERRY_PICK_ABORT:
		return op_reset_state(c, "Cherry-pick aborted");
	case OP_REVERT:
		return op_pick(c, a0, true);
	case OP_COMMIT:
		return op_commit(c, in, fl & F_A, fl & F_B, fl & F_C);
	case OP_AMEND_NO_EDIT:
		return op_commit(c, NULL, true, false, true);
	case OP_UNDO_COMMIT:
		return op_undo_commit(c);
	case OP_UNSTAGE_ALL:
		return op_unstage_all(c);
	case OP_DISCARD_ALL:
		return op_discard_all(c);
	case OP_FETCH:
		return op_fetch(c, fl & F_A, fl & F_B);
	case OP_PULL:
		return op_pull(c, fl & F_A);
	case OP_PUSH:
		return op_push(c, fl & F_A);
	case OP_PUBLISH:
		return op_publish(c, a0, a1);
	case OP_TAG_CREATE:
		return op_tag_create(c, a0, in, *a1 ? a1 : NULL);
	case OP_TAG_DELETE:
		if (git_tag_delete(c->r, a0) != 0)
			return lgfail(c, "cannot delete the tag");
		say(c, "Deleted tag '%s'", a0);
		return 0;
	}
	return complain(c, "error: unknown operation");
}

static void *worker(void *arg)
{
	GitOps *o = arg;
	pthread_mutex_lock(&o->mu);
	for (;;) {
		while (!o->q.n && !o->quit)
			pthread_cond_wait(&o->cv, &o->mu);
		if (o->quit)
			break;
		u32 id = o->q.id[0], fl = o->q.flags[0], index = o->q.index[0];
		u8 kind = o->q.kind[0];
		char *label = o->q.label[0], *args = o->q.args[0], *in = o->q.in[0];
		q_pop_front(&o->q);
		o->running = true;
		o->running_label = label;
		pthread_mutex_unlock(&o->mu);

		u64 t0 = vx_now_ns();
		/* pick up index changes made outside this handle (cheap stat check) */
		if (o->wrepo) {
			git_index *ix = NULL;
			if (git_repository_index(&ix, o->wrepo) == 0)
				git_index_read(ix, 0);
			git_index_free(ix);
		}
		Ctx c = {0};
		c.o = o;
		c.r = o->wrepo;
		buf_add(&c.out, &c.nout, &c.cout, "", 0);
		buf_add(&c.err, &c.nerr, &c.cerr, "", 0);
		int st = o->wrepo || kind == OP_CLI ? dispatch(&c, kind, fl, index, args, in)
		                                    : complain(&c, "error: the repository could not be opened");
		u64 dt = vx_now_ns() - t0;
		free(args);
		free(in);

		pthread_mutex_lock(&o->mu);
		o->running = false;
		o->running_label = NULL;
		d_push(&o->d, id, label, st, c.out, c.err, dt);
		pthread_cond_broadcast(&o->cv);
	}
	pthread_mutex_unlock(&o->mu);
	return NULL;
}

GitOps *gitops_start(const char *workdir)
{
	gitops_libgit2_init();
	GitOps *o = vx_alloc(sizeof *o);
	o->workdir = vx_strdup(workdir);
	o->next_id = 1;
	if (git_repository_open_ext(&o->wrepo, workdir, 0, NULL) != 0 ||
	    git_repository_open_ext(&o->qrepo, workdir, 0, NULL) != 0) {
		git_repository_free(o->wrepo);
		free(o->workdir);
		free(o);
		return NULL;
	}
	pthread_mutex_init(&o->mu, NULL);
	pthread_cond_init(&o->cv, NULL);
	if (pthread_create(&o->th, NULL, worker, o) != 0) {
		git_repository_free(o->wrepo);
		git_repository_free(o->qrepo);
		free(o->workdir);
		free(o);
		return NULL;
	}
	return o;
}

void gitops_stop(GitOps *o)
{
	if (!o)
		return;
	pthread_mutex_lock(&o->mu);
	o->quit = true;
	o->cancel = 1;
	for (u32 i = 0; i < o->q.n; i++) {
		free(o->q.label[i]);
		free(o->q.args[i]);
		free(o->q.in[i]);
	}
	o->q.n = 0;
	int pid = o->pid;
	if (o->running && pid > 0)
		kill(-pid, SIGTERM); /* a CLI fallback child's process group */
	pthread_cond_broadcast(&o->cv);
	if (o->running) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 500000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		while (o->running) {
			if (pthread_cond_timedwait(&o->cv, &o->mu, &ts) == ETIMEDOUT)
				break;
		}
		pid = o->pid;
		if (o->running && pid > 0) {
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
		}
	}
	pthread_mutex_unlock(&o->mu);
	pthread_join(o->th, NULL);
	for (u32 i = 0; i < o->d.n; i++) {
		free(o->d.label[i]);
		free(o->d.out[i]);
		free(o->d.err[i]);
	}
	SoaCol qc[] = {OPQ_COLS(&o->q)}, dc[] = {DONEQ_COLS(&o->d)};
	soa_free(qc, 7);
	soa_free(dc, 6);
	pthread_mutex_destroy(&o->mu);
	pthread_cond_destroy(&o->cv);
	git_repository_free(o->wrepo);
	git_repository_free(o->qrepo);
	free(o->workdir);
	free(o);
}

const char *gitops_workdir(const GitOps *o) { return o->workdir; }

static char *pack(const char *a, const char *b)
{
	size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
	char *p = vx_alloc(la + lb + 3);
	if (la)
		memcpy(p, a, la);
	p[la] = 0;
	if (lb)
		memcpy(p + la + 1, b, lb);
	p[la + 1 + lb] = 0;
	p[la + 2 + lb] = 0;
	return p; /* "a\0b\0\0"; the first argument is never empty */
}

static u32 enqueue(GitOps *o, const char *label, u8 kind, u32 flags, u32 index, char *args, const char *in)
{
	pthread_mutex_lock(&o->mu);
	u32 id = o->next_id++;
	q_push(&o->q, id, kind, flags, index, vx_strdup(label), args ? args : pack(NULL, NULL),
	       in ? vx_strdup(in) : NULL);
	pthread_cond_broadcast(&o->cv);
	pthread_mutex_unlock(&o->mu);
	return id;
}

/* Raw argv: the actions the UI sends this way run on libgit2 too; anything
 * else goes to the git CLI. */
u32 gitops_run(GitOps *o, const char *label, const char *const *argv, const char *stdin_text)
{
	u32 na = 0;
	while (argv[na])
		na++;
	if (na == 2 && !strcmp(argv[0], "reset") && !strcmp(argv[1], "-q"))
		return enqueue(o, label ? label : "Unstaging...", OP_UNSTAGE_ALL, 0, 0, NULL, NULL);
	if (na == 4 && !strcmp(argv[0], "restore") && !strcmp(argv[1], "--worktree") && !strcmp(argv[2], "--") &&
	    !strcmp(argv[3], "."))
		return enqueue(o, label ? label : "Discarding changes...", OP_DISCARD_ALL, 0, 0, NULL, NULL);
	if (na == 3 && !strcmp(argv[0], "checkout") && !strcmp(argv[1], "--detach"))
		return enqueue(o, label ? label : "Checking out...", OP_DETACH, 0, 0, pack(argv[2], NULL), NULL);
	size_t len = 1;
	for (u32 i = 0; i < na; i++)
		len += strlen(argv[i]) + 1;
	char *args = vx_alloc(len + 1);
	size_t k = 0;
	for (u32 i = 0; i < na; i++) {
		size_t l = strlen(argv[i]);
		memcpy(args + k, argv[i], l + 1);
		k += l + 1;
	}
	args[k] = 0;
	return enqueue(o, label ? label : "git", OP_CLI, 0, 0, args, stdin_text);
}

bool gitops_busy(GitOps *o, char *label, size_t cap)
{
	pthread_mutex_lock(&o->mu);
	bool busy = o->running || o->q.n;
	if (label && cap) {
		const char *l = o->running ? o->running_label : o->q.n ? o->q.label[0] : "";
		snprintf(label, cap, "%s", l ? l : "");
	}
	pthread_mutex_unlock(&o->mu);
	return busy;
}

bool gitops_poll(GitOps *o, GitOpResult *out)
{
	pthread_mutex_lock(&o->mu);
	bool ok = o->d.n > 0;
	if (ok) {
		out->id = o->d.id[0];
		out->label = o->d.label[0];
		out->status = o->d.status[0];
		out->out = o->d.out[0];
		out->err = o->d.err[0];
		out->ns = o->d.ns[0];
		DoneQueue *d = &o->d;
		d->n--;
		memmove(d->id, d->id + 1, d->n * sizeof(u32));
		memmove(d->label, d->label + 1, d->n * sizeof(char *));
		memmove(d->status, d->status + 1, d->n * sizeof(int));
		memmove(d->out, d->out + 1, d->n * sizeof(char *));
		memmove(d->err, d->err + 1, d->n * sizeof(char *));
		memmove(d->ns, d->ns + 1, d->n * sizeof(u64));
	}
	pthread_mutex_unlock(&o->mu);
	return ok;
}

void gitops_result_free(GitOpResult *r)
{
	free(r->label);
	free(r->out);
	free(r->err);
	memset(r, 0, sizeof *r);
}

/* ---- action helpers --------------------------------------------------------------- */

u32 gitops_stash_push(GitOps *o, bool include_untracked, const char *message)
{
	return enqueue(o, "Stashing...", OP_STASH_PUSH, include_untracked ? F_A : 0, 0, NULL,
	               message && *message ? message : NULL);
}
u32 gitops_stash_pop(GitOps *o, u32 i) { return enqueue(o, "Popping stash...", OP_STASH_POP, 0, i, NULL, NULL); }
u32 gitops_stash_apply(GitOps *o, u32 i) { return enqueue(o, "Applying stash...", OP_STASH_APPLY, 0, i, NULL, NULL); }
u32 gitops_stash_drop(GitOps *o, u32 i) { return enqueue(o, "Dropping stash...", OP_STASH_DROP, 0, i, NULL, NULL); }

u32 gitops_checkout(GitOps *o, const char *branch, bool remote)
{
	return enqueue(o, "Checking out...", OP_CHECKOUT, remote ? F_A : 0, 0, pack(branch, NULL), NULL);
}

u32 gitops_branch_create(GitOps *o, const char *name, const char *from, bool checkout)
{
	return enqueue(o, "Creating branch...", OP_BRANCH_CREATE, checkout ? F_A : 0, 0, pack(name, from), NULL);
}

u32 gitops_branch_delete(GitOps *o, const char *name, bool force)
{
	return enqueue(o, "Deleting branch...", OP_BRANCH_DELETE, force ? F_A : 0, 0, pack(name, NULL), NULL);
}

u32 gitops_branch_rename(GitOps *o, const char *new_name)
{
	return enqueue(o, "Renaming branch...", OP_BRANCH_RENAME, 0, 0, pack(new_name, NULL), NULL);
}

u32 gitops_merge(GitOps *o, const char *branch) { return enqueue(o, "Merging...", OP_MERGE, 0, 0, pack(branch, NULL), NULL); }
u32 gitops_merge_abort(GitOps *o) { return enqueue(o, "Aborting merge...", OP_MERGE_ABORT, 0, 0, NULL, NULL); }
u32 gitops_rebase(GitOps *o, const char *onto) { return enqueue(o, "Rebasing...", OP_REBASE, 0, 0, pack(onto, NULL), NULL); }
u32 gitops_rebase_continue(GitOps *o) { return enqueue(o, "Continuing rebase...", OP_REBASE_CONTINUE, 0, 0, NULL, NULL); }
u32 gitops_rebase_abort(GitOps *o) { return enqueue(o, "Aborting rebase...", OP_REBASE_ABORT, 0, 0, NULL, NULL); }
u32 gitops_rebase_skip(GitOps *o) { return enqueue(o, "Skipping commit...", OP_REBASE_SKIP, 0, 0, NULL, NULL); }
u32 gitops_cherry_pick(GitOps *o, const char *sha)
{
	return enqueue(o, "Cherry picking...", OP_CHERRY_PICK, 0, 0, pack(sha, NULL), NULL);
}
u32 gitops_cherry_pick_abort(GitOps *o)
{
	return enqueue(o, "Aborting cherry-pick...", OP_CHERRY_PICK_ABORT, 0, 0, NULL, NULL);
}
u32 gitops_revert(GitOps *o, const char *sha) { return enqueue(o, "Reverting...", OP_REVERT, 0, 0, pack(sha, NULL), NULL); }

u32 gitops_commit(GitOps *o, const char *message, bool amend, bool all, bool allow_empty)
{
	return enqueue(o, amend ? "Amending commit..." : "Committing...", OP_COMMIT,
	               (amend ? F_A : 0) | (all ? F_B : 0) | (allow_empty ? F_C : 0), 0, NULL, message ? message : "");
}

u32 gitops_commit_amend_no_edit(GitOps *o) { return enqueue(o, "Amending commit...", OP_AMEND_NO_EDIT, 0, 0, NULL, NULL); }
u32 gitops_undo_last_commit(GitOps *o) { return enqueue(o, "Undoing last commit...", OP_UNDO_COMMIT, 0, 0, NULL, NULL); }

u32 gitops_fetch(GitOps *o, bool all, bool prune)
{
	return enqueue(o, "Fetching...", OP_FETCH, (all ? F_A : 0) | (prune ? F_B : 0), 0, NULL, NULL);
}

u32 gitops_pull(GitOps *o, bool rebase) { return enqueue(o, "Pulling...", OP_PULL, rebase ? F_A : 0, 0, NULL, NULL); }
u32 gitops_push(GitOps *o, bool force_with_lease)
{
	return enqueue(o, "Pushing...", OP_PUSH, force_with_lease ? F_A : 0, 0, NULL, NULL);
}

u32 gitops_publish(GitOps *o, const char *remote, const char *branch)
{
	return enqueue(o, "Publishing branch...", OP_PUBLISH, 0, 0, pack(remote && *remote ? remote : "origin", branch), NULL);
}

u32 gitops_tag_create(GitOps *o, const char *name, const char *message, const char *sha)
{
	return enqueue(o, "Creating tag...", OP_TAG_CREATE, 0, 0, pack(name, sha), message && *message ? message : NULL);
}

u32 gitops_tag_delete(GitOps *o, const char *name) { return enqueue(o, "Deleting tag...", OP_TAG_DELETE, 0, 0, pack(name, NULL), NULL); }

/* ---- synchronous queries (caller thread, its own repository handle) -------------------- */

#define BRANCH_COLS(l)                                                                                       \
	SOA((l)->time), SOA((l)->name), SOA((l)->upstream), SOA((l)->hash), SOA((l)->ahead), SOA((l)->behind), \
	    SOA((l)->flags)
#define STASH_COLS(l) SOA((l)->ref), SOA((l)->message)
#define NAME_COLS(l) SOA((l)->name), SOA((l)->hash)

static void branch_reserve(GitBranchList *l)
{
	SoaCol c[] = {BRANCH_COLS(l)};
	soa_grow(c, 7, &l->cap, VX_MAX(l->n + 1, 32u), l->n);
}

int gitops_branches(GitOps *o, GitBranchList *l)
{
	memset(l, 0, sizeof *l);
	git_branch_iterator *it = NULL;
	if (git_branch_iterator_new(&it, o->qrepo, GIT_BRANCH_ALL) != 0)
		return -1;
	git_reference *ref;
	git_branch_t type;
	while (git_branch_next(&ref, &type, it) == 0) {
		if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) { /* origin/HEAD */
			git_reference_free(ref);
			continue;
		}
		const char *name = NULL;
		git_branch_name(&name, ref);
		const git_oid *tip = git_reference_target(ref);
		branch_reserve(l);
		u32 i = l->n++;
		l->name[i] = pool_add(&l->pool, name ? name : "", name ? (u32)strlen(name) : 0);
		char hex[GIT_OID_SHA1_HEXSIZE + 1] = "";
		if (tip)
			git_oid_tostr(hex, sizeof hex, tip);
		l->hash[i] = pool_add(&l->pool, hex, (u32)strlen(hex));
		l->flags[i] = (u8)((type == GIT_BRANCH_LOCAL ? GB_LOCAL : GB_REMOTE) | (git_branch_is_head(ref) == 1 ? GB_CURRENT : 0));
		l->ahead[i] = l->behind[i] = 0;
		const char *upn = "";
		git_reference *up = NULL;
		if (type == GIT_BRANCH_LOCAL && git_branch_upstream(&up, ref) == 0) {
			git_branch_name(&upn, up);
			size_t a = 0, b = 0;
			if (tip && git_reference_target(up) &&
			    git_graph_ahead_behind(&a, &b, o->qrepo, tip, git_reference_target(up)) == 0) {
				l->ahead[i] = (u32)a;
				l->behind[i] = (u32)b;
			}
		}
		l->upstream[i] = pool_add(&l->pool, upn ? upn : "", upn ? (u32)strlen(upn) : 0);
		git_reference_free(up);
		l->time[i] = 0;
		git_commit *cm = NULL;
		if (tip && git_commit_lookup(&cm, o->qrepo, tip) == 0)
			l->time[i] = (i64)git_commit_time(cm);
		git_commit_free(cm);
		git_reference_free(ref);
	}
	git_branch_iterator_free(it);
	return 0;
}

static int stash_cb(size_t index, const char *message, const git_oid *id, void *payload)
{
	(void)id;
	GitStashList *l = payload;
	if (l->n == l->cap) {
		SoaCol c[] = {STASH_COLS(l)};
		soa_grow(c, 2, &l->cap, l->n + 1, l->n);
	}
	char r[32];
	snprintf(r, sizeof r, "stash@{%zu}", index);
	u32 i = l->n++;
	l->ref[i] = pool_add(&l->pool, r, (u32)strlen(r));
	l->message[i] = pool_add(&l->pool, message ? message : "", message ? (u32)strlen(message) : 0);
	return 0;
}

int gitops_stashes(GitOps *o, GitStashList *l)
{
	memset(l, 0, sizeof *l);
	int rc = git_stash_foreach(o->qrepo, stash_cb, l);
	return rc == 0 || rc == GIT_ENOTFOUND ? 0 : -1;
}

static void names_add(GitNameList *l, const char *a, size_t na, const char *b, size_t nb)
{
	if (l->n == l->cap) {
		SoaCol c[] = {NAME_COLS(l)};
		soa_grow(c, 2, &l->cap, l->n + 1, l->n);
	}
	u32 i = l->n++;
	l->name[i] = pool_add(&l->pool, a, (u32)na);
	l->hash[i] = pool_add(&l->pool, b, (u32)nb);
}

typedef struct {
	GitOps *o;
	GitNameList *l;
	i64 *time;
	u32 cap;
} TagAcc;

static int tag_cb(const char *name, git_oid *oid, void *payload)
{
	TagAcc *t = payload;
	const char *s = !strncmp(name, "refs/tags/", 10) ? name + 10 : name;
	char hex[GIT_OID_SHA1_HEXSIZE + 1];
	git_oid_tostr(hex, sizeof hex, oid);
	names_add(t->l, s, strlen(s), hex, strlen(hex));
	/* creator date: tagger time for annotated tags, else the commit's */
	i64 when = 0;
	git_object *obj = NULL;
	if (git_object_lookup(&obj, t->o->qrepo, oid, GIT_OBJECT_ANY) == 0) {
		if (git_object_type(obj) == GIT_OBJECT_TAG) {
			const git_signature *sg = git_tag_tagger((git_tag *)obj);
			when = sg ? (i64)sg->when.time : 0;
		} else if (git_object_type(obj) == GIT_OBJECT_COMMIT) {
			when = (i64)git_commit_time((git_commit *)obj);
		}
		git_object_free(obj);
	}
	VX_GROW(t->time, t->cap, t->l->n);
	t->time[t->l->n - 1] = when;
	return 0;
}

int gitops_tags(GitOps *o, GitNameList *l)
{
	memset(l, 0, sizeof *l);
	TagAcc t = {o, l, NULL, 0};
	int rc = git_tag_foreach(o->qrepo, tag_cb, &t);
	if (rc != 0 && rc != GIT_ENOTFOUND) {
		free(t.time);
		return -1;
	}
	/* newest first (git for-each-ref --sort=-creatordate) */
	for (u32 i = 1; i < l->n; i++) {
		u32 a = l->name[i], b = l->hash[i];
		i64 w = t.time[i];
		u32 j = i;
		while (j > 0 && t.time[j - 1] < w) {
			l->name[j] = l->name[j - 1];
			l->hash[j] = l->hash[j - 1];
			t.time[j] = t.time[j - 1];
			j--;
		}
		l->name[j] = a;
		l->hash[j] = b;
		t.time[j] = w;
	}
	free(t.time);
	return 0;
}

int gitops_remotes(GitOps *o, GitNameList *l)
{
	memset(l, 0, sizeof *l);
	git_strarray rl = {0};
	if (git_remote_list(&rl, o->qrepo) != 0)
		return -1;
	for (size_t i = 0; i < rl.count; i++) {
		git_remote *r = NULL;
		if (git_remote_lookup(&r, o->qrepo, rl.strings[i]) != 0)
			continue;
		const char *u = git_remote_url(r);
		names_add(l, rl.strings[i], strlen(rl.strings[i]), u ? u : "", u ? strlen(u) : 0);
		git_remote_free(r);
	}
	git_strarray_dispose(&rl);
	return 0;
}

u32 gitops_state(GitOps *o)
{
	switch (git_repository_state(o->qrepo)) {
	case GIT_REPOSITORY_STATE_REBASE:
	case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
	case GIT_REPOSITORY_STATE_REBASE_MERGE:
	case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE:
		return GST_REBASE;
	case GIT_REPOSITORY_STATE_MERGE:
		return GST_MERGE;
	case GIT_REPOSITORY_STATE_CHERRYPICK:
	case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
		return GST_CHERRY_PICK;
	case GIT_REPOSITORY_STATE_REVERT:
	case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
		return GST_REVERT;
	case GIT_REPOSITORY_STATE_BISECT:
		return GST_BISECT;
	default:
		return 0;
	}
}

void gitops_branches_free(GitBranchList *l)
{
	SoaCol c[] = {BRANCH_COLS(l)};
	soa_free(c, 7);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}

void gitops_stashes_free(GitStashList *l)
{
	SoaCol c[] = {STASH_COLS(l)};
	soa_free(c, 2);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}

void gitops_names_free(GitNameList *l)
{
	SoaCol c[] = {NAME_COLS(l)};
	soa_free(c, 2);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}
