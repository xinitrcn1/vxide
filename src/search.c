#define _POSIX_C_SOURCE 200809L
#include "search.h"
#include "fstree.h"

#include <ctype.h>
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

static void fl_add(FileList *fl, const char *rel, u32 n)
{
	if (fl->n == fl->cap) {
		u32 c = fl->cap ? fl->cap * 2 : 1024;
		fl->path = vx_realloc(fl->path, c * sizeof(u32));
		fl->base = vx_realloc(fl->base, c * sizeof(u32));
		fl->cap = c;
	}
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
		char full[8192];
		snprintf(full, sizeof full, "%s/%s", root, rel);
		struct stat st;
		bool dir = false;
		if (lstat(full, &st) == 0) {
			dir = S_ISDIR(st.st_mode); /* do not follow symlinked dirs */
			if (!dir && !S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
				rel[rl] = 0;
				continue;
			}
		}
		if (!(skip && skip(ctx, rel, dir))) {
			if (dir)
				walk(fl, root, rel, l2, skip, ctx, limit, depth + 1);
			else
				fl_add(fl, rel, l2);
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

void search_text(SearchResults *r, const char *root, const FileList *fl, const char *q,
                 size_t nq, int flags, u32 limit)
{
	r->n = 0;
	r->files_hit = 0;
	r->truncated = false;
	pool_clear(&r->pool);
	if (!nq)
		return;
	for (u32 f = 0; f < fl->n && !r->truncated; f++) {
		char path[8192];
		snprintf(path, sizeof path, "%s/%s", root, fl->pool.b + fl->path[f]);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		struct stat st;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0 ||
		    st.st_size > (64 << 20)) {
			close(fd);
			continue;
		}
		u32 n = (u32)st.st_size;
		const char *b = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (b == MAP_FAILED)
			continue;
		/* Skip binaries: NUL in the first 8 KiB. */
		if (memchr(b, 0, VX_MIN(n, 8192u))) {
			munmap((void *)b, n);
			continue;
		}
		u32 hits = r->n;
		u32 line = 0, ls = 0;
		u32 pos = 0;
		while (pos + nq <= n) {
			u32 m = find_in(b + pos, n - pos, q, (u32)nq, flags, 0, false);
			if (m == UINT32_MAX)
				break;
			/* find_in wraps; reject wrapped results */
			m += pos;
			if (m < pos)
				break;
			for (u32 k = ls; k < m; k++)
				if (b[k] == '\n') {
					line++;
					ls = k + 1;
				}
			u32 le = ls;
			while (le < n && b[le] != '\n')
				le++;
			res_add(r, f, line, m - ls, (u32)nq, b + ls, le - ls);
			if (r->n >= limit) {
				r->truncated = true;
				break;
			}
			pos = m + (u32)nq;
		}
		if (r->n > hits)
			r->files_hit++;
		munmap((void *)b, n);
	}
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
