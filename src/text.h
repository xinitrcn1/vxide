/* Document text: a piece table over two sources, the file mapped read-only
 * with mmap (MAP_PRIVATE) and an append-only add buffer. A flat array of
 * line starts is kept in step with every edit; the undo log is SoA with
 * its bytes in a pool. Saving writes a temp file and renames it over the
 * original (so the old mapping never sees a truncated file), then maps the
 * new file and collapses the table back to one piece. */
#ifndef VX_TEXT_H
#define VX_TEXT_H

#include "base.h"

enum { SRC_FILE, SRC_ADD };

typedef struct {
	/* sources */
	const char *file; /* mmap'd (or malloc'd for non-regular files) */
	size_t file_len;
	bool mapped;
	char *add;
	u32 add_n, add_cap;

	/* pieces (SoA); pstart = prefix sums of plen */
	u32 np, pcap;
	u8 *psrc;
	u32 *poff, *plen, *pstart;

	/* line starts; ls[0] == 0, nl >= 1 */
	u32 nl, lcap;
	u32 *ls;
	u32 len;

	/* undo log (SoA) */
	u32 nu, ucap, upos; /* entries [0, upos) are applied */
	u32 *u_pos, *u_del, *u_delo, *u_ins, *u_inso, *u_group;
	Pool upool;
	u32 saved_upos; /* UINT32_MAX when the saved state is unreachable */

	bool crlf;    /* file used \r\n line ends */
	u32 version;  /* bumps on every change */
	u64 mtime_ns; /* of the file when loaded/saved */
	u64 inode;

	/* scratch for lines spanning pieces and for flattening */
	char *scratch;
	u32 scap;
	char *flat;
	u32 flat_version;
	bool flat_ok;
} Text;

void text_init(Text *t);
int text_open(Text *t, const char *path); /* 0 ok; missing file = empty, -1 on error */
void text_free(Text *t);
int text_save(Text *t, const char *path); /* atomic replace + remap */
/* Did the file on disk change since load/save? */
bool text_changed_on_disk(const Text *t, const char *path);
/* Remember the file's current mtime/inode (after reloading its content). */
void text_restat(Text *t, const char *path);

static inline u32 text_lines(const Text *t) { return t->nl; }
static inline u32 text_line_start(const Text *t, u32 line) { return t->ls[line]; }
/* Length of a line without its "\n" (and "\r"). */
u32 text_line_len(const Text *t, u32 line);
/* Bytes of a line (without line end). Zero-copy when the line lies in one
 * piece; otherwise copied into scratch. Valid until the next call or edit. */
const char *text_line(Text *t, u32 line, u32 *n);
u32 text_line_of(const Text *t, u32 off);
u8 text_byte(const Text *t, u32 off);
void text_copy(const Text *t, u32 off, u32 n, char *dst);
/* Whole text as one contiguous buffer, cached until the next edit. */
const char *text_flat(Text *t);

/* Replace del bytes at pos with ins. group != 0 records undo (entries with
 * the same group are undone together). */
void text_replace(Text *t, u32 pos, u32 del, const char *ins, u32 nins, u32 group);
/* Undo/redo one group. Returns false if nothing to do; pos and end receive
 * the range of the last change (for cursor placement). */
bool text_undo(Text *t, u32 *pos, u32 *end);
bool text_redo(Text *t, u32 *pos, u32 *end);
u32 text_last_group(const Text *t);
static inline bool text_dirty(const Text *t) { return t->upos != t->saved_upos; }
const char *text_eol(const Text *t);

#endif
