/* Source Control Graph: order, lanes, edges, refs, commit details. */
#define _POSIX_C_SOURCE 200809L
#include "gitgraph.h"
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

static u32 row_of(const GitGraph *g, const char *hash)
{
	for (u32 i = 0; i < g->n; i++)
		if (!strcmp(g->hash[i], hash))
			return i;
	return UINT32_MAX;
}

static void rev(const char *what, char out[41])
{
	char cmd[256];
	snprintf(cmd, sizeof cmd, "cd %s && git rev-parse %s", dir, what);
	FILE *p = popen(cmd, "r");
	out[0] = 0;
	if (p) {
		if (fgets(out, 41, p) == NULL)
			out[0] = 0;
		pclose(p);
	}
	out[40] = 0;
}

static u32 count_of(const char *in, const char *what)
{
	char cmd[256], line[64];
	snprintf(cmd, sizeof cmd, "cd %s && git rev-list --count %s", in, what);
	FILE *p = popen(cmd, "r");
	u32 n = 0;
	if (p && fgets(line, sizeof line, p))
		n = (u32)strtoul(line, NULL, 10);
	if (p)
		pclose(p);
	return n;
}

static void sh_in(const char *in, const char *cmd)
{
	char buf[2048];
	snprintf(buf, sizeof buf, "cd %s && (%s) >/dev/null 2>&1", in, cmd);
	if (system(buf) != 0)
		fprintf(stderr, "command failed: %s\n", cmd);
}

static void rev_in(const char *in, const char *what, char out[41])
{
	char cmd[256];
	snprintf(cmd, sizeof cmd, "cd %s && git rev-parse %s", in, what);
	FILE *p = popen(cmd, "r");
	out[0] = 0;
	if (p) {
		if (fgets(out, 41, p) == NULL)
			out[0] = 0;
		pclose(p);
	}
	out[40] = 0;
}

static bool in_set(const u16 *v, u32 n, u16 x)
{
	for (u32 i = 0; i < n; i++)
		if (v[i] == x)
			return true;
	return false;
}

