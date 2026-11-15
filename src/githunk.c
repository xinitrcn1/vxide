/* Hunks, word changes, range staging and conflicts on libgit2. See githunk.h. */
#define _POSIX_C_SOURCE 200809L
#include "githunk.h"
#include "gitops.h"

#include <ctype.h>
#include <git2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

static char g_err[1024];

const char *githunk_error(void) { return g_err; }

static int fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_err, sizeof g_err, fmt, ap);
	va_end(ap);
	return -1;
}

static int lgfail(const char *what)
{
	const git_error *e = git_error_last();
	return fail("%s%s%s", what, e && e->message ? ": " : "", e && e->message ? e->message : "");
}

/* ---- repository handle cache (one per work tree, like gitgraph) ---------------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_workdir;
static git_repository *g_repo;

static git_repository *repo_lock(const char *workdir)
{
	gitops_libgit2_init();
	pthread_mutex_lock(&g_mu);
	if (g_repo && g_workdir && !strcmp(g_workdir, workdir))
		return g_repo;
	git_repository_free(g_repo);
	free(g_workdir);
	g_repo = NULL;
	g_workdir = NULL;
	if (git_repository_open_ext(&g_repo, workdir, 0, NULL) != 0) {
		lgfail("cannot open the repository");
		g_repo = NULL;
		pthread_mutex_unlock(&g_mu);
		return NULL;
	}
	g_workdir = vx_strdup(workdir);
	return g_repo;
}

static void repo_unlock(void) { pthread_mutex_unlock(&g_mu); }

/* The repository index, re-read from disk if it changed (cheap stat). */
static int index_of(git_repository *r, git_index **idx)
{
	if (git_repository_index(idx, r) != 0)
		return lgfail("cannot open the index");
	git_index_read(*idx, 0);
	return 0;
}

/* ---- line tables ------------------------------------------------------------------ */

/* Byte offset of each line start, plus a sentinel at n (lines keep their
 * '\n'). A trailing line without '\n' counts as a line. */
static u32 line_starts(const char *s, size_t n, u32 **out)
{
	u32 cnt = 1;
	for (size_t i = 0; i < n; i++)
		cnt += s[i] == '\n';
	if (n && s[n - 1] == '\n')
		cnt--; /* no empty last line after a final '\n' */
	if (!n)
		cnt = 0;
	u32 *ls = vx_alloc((cnt + 1) * sizeof(u32));
	u32 k = 0;
	if (n)
		ls[k++] = 0;
	for (size_t i = 0; i + 1 < n; i++)
		if (s[i] == '\n')
			ls[k++] = (u32)(i + 1);
	ls[cnt] = (u32)n;
	*out = ls;
	return cnt;
}

/* ---- diff --------------------------------------------------------------------------- */

#define HUNK_COLS(h)                                                                                     \
	SOA((h)->old_start), SOA((h)->old_lines), SOA((h)->new_start), SOA((h)->new_lines), SOA((h)->chg_first), \
	    SOA((h)->chg_count)
#define CHG_COLS(h) SOA((h)->c_old_off), SOA((h)->c_old_len), SOA((h)->c_new_off), SOA((h)->c_new_len)
#define CONFLICT_COLS(c) SOA((c)->path), SOA((c)->sides)
#define MARKER_COLS(m)                                                                                        \
	SOA((m)->start), SOA((m)->base), SOA((m)->sep), SOA((m)->end), SOA((m)->ours_label), SOA((m)->base_label), \
	    SOA((m)->theirs_label)

static void hunk_add(GitHunks *h, u32 os, u32 ol, u32 ns, u32 nl)
{
	if (h->n == h->cap) {
		SoaCol c[] = {HUNK_COLS(h)};
		soa_grow(c, 6, &h->cap, h->n + 1, h->n);
	}
	u32 i = h->n++;
	h->old_start[i] = os;
	h->old_lines[i] = ol;
	h->new_start[i] = ns;
	h->new_lines[i] = nl;
	h->chg_first[i] = h->nc;
	h->chg_count[i] = 0;
}

