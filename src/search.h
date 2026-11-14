/* Workspace file listing (Quick Open), fuzzy matching and text search. */
#ifndef VX_SEARCH_H
#define VX_SEARCH_H

#include "base.h"

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

enum { SEARCH_CASE = 1, SEARCH_WORD = 2 };

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

void search_text(SearchResults *r, const char *root, const FileList *fl, const char *query,
                 size_t nq, int flags, u32 limit);
void search_free(SearchResults *r);

/* Find in one buffer (used by the editor find widget). Returns the byte
 * offset of the next match at or after from (wrapping), or UINT32_MAX. */
u32 find_in(const char *hay, u32 n, const char *q, u32 nq, int flags, u32 from, bool backwards);
bool is_word_byte(u8 c);

#endif
