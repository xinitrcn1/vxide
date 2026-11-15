/* Git CLI runner: async op queue + synchronous queries. See gitops.h. */
#define _POSIX_C_SOURCE 200809L
#include "gitops.h"

#include <errno.h>
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

/* ---- async runner ---------------------------------------------------------- */

/* Queue entries (SoA). argv strings are packed NUL-separated in a pool. */
typedef struct {
	u32 n, cap;
	u32 *id;
	char **label;
	char **args; /* packed: "a\0b\0c\0\0" */
	char **in;   /* stdin text or NULL */
} OpQueue;

typedef struct {
	u32 n, cap;
	u32 *id;
	char **label;
	int *status;
	char **out, **err;
	u64 *ns;
} DoneQueue;

struct GitOps {
	char *workdir;
	char *gitdir; /* absolute, for gitops_state */
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool quit;
	bool running;
	char *running_label;
	volatile int pid; /* running child */
	u32 next_id;
	OpQueue q;
	DoneQueue d;
};

static void q_push(OpQueue *q, u32 id, char *label, char *args, char *in)
{
	if (q->n == q->cap) {
		u32 c = q->cap ? q->cap * 2 : 8;
		q->id = vx_realloc(q->id, c * sizeof(u32));
		q->label = vx_realloc(q->label, c * sizeof(char *));
		q->args = vx_realloc(q->args, c * sizeof(char *));
		q->in = vx_realloc(q->in, c * sizeof(char *));
		q->cap = c;
	}
	q->id[q->n] = id;
	q->label[q->n] = label;
	q->args[q->n] = args;
	q->in[q->n] = in;
	q->n++;
}

static void q_pop_front(OpQueue *q)
{
	q->n--;
	memmove(q->id, q->id + 1, q->n * sizeof(u32));
	memmove(q->label, q->label + 1, q->n * sizeof(char *));
	memmove(q->args, q->args + 1, q->n * sizeof(char *));
	memmove(q->in, q->in + 1, q->n * sizeof(char *));
}

static void d_push(DoneQueue *d, u32 id, char *label, int status, char *out, char *err, u64 ns)
{
	if (d->n == d->cap) {
		u32 c = d->cap ? d->cap * 2 : 8;
		d->id = vx_realloc(d->id, c * sizeof(u32));
		d->label = vx_realloc(d->label, c * sizeof(char *));
		d->status = vx_realloc(d->status, c * sizeof(int));
		d->out = vx_realloc(d->out, c * sizeof(char *));
		d->err = vx_realloc(d->err, c * sizeof(char *));
		d->ns = vx_realloc(d->ns, c * sizeof(u64));
		d->cap = c;
	}
	u32 i = d->n++;
	d->id[i] = id;
	d->label[i] = label;
	d->status[i] = status;
	d->out[i] = out;
	d->err[i] = err;
	d->ns[i] = ns;
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
		u32 id = o->q.id[0];
		char *label = o->q.label[0], *args = o->q.args[0], *in = o->q.in[0];
		q_pop_front(&o->q);
		o->running = true;
		o->running_label = label;
		pthread_mutex_unlock(&o->mu);

		/* unpack argv */
		u32 na = 0;
		for (char *p = args; *p; p += strlen(p) + 1)
			na++;
		const char **av = vx_alloc((na + 1) * sizeof(char *));
		na = 0;
		for (char *p = args; *p; p += strlen(p) + 1)
			av[na++] = p;
		av[na] = NULL;
		u64 t0 = vx_now_ns();
		GitRun r;
		git_cli(o->workdir, av, in, &r, &o->pid);
		u64 dt = vx_now_ns() - t0;
		free(av);
		free(args);
		free(in);

		pthread_mutex_lock(&o->mu);
		o->running = false;
		o->running_label = NULL;
		if (!r.out)
			r.out = vx_strdup("");
		if (!r.err)
			r.err = vx_strdup(r.status < 0 ? "git could not be run" : "");
		d_push(&o->d, id, label, r.status, r.out, r.err, dt);
		pthread_cond_broadcast(&o->cv);
	}
	pthread_mutex_unlock(&o->mu);
	return NULL;
}