static void chg_add(GitHunks *h, u32 oo, u32 ol, u32 no, u32 nl)
{
	if (h->nc == h->ccap) {
		SoaCol c[] = {CHG_COLS(h)};
		soa_grow(c, 4, &h->ccap, VX_MAX(h->nc + 1, 32u), h->nc);
	}
	u32 i = h->nc++;
	h->c_old_off[i] = oo;
	h->c_old_len[i] = ol;
	h->c_new_off[i] = no;
	h->c_new_len[i] = nl;
	h->chg_count[h->n - 1]++;
}

static int hunk_cb(const git_diff_delta *d, const git_diff_hunk *hk, void *payload)
{
	(void)d;
	GitHunks *h = payload;
	/* unified diff numbers are 1-based; an empty side names the line before */
	u32 os = hk->old_lines ? (u32)hk->old_start - 1 : (u32)hk->old_start;
	u32 ns = hk->new_lines ? (u32)hk->new_start - 1 : (u32)hk->new_start;
	hunk_add(h, os, (u32)hk->old_lines, ns, (u32)hk->new_lines);
	return 0;
}

static int binary_cb(const git_diff_delta *d, const git_diff_binary *b, void *payload)
{
	(void)d;
	(void)b;
	((GitHunks *)payload)->binary = true;
	return 0;
}

/* Tokens: runs of word characters, runs of blanks, single other bytes
 * (UTF-8 sequences kept whole). Writes token start offsets; returns count. */
#define MAX_TOK 512

static u32 tokens(const char *s, u32 n, u32 *st)
{
	u32 k = 0, i = 0;
	while (i < n) {
		if (k == MAX_TOK)
			return MAX_TOK + 1; /* too long: the caller marks the whole line */
		st[k++] = i;
		u8 c = (u8)s[i];
		if (isalnum(c) || c == '_' || c >= 0x80) {
			while (i < n && (isalnum((u8)s[i]) || s[i] == '_' || (u8)s[i] >= 0x80))
				i++;
		} else if (c == ' ' || c == '\t') {
			while (i < n && (s[i] == ' ' || s[i] == '\t'))
				i++;
		} else {
			i++;
		}
	}
	st[k] = n;
	return k;
}

static bool tok_eq(const char *a, const u32 *as, u32 i, const char *b, const u32 *bs, u32 j)
{
	u32 la = as[i + 1] - as[i], lb = bs[j + 1] - bs[j];
	return la == lb && !memcmp(a + as[i], b + bs[j], la);
}

/* Changed byte ranges between two lines by an LCS over tokens. */
static void word_diff(GitHunks *h, const char *a, u32 aoff, u32 an, const char *b, u32 boff, u32 bn)
{
	static u32 as[MAX_TOK + 2], bs[MAX_TOK + 2];
	static u16 dp[(MAX_TOK + 1) * (MAX_TOK + 1)];
	/* strip the line end from both */
	while (an && (a[an - 1] == '\n' || a[an - 1] == '\r'))
		an--;
	while (bn && (b[bn - 1] == '\n' || b[bn - 1] == '\r'))
		bn--;
	u32 na = tokens(a, an, as), nb = tokens(b, bn, bs);
	if (na > MAX_TOK || nb > MAX_TOK) {
		chg_add(h, aoff, an, boff, bn);
		return;
	}
	/* dp[i][j] = LCS of a[i..] and b[j..] */
	u32 W = nb + 1;
	for (u32 i = na + 1; i-- > 0;)
		for (u32 j = nb + 1; j-- > 0;) {
			u16 v;
			if (i == na || j == nb)
				v = 0;
			else if (tok_eq(a, as, i, b, bs, j))
				v = (u16)(dp[(i + 1) * W + j + 1] + 1);
			else
				v = VX_MAX(dp[(i + 1) * W + j], dp[i * W + j + 1]);
			dp[i * W + j] = v;
		}
	/* walk: unmatched tokens accumulate into one change until a match */
	u32 i = 0, j = 0;
	u32 ca = UINT32_MAX, cb = UINT32_MAX;
	while (i < na || j < nb) {
		if (i < na && j < nb && tok_eq(a, as, i, b, bs, j)) {
			if (ca != UINT32_MAX || cb != UINT32_MAX) {
				u32 a0 = ca == UINT32_MAX ? as[i] : as[ca], b0 = cb == UINT32_MAX ? bs[j] : bs[cb];
				chg_add(h, aoff + a0, as[i] - a0, boff + b0, bs[j] - b0);
				ca = cb = UINT32_MAX;
			}
			i++;
			j++;
		} else if (j == nb || (i < na && dp[(i + 1) * W + j] >= dp[i * W + j + 1])) {
			if (ca == UINT32_MAX)
				ca = i;
			i++;
		} else {
			if (cb == UINT32_MAX)
				cb = j;
			j++;
		}
	}
	if (ca != UINT32_MAX || cb != UINT32_MAX) {
		u32 a0 = ca == UINT32_MAX ? as[na] : as[ca], b0 = cb == UINT32_MAX ? bs[nb] : bs[cb];
		chg_add(h, aoff + a0, as[na] - a0, boff + b0, bs[nb] - b0);
	}
}

