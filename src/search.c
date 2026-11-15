#define _DEFAULT_SOURCE /* d_type, d_ino, posix_fadvise */
#include "search.h"
#include "fstree.h"

#include <ctype.h>
#include <stdatomic.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

bool is_word_byte(u8 c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* search.exclude defaults on top of files.exclude. */
static bool search_excluded(const char *name)
{
	return fs_excluded(name) || strcmp(name, "node_modules") == 0 ||
	       strcmp(name, "bower_components") == 0;
}

static void fl_add(FileList *fl, const char *rel, u32 n, u64 ino)
{
	if (fl->n == fl->cap) {
		u32 c = fl->cap ? fl->cap * 2 : 1024;
		fl->path = vx_realloc(fl->path, c * sizeof(u32));
		fl->base = vx_realloc(fl->base, c * sizeof(u32));
		fl->ino = vx_realloc(fl->ino, c * sizeof(u64));
		fl->cap = c;
	}
	fl->ino[fl->n] = ino;
	const char *slash = strrchr(rel, '/');
	fl->base[fl->n] = slash ? (u32)(slash - rel + 1) : 0;
	fl->path[fl->n] = pool_add(&fl->pool, rel, n);
	fl->n++;
}

static void walk(FileList *fl, const char *root, char *rel, u32 rl, SkipFn skip, void *ctx,
                 u32 limit, int depth)
{
	char path[8192];
	snprintf(path, sizeof path, "%s/%s", root, rel);
	DIR *d = opendir(path);
	if (!d || depth > 64) {
		if (d)
			closedir(d);
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL && fl->n < limit) {
		const char *nm = de->d_name;
		if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
			continue;
		if (search_excluded(nm))
			continue;
		u32 nl = (u32)strlen(nm);
		if (rl + nl + 2 >= 4096)
			continue;
		u32 l2 = rl;
		if (rl)
			rel[l2++] = '/';
		memcpy(rel + l2, nm, nl + 1);
		l2 += nl;
		/* d_type: no inode reads (seeks on a slow disk) for the common case */
		bool dir = de->d_type == DT_DIR;
		if (de->d_type == DT_UNKNOWN) {
			char full[8192];
			snprintf(full, sizeof full, "%s/%s", root, rel);
			struct stat st;
			if (lstat(full, &st) == 0)
				dir = S_ISDIR(st.st_mode); /* do not follow symlinked dirs */
		} else if (!dir && de->d_type != DT_REG && de->d_type != DT_LNK) {
			rel[rl] = 0; /* sockets, fifos, devices */
			continue;
		}
		if (!(skip && skip(ctx, rel, dir))) {
			if (dir)
				walk(fl, root, rel, l2, skip, ctx, limit, depth + 1);
			else
				fl_add(fl, rel, l2, (u64)de->d_ino);
		}
		rel[rl] = 0;
	}
	closedir(d);
}

void files_scan(FileList *fl, const char *root, SkipFn skip, void *ctx, u32 limit)
{
	fl->n = 0;
	pool_clear(&fl->pool);
	char rel[4096] = "";
	walk(fl, root, rel, 0, skip, ctx, limit, 0);
}

void files_free(FileList *fl)
{
	free(fl->path);
	free(fl->base);
	free(fl->ino);
	pool_free(&fl->pool);
	memset(fl, 0, sizeof *fl);
}

int fuzzy_score(const char *pat, size_t np, const char *s, size_t ns, u64 *mask)
{
	*mask = 0;
	if (np == 0)
		return 0;
	/* Greedy from the right end of the string is a decent heuristic for
	 * paths: prefer matches in the basename. First check the subsequence
	 * exists, then score a left-to-right greedy match that prefers word
	 * starts. */
	size_t i = 0, j = 0;
	int score = 0, run = 0;
	size_t last = (size_t)-1;
	while (i < np && j < ns) {
		u8 pc = (u8)tolower((u8)pat[i]);
		/* look ahead for a word-start occurrence of pc within a window */
		size_t pick = (size_t)-1;
		for (size_t k = j; k < ns; k++) {
			if ((u8)tolower((u8)s[k]) != pc)
				continue;
			bool start = k == 0 || s[k - 1] == '/' || s[k - 1] == '_' || s[k - 1] == '-' ||
			             s[k - 1] == '.' || s[k - 1] == ' ' ||
			             (isupper((u8)s[k]) && islower((u8)s[k - 1]));
			if (pick == (size_t)-1)
				pick = k;
			if (k == last + 1) {
				pick = k;
				break;
			}
			if (start) {
				pick = k;
				break;
			}
		}
		if (pick == (size_t)-1)
			return -1;
		bool start = pick == 0 || !isalnum((u8)s[pick - 1]) ||
		             (isupper((u8)s[pick]) && islower((u8)s[pick - 1]));
		if (pick == last + 1) {
			run++;
			score += 5 + run * 2;
		} else {
			run = 0;
			score += 1;
			score -= (int)VX_MIN(pick - (last == (size_t)-1 ? 0 : last + 1), (size_t)10) / 3;
		}
		if (start)
			score += 8;
		if (pat[i] == s[pick])
			score += 1;
		if (pick < 64)
			*mask |= 1ull << pick;
		last = pick;
		i++;
		j = pick + 1;
	}
	if (i < np)
		return -1;
	return score;
}

static bool match_at(const char *h, const char *q, u32 nq, bool fold)
{
	if (!fold)
		return memcmp(h, q, nq) == 0;
	for (u32 i = 0; i < nq; i++)
		if (tolower((u8)h[i]) != tolower((u8)q[i]))
			return false;
	return true;
}

u32 find_in(const char *hay, u32 n, const char *q, u32 nq, int flags, u32 from, bool back)
{
	if (!nq || nq > n)
		return UINT32_MAX;
	bool fold = !(flags & SEARCH_CASE);
	bool word = flags & SEARCH_WORD;
	u32 last = n - nq;
	for (u32 step = 0; step <= last; step++) {
		u32 i;
		if (back)
			i = (u32)(((i64)from - 1 - step + 2 * (i64)(last + 1)) % (last + 1));
		else
			i = (from + step) % (last + 1);
		if (!match_at(hay + i, q, nq, fold))
			continue;
		if (word && ((i > 0 && is_word_byte((u8)hay[i - 1])) ||
		             (i + nq < n && is_word_byte((u8)hay[i + nq]))))
			continue;
		return i;
	}
	return UINT32_MAX;
}

static void res_add(SearchResults *r, u32 file, u32 line, u32 col, u32 len, const char *ls, u32 ll)
{
	if (r->n == r->cap) {
		u32 c = r->cap ? r->cap * 2 : 256;
		r->file = vx_realloc(r->file, c * sizeof(u32));
		r->line = vx_realloc(r->line, c * sizeof(u32));
		r->col = vx_realloc(r->col, c * sizeof(u32));
		r->len = vx_realloc(r->len, c * sizeof(u32));
		r->text = vx_realloc(r->text, c * sizeof(u32));
		r->text_col = vx_realloc(r->text_col, c * sizeof(u32));
		r->cap = c;
	}
	/* Preview: trim leading whitespace, keep some context before the match. */
	u32 s = 0;
	while (s < col && (ls[s] == ' ' || ls[s] == '\t'))
		s++;
	if (col > s + 40)
		s = col - 30;
	while (s > 0 && s < ll && utf8_cont((u8)ls[s]))
		s--;
	u32 e = VX_MIN(ll, s + 200);
	u32 i = r->n++;
	r->file[i] = file;
	r->line[i] = line;
	r->col[i] = col;
	r->len[i] = len;
	r->text[i] = pool_add(&r->pool, ls + s, e - s);
	r->text_col[i] = col - s;
}

/* ---- matcher ------------------------------------------------------------------ */

bool matcher_init(Matcher *m, const char *q, size_t nq, int flags)
{
	memset(m, 0, sizeof *m);
	m->flags = flags;
	m->q = vx_strndup(q, nq);
	m->nq = (u32)nq;
	if (!nq)
		return false;
	if (flags & SEARCH_REGEX) {
		const char *pat = q;
		size_t np = nq;
		char *wrapped = NULL;
		if (flags & SEARCH_WORD) {
			wrapped = vx_alloc(nq + 16);
			np = (size_t)snprintf(wrapped, nq + 16, "\\b(?:%.*s)\\b", (int)nq, q);
			pat = wrapped;
		}
		m->re = re_compile(pat, np, (flags & SEARCH_CASE) ? 0 : RE_ICASE, m->err, sizeof m->err);
		free(wrapped);
		if (!m->re)
			return false;
	}
	return true;
}

void matcher_free(Matcher *m)
{
	free(m->q);
	re_free(m->re);
	memset(m, 0, sizeof *m);
}

bool matcher_next(Matcher *m, const char *hay, u32 n, u32 from, u32 *a, u32 *b)
{
	if (!m->nq || from > n)
		return false;
	if (m->flags & SEARCH_REGEX) {
		if (!m->re || !re_search(m->re, hay, n, from, m->caps))
			return false;
		*a = m->caps[0];
		*b = m->caps[1];
		return true;
	}
	if (m->nq > n - from)
		return false;
	u32 x = find_in(hay + from, n - from, m->q, m->nq, m->flags, 0, false);
	if (x == UINT32_MAX)
		return false;
	/* find_in scans from 0 without wrapping when from = 0 */
	*a = from + x;
	*b = *a + m->nq;
	m->caps[0] = *a;
	m->caps[1] = *b;
	return true;
}

void matcher_expand(Matcher *m, const char *repl, size_t nr, const char *hay, char **out, u32 *len, u32 *cap)
{
	if (m->re) {
		re_expand(m->re, repl, nr, hay, m->caps, out, len, cap);
		return;
	}
	VX_GROW(*out, *cap, *len + (u32)nr + 1);
	memcpy(*out + *len, repl, nr);
	*len += (u32)nr;
	(*out)[*len] = 0;
}

/* ---- globs ---------------------------------------------------------------------- */

/* Glob with *, **, ?, [...], {a,b}; '/' separates path segments. */
bool glob_match(const char *p, size_t np, const char *s, size_t ns)
{
	size_t pi = 0, si = 0;
	while (pi < np) {
		char c = p[pi];
		if (c == '*') {
			bool dbl = pi + 1 < np && p[pi + 1] == '*';
			if (dbl) {
				/* "**" (optionally followed by '/') crosses segments */
				size_t rest = pi + 2;
				if (rest < np && p[rest] == '/')
					rest++;
				for (size_t k = si; k <= ns; k++)
					if ((k == si || s[k - 1] == '/' || rest >= np) && glob_match(p + rest, np - rest, s + k, ns - k))
						return true;
				return false;
			}
			for (size_t k = si; k <= ns; k++) {
				if (glob_match(p + pi + 1, np - pi - 1, s + k, ns - k))
					return true;
				if (k < ns && s[k] == '/')
					break;
			}
			return false;
		}
		if (c == '{') {
			/* alternatives: try each with the rest of the pattern */
			size_t depth = 0, end = pi;
			for (; end < np; end++) {
				if (p[end] == '{')
					depth++;
				else if (p[end] == '}' && --depth == 0)
					break;
			}
			if (end >= np)
				return false;
			size_t st = pi + 1;
			for (size_t k = pi + 1; k <= end; k++) {
				if (k == end || (p[k] == ',' )) {
					size_t alen = k - st, rlen = np - end - 1;
					char *tmp = vx_alloc(alen + rlen + 1);
					memcpy(tmp, p + st, alen);
					memcpy(tmp + alen, p + end + 1, rlen);
					bool ok = glob_match(tmp, alen + rlen, s + si, ns - si);
					free(tmp);
					if (ok)
						return true;
					st = k + 1;
				}
			}
			return false;
		}
		if (si >= ns)
			return false;
		if (c == '?') {
			if (s[si] == '/')
				return false;
			pi++;
			si++;
			continue;
		}
		if (c == '[') {
			size_t k = pi + 1;
			bool neg = k < np && (p[k] == '!' || p[k] == '^');
			if (neg)
				k++;
			bool in = false;
			for (; k < np && p[k] != ']'; k++) {
				if (k + 2 < np && p[k + 1] == '-' && p[k + 2] != ']') {
					if (s[si] >= p[k] && s[si] <= p[k + 2])
						in = true;
					k += 2;
				} else if (p[k] == s[si]) {
					in = true;
				}
			}
			if (k >= np || in == neg || s[si] == '/')
				return false;
			pi = k + 1;
			si++;
			continue;
		}
		if (c != s[si])
			return false;
		pi++;
		si++;
	}
	return si == ns;
}

bool glob_list_match(const char *list, const char *rel)
{
	if (!list)
		return false;
	size_t ns = strlen(rel);
	const char *p = list;
	while (*p) {
		/* split on commas outside braces */
		const char *e = p;
		int depth = 0;
		while (*e && !(*e == ',' && depth == 0)) {
			if (*e == '{')
				depth++;
			else if (*e == '}')
				depth--;
			e++;
		}
		const char *a = p, *b = e;
		while (a < b && (*a == ' ' || *a == '\t'))
			a++;
		while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
			b--;
		if (b > a) {
			char pat[1024];
			size_t n;
			if (b - a >= 2 && a[0] == '.' && a[1] == '/')
				n = (size_t)snprintf(pat, sizeof pat, "%.*s", (int)(b - a - 2), a + 2);
			else if (a[0] == '/')
				n = (size_t)snprintf(pat, sizeof pat, "%.*s", (int)(b - a - 1), a + 1);
			else if (b - a >= 3 && !strncmp(a, "**/", 3))
				n = (size_t)snprintf(pat, sizeof pat, "%.*s", (int)(b - a), a);
			else
				n = (size_t)snprintf(pat, sizeof pat, "**/%.*s", (int)(b - a), a);
			n = VX_MIN(n, sizeof pat - 4);
			while (n && pat[n - 1] == '/')
				n--;
			if (glob_match(pat, n, rel, ns))
				return true;
			/* a folder pattern covers its contents */
			memcpy(pat + n, "/**", 4);
			if (glob_match(pat, n + 3, rel, ns))
				return true;
		}
		p = *e ? e + 1 : e;
	}
	return false;
}

/* ---- workspace search ------------------------------------------------------------- */

static const FileList *g_sort_fl;
static int ino_cmp(const void *a, const void *b)
{
	u64 x = g_sort_fl->ino[*(const u32 *)a], y = g_sort_fl->ino[*(const u32 *)b];
	return x < y ? -1 : x > y;
}

static const SearchResults *g_sort_r;
static int res_cmp(const void *a, const void *b)
{
	u32 i = *(const u32 *)a, j = *(const u32 *)b;
	const SearchResults *r = g_sort_r;
	if (r->file[i] != r->file[j]) {
		int c = strcmp(g_sort_fl->pool.b + g_sort_fl->path[r->file[i]], g_sort_fl->pool.b + g_sort_fl->path[r->file[j]]);
		if (c)
			return c;
	}
	if (r->line[i] != r->line[j])
		return r->line[i] < r->line[j] ? -1 : 1;
	return r->col[i] < r->col[j] ? -1 : r->col[i] > r->col[j];
}

static void permute_u32(u32 *a, const u32 *perm, u32 n)
{
	u32 *t = vx_alloc((n + 1) * sizeof(u32));
	for (u32 i = 0; i < n; i++)
		t[i] = a[perm[i]];
	memcpy(a, t, n * sizeof(u32));
	free(t);
}

void search_text_ex(SearchResults *r, const char *root, const FileList *fl, Matcher *m, const char *include,
                    const char *exclude, u32 limit, const _Atomic int *cancel, _Atomic u32 *done)
{
	r->n = 0;
	r->files_hit = 0;
	r->truncated = false;
	pool_clear(&r->pool);
	if (!m->nq || ((m->flags & SEARCH_REGEX) && !m->re))
		return;
	bool inc = include && *include, exc = exclude && *exclude;
	/* Visit files in inode order: inodes are allocated close to their data
	 * on UFS and roughly in creation order on ZFS, so a slow disk seeks
	 * forward instead of back and forth. */
	u32 *order = vx_alloc((fl->n + 1) * sizeof(u32));
	u32 no = 0;
	for (u32 f = 0; f < fl->n; f++) {
		const char *rel = fl->pool.b + fl->path[f];
		if (inc && !glob_list_match(include, rel))
			continue;
		if (exc && glob_list_match(exclude, rel))
			continue;
		order[no++] = f;
	}
	if (fl->ino) {
		g_sort_fl = fl;
		qsort(order, no, sizeof(u32), ino_cmp);
	}
	char *buf = NULL;
	u32 cap = 0;
	for (u32 oi = 0; oi < no && !r->truncated; oi++) {
		if (cancel && atomic_load(cancel))
			break;
		if (done)
			atomic_store(done, oi);
		u32 f = order[oi];
		char path[8192];
		snprintf(path, sizeof path, "%s/%s", root, fl->pool.b + fl->path[f]);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
#ifdef POSIX_FADV_SEQUENTIAL
		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
		/* read() in big chunks: one sequential request per file rather than
		 * a page fault per 4 KiB as with mmap */
		u32 n = 0;
		bool binary = false, too_big = false;
		for (;;) {
			VX_GROW(buf, cap, n + 262144);
			ssize_t k = read(fd, buf + n, cap - n);
			if (k <= 0)
				break;
			if (n < 8192 && memchr(buf + n, 0, (size_t)VX_MIN(k, 8192 - (ssize_t)n))) {
				binary = true; /* NUL in the first 8 KiB */
				break;
			}
			n += (u32)k;
			if (n > (64u << 20)) {
				too_big = true;
				break;
			}
		}
		close(fd);
		if (binary || too_big || !n)
			continue;
		const char *b = buf;
		u32 hits = r->n;
		u32 line = 0, ls = 0, scanned = 0;
		u32 pos = 0;
		u32 ma, mb;
		while (pos <= n && matcher_next(m, b, n, pos, &ma, &mb)) {
			if (mb == ma) {
				/* ripgrep-style: empty matches are not results */
				pos = ma + 1;
				while (pos < n && utf8_cont((u8)b[pos]))
					pos++;
				continue;
			}
			for (u32 k = scanned; k < ma; k++)
				if (b[k] == '\n') {
					line++;
					ls = k + 1;
				}
			scanned = ma;
			u32 le = ls;
			while (le < n && b[le] != '\n')
				le++;
			res_add(r, f, line, ma - ls, mb - ma, b + ls, le - ls);
			if (r->n >= limit) {
				r->truncated = true;
				break;
			}
			pos = mb;
		}
		if (r->n > hits)
			r->files_hit++;
	}
	free(buf);
	free(order);
	/* present the results in path order, like VSCodium */
	if (r->n > 1) {
		u32 *perm = vx_alloc(r->n * sizeof(u32));
		for (u32 i = 0; i < r->n; i++)
			perm[i] = i;
		g_sort_fl = fl;
		g_sort_r = r;
		qsort(perm, r->n, sizeof(u32), res_cmp);
		permute_u32(r->file, perm, r->n);
		permute_u32(r->line, perm, r->n);
		permute_u32(r->col, perm, r->n);
		permute_u32(r->len, perm, r->n);
		permute_u32(r->text, perm, r->n);
		permute_u32(r->text_col, perm, r->n);
		free(perm);
	}
}

void search_text(SearchResults *r, const char *root, const FileList *fl, Matcher *m, const char *include,
                 const char *exclude, u32 limit)
{
	search_text_ex(r, root, fl, m, include, exclude, limit, NULL, NULL);
}

void search_free(SearchResults *r)
{
	free(r->file);
	free(r->line);
	free(r->col);
	free(r->len);
	free(r->text);
	free(r->text_col);
	pool_free(&r->pool);
	memset(r, 0, sizeof *r);
}