GitOps *gitops_start(const char *workdir)
{
	if (!git_cli_path())
		return NULL;
	GitOps *o = vx_alloc(sizeof *o);
	o->workdir = vx_strdup(workdir);
	o->next_id = 1;
	GitRun r;
	const char *a[] = {"rev-parse", "--absolute-git-dir", NULL};
	if (git_cli(workdir, a, NULL, &r, NULL) == 0) {
		size_t n = r.nout;
		while (n && (r.out[n - 1] == '\n' || r.out[n - 1] == '\r'))
			n--;
		o->gitdir = vx_strndup(r.out, n);
	}
	git_run_free(&r);
	pthread_mutex_init(&o->mu, NULL);
	pthread_cond_init(&o->cv, NULL);
	if (pthread_create(&o->th, NULL, worker, o) != 0) {
		free(o->workdir);
		free(o->gitdir);
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
	for (u32 i = 0; i < o->q.n; i++) {
		free(o->q.label[i]);
		free(o->q.args[i]);
		free(o->q.in[i]);
	}
	o->q.n = 0;
	int pid = o->pid;
	if (o->running && pid > 0)
		kill(-pid, SIGTERM); /* the child's process group */
	pthread_cond_broadcast(&o->cv);
	/* give it 500 ms, then SIGKILL */
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
	free(o->q.id);
	free(o->q.label);
	free(o->q.args);
	free(o->q.in);
	free(o->d.id);
	free(o->d.label);
	free(o->d.status);
	free(o->d.out);
	free(o->d.err);
	free(o->d.ns);
	pthread_mutex_destroy(&o->mu);
	pthread_cond_destroy(&o->cv);
	free(o->workdir);
	free(o->gitdir);
	free(o);
}

const char *gitops_workdir(const GitOps *o) { return o->workdir; }

u32 gitops_run(GitOps *o, const char *label, const char *const *argv, const char *stdin_text)
{
	size_t len = 1;
	for (u32 i = 0; argv[i]; i++)
		len += strlen(argv[i]) + 1;
	char *args = vx_alloc(len + 1);
	size_t k = 0;
	for (u32 i = 0; argv[i]; i++) {
		/* an empty argument would end the packed list: keep it as " " never;
		 * git has no meaningful empty args in our helpers */
		size_t l = strlen(argv[i]);
		memcpy(args + k, argv[i], l + 1);
		k += l + 1;
	}
	args[k] = 0;
	pthread_mutex_lock(&o->mu);
	u32 id = o->next_id++;
	q_push(&o->q, id, vx_strdup(label ? label : "git"), args, stdin_text ? vx_strdup(stdin_text) : NULL);
	pthread_cond_broadcast(&o->cv);
	pthread_mutex_unlock(&o->mu);
	return id;
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

/* ---- action helpers --------------------------------------------------------- */

#define RUN(label, ...)                                         \
	do {                                                        \
		const char *argv_[] = {__VA_ARGS__, NULL};              \
		return gitops_run(o, label, argv_, NULL);               \
	} while (0)

u32 gitops_stash_push(GitOps *o, bool include_untracked, const char *msg)
{
	if (msg && *msg) {
		if (include_untracked)
			RUN("Stashing...", "stash", "push", "--include-untracked", "-m", msg);
		RUN("Stashing...", "stash", "push", "-m", msg);
	}
	if (include_untracked)
		RUN("Stashing...", "stash", "push", "--include-untracked");
	RUN("Stashing...", "stash", "push");
}

static u32 stash_cmd(GitOps *o, const char *label, const char *verb, u32 index)
{
	char ref[32];
	snprintf(ref, sizeof ref, "stash@{%u}", index);
	RUN(label, "stash", verb, ref);
}

u32 gitops_stash_pop(GitOps *o, u32 i) { return stash_cmd(o, "Popping stash...", "pop", i); }
u32 gitops_stash_apply(GitOps *o, u32 i) { return stash_cmd(o, "Applying stash...", "apply", i); }
u32 gitops_stash_drop(GitOps *o, u32 i) { return stash_cmd(o, "Dropping stash...", "drop", i); }

u32 gitops_checkout(GitOps *o, const char *branch, bool remote)
{
	/* a remote branch becomes a local tracking branch, like VSCodium */
	if (remote)
		RUN("Checking out...", "switch", "--track", branch);
	RUN("Checking out...", "switch", branch);
}

u32 gitops_branch_create(GitOps *o, const char *name, const char *from, bool checkout)
{
	if (checkout) {
		if (from && *from)
			RUN("Creating branch...", "switch", "-c", name, from);
		RUN("Creating branch...", "switch", "-c", name);
	}
	if (from && *from)
		RUN("Creating branch...", "branch", name, from);
	RUN("Creating branch...", "branch", name);
}

u32 gitops_branch_delete(GitOps *o, const char *name, bool force)
{
	RUN("Deleting branch...", "branch", force ? "-D" : "-d", name);
}

u32 gitops_branch_rename(GitOps *o, const char *new_name) { RUN("Renaming branch...", "branch", "-m", new_name); }
u32 gitops_merge(GitOps *o, const char *branch) { RUN("Merging...", "merge", "--no-edit", branch); }
u32 gitops_merge_abort(GitOps *o) { RUN("Aborting merge...", "merge", "--abort"); }
u32 gitops_rebase(GitOps *o, const char *onto) { RUN("Rebasing...", "rebase", onto); }
u32 gitops_rebase_continue(GitOps *o) { RUN("Continuing rebase...", "rebase", "--continue"); }
u32 gitops_rebase_abort(GitOps *o) { RUN("Aborting rebase...", "rebase", "--abort"); }
u32 gitops_rebase_skip(GitOps *o) { RUN("Skipping commit...", "rebase", "--skip"); }
u32 gitops_cherry_pick(GitOps *o, const char *sha) { RUN("Cherry picking...", "cherry-pick", sha); }
u32 gitops_cherry_pick_abort(GitOps *o) { RUN("Aborting cherry-pick...", "cherry-pick", "--abort"); }
u32 gitops_revert(GitOps *o, const char *sha) { RUN("Reverting...", "revert", "--no-edit", sha); }

u32 gitops_commit(GitOps *o, const char *message, bool amend, bool all, bool allow_empty)
{
	/* message through stdin (-F -): no quoting or length limits */
	const char *argv[8];
	int k = 0;
	argv[k++] = "commit";
	if (amend)
		argv[k++] = "--amend";
	if (all)
		argv[k++] = "--all";
	if (allow_empty)
		argv[k++] = "--allow-empty";
	argv[k++] = "-F";
	argv[k++] = "-";
	argv[k] = NULL;
	return gitops_run(o, amend ? "Amending commit..." : "Committing...", argv, message ? message : "");
}

u32 gitops_commit_amend_no_edit(GitOps *o) { RUN("Amending commit...", "commit", "--amend", "--no-edit"); }
u32 gitops_undo_last_commit(GitOps *o) { RUN("Undoing last commit...", "reset", "--soft", "HEAD~1"); }

u32 gitops_fetch(GitOps *o, bool all, bool prune)
{
	if (all && prune)
		RUN("Fetching...", "fetch", "--all", "--prune");
	if (all)
		RUN("Fetching...", "fetch", "--all");
	if (prune)
		RUN("Fetching...", "fetch", "--prune");
	RUN("Fetching...", "fetch");
}

u32 gitops_pull(GitOps *o, bool rebase)
{
	if (rebase)
		RUN("Pulling...", "pull", "--rebase");
	RUN("Pulling...", "pull", "--no-rebase");
}

u32 gitops_push(GitOps *o, bool force_with_lease)
{
	if (force_with_lease)
		RUN("Pushing...", "push", "--force-with-lease");
	RUN("Pushing...", "push");
}

u32 gitops_publish(GitOps *o, const char *remote, const char *branch)
{
	RUN("Publishing branch...", "push", "-u", remote && *remote ? remote : "origin", branch);
}

u32 gitops_tag_create(GitOps *o, const char *name, const char *message, const char *sha)
{
	if (message && *message) {
		if (sha && *sha)
			RUN("Creating tag...", "tag", "-a", name, "-m", message, sha);
		RUN("Creating tag...", "tag", "-a", name, "-m", message);
	}
	if (sha && *sha)
		RUN("Creating tag...", "tag", name, sha);
	RUN("Creating tag...", "tag", name);
}

u32 gitops_tag_delete(GitOps *o, const char *name) { RUN("Deleting tag...", "tag", "-d", name); }

/* ---- synchronous queries ------------------------------------------------------ */

static int query(GitOps *o, const char *const *argv, GitRun *r)
{
	int st = git_cli(o->workdir, argv, NULL, r, NULL);
	if (st != 0) {
		git_run_free(r);
		return -1;
	}
	return 0;
}

/* Split s into fields at sep (in place); returns the count. */
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

static void track_counts(const char *t, u32 *ahead, u32 *behind)
{
	*ahead = *behind = 0;
	const char *a = strstr(t, "ahead ");
	const char *b = strstr(t, "behind ");
	if (a)
		*ahead = (u32)strtoul(a + 6, NULL, 10);
	if (b)
		*behind = (u32)strtoul(b + 7, NULL, 10);
}

int gitops_branches(GitOps *o, GitBranchList *l)
{
	memset(l, 0, sizeof *l);
	const char *argv[] = {"for-each-ref",
	                      "--format=%(refname)%1f%(refname:short)%1f%(HEAD)%1f%(upstream:short)%1f"
	                      "%(upstream:track,nobracket)%1f%(objectname)%1f%(committerdate:unix)%1f%(symref)%1e",
	                      "refs/heads", "refs/remotes", NULL};
	GitRun r;
	if (query(o, argv, &r) != 0)
		return -1;
	char *p = r.out;
	while (*p) {
		char *e = strchr(p, 0x1e);
		if (!e)
			break;
		*e = 0;
		while (*p == '\n')
			p++;
		char *f[8];
		if (split(p, 0x1f, f, 8) == 8 && !*f[7] /* skip origin/HEAD symrefs */) {
			if (l->n == l->cap) {
				u32 c = l->cap ? l->cap * 2 : 32;
				l->name = vx_realloc(l->name, c * sizeof(u32));
				l->upstream = vx_realloc(l->upstream, c * sizeof(u32));
				l->hash = vx_realloc(l->hash, c * sizeof(u32));
				l->flags = vx_realloc(l->flags, c);
				l->ahead = vx_realloc(l->ahead, c * sizeof(u32));
				l->behind = vx_realloc(l->behind, c * sizeof(u32));
				l->time = vx_realloc(l->time, c * sizeof(i64));
				l->cap = c;
			}
			u32 i = l->n++;
			l->name[i] = pool_add(&l->pool, f[1], (u32)strlen(f[1]));
			l->upstream[i] = pool_add(&l->pool, f[3], (u32)strlen(f[3]));
			l->hash[i] = pool_add(&l->pool, f[5], (u32)strlen(f[5]));
			l->flags[i] = (u8)((!strncmp(f[0], "refs/heads/", 11) ? GB_LOCAL : GB_REMOTE) |
			                   (f[2][0] == '*' ? GB_CURRENT : 0));
			track_counts(f[4], &l->ahead[i], &l->behind[i]);
			l->time[i] = strtoll(f[6], NULL, 10);
		}
		p = e + 1;
	}
	git_run_free(&r);
	return 0;
}

int gitops_stashes(GitOps *o, GitStashList *l)
{
	memset(l, 0, sizeof *l);
	const char *argv[] = {"stash", "list", "--format=%gd%x1f%gs%x1e", NULL};
	GitRun r;
	if (query(o, argv, &r) != 0)
		return -1;
	char *p = r.out;
	while (*p) {
		char *e = strchr(p, 0x1e);
		if (!e)
			break;
		*e = 0;
		while (*p == '\n')
			p++;
		char *f[2];
		if (split(p, 0x1f, f, 2) == 2) {
			if (l->n == l->cap) {
				u32 c = l->cap ? l->cap * 2 : 16;
				l->ref = vx_realloc(l->ref, c * sizeof(u32));
				l->message = vx_realloc(l->message, c * sizeof(u32));
				l->cap = c;
			}
			u32 i = l->n++;
			l->ref[i] = pool_add(&l->pool, f[0], (u32)strlen(f[0]));
			l->message[i] = pool_add(&l->pool, f[1], (u32)strlen(f[1]));
		}
		p = e + 1;
	}
	git_run_free(&r);
	return 0;
}

static void names_add(GitNameList *l, const char *a, size_t na, const char *b, size_t nb)
{
	if (l->n == l->cap) {
		u32 c = l->cap ? l->cap * 2 : 16;
		l->name = vx_realloc(l->name, c * sizeof(u32));
		l->hash = vx_realloc(l->hash, c * sizeof(u32));
		l->cap = c;
	}
	u32 i = l->n++;
	l->name[i] = pool_add(&l->pool, a, (u32)na);
	l->hash[i] = pool_add(&l->pool, b, (u32)nb);
}

int gitops_tags(GitOps *o, GitNameList *l)
{
	memset(l, 0, sizeof *l);
	const char *argv[] = {"for-each-ref", "--sort=-creatordate",
	                      "--format=%(refname:short)%1f%(objectname)%1e", "refs/tags", NULL};
	GitRun r;
	if (query(o, argv, &r) != 0)
		return -1;
	char *p = r.out;
	while (*p) {
		char *e = strchr(p, 0x1e);
		if (!e)
			break;
		*e = 0;
		while (*p == '\n')
			p++;
		char *f[2];
		if (split(p, 0x1f, f, 2) == 2)
			names_add(l, f[0], strlen(f[0]), f[1], strlen(f[1]));
		p = e + 1;
	}
	git_run_free(&r);
	return 0;
}

int gitops_remotes(GitOps *o, GitNameList *l)
{
	memset(l, 0, sizeof *l);
	const char *argv[] = {"remote", "-v", NULL};
	GitRun r;
	if (query(o, argv, &r) != 0)
		return -1;
	/* "origin\t/url (fetch)" */
	char *p = r.out;
	while (*p) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		char *tab = strchr(p, '\t');
		char *sp = tab ? strrchr(tab, ' ') : NULL;
		if (tab && sp && !strcmp(sp, " (fetch)"))
			names_add(l, p, (size_t)(tab - p), tab + 1, (size_t)(sp - tab - 1));
		if (!e)
			break;
		p = e + 1;
	}
	git_run_free(&r);
	return 0;
}

static bool gd_exists(GitOps *o, const char *name)
{
	if (!o->gitdir)
		return false;
	char p[4096];
	snprintf(p, sizeof p, "%s/%s", o->gitdir, name);
	struct stat st;
	return stat(p, &st) == 0;
}

u32 gitops_state(GitOps *o)
{
	u32 s = 0;
	if (gd_exists(o, "rebase-merge") || gd_exists(o, "rebase-apply"))
		s |= GST_REBASE;
	if (gd_exists(o, "MERGE_HEAD"))
		s |= GST_MERGE;
	if (gd_exists(o, "CHERRY_PICK_HEAD"))
		s |= GST_CHERRY_PICK;
	if (gd_exists(o, "REVERT_HEAD"))
		s |= GST_REVERT;
	if (gd_exists(o, "BISECT_LOG"))
		s |= GST_BISECT;
	return s;
}

void gitops_branches_free(GitBranchList *l)
{
	free(l->name);
	free(l->upstream);
	free(l->hash);
	free(l->flags);
	free(l->ahead);
	free(l->behind);
	free(l->time);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}

void gitops_stashes_free(GitStashList *l)
{
	free(l->ref);
	free(l->message);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}

void gitops_names_free(GitNameList *l)
{
	free(l->name);
	free(l->hash);
	pool_free(&l->pool);
	memset(l, 0, sizeof *l);
}