static int diff_impl(const char *old, size_t no, const char *new_, size_t nn, GitHunks *out, bool words)
{
	memset(out, 0, sizeof *out);
	gitops_libgit2_init();
	git_diff_options o;
	git_diff_options_init(&o, GIT_DIFF_OPTIONS_VERSION);
	o.context_lines = 0;
	o.interhunk_lines = 0;
	if (git_diff_buffers(old, no, "a", new_, nn, "b", &o, NULL, binary_cb, hunk_cb, NULL, out) != 0)
		return lgfail("diff failed");
	if (!words || out->binary)
		return 0;
	u32 *ols, *nls;
	u32 onl = line_starts(old, no, &ols), nnl = line_starts(new_, nn, &nls);
	/* chg ranges were appended per hunk in order: rebuild with word changes */
	out->nc = 0;
	u32 nh = out->n;
	for (u32 k = 0; k < nh; k++) {
		out->chg_first[k] = out->nc;
		out->chg_count[k] = 0;
		u32 pairs = VX_MIN(out->old_lines[k], out->new_lines[k]);
		/* chg_add increments chg_count of the last hunk: emulate with n */
		u32 save_n = out->n;
		out->n = k + 1;
		for (u32 p = 0; p < pairs && p < 256; p++) {
			u32 ol = out->old_start[k] + p, nl = out->new_start[k] + p;
			if (ol >= onl || nl >= nnl)
				break;
			word_diff(out, old + ols[ol], ols[ol], ols[ol + 1] - ols[ol], new_ + nls[nl], nls[nl], nls[nl + 1] - nls[nl]);
		}
		out->n = save_n;
	}
	free(ols);
	free(nls);
	return 0;
}

int githunk_diff(const char *old, size_t no, const char *new_, size_t nn, GitHunks *out)
{
	return diff_impl(old ? old : "", old ? no : 0, new_ ? new_ : "", new_ ? nn : 0, out, true) == 0 ? 0 : -1;
}

void githunk_free(GitHunks *h)
{
	SoaCol hc[] = {HUNK_COLS(h)}, cc[] = {CHG_COLS(h)};
	soa_free(hc, 6);
	soa_free(cc, 4);
	memset(h, 0, sizeof *h);
}

/* ---- sources ---------------------------------------------------------------------- */

static int blob_text(git_repository *r, const git_oid *id, char **text, size_t *n)
{
	git_blob *b = NULL;
	if (git_blob_lookup(&b, r, id) != 0)
		return lgfail("cannot read the blob");
	*n = (size_t)git_blob_rawsize(b);
	*text = vx_strndup(git_blob_rawcontent(b), *n);
	git_blob_free(b);
	return 0;
}

static int read_index_locked(git_repository *r, const char *relpath, char **text, size_t *n, u32 *mode)
{
	git_index *idx = NULL;
	if (index_of(r, &idx) != 0)
		return -1;
	const git_index_entry *e = git_index_get_bypath(idx, relpath, 0);
	int rc;
	if (!e) {
		*text = vx_strdup("");
		*n = 0;
		rc = 1;
	} else {
		if (mode)
			*mode = e->mode;
		rc = blob_text(r, &e->id, text, n);
	}
	git_index_free(idx);
	return rc;
}