int main(void)
{
	snprintf(dir, sizeof dir, "/tmp/vxide-graph-XXXXXX");
	CHECK(mkdtemp(dir) != NULL);

	/* empty repository: an empty graph, not an error */
	sh("git init -q -b main && git config user.name Ada && git config user.email ada@x");
	GitGraph g;
	CHECK(git_graph_load(&g, dir, 100) == 0 && g.n == 0);
	git_graph_free(&g);

	sh("printf 'a\\n' > a.txt && git add a.txt && git commit -qm c1");
	sh("printf 'a\\nb\\nc\\n' > a.txt && git commit -qam c2");
	sh("git branch feat HEAD~1 && git switch -q feat && printf 'f\\n' > f.txt && git add f.txt && git commit -qm f1");
	sh("printf 'f\\ng\\n' > f.txt && git commit -qam f2");
	sh("git switch -q main && git branch other && git switch -q other && printf 'o\\n' > o.txt && git add o.txt && git commit -qm o1");
	sh("git switch -q main && git merge -q --no-ff --no-edit feat");
	/* crossing branches + an octopus merge */
	sh("git branch x && git branch y && git switch -q x && printf 'x\\n' > x.txt && git add x.txt && git commit -qm x1");
	sh("git switch -q y && printf 'y\\n' > y.txt && git add y.txt && git commit -qm y1");
	sh("git switch -q main && printf 'm\\n' > m.txt && git add m.txt && git commit -qm m2");
	sh("git merge -q --no-edit x y");
	sh("git tag v1 && git mv a.txt b.txt && printf 'a\\nb\\nc\\nd\\n' > b.txt && git add b.txt && git commit -qm rename");
	sh("printf '\\001\\002\\000\\003' > bin.dat && git add bin.dat && git commit -qm binary");
	/* a fake remote-tracking branch */
	sh("git update-ref refs/remotes/origin/main HEAD~2");

	CHECK(git_graph_load(&g, dir, 1000) == 0);
	/* scope: main has no upstream and is itself the default base, so only
	 * HEAD's history is shown (VSCodium's "auto") */
	CHECK(g.n == count_of(dir, "HEAD") && g.n == 11);
	CHECK_STR(g.pool.b + g.cur_ref, "main");
	CHECK_STR(g.pool.b + g.up_ref, "");
	CHECK_STR(g.pool.b + g.base_ref, "");
	/* topo order: children before parents */
	char h[41];
	rev("HEAD", h);
	CHECK(row_of(&g, h) == 0);
	CHECK(g.flags[0] & GG_HEAD);
	CHECK(strstr(g.pool.b + g.refs[0], "HEAD -> main") != NULL);
	rev("v1", h);
	u32 rv = row_of(&g, h);
	CHECK(rv != UINT32_MAX && strstr(g.pool.b + g.refs[rv], "tag: v1"));
	CHECK(strstr(g.pool.b + g.refs[rv], "origin/main") != NULL);
	CHECK(g.flags[rv] & GG_MERGE); /* the octopus */
	rev("other", h);
	CHECK(row_of(&g, h) == UINT32_MAX); /* an unrelated branch is not shown */
	CHECK(g.flags[g.n - 1] & GG_ROOT);
	CHECK_STR(g.pool.b + g.subject[g.n - 1], "c1");
	CHECK_STR(g.pool.b + g.author[0], "Ada");
	CHECK(g.lanes >= 3);

	/* parent rows come after child rows; edge invariants */
	bool order_ok = true, cont_ok = true, dot_ok = true, colors_ok = true;
	for (u32 i = 0; i < g.n; i++) {
		char cmd[256];
		snprintf(cmd, sizeof cmd, "cd %s && git rev-parse %s^@", dir, g.hash[i]);
		FILE *p = popen(cmd, "r");
		char line[64];
		while (p && fgets(line, sizeof line, p)) {
			line[40] = 0;
			u32 r = row_of(&g, line);
			if (r == UINT32_MAX || r <= i)
				order_ok = false;
		}
		if (p)
			pclose(p);
		u16 up0[64], lo1[64], nup = 0, nlo = 0;
		for (u32 k = g.edge_first[i]; k < g.edge_first[i] + g.edge_count[i]; k++) {
			if (g.e_color[k] > 4)
				colors_ok = false;
			if (g.e_half[k] == 0) {
				/* upper half ends at the dot or passes straight through */
				if (g.e_x1[k] != g.lane[i] && g.e_x1[k] != g.e_x0[k])
					dot_ok = false;
				up0[nup++] = g.e_x0[k];
			} else {
				if (g.e_x0[k] != g.lane[i] && g.e_x0[k] != g.e_x1[k])
					dot_ok = false;
				lo1[nlo++] = g.e_x1[k];
			}
		}
		if (i + 1 < g.n) {
			u16 nup2 = 0, up2[64];
			for (u32 k = g.edge_first[i + 1]; k < g.edge_first[i + 1] + g.edge_count[i + 1]; k++)
				if (g.e_half[k] == 0)
					up2[nup2++] = g.e_x0[k];
			for (u16 a = 0; a < nlo; a++)
				if (!in_set(up2, nup2, lo1[a]))
					cont_ok = false;
			for (u16 a = 0; a < nup2; a++)
				if (!in_set(lo1, nlo, up2[a]))
					cont_ok = false;
		}
		(void)up0;
	}
	CHECK(order_ok);
	CHECK(cont_ok);
	CHECK(dot_ok);
	CHECK(colors_ok);
	/* the octopus has 3 lower edges from its dot */
	{
		u32 n = 0;
		for (u32 k = g.edge_first[rv]; k < g.edge_first[rv] + g.edge_count[rv]; k++)
			if (g.e_half[k] == 1 && g.e_x0[k] == g.lane[rv])
				n++;
		CHECK(n == 3);
	}
	/* limit */
	git_graph_free(&g);
	CHECK(git_graph_load(&g, dir, 3) == 0 && g.n == 3);
	git_graph_free(&g);

	/* details: rename with changes, binary, root, merge */
	GitCommitInfo ci;
	rev("HEAD~1", h);
	CHECK(git_commit_details(dir, h, &ci) == 0);
	CHECK_STR(ci.pool.b + ci.message, "rename");
	CHECK_STR(ci.pool.b + ci.author, "Ada");
	CHECK(ci.nparents == 1 && ci.nfiles == 1);
	CHECK(ci.nfiles == 1 && ci.status[0] == 'R');
	CHECK_STR(ci.pool.b + ci.path[0], "b.txt");
	CHECK_STR(ci.pool.b + ci.old_path[0], "a.txt");
	CHECK(ci.ins[0] == 1 && ci.del[0] == 0);
	git_commit_info_free(&ci);
	rev("HEAD", h);
	CHECK(git_commit_details(dir, h, &ci) == 0);
	CHECK(ci.nfiles == 1 && ci.status[0] == 'A' && ci.ins[0] == -1);
	git_commit_info_free(&ci);
	rev(":/^c2", h);
	CHECK(git_commit_details(dir, h, &ci) == 0);
	CHECK(ci.nfiles == 1 && ci.status[0] == 'M' && ci.ins[0] == 2 && ci.del[0] == 0);
	CHECK(ci.total_ins == 2 && ci.total_del == 0);
	git_commit_info_free(&ci);
	rev(":/^c1", h);
	CHECK(git_commit_details(dir, h, &ci) == 0);
	CHECK(ci.nparents == 0 && ci.nfiles == 1 && ci.status[0] == 'A');
	git_commit_info_free(&ci);
	CHECK(git_commit_details(dir, "--evil", &ci) != 0);

	/* one file's diff */
	rev(":/^c2", h);
	char *d = git_show_file_diff(dir, h, "a.txt");
	CHECK(d && strstr(d, "+b\n") && strstr(d, "+c\n") && strstr(d, "@@"));
	free(d);
	rev(":/^c1", h);
	d = git_show_file_diff(dir, h, "a.txt");
	CHECK(d && strstr(d, "new file mode") && strstr(d, "+a\n"));
	free(d);

	/* a whole commit, git show style */
	rev(":/^c2", h);
	d = git_show_commit(dir, h);
	CHECK(d && !strncmp(d, "commit ", 7) && strstr(d, "Author:     Ada <ada@x>") && strstr(d, "    c2\n") &&
	      strstr(d, "1 file changed, 2 insertions(+)") && strstr(d, "+++ b/a.txt"));
	free(d);
	CHECK(git_show_commit(dir, "--evil") == NULL);

	/* detached HEAD: just HEAD */
	sh("git checkout -q --detach HEAD~1");
	CHECK(git_graph_load(&g, dir, 1000) == 0);
	CHECK(g.n == count_of(dir, "HEAD"));
	CHECK_STR(g.pool.b + g.cur_ref, "");
	CHECK(g.flags[0] & GG_HEAD);
	CHECK(!strcmp(g.pool.b + g.refs[0], "HEAD") || !strncmp(g.pool.b + g.refs[0], "HEAD,", 5));
	git_graph_free(&g);
	sh("git switch -q main");

	/* feature branch: its upstream on a bare remote and its base (main) */
	{
		char repo[96], bare[96], clone[96];
		snprintf(repo, sizeof repo, "%s.feat", dir);
		snprintf(bare, sizeof bare, "%s.bare.git", dir);
		snprintf(clone, sizeof clone, "%s.clone", dir);
		char cmd[1024];
		snprintf(cmd, sizeof cmd,
		         "git init -q -b main %s && git init -q --bare -b main %s && cd %s && git config user.name Ada && "
		         "git config user.email ada@x && printf '1\\n' > a && git add a && git commit -qm c1 && "
		         "git commit -q --allow-empty -m c2 && git remote add origin %s && git push -q -u origin main && "
		         "git switch -q -c feature main && git commit -q --allow-empty -m f1 && git push -q -u origin feature",
		         repo, bare, repo, bare);
		sh_in("/tmp", cmd);
		snprintf(cmd, sizeof cmd,
		         "git clone -q -b feature %s %s && cd %s && git config user.name Bob && git config user.email bob@x && "
		         "git commit -q --allow-empty -m r1 && git push -q origin feature",
		         bare, clone, clone);
		sh_in("/tmp", cmd);
		sh_in(repo, "git fetch -q origin && git switch -q main && git commit -q --allow-empty -m m3 && "
		            "git branch third main~1 && git switch -q third && git commit -q --allow-empty -m t1 && "
		            "git switch -q feature");
		CHECK(git_graph_load(&g, repo, 1000) == 0);
		CHECK_STR(g.pool.b + g.cur_ref, "feature");
		CHECK_STR(g.pool.b + g.up_ref, "origin/feature");
		CHECK_STR(g.pool.b + g.base_ref, "main"); /* reflog: "branch: Created from main" */
		char hm[41], hr[41], hf[41], ht[41];
		rev_in(repo, "main", hm);
		rev_in(repo, "origin/feature", hr);
		rev_in(repo, "feature", hf);
		rev_in(repo, "third", ht);
		CHECK(row_of(&g, hm) != UINT32_MAX); /* the base's newer commit */
		CHECK(row_of(&g, hr) != UINT32_MAX); /* the remote's commit */
		CHECK(row_of(&g, hf) != UINT32_MAX);
		CHECK(row_of(&g, ht) == UINT32_MAX); /* an unrelated third branch is absent */
		u32 rf = row_of(&g, hf);
		CHECK(rf != UINT32_MAX && strstr(g.pool.b + g.refs[rf], "HEAD -> feature"));
		u32 rr = row_of(&g, hr);
		CHECK(rr != UINT32_MAX && strstr(g.pool.b + g.refs[rr], "origin/feature"));
		CHECK(g.n == count_of(repo, "feature origin/feature main"));
		git_graph_free(&g);
		/* VSCodium's merge-base config wins over the reflog */
		sh_in(repo, "git config branch.feature.vscode-merge-base third");
		CHECK(git_graph_load(&g, repo, 1000) == 0);
		CHECK_STR(g.pool.b + g.base_ref, "third");
		CHECK(row_of(&g, ht) != UINT32_MAX && row_of(&g, hm) == UINT32_MAX);
		git_graph_free(&g);
		/* without config or reflog hint: the upstream remote's default branch */
		sh_in(repo, "git config --unset branch.feature.vscode-merge-base && rm -f .git/logs/refs/heads/feature");
		CHECK(git_graph_load(&g, repo, 1000) == 0);
		CHECK_STR(g.pool.b + g.base_ref, "origin/main");
		git_graph_free(&g);
		snprintf(cmd, sizeof cmd, "rm -rf %s %s %s", repo, bare, clone);
		system(cmd);
	}

	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
