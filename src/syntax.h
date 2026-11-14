/* Syntax highlighting: line-at-a-time lexers producing token spans.
 * Token classes mirror the TextMate scopes that VSCodium's Dark+/Light+
 * (Dark Modern / Light Modern) themes colour; theme.c maps class -> colour. */
#ifndef VX_SYNTAX_H
#define VX_SYNTAX_H

#include "base.h"

enum {
	TK_TEXT,      /* default foreground (punctuation, plain identifiers)          */
	TK_COMMENT,   /* comment                               #6A9955 / #008000     */
	TK_STRING,    /* string, char literal, <header>        #CE9178 / #A31515     */
	TK_ESCAPE,    /* constant.character.escape             #D7BA7D / #EE0000     */
	TK_NUMBER,    /* constant.numeric                      #B5CEA8 / #098658     */
	TK_STORAGE,   /* storage.type/modifier, keyword        #569CD6 / #0000FF     */
	TK_CONTROL,   /* keyword.control (if/return/#include)  #C586C0 / #AF00DB     */
	TK_TYPE,      /* entity.name.type, support.type        #4EC9B0 / #267F99     */
	TK_FUNCTION,  /* entity.name.function                  #DCDCAA / #795E26     */
	TK_VARIABLE,  /* variable                              #9CDCFE / #001080     */
	TK_CONSTANT,  /* variable.other.constant, enum member  #4FC1FF / #0070C1     */
	TK_LANGCONST, /* constant.language (true/nullptr/nil)  #569CD6 / #0000FF     */
	TK_PREPROC,   /* meta.preprocessor / macro name        #569CD6 / #0000FF     */
	TK_OPERATOR,  /* keyword.operator                      #D4D4D4 / #000000     */
	TK_LABEL,     /* entity.name.label                     #C8C8C8 / #000000     */
	TK_INVALID,   /* invalid.illegal                       #F44747 / #CD3131     */
	TK_COUNT
};

enum {
	LANG_NONE,
	LANG_C,         /* C23 (.c .h)                                   */
	LANG_ASM_GAS,   /* x86 AT&T / GNU as (.s .S .asm with % regs)    */
	LANG_ASM_INTEL, /* x86 Intel / NASM / YASM (.asm .nasm .inc)     */
	LANG_LISP,      /* Common Lisp / Scheme / Emacs Lisp / Clojure   */
	LANG_COUNT
};

/* Output spans for one line. Arrays are caller-owned with capacity cap;
 * the lexer stops emitting (but keeps tracking state) once count == cap.
 * Spans are in ascending start order, non-overlapping; gaps are TK_TEXT. */
typedef struct {
	u32 *start; /* byte offset within the line */
	u32 *len;   /* byte length                 */
	u8 *cls;    /* TK_*                        */
	u32 count, cap;
} SynSpans;

/* Pick a language from the file path (extension/basename) and, for
 * ambiguous extensions such as .asm/.s, from the first bytes of the file. */
int syn_lang_for_path(const char *path, const char *head, size_t headlen);
const char *syn_lang_name(int lang);  /* "C", "x86 Assembly (AT&T)", ... */
const char *syn_lang_lsp_id(int lang); /* LSP languageId: "c", "asm", "lisp" or NULL */

/* Lex one line (without its '\n'). `state` is the lexer state at the start
 * of the line (0 at the start of the file); returns the state at its end.
 * Pure function of (lang, state, bytes): the editor caches per-line states. */
u32 syn_line(int lang, u32 state, const char *s, u32 n, SynSpans *out);

/* Line-comment prefix for toggle comment (Ctrl+/), or NULL. */
const char *syn_line_comment(int lang);

#endif