static int read_head_locked(git_repository *r, const char *relpath, char **text, size_t *n)
{
	git_object *t = NULL;
	if (git_revparse_single(&t, r, "HEAD^{tree}") != 0) {
		*text = vx_strdup(""); /* unborn HEAD */
		*n = 0;
		return 1;
	}
	git_tree_entry *e = NULL;
	int rc;
	if (git_tree_entry_bypath(&e, (git_tree *)t, relpath) != 0) {
		*text = vx_strdup("");
		*n = 0;
		rc = 1;
	} else {
		rc = blob_text(r, git_tree_entry_id(e), text, n);
	}
	git_tree_entry_free(e);
	git_object_free(t);
	return rc;
}

int githunk_read_index(const char *workdir, const char *relpath, char **text, size_t *n)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	int rc = read_index_locked(r, relpath, text, n, NULL);
	repo_unlock();
	return rc;
}

int githunk_read_head(const char *workdir, const char *relpath, char **text, size_t *n)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	int rc = read_head_locked(r, relpath, text, n);
	repo_unlock();
	return rc;
}

/* ---- composing texts from hunks ----------------------------------------------------- */

/* Does hunk k (b-side lines) touch any range? A pure deletion (no b lines)
 * sits before b line new_start: it touches ranges covering that line or
 * the line just above (its gutter mark). */
static bool touches(const GitHunks *h, u32 k, const u32 *lo, const u32 *hi, u32 nr)
{
	u32 s = h->new_start[k], len = h->new_lines[k];
	for (u32 i = 0; i < nr; i++) {
		if (len) {
			if (s <= hi[i] && s + len - 1 >= lo[i])
				return true;
		} else if ((s >= lo[i] && s <= hi[i] + 1) || (s && s - 1 >= lo[i] && s - 1 <= hi[i])) {
			return true;
		}
	}
	return false;
}

/* Text from hunks a -> b: selected hunks take side `sel_side` (1 = b),
 * the others the other side; unchanged lines are copied from a. */
static char *compose(const char *a, size_t na, const char *b, size_t nb, const GitHunks *h, const u8 *sel,
                     int sel_side, size_t *out_n)
{
	u32 *als, *bls;
	u32 anl = line_starts(a, na, &als);
	u32 bnl = line_starts(b, nb, &bls);
	(void)bnl;
	char *out = vx_alloc(na + nb + 1);
	size_t o = 0;
	u32 al = 0;
	for (u32 k = 0; k < h->n; k++) {
		u32 os = h->old_start[k], ol = h->old_lines[k], ns = h->new_start[k], nl = h->new_lines[k];
		/* unchanged a lines up to the hunk */
		u32 from = als[al], to = als[VX_MIN(os, anl)];
		memcpy(out + o, a + from, to - from);
		o += to - from;
		bool take_b = sel[k] ? sel_side == 1 : sel_side != 1;
		if (take_b) {
			u32 bf = bls[ns], bt = bls[VX_MIN(ns + nl, bnl)];
			memcpy(out + o, b + bf, bt - bf);
			o += bt - bf;
		} else {
			u32 af = als[os], at = als[VX_MIN(os + ol, anl)];
			memcpy(out + o, a + af, at - af);
			o += at - af;
		}
		al = VX_MIN(os + ol, anl);
	}
	memcpy(out + o, a + als[al], na - als[al]);
	o += na - als[al];
	out[o] = 0;
	free(als);
	free(bls);
	*out_n = o;
	return out;
}

/* Write text as the index blob of relpath (stat data zeroed so status
 * rehashes the worktree file). */
static int index_write_blob(git_repository *r, const char *relpath, const char *text, size_t n, u32 mode)
{
	git_oid id;
	if (git_blob_create_from_buffer(&id, r, text, n) != 0)
		return lgfail("cannot write the blob");
	git_index *idx = NULL;
	if (index_of(r, &idx) != 0)
		return -1;
	git_index_entry e;
	memset(&e, 0, sizeof e);
	const git_index_entry *old = git_index_get_bypath(idx, relpath, 0);
	e.mode = old ? old->mode : mode;
	e.path = relpath;
	e.id = id;
	e.file_size = (uint32_t)n;
	int rc = 0;
	if (git_index_add(idx, &e) != 0 || git_index_write(idx) != 0)
		rc = lgfail("cannot update the index");
	git_index_free(idx);
	return rc;
}

