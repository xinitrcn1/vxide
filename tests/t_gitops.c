/* Git actions through the async runner + synchronous queries. */
#define _POSIX_C_SOURCE 200809L
#include "gitops.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static char dir[64], remote[96], other[96];

static void sh(const char *in, const char *cmd)
{
	char buf[2048];
	snprintf(buf, sizeof buf, "cd %s && (%s) >/dev/null 2>&1", in, cmd);
	if (system(buf) != 0)
		fprintf(stderr, "command failed: %s\n", cmd);
}

/* Wait for op id; returns its exit status (or -99 on timeout). */
static int wait_op(GitOps *o, u32 id, char **err_out)
{
	for (int i = 0; i < 20000; i++) {
		GitOpResult r;
		while (gitops_poll(o, &r)) {
			bool mine = r.id == id;
			int st = r.status;
			if (mine && err_out)
				*err_out = vx_strdup(r.err);
			gitops_result_free(&r);
			if (mine)
				return st;
		}
		struct timespec ts = {0, 1000000};
		nanosleep(&ts, NULL);
	}
	return -99;
}

/* Wait for op id and return its stdout (malloc'd) and status. */
static char *run_out(GitOps *o, u32 id, int *st)
{
	for (int i = 0; i < 20000; i++) {
		GitOpResult r;
		while (gitops_poll(o, &r)) {
			if (r.id == id) {
				char *out = vx_strdup(r.out ? r.out : "");
				*st = r.status;
				if (r.status != 0)
					fprintf(stderr, "op %u failed (%d): %s\n", id, r.status, r.err ? r.err : "");
				gitops_result_free(&r);
				return out;
			}
			gitops_result_free(&r);
		}
		struct timespec ts = {0, 1000000};
		nanosleep(&ts, NULL);
	}
	*st = -99;
	return vx_strdup("");
}

/* A network op that must have run on libgit2 (no CLI retry note). */
static int net_ok(GitOps *o, u32 id)
{
	int st;
	char *out = run_out(o, id, &st);
	bool cli = strstr(out, "retrying with git") != NULL;
	if (cli)
		fprintf(stderr, "op %u fell back to the CLI: %s\n", id, out);
	free(out);
	return cli ? -1 : st;
}

static int run_ok(GitOps *o, u32 id)
{
	char *err = NULL;
	int st = wait_op(o, id, &err);
	if (st != 0)
		fprintf(stderr, "op %u failed (%d): %s\n", id, st, err ? err : "");
	free(err);
	return st;
}

static const char *current_branch(GitOps *o, GitBranchList *l)
{
	gitops_branches_free(l);
	gitops_branches(o, l);
	for (u32 i = 0; i < l->n; i++)
		if (l->flags[i] & GB_CURRENT)
			return l->pool.b + l->name[i];
	return "";
}

static i64 find_branch(const GitBranchList *l, const char *name)
{
	for (u32 i = 0; i < l->n; i++)
		if (!strcmp(l->pool.b + l->name[i], name))
			return i;
	return -1;
}

static bool file_has(const char *path, const char *needle)
{
	size_t n;
	char *s = vx_read_file(path, &n);
	bool ok = s && strstr(s, needle);
	free(s);
	return ok;
}

