/* Language Server Protocol client (JSON-RPC 2.0 over a child's stdio).
 * Tested against clangd. Positions are byte columns: the client asks for
 * positionEncoding "utf-8" and converts from UTF-16 if the server refuses. */
#ifndef VX_LSP_H
#define VX_LSP_H

#include "base.h"

typedef struct Lsp Lsp;

enum { LSP_SEV_ERROR = 1, LSP_SEV_WARNING, LSP_SEV_INFO, LSP_SEV_HINT };

enum {
	LSP_EV_NONE,
	LSP_EV_READY,       /* initialize finished                              */
	LSP_EV_DIAGNOSTICS, /* diagnostics for one document replaced            */
	LSP_EV_COMPLETION,  /* reply to lsp_completion                          */
	LSP_EV_HOVER,       /* reply to lsp_hover                               */
	LSP_EV_LOCATION,    /* reply to lsp_definition (may hold 0..n locations)*/
	LSP_EV_SEMANTIC,    /* reply to lsp_semantic_tokens                     */
	LSP_EV_LOG,         /* window/logMessage, showMessage, stderr lines     */
	LSP_EV_EXIT,        /* server process exited                            */
};

/* Diagnostics of one document (SoA). Lines/cols are 0-based bytes. */
typedef struct {
	u32 count;
	u32 *line0, *col0, *line1, *col1;
	u8 *severity;
	u32 *message; /* offsets into pool */
	u32 *source;  /* offsets into pool ("clang", "clang-tidy"), may be empty */
} LspDiags;

/* Completion items (SoA). */
typedef struct {
	u32 count;
	u32 *label, *detail, *insert; /* pool offsets; insert = text to insert */
	u8 *kind;                     /* LSP CompletionItemKind */
	/* Replace range from textEdit when present, else start of the word. */
	u32 *line0, *col0, *line1, *col1;
	u8 *has_range;
} LspCompletion;

typedef struct {
	u32 count;
	u32 *path; /* absolute file paths (file:// decoded), pool offsets */
	u32 *line, *col;
} LspLocations;

/* Semantic tokens decoded to absolute positions (SoA), with the legend
 * mapped to vxide token classes (TK_* from syntax.h) and modifier bits. */
typedef struct {
	u32 count;
	u32 *line, *col, *len;
	u8 *cls;   /* TK_* */
	u32 *mods; /* raw modifier bitset per the server's legend */
} LspSemantic;

typedef struct {
	int type;     /* LSP_EV_* */
	i32 req;      /* request id for replies, else -1 */
	const char *path; /* document path for DIAGNOSTICS/SEMANTIC/replies */
	const Pool *pool; /* string storage for the tables below */
	const LspDiags *diags;
	const LspCompletion *completion;
	const LspLocations *locations;
	const LspSemantic *semantic;
	const char *text; /* HOVER markdown/plain, LOG line */
} LspEvent;

/* Start a server (argv[0] looked up in PATH) for a project root. Returns
 * NULL if it cannot be spawned. The initialize request is sent at once. */
Lsp *lsp_start(const char *root, char *const argv[]);
void lsp_stop(Lsp *l); /* shutdown + exit, then kills after a grace period */
int lsp_fd(Lsp *l);    /* readable fd for poll(); -1 when dead */
bool lsp_ready(Lsp *l);

/* Read whatever is available without blocking and queue events. */
void lsp_pump(Lsp *l);
/* Pop the next event; returns false when none. Pointers in ev stay valid
 * until the next lsp_next/lsp_pump call. */
bool lsp_next(Lsp *l, LspEvent *ev);

/* Document sync (full text). path is absolute; lang_id "c", "cpp", ... */
void lsp_did_open(Lsp *l, const char *path, const char *lang_id, i32 version,
                  const char *text, size_t n);
void lsp_did_change(Lsp *l, const char *path, i32 version, const char *text, size_t n);
void lsp_did_save(Lsp *l, const char *path);
void lsp_did_close(Lsp *l, const char *path);

/* Requests return the request id (>0), or -1 when not sent. Lines/cols are
 * 0-based byte positions. The text is needed for UTF-16 fallback only and
 * may be NULL when the server accepted utf-8. */
i32 lsp_completion(Lsp *l, const char *path, u32 line, u32 col);
i32 lsp_hover(Lsp *l, const char *path, u32 line, u32 col);
i32 lsp_definition(Lsp *l, const char *path, u32 line, u32 col);
/* These reply with LSP_EV_LOCATION as well (match by request id). */
i32 lsp_declaration(Lsp *l, const char *path, u32 line, u32 col);
i32 lsp_type_definition(Lsp *l, const char *path, u32 line, u32 col);
i32 lsp_references(Lsp *l, const char *path, u32 line, u32 col); /* includeDeclaration */
i32 lsp_semantic_tokens(Lsp *l, const char *path);

/* Position encoding the server chose (true = utf-8 byte columns). */
bool lsp_utf8(Lsp *l);

/* Column conversion within one line (s[0..n), no '\n'): UTF-16 code units
 * <-> byte offsets. Out-of-range columns clamp to the line end. */
u32 lsp_utf16_to_byte(const char *s, u32 n, u32 col16);
u32 lsp_byte_to_utf16(const char *s, u32 n, u32 col);

/* file:// URI <-> absolute path (percent-encoding); malloc'd results. */
char *lsp_path_to_uri(const char *path);
char *lsp_uri_to_path(const char *uri, size_t n);

#endif