static u32 worktree_mode(const char *workdir_path, const char *relpath)
{
	char p[8192];
	size_t wl = strlen(workdir_path);
	snprintf(p, sizeof p, "%s%s%s", workdir_path, wl && workdir_path[wl - 1] == '/' ? "" : "/", relpath);
	struct stat st;
	if (stat(p, &st) == 0 && (st.st_mode & 0111))
		return GIT_FILEMODE_BLOB_EXECUTABLE;
	return GIT_FILEMODE_BLOB;
}

int githunk_stage_ranges(const char *workdir, const char *relpath, const char *wt, size_t n, const u32 *lo,
                         const u32 *hi, u32 nr)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	int rc = 0;
	if (!wt) {
		/* deleted in the worktree: stage the removal */
		git_index *idx = NULL;
		if (index_of(r, &idx) != 0)
			rc = -1;
		else if (git_index_remove_bypath(idx, relpath) != 0 || git_index_write(idx) != 0)
			rc = lgfail("cannot stage the deletion");
		git_index_free(idx);
		repo_unlock();
		return rc;
	}
	char *it = NULL;
	size_t in = 0;
	u32 mode = 0;
	int k = read_index_locked(r, relpath, &it, &in, &mode);
	if (k < 0) {
		repo_unlock();
		return -1;
	}
	GitHunks h;
	if (diff_impl(it, in, wt, n, &h, false) != 0 || h.binary) {
		free(it);
		githunk_free(&h);
		repo_unlock();
		return h.binary ? fail("binary file: stage the whole file") : -1;
	}
	u8 *sel = vx_alloc(h.n + 1);
	u32 any = 0;
	for (u32 i = 0; i < h.n; i++)
		any += sel[i] = touches(&h, i, lo, hi, nr);
	if (!any && k == 0) {
		free(sel);
		free(it);
		githunk_free(&h);
		repo_unlock();
		return 0; /* nothing selected */
	}
	size_t on;
	char *txt = compose(it, in, wt, n, &h, sel, 1, &on);
	rc = index_write_blob(r, relpath, txt, on, k == 1 ? worktree_mode(git_repository_workdir(r), relpath) : mode);
	free(txt);
	free(sel);
	free(it);
	githunk_free(&h);
	repo_unlock();
	return rc;
}

int githunk_unstage_ranges(const char *workdir, const char *relpath, const u32 *lo, const u32 *hi, u32 nr)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	char *ht = NULL, *it = NULL;
	size_t hn = 0, in = 0;
	u32 mode = GIT_FILEMODE_BLOB;
	int kh = read_head_locked(r, relpath, &ht, &hn);
	int ki = read_index_locked(r, relpath, &it, &in, &mode);
	int rc = 0;
	if (kh < 0 || ki < 0) {
		rc = -1;
	} else if (ki == 1) {
		rc = fail("'%s' is not in the index", relpath);
	} else {
		GitHunks h;
		if (diff_impl(ht, hn, it, in, &h, false) != 0 || h.binary) {
			rc = h.binary ? fail("binary file: unstage the whole file") : -1;
		} else {
			u8 *sel = vx_alloc(h.n + 1);
			for (u32 i = 0; i < h.n; i++)
				sel[i] = touches(&h, i, lo, hi, nr);
			size_t on;
			/* selected hunks go back to HEAD (side a), the rest stay */
			char *txt = compose(ht, hn, it, in, &h, sel, 0, &on);
			if (kh == 1 && on == 0) {
				git_index *idx = NULL;
				if (index_of(r, &idx) != 0)
					rc = -1;
				else if (git_index_remove_bypath(idx, relpath) != 0 || git_index_write(idx) != 0)
					rc = lgfail("cannot unstage");
				git_index_free(idx);
			} else {
				rc = index_write_blob(r, relpath, txt, on, mode);
			}
			free(txt);
			free(sel);
		}
		githunk_free(&h);
	}
	free(ht);
	free(it);
	repo_unlock();
	return rc;
}

