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
	TK_HEADING,   /* markup.heading (bold)                 #569CD6 / #800000     */
	TK_BOLD,      /* markup.bold (bold)                    #569CD6 / #000080     */
	TK_ITALIC,    /* markup.italic (italic)                #C586C0 / #800080     */
	TK_STRIKE,    /* markup.strikethrough (strike)         fg                    */
	TK_RAW,       /* markup.inline.raw                     #CE9178 / #800000     */
	TK_LINK,      /* markup.underline.link (underline)     fg                    */
	TK_LIST,      /* punctuation.definition.list.begin     #6796E6 / #0451A5     */
	TK_QUOTE,     /* punctuation.definition.quote.begin    #6A9955 / #0451A5     */
	TK_TAG,       /* entity.name.tag                       #569CD6 / #800000     */
	TK_TAGPUNCT,  /* punctuation.definition.tag            #808080 / #800000     */
	TK_ATTR,      /* entity.other.attribute-name           #9CDCFE / #E50000     */
	TK_REGEX,     /* string.regexp                         #D16969 / #811F3F     */
	TK_PROPERTY,  /* support.type.property-name.json,
	                 support.function.git-rebase           #9CDCFE / #0451A5     */
	TK_INSERTED,  /* markup.inserted                       #B5CEA8 / #098658     */
	TK_DELETED,   /* markup.deleted                        #CE9178 / #A31515     */
	TK_CHANGED,   /* markup.changed                        #569CD6 / #0451A5     */
	TK_DIFFHDR,   /* meta.diff.header                      #569CD6 / #000080     */
	TK_UNQUOTED,  /* string.unquoted.plain/block, string.quoted.single (YAML)
	                                                       #CE9178 / #0000FF     */
	TK_COUNT
};

enum {
	LANG_NONE,
	LANG_C,         /* C23 (.c .h)                                   */
	LANG_ASM_GAS,   /* x86 AT&T / GNU as (.s .S .asm with % regs)    */
	LANG_ASM_INTEL, /* x86 Intel / NASM / YASM (.asm .nasm .inc)     */
	LANG_LISP,      /* Common Lisp / Scheme / Emacs Lisp / Clojure   */
	LANG_SH,        /* sh / bash / zsh / ksh                         */
	LANG_MARKDOWN,  /* Markdown (CommonMark + GFM bits)              */
	LANG_TROFF,     /* roff: man, mdoc, ms, me, mm                   */
	LANG_PERL,      /* Perl 5 (+ POD)                                */
	LANG_CMAKE,     /* CMakeLists.txt, *.cmake                       */
	LANG_MAKE,      /* BSD make and GNU make                         */
	LANG_JSON,      /* JSON (comments tolerated)                     */
	LANG_IGNORE,    /* .gitignore, .dockerignore, ...                */
	LANG_DIFF,      /* unified/context diffs, git show output        */
	LANG_GIT_COMMIT, /* COMMIT_EDITMSG, MERGE_MSG, TAG_EDITMSG       */
	LANG_GIT_REBASE, /* git-rebase-todo                              */
	LANG_JSONC,     /* JSON with Comments (.jsonc, settings.json)    */
	LANG_PYTHON,    /* Python 3 (.py .pyi, shebangs)                 */
	LANG_YAML,      /* YAML 1.2 (.yml .yaml, .clang-format)          */
	LANG_CODEOWNERS, /* CODEOWNERS (GitHub / GitLab)                 */
	LANG_JAVA,      /* Java (.java)                                  */
	LANG_KOTLIN,    /* Kotlin (.kt .kts)                             */
	LANG_TOML,      /* TOML (.toml, Cargo.lock, Pipfile)             */
	LANG_CPP,       /* C++26 (.cpp .cc .hpp ..., .h with C++ markers) */
	LANG_RUST,      /* Rust (.rs)                                    */
	LANG_CSHARP,    /* C# 13 (.cs .csx)                              */
	LANG_QML,       /* QML (.qml) with JavaScript bindings           */
	LANG_QMAKE,     /* qmake project files (.pro .pri .prf .prl)     */
	LANG_GDSCRIPT,  /* GDScript (.gd)                                */
	LANG_GODOT_RES, /* Godot resources (.tscn .tres .godot .import)  */
	LANG_XML,       /* XML (.xml .ui .qrc .svg .csproj ..., <?xml)   */
	LANG_GLSL,      /* GLSL (.glsl .comp .frag .vert ...): the C lexer, GLSL's words */
	/* Markdown keeps a fence language in 5 bits (0..31): the languages from
	 * here on are not fence languages (their fences stay plain) */
	LANG_SPIRV,     /* SPIR-V assembly (.spvasm, spirv-dis output)   */
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

/* Lexer states fit in 20 bits for every language (Markdown keeps an
 * embedded fenced-code state in its own state word). */
#define SYN_STATE_BITS 20

/* Lex one line (without its '\n'). `state` is the lexer state at the start
 * of the line (0 at the start of the file); returns the state at its end.
 * Pure function of (lang, state, bytes): the editor caches per-line states. */
u32 syn_line(int lang, u32 state, const char *s, u32 n, SynSpans *out);

/* The language of a Markdown fence's info string ("c", "rust", "sh"...),
 * LANG_NONE when unknown. */
int syn_fence_lang(const char *w, u32 n);
/* The language of a ucas code block's tag (```glsl, ```comp, ```spv, ```c
 * ...): the shader languages too. LANG_NONE when unknown. */
int syn_block_lang(const char *w, u32 n);

/* Line-comment prefix for toggle comment (Ctrl+/), or NULL. */
const char *syn_line_comment(int lang);

#endif
