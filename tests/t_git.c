#define _POSIX_C_SOURCE 200809L
#include "git.h"
#include "tap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256];

static int sh(const char *fmt, ...)
{
	char cmd[1024];
	int n = snprintf(cmd, sizeof cmd, "cd '%s' && ", root);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd + n, sizeof cmd - (size_t)n, fmt, ap);
	va_end(ap);
	return system(cmd);
}

static void put(const char *rel, const char *text)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", root, rel);
	vx_write_file(p, text, strlen(text));
}

static bool exists(const char *rel)
{
	char p[512];
	struct stat sb;
	snprintf(p, sizeof p, "%s/%s", root, rel);
	return stat(p, &sb) == 0;
}

static u32 count_lines(const char *t)
{
	u32 n = 1;
	for (; *t; t++)
		n += *t == '\n';
	return n;
}

/* marks as a string: . + ~ ^ */
static const char *marks_str(Git *g, const char *rel, const char *text)
{
	static char out[64];
	u8 m[64];
	u32 n = count_lines(text);
	if (git_line_marks(g, rel, text, strlen(text), m, n) != 0)
		return "error";
	for (u32 i = 0; i < n; i++)
		out[i] = ".+~^"[m[i]];
	out[n] = 0;
	return out;
}

int main(void)
{
	snprintf(root, sizeof root, "/tmp/vxgitXXXXXX");
	if (!mkdtemp(root))
		return 1;
	CHECK(git_open(root) == NULL);
	CHECK(sh("git init -q -b main && git config user.name T && git config user.email t@e"
	         " && git config commit.gpgsign false") == 0);
	sh("mkdir -p sub/deep");

	/* discovery from a subdirectory */
	char subp[512];
	snprintf(subp, sizeof subp, "%s/sub/deep", root);
	Git *g = git_open(subp);
	CHECK(g != NULL);
	if (!g)
		return tap_done();
	CHECK(git_workdir(g)[strlen(git_workdir(g)) - 1] == '/');
	CHECK_STR(git_branch(g), "main");

	/* unborn: untracked file */
	put("a.txt", "1\n2\n3\n4\n5\n");
	git_refresh(g);
	CHECK(git_status(g)->count == 1);
	CHECK(git_path_flags(g, "a.txt") == GS_WT_NEW);
	CHECK_STR(marks_str(g, "a.txt", "1\n2\n"), "+++");

	/* stage / unstage on an unborn branch */
	CHECK(git_stage(g, "a.txt") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_INDEX_NEW);
	CHECK(git_unstage(g, "a.txt") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_WT_NEW);

	/* first commit */
	CHECK(git_commit_staged(g, "nothing") != 0); /* nothing staged */
	CHECK(git_last_error()[0] != 0);
	CHECK(git_stage(g, "a.txt") == 0);
	CHECK(git_commit_staged(g, "") != 0); /* empty message */
	CHECK(git_commit_staged(g, "first\n\n# comment\n") == 0);
	git_refresh(g);
	CHECK(git_status(g)->count == 0);
	CHECK(git_path_flags(g, "a.txt") == 0);
	CHECK_STR(git_branch(g), "main");
	CHECK(git_commit_staged(g, "again") != 0);
	CHECK(strstr(git_last_error(), "nothing") != NULL);

	/* quick diff marks against the index */
	CHECK_STR(marks_str(g, "a.txt", "1\n2\n3\n4\n5\n"), "......");
	CHECK_STR(marks_str(g, "a.txt", "1\nX\n3\n4\n5\n6\n"), ".~...+.");
	CHECK_STR(marks_str(g, "a.txt", "1\n2\n4\n5\n"), "..^..");
	CHECK_STR(marks_str(g, "a.txt", "2\n3\n4\n5\n"), "^....");
	CHECK_STR(marks_str(g, "a.txt", "0\n1\n2\n3\n4\n5\n"), "+......");
	CHECK_STR(marks_str(g, "a.txt", "1\n2\n3\n"), "...^");

	/* modified file */
	put("a.txt", "1\nX\n3\n4\n5\n");
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_WT_MODIFIED);

	/* directory aggregation and prefix confusion */
	put("dir-x.txt", "x\n");
	put("dir.c", "x\n");
	sh("mkdir -p dir/sub");
	put("dir/sub/b.c", "b\n");
	put("dir/c.c", "c\n");
	CHECK(git_stage(g, "dir/c.c") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "dir") == (GS_WT_NEW | GS_INDEX_NEW));
	CHECK(git_path_flags(g, "dir/") == (GS_WT_NEW | GS_INDEX_NEW));
	CHECK(git_path_flags(g, "dir/sub") == GS_WT_NEW);
	CHECK(git_path_flags(g, "di") == 0);
	CHECK(git_path_flags(g, "") & GS_WT_MODIFIED);

	/* ignored */
	put(".gitignore", "*.o\nbuild/\n");
	sh("mkdir -p build && touch build/x.c x.o");
	git_refresh(g);
	CHECK(git_path_flags(g, "x.o") == GS_IGNORED);
	CHECK(git_path_flags(g, "build") == GS_IGNORED);
	CHECK(git_path_flags(g, "build/x.c") == GS_IGNORED);
	CHECK(git_path_flags(g, "x.o") == GS_IGNORED); /* memoised */
	CHECK(git_path_flags(g, "a.txt") == GS_WT_MODIFIED);
	CHECK(git_path_flags(g, ".gitignore") == GS_WT_NEW);

	/* diff text */
	char *d = git_diff_text(g, "a.txt", 0);
	CHECK(d && strstr(d, "\n-2\n") && strstr(d, "\n+X\n"));
	free(d);
	d = git_diff_text(g, "a.txt", 1);
	CHECK(d && d[0] == 0);
	free(d);
	CHECK(git_stage(g, "a.txt") == 0);
	d = git_diff_text(g, "a.txt", 1);
	CHECK(d && strstr(d, "\n+X\n"));
	free(d);
	d = git_diff_text(g, "dir/sub/b.c", 0);
	CHECK(d && strstr(d, "\n+b\n"));
	free(d);

	/* unstage + discard */
	CHECK(git_unstage(g, "a.txt") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_WT_MODIFIED);
	CHECK(git_discard(g, "a.txt") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == 0);
	{
		char p[512];
		snprintf(p, sizeof p, "%s/a.txt", root);
		char *t = vx_read_file(p, NULL);
		CHECK_STR(t, "1\n2\n3\n4\n5\n");
		free(t);
	}
	CHECK(git_discard(g, "dir-x.txt") == 0);
	CHECK(!exists("dir-x.txt"));

	/* deleted in worktree, staged via git_stage */
	sh("rm a.txt");
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_WT_DELETED);
	CHECK(git_stage(g, "a.txt") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "a.txt") == GS_INDEX_DELETED);
	CHECK(git_unstage(g, "a.txt") == 0);
	CHECK(git_discard(g, "a.txt") == 0);
	CHECK(exists("a.txt"));

	/* stage all + commit, then ahead/behind against a local upstream */
	CHECK(git_stage_all(g) == 0);
	CHECK(git_commit_staged(g, "second") == 0);
	git_refresh(g);
	CHECK(git_path_flags(g, "") == 0);
	CHECK(sh("git branch -q base HEAD~1 && git branch -q -u base") == 0);
	git_refresh(g);
	u32 ah = 9, bh = 9;
	git_ahead_behind(g, &ah, &bh);
	CHECK(ah == 1 && bh == 0);

	/* detached HEAD */
	CHECK(sh("git checkout -q --detach HEAD") == 0);
	git_refresh(g);
	CHECK(strlen(git_branch(g)) == 7);
	git_ahead_behind(g, &ah, &bh);
	CHECK(ah == 0 && bh == 0);

	git_close(g);

	/* bare repo -> NULL */
	CHECK(sh("git init -q --bare bare.git") == 0);
	char bp[512];
	snprintf(bp, sizeof bp, "%s/bare.git", root);
	CHECK(git_open(bp) == NULL);

	char rm[512];
	snprintf(rm, sizeof rm, "rm -rf '%s'", root);
	system(rm);
	return tap_done();
}
