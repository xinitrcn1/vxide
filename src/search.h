/* Workspace file listing (Quick Open), fuzzy matching and text search. */
#ifndef VX_SEARCH_H
#define VX_SEARCH_H

#include "base.h"
#include "regex.h"

/* Return true to skip a repo-relative path (e.g. git-ignored). */
typedef bool (*SkipFn)(void *ctx, const char *rel, bool dir);

typedef struct {
	u32 n, cap;
	u32 *path; /* relative paths in pool */
	u32 *base; /* offset of the basename within the path */
	Pool pool;
} FileList;

/* Walk root (files.exclude + search.exclude defaults, plus skip()). */
void files_scan(FileList *fl, const char *root, SkipFn skip, void *ctx, u32 limit);
void files_free(FileList *fl);

/* Fuzzy score of pattern against s (higher is better; -1 = no match).
 * Matched byte positions (< 64) are set in *mask. Case-insensitive;
 * consecutive and word-start matches score more. */
int fuzzy_score(const char *pat, size_t np, const char *s, size_t ns, u64 *mask);

enum { SEARCH_CASE = 1, SEARCH_WORD = 2, SEARCH_REGEX = 4 };

/* One compiled query: literal (case/word) or RE2-style regex. Whole word
 * with a regex wraps it as \b(?:...)\b, like VSCodium. */
typedef struct {
	int flags;
	char *q;
	u32 nq;
	Regex *re;
	char err[160]; /* regex syntax error, empty when fine */
	u32 caps[2 * RE_MAX_CAPS];
} Matcher;

bool matcher_init(Matcher *m, const char *q, size_t nq, int flags); /* false: error or empty */
void matcher_free(Matcher *m);
/* Next match at or after from (no wrap). Empty regex matches are allowed;
 * callers advance past them. Sets *a, *b; m->caps holds submatches. */
bool matcher_next(Matcher *m, const char *hay, u32 n, u32 from, u32 *a, u32 *b);
/* Replacement text for the last match (regex: $1 ${name} \U...; literal:
 * as is). Appends to *out. */
void matcher_expand(Matcher *m, const char *repl, size_t nr, const char *hay, char **out, u32 *len, u32 *cap);

/* VSCodium glob lists: comma-separated patterns with *, ?, [...], {a,b}
 * and a double star for any number of folders. A pattern that does not
 * start with "./" or "/" matches at any depth, and a folder pattern
 * matches everything inside it. An empty list matches nothing. */
bool glob_list_match(const char *list, const char *relpath);
bool glob_match(const char *pat, size_t np, const char *s, size_t ns);

typedef struct {
	u32 n, cap;
	u32 *file;  /* index into the FileList */
	u32 *line;  /* 0-based */
	u32 *col;   /* byte column */
	u32 *len;
	u32 *text;  /* line preview in pool (trimmed to 200 bytes) */
	u32 *text_col; /* match column within the preview */
	Pool pool;
	u32 files_hit;
	bool truncated;
} SearchResults;

/* Search every file of fl (filtered by the include/exclude glob lists,
 * NULL or "" for none). Results keep a byte length per match; regex
 * matches may span lines when the pattern mentions \n. */
void search_text(SearchResults *r, const char *root, const FileList *fl, Matcher *m,
                 const char *include, const char *exclude, u32 limit);
void search_free(SearchResults *r);

/* Find in one buffer (used by the editor find widget). Returns the byte
 * offset of the next match at or after from (wrapping), or UINT32_MAX. */
u32 find_in(const char *hay, u32 n, const char *q, u32 nq, int flags, u32 from, bool backwards);
bool is_word_byte(u8 c);

#endif
