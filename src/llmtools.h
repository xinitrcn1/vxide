/* The LLM agent's default harness: the tools of mcp-fbsd (read, write,
 * edit, ls, stat, mv, mkdir, search, exec), built in and confined to the
 * project folder. Same names, arguments and terse answers as the server
 * (a write says "ok", a partial read ends with "[lines A-B of N]", exec
 * ends with "[exit N]"), so a model that knows one knows the other. No
 * window or thread here: unit-tested on its own (tests/t_llmtools.c). */
#ifndef VX_LLMTOOLS_H
#define VX_LLMTOOLS_H

#include "base.h"
#include "json.h"

#define LLMT_OUT_MAX (32u << 10) /* longer output keeps its head and tail */

typedef struct {
	const char *root;    /* the project folder (absolute); every path is under it */
	bool read_only;      /* write, edit, mv, mkdir and exec are not offered */
	bool no_exec;        /* exec is not offered */
	int exec_timeout_ms; /* exec's default timeout (10 s when 0) */
	const volatile int *cancel; /* non-zero: a running exec is killed */
} LlmTools;

/* The "tools" array of a chat completion request (OpenAI function tools). */
void llmt_schema(const LlmTools *t, JBuf *b);
/* Run tool `name` with its JSON arguments; its text goes to *out (reset
 * first, NUL-terminated). false: the tool failed (out says why). */
bool llmt_call(const LlmTools *t, const char *name, const char *args, size_t n, JBuf *out);
/* A one-line summary of a call for the transcript ("read src/main.c") */
void llmt_title(const char *name, const char *args, size_t n, char *out, size_t cap);

#endif
