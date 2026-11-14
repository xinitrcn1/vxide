/* File walk, fuzzy matching, text search, find_in. */
#define _POSIX_C_SOURCE 200809L
#include "search.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static bool skip_build(void *ctx, const char *rel, bool dir)
{
	(void)ctx;
	return dir && strcmp(rel, "build") == 0;
}

int main(void)
{
	char dir[] = "/tmp/vxide-search-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char p[512];
	const char *files[] = {"src/main.c", "src/util.c", "include/util.h", "build/out.o", "node_modules/x.js",
	                       ".git/config", "README.md"};
	for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
		snprintf(p, sizeof p, "%s/%s", dir, files[i]);
		char d[512];
		snprintf(d, sizeof d, "%s", p);
		*strrchr(d, '/') = 0;
		mkdir(d, 0755);
		const char *body = "int total = 0;\ntotal += 1; // Total\n";
		vx_write_file(p, body, strlen(body));
	}
	FileList fl = {0};
	files_scan(&fl, dir, skip_build, NULL, 1000);
	CHECK(fl.n == 4); /* main.c util.c util.h README.md */
	bool saw_git = false;
	for (u32 i = 0; i < fl.n; i++) {
		const char *r = fl.pool.b + fl.path[i];
		if (strstr(r, ".git") || strstr(r, "node_modules") || strstr(r, "build"))
			saw_git = true;
	}
	CHECK(!saw_git);

	u64 m;
	CHECK(fuzzy_score("mc", 2, "main.c", 6, &m) >= 0);
	CHECK((m & 1) && (m & (1u << 5)));
	CHECK(fuzzy_score("xyz", 3, "main.c", 6, &m) < 0);
	/* word starts beat scattered matches */
	CHECK(fuzzy_score("uh", 2, "util.h", 6, &m) > fuzzy_score("uh", 2, "pushup", 6, &m));
	CHECK(fuzzy_score("util", 4, "util.c", 6, &m) > fuzzy_score("util", 4, "u_t_i_l.c", 9, &m));

	SearchResults r = {0};
	search_text(&r, dir, &fl, "total", 5, 0, 1000);
	CHECK(r.n == 4 * 3);
	CHECK(r.files_hit == 4);
	search_text(&r, dir, &fl, "total", 5, SEARCH_CASE, 1000);
	CHECK(r.n == 4 * 2);
	CHECK(r.line[1] == 1 && r.col[1] == 0);
	search_text(&r, dir, &fl, "tot", 3, SEARCH_WORD, 1000);
	CHECK(r.n == 0);
	search_text(&r, dir, &fl, "total", 5, 0, 5);
	CHECK(r.truncated && r.n == 5);

	const char *h = "abc abc ABC";
	CHECK(find_in(h, 11, "abc", 3, 0, 0, false) == 0);
	CHECK(find_in(h, 11, "abc", 3, 0, 1, false) == 4);
	CHECK(find_in(h, 11, "abc", 3, SEARCH_CASE, 5, false) == 0); /* wraps */
	CHECK(find_in(h, 11, "abc", 3, 0, 5, false) == 8);
	CHECK(find_in(h, 11, "abc", 3, 0, 4, true) == 0);
	CHECK(find_in(h, 11, "ab", 2, SEARCH_WORD, 0, false) == UINT32_MAX);

	files_free(&fl);
	search_free(&r);
	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
