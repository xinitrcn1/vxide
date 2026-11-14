/* Explorer tree: exclusions, sort order, lazy load, reveal, refresh. */
#define _POSIX_C_SOURCE 200809L
#include "fstree.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *dir, const char *rel)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, rel);
	vx_write_file(p, "x", 1);
}

static void mk(const char *dir, const char *rel)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, rel);
	mkdir(p, 0755);
}

int main(void)
{
	char dir[] = "/tmp/vxide-fs-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	mk(dir, ".git");
	touch(dir, ".git/HEAD");
	mk(dir, "src");
	mk(dir, "Docs");
	touch(dir, "src/b.c");
	touch(dir, "src/A.h");
	touch(dir, "zeta.txt");
	touch(dir, "Alpha.md");
	touch(dir, ".DS_Store");
	touch(dir, ".gitignore");

	FsTree t;
	fs_init(&t, dir);
	u32 *rows = NULL, cap = 0;
	u32 n = fs_rows(&t, &rows, &cap);
	/* folders first, case-insensitive; .git and .DS_Store hidden */
	const char *want[] = {"Docs", "src", ".gitignore", "Alpha.md", "zeta.txt"};
	CHECK(n == 5);
	for (u32 i = 0; i < n && i < 5; i++)
		CHECK_STR(fs_name(&t, rows[i]), want[i]);
	CHECK(fs_excluded(".git") && fs_excluded("Thumbs.db") && !fs_excluded("git"));

	/* expand src lazily */
	u32 src = rows[1];
	CHECK(!(t.flags[src] & FS_LOADED));
	fs_toggle(&t, src);
	n = fs_rows(&t, &rows, &cap);
	CHECK(n == 7);
	CHECK_STR(fs_name(&t, rows[2]), "A.h");
	CHECK_STR(fs_name(&t, rows[3]), "b.c");
	char rel[256];
	fs_relpath(&t, rows[3], rel, sizeof rel);
	CHECK_STR(rel, "src/b.c");

	/* refresh keeps open folders and sees new files */
	touch(dir, "src/c.c");
	fs_refresh(&t);
	n = fs_rows(&t, &rows, &cap);
	CHECK(n == 8);

	/* reveal opens ancestors */
	mk(dir, "Docs/deep");
	touch(dir, "Docs/deep/x.c");
	fs_refresh(&t);
	u32 x = fs_reveal(&t, "Docs/deep/x.c");
	CHECK(x != FS_NONE);
	fs_relpath(&t, x, rel, sizeof rel);
	CHECK_STR(rel, "Docs/deep/x.c");
	n = fs_rows(&t, &rows, &cap);
	CHECK(n == 10); /* Docs deep x.c src A.h b.c c.c .gitignore Alpha.md zeta.txt */
	CHECK(fs_reveal(&t, "nope/x") == FS_NONE);

	fs_collapse_all(&t);
	n = fs_rows(&t, &rows, &cap);
	CHECK(n == 5);
	free(rows);
	fs_free(&t);
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
