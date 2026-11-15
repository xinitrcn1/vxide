/* Inline git blame (VSCodium's git.blame.editorDecoration): who last
 * changed each line of the live buffer. A worker thread runs
 * `git blame --incremental --contents <snapshot>` like VSCodium (the CLI
 * uses the commit-graph Bloom filters: ~0.25 s where libgit2's blame took
 * 12 s on a 99-commit, 22k-line file). Without a git binary it falls back
 * to libgit2: blame the committed file once per HEAD, then replay the
 * buffer with git_blame_buffer. Edited lines read "Not Committed Yet".
 * The UI thread only reads the published table. */
#ifndef VX_BLAME_H
#define VX_BLAME_H

#include "base.h"

typedef struct Blame Blame;

typedef struct {
	bool uncommitted;
	const char *author;  /* valid until the next blame_line / blame_request */
	const char *summary;
	char hash[12];
	i64 time; /* seconds since the epoch */
} BlameLine;

Blame *blame_start(const char *workdir); /* NULL when the thread cannot start */
void blame_stop(Blame *b);
/* Ask for a blame of relpath (repo-relative) with this buffer text; key
 * identifies the text version. Newer requests replace pending ones. */
void blame_request(Blame *b, const char *relpath, const char *text, size_t n, u32 key);
/* Result for one 0-based line; false while (relpath, key) is not ready or
 * the file is not tracked. */
bool blame_line(Blame *b, const char *relpath, u32 key, u32 line, BlameLine *out);
/* True when a result for (relpath, key) is published, even if empty. */
bool blame_ready(Blame *b, const char *relpath, u32 key);
/* True once after the worker published something new (redraw). */
bool blame_take_update(Blame *b);
/* "3 days ago", like VSCodium's fromNow. */
void blame_ago(i64 when, i64 now, char *buf, size_t cap);

#endif