char *githunk_revert_ranges(const char *workdir, const char *relpath, const char *wt, size_t n, const u32 *lo,
                            const u32 *hi, u32 nr, size_t *out_n)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return NULL;
	char *it = NULL;
	size_t in = 0;
	char *res = NULL;
	if (read_index_locked(r, relpath, &it, &in, NULL) >= 0) {
		GitHunks h;
		if (diff_impl(it, in, wt ? wt : "", wt ? n : 0, &h, false) == 0 && !h.binary) {
			u8 *sel = vx_alloc(h.n + 1);
			for (u32 i = 0; i < h.n; i++)
				sel[i] = touches(&h, i, lo, hi, nr);
			/* selected hunks back to the index (a), the rest keep the worktree */
			res = compose(it, in, wt ? wt : "", wt ? n : 0, &h, sel, 0, out_n);
			free(sel);
		} else if (h.binary) {
			fail("binary file: cannot revert ranges");
		}
		githunk_free(&h);
	}
	free(it);
	repo_unlock();
	return res;
}

/* ---- conflicts ---------------------------------------------------------------------- */

int githunk_conflicts(const char *workdir, GitConflicts *c)
{
	memset(c, 0, sizeof *c);
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	git_index *idx = NULL;
	int rc = 0;
	if (index_of(r, &idx) != 0) {
		repo_unlock();
		return -1;
	}
	git_index_conflict_iterator *it = NULL;
	if (git_index_has_conflicts(idx) && git_index_conflict_iterator_new(&it, idx) == 0) {
		const git_index_entry *a, *o, *t;
		while (git_index_conflict_next(&a, &o, &t, it) == 0) {
			const char *p = o ? o->path : t ? t->path : a->path;
			if (c->n == c->cap) {
				SoaCol cc[] = {CONFLICT_COLS(c)};
				soa_grow(cc, 2, &c->cap, c->n + 1, c->n);
			}
			u32 i = c->n++;
			c->path[i] = pool_add(&c->pool, p, (u32)strlen(p));
			c->sides[i] = (u8)((a ? GC_BASE : 0) | (o ? GC_OURS : 0) | (t ? GC_THEIRS : 0));
		}
		git_index_conflict_iterator_free(it);
	}
	git_index_free(idx);
	repo_unlock();
	return rc;
}

void githunk_conflicts_free(GitConflicts *c)
{
	SoaCol cc[] = {CONFLICT_COLS(c)};
	soa_free(cc, 2);
	pool_free(&c->pool);
	memset(c, 0, sizeof *c);
}

int githunk_conflict_blobs(const char *workdir, const char *relpath, char **base, size_t *nb, char **ours,
                           size_t *no, char **theirs, size_t *nt)
{
	*base = *ours = *theirs = NULL;
	*nb = *no = *nt = 0;
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	git_index *idx = NULL;
	if (index_of(r, &idx) != 0) {
		repo_unlock();
		return -1;
	}
	const git_index_entry *a = NULL, *o = NULL, *t = NULL;
	int rc = 0;
	if (git_index_conflict_get(&a, &o, &t, idx, relpath) != 0) {
		rc = fail("'%s' has no conflict", relpath);
	} else {
		if (a && blob_text(r, &a->id, base, nb) != 0)
			rc = -1;
		if (o && blob_text(r, &o->id, ours, no) != 0)
			rc = -1;
		if (t && blob_text(r, &t->id, theirs, nt) != 0)
			rc = -1;
	}
	git_index_free(idx);
	repo_unlock();
	return rc;
}