int main(void)
{
	CHECK(git_cli_path() != NULL && git_cli_path()[0] == '/');
	snprintf(dir, sizeof dir, "/tmp/vxide-ops-XXXXXX");
	CHECK(mkdtemp(dir) != NULL);
	snprintf(remote, sizeof remote, "%s.remote.git", dir);
	snprintf(other, sizeof other, "%s.other", dir);
	sh(dir, "git init -q -b main && git config user.name Ada && git config user.email ada@x");
	sh(dir, "printf 'a\\n' > a.txt && git add a.txt && git commit -qm c1");

	GitOps *o = gitops_start(dir);
	CHECK(o != NULL);
	char p[256];

	/* low level run with stdin */
	{
		GitRun r;
		const char *argv[] = {"hash-object", "--stdin", NULL};
		CHECK(git_cli(dir, argv, "hello\n", &r, NULL) == 0);
		CHECK(r.nout >= 40 && !strncmp(r.out, "ce013625030ba8dba906f756967f9e9ca394464a", 40));
		git_run_free(&r);
		const char *bad[] = {"no-such-command", NULL};
		CHECK(git_cli(dir, bad, NULL, &r, NULL) != 0 && r.nerr > 0);
		git_run_free(&r);
	}

	/* stash push / list / pop (with an untracked file) */
	sh(dir, "printf 'a\\nchange\\n' > a.txt && printf 'u\\n' > u.txt");
	CHECK(run_ok(o, gitops_stash_push(o, true, "my work")) == 0);
	GitStashList sl;
	CHECK(gitops_stashes(o, &sl) == 0 && sl.n == 1);
	CHECK_STR(sl.pool.b + sl.ref[0], "stash@{0}");
	CHECK(strstr(sl.pool.b + sl.message[0], "my work") != NULL);
	gitops_stashes_free(&sl);
	snprintf(p, sizeof p, "%s/u.txt", dir);
	CHECK(!file_has(p, "u"));
	CHECK(run_ok(o, gitops_stash_pop(o, 0)) == 0);
	CHECK(file_has(p, "u"));
	snprintf(p, sizeof p, "%s/a.txt", dir);
	CHECK(file_has(p, "change"));
	CHECK(gitops_stashes(o, &sl) == 0 && sl.n == 0);
	gitops_stashes_free(&sl);
	/* stash apply + drop */
	CHECK(run_ok(o, gitops_stash_push(o, false, NULL)) == 0);
	CHECK(run_ok(o, gitops_stash_apply(o, 0)) == 0);
	CHECK(file_has(p, "change"));
	CHECK(run_ok(o, gitops_stash_drop(o, 0)) == 0);
	sh(dir, "git checkout -q -- a.txt && rm -f u.txt");

	/* commit through stdin, amend, undo */
	sh(dir, "printf 'b\\n' > b.txt && git add b.txt");
	CHECK(run_ok(o, gitops_commit(o, "second\n\nbody line", false, false, false)) == 0);
	sh(dir, "git log -1 --format=%B > ../msg.txt");
	sh(dir, "printf 'b2\\n' >> b.txt && git add b.txt");
	CHECK(run_ok(o, gitops_commit_amend_no_edit(o)) == 0);
	{
		GitRun r;
		const char *argv[] = {"log", "--format=%s", NULL};
		git_cli(dir, argv, NULL, &r, NULL);
		CHECK_STR(r.out, "second\nc1\n");
		git_run_free(&r);
	}
	CHECK(run_ok(o, gitops_undo_last_commit(o)) == 0);
	{
		GitRun r;
		const char *argv[] = {"status", "--porcelain", NULL};
		git_cli(dir, argv, NULL, &r, NULL);
		CHECK(strstr(r.out, "A  b.txt") != NULL); /* soft reset keeps it staged */
		git_run_free(&r);
	}
	CHECK(run_ok(o, gitops_commit(o, "second", false, false, false)) == 0);

	/* branches: create + checkout + list */
	GitBranchList bl = {0};
	CHECK(run_ok(o, gitops_branch_create(o, "feature/x", NULL, true)) == 0);
	CHECK_STR(current_branch(o, &bl), "feature/x");
	sh(dir, "printf 'x\\n' > x.txt && git add x.txt && git commit -qm x1");
	CHECK(run_ok(o, gitops_checkout(o, "main", false)) == 0);
	CHECK_STR(current_branch(o, &bl), "main");
	CHECK(find_branch(&bl, "feature/x") >= 0);
	CHECK(bl.flags[find_branch(&bl, "feature/x")] & GB_LOCAL);

	/* merge with a conflict -> state, abort */
	sh(dir, "git switch -q -c conflict && printf 'conflict A\\n' > a.txt && git commit -qam ca");
	sh(dir, "git switch -q main && printf 'conflict B\\n' > a.txt && git commit -qam cb");
	CHECK(gitops_state(o) == 0);
	CHECK(wait_op(o, gitops_merge(o, "conflict"), NULL) != 0);
	CHECK(gitops_state(o) & GST_MERGE);
	CHECK(run_ok(o, gitops_merge_abort(o)) == 0);
	CHECK(gitops_state(o) == 0);
	/* rebase conflict -> state, abort; then a clean rebase */
	CHECK(wait_op(o, gitops_rebase(o, "conflict"), NULL) != 0);
	CHECK(gitops_state(o) & GST_REBASE);
	CHECK(run_ok(o, gitops_rebase_abort(o)) == 0);
	CHECK(gitops_state(o) == 0);
	CHECK(run_ok(o, gitops_checkout(o, "feature/x", false)) == 0);
	CHECK(run_ok(o, gitops_rebase(o, "main")) == 0);
	{
		GitRun r;
		const char *argv[] = {"merge-base", "--is-ancestor", "main", "feature/x", NULL};
		CHECK(git_cli(dir, argv, NULL, &r, NULL) == 0);
		git_run_free(&r);
	}
	CHECK(run_ok(o, gitops_checkout(o, "main", false)) == 0);
	CHECK(run_ok(o, gitops_merge(o, "feature/x")) == 0);
	CHECK(run_ok(o, gitops_branch_delete(o, "feature/x", false)) == 0);
	CHECK(run_ok(o, gitops_branch_delete(o, "conflict", true)) == 0);
	/* cherry-pick */
	sh(dir, "git switch -q -c side && printf 's\\n' > s.txt && git add s.txt && git commit -qm s1 && git switch -q main");
	{
		GitRun r;
		const char *argv[] = {"rev-parse", "side", NULL};
		git_cli(dir, argv, NULL, &r, NULL);
		r.out[40] = 0;
		CHECK(run_ok(o, gitops_cherry_pick(o, r.out)) == 0);
		git_run_free(&r);
	}
	snprintf(p, sizeof p, "%s/s.txt", dir);
	CHECK(file_has(p, "s"));

	/* deleting an unmerged branch is refused with git's wording */
	sh(dir, "git switch -q -c unmerged && printf 'z\\n' > z.txt && git add z.txt && git commit -qm z1 && git switch -q main");
	{
		char *err = NULL;
		CHECK(wait_op(o, gitops_branch_delete(o, "unmerged", false), &err) != 0);
		CHECK(err && strstr(err, "not fully merged"));
		free(err);
	}
	/* a clean non-fast-forward merge makes a merge commit */
	sh(dir, "printf 'm\\n' > mm.txt && git add mm.txt && git commit -qm m1");
	CHECK(run_ok(o, gitops_merge(o, "unmerged")) == 0);
	{
		GitRun r;
		const char *argv[] = {"log", "-1", "--format=%s %p", NULL};
		git_cli(dir, argv, NULL, &r, NULL);
		CHECK(!strncmp(r.out, "Merge branch 'unmerged'", 23) && strchr(strchr(r.out, ' ') + 23, ' '));
		git_run_free(&r);
	}
	CHECK(run_ok(o, gitops_branch_delete(o, "unmerged", false)) == 0); /* merged now */
	/* revert makes a commit that undoes another */
	{
		GitRun r;
		const char *argv[] = {"rev-parse", "HEAD~1", NULL};
		git_cli(dir, argv, NULL, &r, NULL);
		r.out[40] = 0;
		CHECK(run_ok(o, gitops_revert(o, r.out)) == 0);
		git_run_free(&r);
		snprintf(p, sizeof p, "%s/mm.txt", dir);
		CHECK(!file_has(p, "m"));
	}
	/* argv the UI sends: unstage all, discard all, detach */
	sh(dir, "printf 'q\\n' > s.txt && git add s.txt");
	{
		const char *a1[] = {"reset", "-q", NULL};
		CHECK(run_ok(o, gitops_run(o, "Unstaging...", a1, NULL)) == 0);
		GitRun r;
		const char *st[] = {"status", "--porcelain", NULL};
		git_cli(dir, st, NULL, &r, NULL);
		CHECK(strstr(r.out, " M s.txt") != NULL);
		git_run_free(&r);
		const char *a2[] = {"restore", "--worktree", "--", ".", NULL};
		CHECK(run_ok(o, gitops_run(o, "Discarding...", a2, NULL)) == 0);
		git_cli(dir, st, NULL, &r, NULL);
		CHECK(r.nout == 0);
		git_run_free(&r);
		const char *a3[] = {"checkout", "--detach", "HEAD~1", NULL};
		CHECK(run_ok(o, gitops_run(o, "Checking out...", a3, NULL)) == 0);
		CHECK(!strcmp(current_branch(o, &bl), ""));
		CHECK(run_ok(o, gitops_checkout(o, "main", false)) == 0);
	}

	/* tags */
	CHECK(run_ok(o, gitops_tag_create(o, "v1", NULL, NULL)) == 0);
	CHECK(run_ok(o, gitops_tag_create(o, "v2", "release two", NULL)) == 0);
	GitNameList tl;
	CHECK(gitops_tags(o, &tl) == 0 && tl.n == 2);
	gitops_names_free(&tl);
	CHECK(run_ok(o, gitops_tag_delete(o, "v1")) == 0);
	CHECK(gitops_tags(o, &tl) == 0 && tl.n == 1);
	CHECK_STR(tl.pool.b + tl.name[0], "v2");
	gitops_names_free(&tl);

	/* remote: publish, ahead/behind, fetch, pull, push */
	{
		char cmd[512];
		snprintf(cmd, sizeof cmd, "git init -q --bare -b main %s && git remote add origin %s", remote, remote);
		sh(dir, cmd);
	}
	GitNameList rl;
	CHECK(gitops_remotes(o, &rl) == 0 && rl.n == 1);
	CHECK_STR(rl.pool.b + rl.name[0], "origin");
	CHECK_STR(rl.pool.b + rl.hash[0], remote);
	gitops_names_free(&rl);
	CHECK(net_ok(o, gitops_publish(o, "origin", "main")) == 0);
	current_branch(o, &bl);
	i64 m = find_branch(&bl, "main");
	CHECK(m >= 0 && !strcmp(bl.pool.b + bl.upstream[m], "origin/main"));
	CHECK(find_branch(&bl, "origin/main") >= 0 && (bl.flags[find_branch(&bl, "origin/main")] & GB_REMOTE));
	sh(dir, "printf 'l\\n' > l.txt && git add l.txt && git commit -qm local1");
	current_branch(o, &bl);
	m = find_branch(&bl, "main");
	CHECK(bl.ahead[m] == 1 && bl.behind[m] == 0);
	{
		char cmd[512];
		snprintf(cmd, sizeof cmd,
		         "git clone -q %s %s && cd %s && git config user.name Bob && git config user.email bob@x && "
		         "printf 'r\\n' > r.txt && git add r.txt && git commit -qm remote1 && git push -q origin main",
		         remote, other, other);
		sh("/tmp", cmd);
	}
	CHECK(net_ok(o, gitops_fetch(o, false, true)) == 0);
	current_branch(o, &bl);
	m = find_branch(&bl, "main");
	CHECK(bl.ahead[m] == 1 && bl.behind[m] == 1);
	CHECK(net_ok(o, gitops_pull(o, true)) == 0);
	current_branch(o, &bl);
	m = find_branch(&bl, "main");
	CHECK(bl.ahead[m] == 1 && bl.behind[m] == 0);
	CHECK(net_ok(o, gitops_push(o, false)) == 0);
	current_branch(o, &bl);
	m = find_branch(&bl, "main");
	CHECK(bl.ahead[m] == 0 && bl.behind[m] == 0);
	/* force with lease: refused when the remote moved since our last fetch */
	sh(other, "git pull -q --no-rebase origin main && printf 'w\\n' > w.txt && git add w.txt && git commit -qm remote2 && git push -q origin main");
	sh(dir, "git commit -q --allow-empty -m local2");
	{
		char *err = NULL;
		CHECK(wait_op(o, gitops_push(o, true), &err) != 0);
		CHECK(err && strstr(err, "stale info"));
		free(err);
	}
	CHECK(net_ok(o, gitops_fetch(o, false, false)) == 0);
	CHECK(net_ok(o, gitops_push(o, true)) == 0); /* known remote state now: forced */
	current_branch(o, &bl);
	m = find_branch(&bl, "main");
	CHECK(bl.ahead[m] == 0 && bl.behind[m] == 0);
	/* remote branch checkout creates a tracking branch */
	sh(other, "git switch -q -c topic && git commit -q --allow-empty -m t && git push -q origin topic");
	CHECK(net_ok(o, gitops_fetch(o, true, false)) == 0);
	CHECK(run_ok(o, gitops_checkout(o, "origin/topic", true)) == 0);
	CHECK_STR(current_branch(o, &bl), "topic");
	CHECK(!strcmp(bl.pool.b + bl.upstream[find_branch(&bl, "topic")], "origin/topic"));
	gitops_branches_free(&bl);

	/* FIFO order and busy label */
	{
		u32 a = gitops_fetch(o, false, false), b = gitops_fetch(o, false, false);
		char lab[64];
		bool busy = gitops_busy(o, lab, sizeof lab);
		CHECK(busy && !strcmp(lab, "Fetching..."));
		GitOpResult r;
		u32 seen[2], ns = 0;
		for (int i = 0; i < 20000 && ns < 2; i++) {
			while (ns < 2 && gitops_poll(o, &r)) {
				seen[ns++] = r.id;
				gitops_result_free(&r);
			}
			struct timespec ts = {0, 1000000};
			nanosleep(&ts, NULL);
		}
		CHECK(ns == 2 && seen[0] == a && seen[1] == b);
		CHECK(!gitops_busy(o, NULL, 0));
	}

	/* stop kills a long-running op and joins promptly */
	{
		const char *argv[] = {"-c", "alias.nap=!sleep 30", "nap", NULL};
		gitops_run(o, "Sleeping...", argv, NULL);
		for (int i = 0; i < 2000 && !gitops_busy(o, NULL, 0); i++) {
			struct timespec ts = {0, 1000000};
			nanosleep(&ts, NULL);
		}
		struct timespec ts = {0, 200000000};
		nanosleep(&ts, NULL);
		char lab[64];
		CHECK(gitops_busy(o, lab, sizeof lab) && !strcmp(lab, "Sleeping..."));
		u64 t0 = vx_now_ns();
		gitops_stop(o);
		u64 dt = vx_now_ns() - t0;
		CHECK(dt < 3000000000ull);
		fprintf(stderr, "stop with a running op: %.0f ms\n", (double)dt / 1e6);
	}

	char cmd[512];
	snprintf(cmd, sizeof cmd, "rm -rf %s %s %s %s/../msg.txt", dir, remote, other, dir);
	system(cmd);
	return tap_done();
}
