/* Diff hunks, word changes, range staging, conflicts. */
#define _POSIX_C_SOURCE 200809L
#include "githunk.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>

static char dir[64];

static void sh(const char *cmd)
{
	char buf[2048];
	snprintf(buf, sizeof buf, "cd %s && (%s) >/dev/null 2>&1", dir, cmd);
	if (system(buf) != 0)
		fprintf(stderr, "command failed: %s\n", cmd);
}

static char *index_text(const char *path)
{
	char *t;
	size_t n;
	if (githunk_read_index(dir, path, &t, &n) < 0)
		return vx_strdup("<error>");
	return t;
}

static void put(const char *path, const char *text)
{
	char p[256];
	snprintf(p, sizeof p, "%s/%s", dir, path);
	vx_write_file(p, text, strlen(text));
}

static char *porcelain(void)
{
	char cmd[256];
	snprintf(cmd, sizeof cmd, "cd %s && git status --porcelain", dir);
	FILE *p = popen(cmd, "r");
	static char buf[4096];
	size_t n = p ? fread(buf, 1, sizeof buf - 1, p) : 0;
	buf[n] = 0;
	if (p)
		pclose(p);
	return buf;
}

int main(void)
{
	/* ---- pure diff + word changes ---- */
	{
		const char *a = "one\ntwo words here\nthree\nfour\n";
		const char *b = "one\ntwo WORDS here!\nthree\nfour\nfive\n";
		GitHunks h;
		CHECK(githunk_diff(a, strlen(a), b, strlen(b), &h) == 0);
		CHECK(h.n == 2);
		CHECK(h.old_start[0] == 1 && h.old_lines[0] == 1 && h.new_start[0] == 1 && h.new_lines[0] == 1);
		CHECK(h.old_start[1] == 4 && h.old_lines[1] == 0 && h.new_start[1] == 4 && h.new_lines[1] == 1);
		/* "words" -> "WORDS" and "" -> "!" */
		CHECK(h.chg_count[0] == 2);
		u32 c = h.chg_first[0];
		CHECK(!strncmp(a + h.c_old_off[c], "words", h.c_old_len[c]) && h.c_old_len[c] == 5);
		CHECK(!strncmp(b + h.c_new_off[c], "WORDS", h.c_new_len[c]) && h.c_new_len[c] == 5);
		CHECK(h.c_old_len[c + 1] == 0 && h.c_new_len[c + 1] == 1 && b[h.c_new_off[c + 1]] == '!');
		CHECK(h.chg_count[1] == 0);
		githunk_free(&h);
		/* deletion position, binary */
		const char *d = "a\nb\nc\n", *e = "a\nc\n";
		CHECK(githunk_diff(d, strlen(d), e, strlen(e), &h) == 0 && h.n == 1);
		CHECK(h.old_start[0] == 1 && h.old_lines[0] == 1 && h.new_start[0] == 1 && h.new_lines[0] == 0);
		githunk_free(&h);
		CHECK(githunk_diff("a\0b", 3, "a\0c", 3, &h) == 0 && h.binary && h.n == 0);
		githunk_free(&h);
		CHECK(githunk_diff(NULL, 0, "x\n", 2, &h) == 0 && h.n == 1 && h.old_lines[0] == 0 && h.new_lines[0] == 1);
		githunk_free(&h);
	}

	snprintf(dir, sizeof dir, "/tmp/vxide-hunk-XXXXXX");
	CHECK(mkdtemp(dir) != NULL);
	sh("git init -q -b main && git config user.name Ada && git config user.email ada@x");
	put("f.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
	sh("git add f.txt && git commit -qm init");

	/* ---- stage only the second of two separated hunks ---- */
	const char *wt = "1\nTWO\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n";
	put("f.txt", wt);
	u32 lo = 7, hi = 7;
	CHECK(githunk_stage_ranges(dir, "f.txt", wt, strlen(wt), &lo, &hi, 1) == 0);
	char *t = index_text("f.txt");
	CHECK_STR(t, "1\n2\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n");
	free(t);
	CHECK(strstr(porcelain(), "MM f.txt") != NULL);
	/* the index text is right for git itself too */
	sh("git diff --cached --quiet; test $? -eq 1");

	/* ---- a range covering part of a hunk stages the whole hunk ---- */
	sh("git reset -q");
	const char *wt2 = "1\nA\nB\nC\n5\n6\n7\n8\n9\n10\n"; /* lines 1-3 replace 2,3,4 */
	put("f.txt", wt2);
	lo = hi = 2;
	CHECK(githunk_stage_ranges(dir, "f.txt", wt2, strlen(wt2), &lo, &hi, 1) == 0);
	t = index_text("f.txt");
	CHECK_STR(t, wt2);
	free(t);

	/* ---- unstage a range (index lines) ---- */
	sh("git reset -q");
	const char *wt3 = "1\nTWO\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n";
	put("f.txt", wt3);
	sh("git add f.txt");
	lo = hi = 1; /* unstage the first change only */
	CHECK(githunk_unstage_ranges(dir, "f.txt", &lo, &hi, 1) == 0);
	t = index_text("f.txt");
	CHECK_STR(t, "1\n2\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n");
	free(t);

	/* ---- revert a range of the worktree against the index ---- */
	{
		/* index has EIGHT; worktree has TWO + EIGHT + an added line */
		const char *w = "1\nTWO\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n11\n";
		u32 los[2] = {1, 10}, his[2] = {1, 10};
		size_t on = 0;
		char *r = githunk_revert_ranges(dir, "f.txt", w, strlen(w), los, his, 2, &on);
		CHECK(r && !strcmp(r, "1\n2\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n") && on == strlen(r));
		free(r);
		lo = hi = 10;
		r = githunk_revert_ranges(dir, "f.txt", w, strlen(w), &lo, &hi, 1, &on);
		CHECK(r && !strcmp(r, "1\nTWO\n3\n4\n5\n6\n7\nEIGHT\n9\n10\n"));
		free(r);
	}

	/* ---- new file: range staging adds it ---- */
	{
		const char *nf = "alpha\nbeta\ngamma\n";
		put("new.txt", nf);
		char *x;
		size_t xn;
		CHECK(githunk_read_index(dir, "new.txt", &x, &xn) == 1 && xn == 0);
		free(x);
		lo = 0;
		hi = 0;
		CHECK(githunk_stage_ranges(dir, "new.txt", nf, strlen(nf), &lo, &hi, 1) == 0);
		t = index_text("new.txt");
		CHECK_STR(t, nf); /* one insertion hunk: staged whole */
		free(t);
		CHECK(strstr(porcelain(), "A  new.txt") != NULL);
		/* unstaging all of it removes it from the index again */
		lo = 0;
		hi = 2;
		CHECK(githunk_unstage_ranges(dir, "new.txt", &lo, &hi, 1) == 0);
		CHECK(strstr(porcelain(), "?? new.txt") != NULL);
		/* deletion staged */
		sh("git add new.txt && git commit -qm new && rm new.txt");
		CHECK(githunk_stage_ranges(dir, "new.txt", NULL, 0, NULL, NULL, 0) == 0);
		CHECK(strstr(porcelain(), "D  new.txt") != NULL);
		sh("git commit -qm gone");
		char *hd;
		size_t hn;
		CHECK(githunk_read_head(dir, "f.txt", &hd, &hn) == 0 && strstr(hd, "EIGHT") && hn == strlen(hd)); /* the staged change was committed */
		free(hd);
	}

	/* ---- conflicts from a real merge ---- */
	sh("git reset -q --hard && git checkout -q -- . ");
	put("c.txt", "top\nmiddle\nbottom\n");
	sh("git add c.txt && git commit -qm base && git switch -q -c other");
	put("c.txt", "top\nTHEIRS\nbottom\n");
	sh("git commit -qam theirs && git switch -q main");
	put("c.txt", "top\nOURS\nbottom\n");
	sh("git commit -qam ours && git merge -q other; true");
	GitConflicts cf;
	CHECK(githunk_conflicts(dir, &cf) == 0 && cf.n == 1);
	CHECK_STR(cf.pool.b + cf.path[0], "c.txt");
	CHECK(cf.sides[0] == (GC_BASE | GC_OURS | GC_THEIRS));
	githunk_conflicts_free(&cf);
	{
		char *b, *o, *th;
		size_t nb, no, nt;
		CHECK(githunk_conflict_blobs(dir, "c.txt", &b, &nb, &o, &no, &th, &nt) == 0);
		CHECK(b && o && th && strstr(b, "middle") && strstr(o, "OURS") && strstr(th, "THEIRS"));
		free(b);
		free(o);
		free(th);
	}
	{
		char p[256];
		snprintf(p, sizeof p, "%s/c.txt", dir);
		size_t n;
		char *txt = vx_read_file(p, &n);
		GitMarkers m;
		CHECK(githunk_find_markers(txt, n, &m) == 1);
		CHECK(m.start[0] == 1 && m.base[0] == UINT32_MAX && m.sep[0] == 3 && m.end[0] == 5);
		CHECK_STR(m.pool.b + m.ours_label[0], "HEAD");
		CHECK_STR(m.pool.b + m.theirs_label[0], "other");
		githunk_markers_free(&m);
		free(txt);
	}
	CHECK(githunk_mark_resolved(dir, "c.txt", false) != 0);
	CHECK(strstr(githunk_error(), "conflict marker") != NULL);
	put("c.txt", "top\nOURS and THEIRS\nbottom\n");
	CHECK(githunk_mark_resolved(dir, "c.txt", false) == 0);
	CHECK(githunk_conflicts(dir, &cf) == 0 && cf.n == 0);
	githunk_conflicts_free(&cf);
	CHECK(strstr(porcelain(), "M  c.txt") != NULL);
	CHECK(githunk_conflict_blobs(dir, "c.txt", &(char *){0}, &(size_t){0}, &(char *){0}, &(size_t){0},
	                             &(char *){0}, &(size_t){0}) != 0);

	/* ---- markers: diff3 style, CRLF, junk ---- */
	{
		const char *d3 = "a\r\n<<<<<<< ours\r\nx\r\n||||||| base\r\ny\r\n=======\r\nz\r\n>>>>>>> theirs\r\nb\r\n"
		                 "<<<<<<< dangling\nq\n"                  /* restarted by the next <<<<<<< */
		                 "<<<<<<<\nm\n=======\nn\n>>>>>>>\n"      /* labels absent */
		                 "<<<<<<< bad\nk\n>>>>>>> bad\n"          /* no separator: skipped */
		                 "======= not a marker\n<<<<<<<< eight\n" /* not markers */
		                 "<<<<<<< open\nu\n=======\nv\n";         /* unterminated */
		GitMarkers m;
		CHECK(githunk_find_markers(d3, strlen(d3), &m) == 2);
		CHECK(m.start[0] == 1 && m.base[0] == 3 && m.sep[0] == 5 && m.end[0] == 7);
		CHECK_STR(m.pool.b + m.ours_label[0], "ours");
		CHECK_STR(m.pool.b + m.base_label[0], "base");
		CHECK_STR(m.pool.b + m.theirs_label[0], "theirs");
		CHECK(m.start[1] == 11 && m.sep[1] == 13 && m.end[1] == 15);
		CHECK_STR(m.pool.b + m.ours_label[1], "");
		githunk_markers_free(&m);
	}

	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