int githunk_mark_resolved(const char *workdir, const char *relpath, bool force)
{
	git_repository *r = repo_lock(workdir);
	if (!r)
		return -1;
	const char *wd = git_repository_workdir(r);
	char p[8192];
	snprintf(p, sizeof p, "%s%s", wd ? wd : "", relpath);
	size_t n = 0;
	char *text = vx_read_file(p, &n);
	int rc = 0;
	if (text && !force) {
		GitMarkers m;
		if (githunk_find_markers(text, n, &m)) {
			rc = fail("'%s' still has %u conflict marker region%s (first at line %u)", relpath, m.n,
			          m.n == 1 ? "" : "s", m.start[0] + 1);
		}
		githunk_markers_free(&m);
	}
	if (rc == 0) {
		git_index *idx = NULL;
		if (index_of(r, &idx) != 0) {
			rc = -1;
		} else {
			int k = text ? git_index_add_bypath(idx, relpath) : git_index_remove_bypath(idx, relpath);
			if (k == 0 && !text)
				git_index_conflict_remove(idx, relpath);
			if (k != 0 || git_index_write(idx) != 0)
				rc = lgfail("cannot mark as resolved");
			git_index_free(idx);
		}
	}
	free(text);
	repo_unlock();
	return rc;
}

/* ---- conflict markers --------------------------------------------------------------- */

/* Line kind: marker char c repeated 7 times, then end of line or a space
 * (for <<<, |||, >>>) / only the line end (for ===). */
static bool marker(const char *s, size_t n, char c, bool label, const char **lab, size_t *labn)
{
	if (n < 7)
		return false;
	for (int i = 0; i < 7; i++)
		if (s[i] != c)
			return false;
	size_t e = n;
	while (e > 7 && (s[e - 1] == '\r' || s[e - 1] == '\n'))
		e--;
	if (e == 7) {
		*lab = s + 7;
		*labn = 0;
		return true;
	}
	if (!label || s[7] != ' ')
		return false;
	*lab = s + 8;
	*labn = e - 8;
	return true;
}

static void mk_add(GitMarkers *m, u32 st, u32 base, u32 sep, u32 end, const char *ol, size_t oln, const char *bl,
                   size_t bln, const char *tl, size_t tln)
{
	if (m->n == m->cap) {
		SoaCol c[] = {MARKER_COLS(m)};
		soa_grow(c, 7, &m->cap, m->n + 1, m->n);
	}
	u32 i = m->n++;
	m->start[i] = st;
	m->base[i] = base;
	m->sep[i] = sep;
	m->end[i] = end;
	m->ours_label[i] = pool_add(&m->pool, ol, (u32)oln);
	m->base_label[i] = pool_add(&m->pool, bl ? bl : "", bl ? (u32)bln : 0);
	m->theirs_label[i] = pool_add(&m->pool, tl, (u32)tln);
}

u32 githunk_find_markers(const char *text, size_t n, GitMarkers *m)
{
	memset(m, 0, sizeof *m);
	/* state: 0 outside, 1 in ours, 2 in base, 3 in theirs */
	int state = 0;
	u32 st = 0, base = UINT32_MAX, sep = 0;
	const char *ol = NULL, *bl = NULL;
	size_t oln = 0, bln = 0;
	u32 line = 0;
	size_t i = 0;
	while (i < n) {
		const char *s = text + i;
		const char *e = memchr(s, '\n', n - i);
		size_t len = e ? (size_t)(e - s) + 1 : n - i;
		const char *lab;
		size_t labn;
		if (marker(s, len, '<', true, &lab, &labn)) {
			/* a new region; an unterminated one before it is dropped */
			state = 1;
			st = line;
			base = UINT32_MAX;
			ol = lab;
			oln = labn;
			bl = NULL;
			bln = 0;
		} else if (state == 1 && marker(s, len, '|', true, &lab, &labn)) {
			state = 2;
			base = line;
			bl = lab;
			bln = labn;
		} else if ((state == 1 || state == 2) && marker(s, len, '=', false, &lab, &labn)) {
			state = 3;
			sep = line;
		} else if (state == 3 && marker(s, len, '>', true, &lab, &labn)) {
			mk_add(m, st, base, sep, line, ol, oln, bl, bln, lab, labn);
			state = 0;
		} else if (state && marker(s, len, '>', true, &lab, &labn)) {
			state = 0; /* >>>>>>> without =======: malformed, skipped */
		}
		i += len;
		line++;
	}
	return m->n;
}

void githunk_markers_free(GitMarkers *m)
{
	SoaCol c[] = {MARKER_COLS(m)};
	soa_free(c, 7);
	pool_free(&m->pool);
	memset(m, 0, sizeof *m);
}
