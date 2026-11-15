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
	Matcher mt;
	CHECK(matcher_init(&mt, "total", 5, 0));
	search_text(&r, dir, &fl, &mt, NULL, NULL, 1000);
	CHECK(r.n == 4 * 3);
	CHECK(r.files_hit == 4);
	matcher_free(&mt);
	matcher_init(&mt, "total", 5, SEARCH_CASE);
	search_text(&r, dir, &fl, &mt, NULL, NULL, 1000);
	CHECK(r.n == 4 * 2);
	CHECK(r.line[1] == 1 && r.col[1] == 0);
	matcher_free(&mt);
	matcher_init(&mt, "tot", 3, SEARCH_WORD);
	search_text(&r, dir, &fl, &mt, NULL, NULL, 1000);
	CHECK(r.n == 0);
	matcher_free(&mt);
	matcher_init(&mt, "total", 5, 0);
	search_text(&r, dir, &fl, &mt, NULL, NULL, 5);
	CHECK(r.truncated && r.n == 5);
	/* include / exclude globs */
	search_text(&r, dir, &fl, &mt, "*.c", NULL, 1000);
	CHECK(r.files_hit == 2);
	search_text(&r, dir, &fl, &mt, "src", "util*", 1000);
	CHECK(r.files_hit == 1);
	search_text(&r, dir, &fl, &mt, NULL, "*.md, include/**", 1000);
	CHECK(r.files_hit == 2);
	matcher_free(&mt);
	/* regex: whole word, case, captures, variable length */
	CHECK(matcher_init(&mt, "t(o)tal\\s\\+=", 12, SEARCH_REGEX));
	search_text(&r, dir, &fl, &mt, NULL, NULL, 1000);
	CHECK(r.n == 4 && r.len[0] == 8 && r.line[0] == 1);
	matcher_free(&mt);
	CHECK(matcher_init(&mt, "TOT\\w*", 6, SEARCH_REGEX | SEARCH_WORD));
	search_text(&r, dir, &fl, &mt, NULL, NULL, 1000);
	CHECK(r.n == 12);
	matcher_free(&mt);
	CHECK(!matcher_init(&mt, "(?=x)", 5, SEARCH_REGEX));
	CHECK(mt.err[0] != 0);
	matcher_free(&mt);
	/* replacement expansion */
	{
		const char *hay = "int total = 0;";
		u32 a, b;
		matcher_init(&mt, "(\\w+) (\\w+)", 11, SEARCH_REGEX);
		CHECK(matcher_next(&mt, hay, 14, 0, &a, &b) && a == 0 && b == 9);
		char *out = NULL;
		u32 len = 0, cap = 0;
		matcher_expand(&mt, "$2 $1", 5, hay, &out, &len, &cap);
		CHECK_STR(out, "total int");
		free(out);
		matcher_free(&mt);
	}
	/* glob details */
	CHECK(glob_list_match("*.c", "a/b/c.c"));
	CHECK(!glob_list_match("*.c", "a/b/c.h"));
	CHECK(glob_list_match("./src", "src/x/y.c"));
	CHECK(!glob_list_match("./src", "lib/src/y.c"));
	CHECK(glob_list_match("src", "lib/src/y.c"));
	CHECK(glob_list_match("**/*.{c,h}", "x/y.h"));
	CHECK(glob_list_match("src/**/include", "src/a/b/include/z.h"));
	CHECK(glob_list_match("[ab]*.txt", "bar.txt") && !glob_list_match("[ab]*.txt", "car.txt"));
	CHECK(!glob_list_match("", "x"));

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
