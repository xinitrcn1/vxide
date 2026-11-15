/* Inline blame: CLI and libgit2 paths, uncommitted lines, untracked files. */
#define _POSIX_C_SOURCE 200809L
#include "blame.h"
#include "lgblame.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void sh(const char *dir, const char *cmd)
{
	char buf[1024];
	snprintf(buf, sizeof buf, "cd %s && %s >/dev/null 2>&1", dir, cmd);
	if (system(buf) != 0)
		fprintf(stderr, "command failed: %s\n", cmd);
}

static void wait_ready(Blame *b, const char *p, u32 key)
{
	for (int i = 0; i < 5000 && !blame_ready(b, p, key); i++) {
		struct timespec ts = {0, 1000000};
		nanosleep(&ts, NULL);
	}
}

static void run(const char *dir)
{
	Blame *b = blame_start(dir);
	CHECK(b != NULL);
	const char *text = "one\nTWO\nthree\nfour\n";
	blame_request(b, "f.txt", text, strlen(text), 1);
	wait_ready(b, "f.txt", 1);
	BlameLine bl;
	CHECK(blame_line(b, "f.txt", 1, 0, &bl) && !bl.uncommitted);
	CHECK_STR(bl.author, "Ada");
	CHECK_STR(bl.summary, "first");
	CHECK(blame_line(b, "f.txt", 1, 1, &bl) && bl.uncommitted);
	CHECK(blame_line(b, "f.txt", 1, 2, &bl) && !bl.uncommitted);
	CHECK_STR(bl.summary, "second");
	CHECK(blame_line(b, "f.txt", 1, 3, &bl) && bl.uncommitted); /* added line */
	CHECK(!blame_line(b, "f.txt", 2, 0, &bl));                  /* other key */
	/* untracked: ready but no lines */
	blame_request(b, "new.txt", "x\n", 2, 3);
	wait_ready(b, "new.txt", 3);
	CHECK(blame_ready(b, "new.txt", 3));
	CHECK(!blame_line(b, "new.txt", 3, 0, &bl));
	blame_stop(b);
}

int main(void)
{
	char dir[] = "/tmp/vxide-blame-test-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	sh(dir, "git init -q -b main && git config user.name Ada && git config user.email ada@x");
	sh(dir, "printf 'one\\ntwo\\n' > f.txt && git add f.txt && git commit -qm first");
	sh(dir, "printf 'one\\ntwo\\nthree\\n' > f.txt && git commit -qam second");
	sh(dir, "printf 'x\\n' > new.txt");
	run(dir);
	/* history with a rename and a merge, checked against `git blame` */
	sh(dir, "printf 'l1\\nl2\\nl3\\nl4\\n' > g.txt && git add g.txt && git commit -qm g1");
	sh(dir, "git mv g.txt h.txt && git commit -qm rename");
	sh(dir, "git checkout -qb side && printf 'l1\\nside\\nl3\\nl4\\n' > h.txt && git commit -qam side");
	sh(dir, "git checkout -q main && printf 'l1\\nl2\\nl3\\nmain\\n' > h.txt && git commit -qam mainchange");
	sh(dir, "git merge -q --no-edit side");
	{
		git_libgit2_init();
		git_repository *repo;
		CHECK(git_repository_open(&repo, dir) == 0);
		LgBlame lb;
		CHECK(lg_blame_head(repo, "h.txt", &lb) == 0);
		CHECK(lb.nlines == 4);
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cd %s && git blame --root -s --abbrev=9 h.txt | cut -c1-9", dir);
		FILE *f = popen(cmd, "r");
		char line[64];
		u32 l = 0;
		bool same = true;
		while (f && fgets(line, sizeof line, f) && l < lb.nlines) {
			char h[16];
			git_oid_tostr(h, 10, &lb.oid[lb.line_commit[l]]);
			if (strncmp(h, line, 9)) {
				fprintf(stderr, "line %u: ours %s git %.9s\n", l + 1, h, line);
				same = false;
			}
			l++;
		}
		if (f)
			pclose(f);
		CHECK(same && l == 4);
		lg_blame_free(&lb);
		CHECK(lg_blame_head(repo, "nope.txt", &lb) == 1);
		git_repository_free(repo);
	}
	char ago[48];
	blame_ago(1000, 1000 + 3 * 86400 + 5, ago, sizeof ago);
	CHECK_STR(ago, "3 days ago");
	blame_ago(1000, 1000 + 400 * 86400, ago, sizeof ago);
	CHECK_STR(ago, "1 year ago");
	blame_ago(1000, 1010, ago, sizeof ago);
	CHECK_STR(ago, "now");
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
