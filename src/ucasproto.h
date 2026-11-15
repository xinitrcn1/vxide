/* The ucas JSON frontend as the ucas tab speaks it: requests written as
 * JSON lines, responses (one a line) read into a flat record. ucas's own
 * description of the protocol: its README, "JSON frontend for agents and
 * programs". No window or process here, so it is unit-tested alone. */
#ifndef VX_UCASPROTO_H
#define VX_UCASPROTO_H

#include "base.h"
#include "json.h"

#define UCAS_NONE 0xFFFFFFFFu /* a string field that is absent */
#define UCAS_MAX_NOTES 16
#define UCAS_MAX_CHECKS 16
#define UCAS_MAX_CONDS 8
#define UCAS_MAX_READINGS 8
#define UCAS_MAX_FILES 8
#define UCAS_MAX_STMTS 16
#define UCAS_MAX_FUNCS 256

/* How far an answer is established (proof.kind) */
enum { UCAS_PROOF_NONE, UCAS_PROOF_SYMBOLIC, UCAS_PROOF_EXHAUSTIVE, UCAS_PROOF_SAT, UCAS_PROOF_NUMERIC };

typedef struct {
	u32 name, mime; /* in str */
	u8 *data;       /* decoded bytes (malloc'd), n of them */
	u32 n;
} UcasFile;

/* Every u32 string field is an offset into str, or UCAS_NONE. */
typedef struct {
	Pool str;
	i64 id;
	bool ok, parsed;
	u32 cmd, ucas, text, kind; /* kind: result.kind */
	/* error */
	u32 err_code, err_msg, err_key, err_how;
	int err_line, err_col; /* 0 when not given */
	u32 nread;
	u32 read_text[UCAS_MAX_READINGS];
	int read_index[UCAS_MAX_READINGS];
	bool read_default[UCAS_MAX_READINGS];
	/* proof, checks, notes, conditions */
	int proof;
	u32 proof_text;
	u32 nchecks;
	u32 check_kind[UCAS_MAX_CHECKS], check_text[UCAS_MAX_CHECKS];
	bool check_passed[UCAS_MAX_CHECKS];
	u32 nnotes;
	u32 note_kind[UCAS_MAX_NOTES], note_text[UCAS_MAX_NOTES];
	u32 nconds;
	u32 cond[UCAS_MAX_CONDS];
	/* english: what the sentence was read as */
	u32 nstmts;
	u32 stmt[UCAS_MAX_STMTS];
	bool understood;
	/* files: plots and animations (GIF), rewritten modules ... */
	u32 nfiles;
	UcasFile files[UCAS_MAX_FILES];
	/* state: the session's functions and the active cost model */
	u32 nfuncs;
	u32 func_sig[UCAS_MAX_FUNCS], func_body[UCAS_MAX_FUNCS];
	u32 model;
} UcasResp;

/* One response line. Returns false when it is not a JSON object; the record
 * is reset either way (the files' data are freed). */
bool ucas_parse(UcasResp *r, const char *line, u32 n);
void ucas_resp_free(UcasResp *r);
static inline const char *ucas_s(const UcasResp *r, u32 off)
{
	return off == UCAS_NONE || !r->str.b || off >= r->str.n ? "" : r->str.b + off;
}

/* Base64 into out (at least n * 3 / 4 bytes); other bytes (JSON's "\/",
 * newlines) are skipped. Returns the length. */
u32 ucas_b64_decode(const char *s, u32 n, u8 *out);

/* A request line (with its newline) appended to b:
 *   {"id": id, "cmd": cmd, "args": {key: text, "code": code, "lang": lang,
 *    "timeout_s": t, "assume": {assume_key: assume_index}}}
 * key/text may be NULL (no argument), code NULL (none), lang NULL or ""
 * (ucas reads the code's language from it), assume_key NULL (none),
 * t <= 0 none. */
void ucas_request(JBuf *b, i64 id, const char *cmd, const char *key, const char *text, size_t n, const char *code,
                  size_t code_n, const char *lang, const char *assume_key, int assume_index, double timeout_s);

/* An input as the REPL sends it: statements, and fenced code blocks
 * (```glsl, ```comp, ```spv, ```c ...) each with the statement on the line
 * before it ("simplify", "animate exec of this seed 1", "c optimize"; none:
 * "simplify"). The pieces point into the input. */
typedef struct {
	const char *stmt; /* the statements (several lines when there is no code) */
	u32 stmt_n;
	const char *code; /* NULL: no block */
	u32 code_n;
	const char *tag; /* the fence's tag ("glsl", "frag", "" ...) */
	u32 tag_n;
	int lang;           /* LANG_* of the tag (syntax.h), LANG_NONE untagged */
	const char *ucas_lang; /* "glsl", "spv", "c", or NULL: ucas reads it from the code */
	bool closed;        /* the closing fence was there */
} UcasSeg;
/* Split s[0..n) into at most max pieces in order; returns how many. Blank
 * pieces are left out. */
u32 ucas_split(const char *s, u32 n, UcasSeg *out, u32 max);
/* The fence opener on this line (``` and a tag), its tag in *tag: true. */
bool ucas_fence_open(const char *l, u32 n, const char **tag, u32 *tag_n);
bool ucas_fence_close(const char *l, u32 n);

/* Does the text read as an English sentence rather than ucas statements?
 * (three words of letters, no braces or bitwise operators) */
bool ucas_looks_english(const char *s, size_t n);
/* Brackets ( [ { still open in s: the statement goes on (ucas's own REPL rule) */
int ucas_open_depth(const char *s, size_t n);

#endif
