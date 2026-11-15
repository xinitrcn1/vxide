/* The LLM agent's chats and their turns, without a window: what a chat
 * holds and how it is kept on disk, the request a chat becomes, the
 * streamed answer of an OpenAI-compatible server (llama.cpp's llama-server
 * by default, http://127.0.0.1:8080), a mock model for when none is
 * running, and the loop of one turn: ask the model, run the tools it
 * calls (the harness, llmtools.h), give it their answers, until it answers
 * without a call. A turn blocks; the tab runs it on a thread of its own.
 * Unit-tested in tests/t_llmchat.c. */
#ifndef VX_LLMCHAT_H
#define VX_LLMCHAT_H

#include "base.h"
#include "json.h"
#include "llmtools.h"

enum { LLM_USER, LLM_ASSISTANT, LLM_TOOL, LLM_NOTE }; /* NOTE: shown, never sent */

typedef struct {
	JBuf id, name, args;
} LlmCall;

typedef struct {
	u8 role;
	bool done;  /* complete (an assistant's answer streams in) */
	bool error; /* a tool that failed, a note that is an error */
	JBuf text;  /* the content */
	JBuf think; /* an assistant's reasoning (reasoning_content) */
	LlmCall *calls; /* an assistant's tool calls */
	u32 ncalls, calls_cap;
	JBuf call_id, name; /* a tool's answer: to which call */
	i64 t0, t1;         /* started, finished (ms since the epoch) */
	void *ui;           /* the tab's (its Markdown layout); not saved */
	u32 ui_flags;
} LlmMsg;

typedef struct {
	char id[40]; /* the file name: when it was started */
	JBuf title;
	i64 created, updated; /* ms since the epoch */
	char model[128];      /* who answered last */
	LlmMsg *m;
	u32 n, cap;
} LlmChat;

void llm_chat_init(LlmChat *c);   /* a new chat, id from the time */
void llm_chat_free(LlmChat *c);   /* m[i].ui must be freed by its owner first */
LlmMsg *llm_msg_add(LlmChat *c, int role, const char *text, size_t n);
LlmCall *llm_call_add(LlmMsg *m);
void llm_chat_set_title(LlmChat *c); /* from the first question */
/* JSON, one file a chat */
void llm_chat_json(const LlmChat *c, JBuf *out);
bool llm_chat_parse(LlmChat *c, const char *s, size_t n); /* into a fresh chat */
bool llm_chat_save(const LlmChat *c, const char *dir);
bool llm_chat_load(LlmChat *c, const char *path);
i64 llm_now_ms(void);

/* ---- a turn ----------------------------------------------------------------------------------- */

enum { LLM_AUTO, LLM_SERVER, LLM_MOCK }; /* AUTO: the server when it answers, else the mock */

typedef struct {
	char server[256]; /* http://host:port[/prefix] */
	char model[128];  /* "" = the server's own */
	char api_key[256]; /* "" = none (sent as a bearer token) */
	int backend;
	const char *system; /* the system prompt, {root} already filled in */
	LlmTools tools;
	int max_steps;     /* model calls per turn (24 when 0) */
	int mock_delay_ms; /* the mock streams a few bytes every this */
	int connect_ms;    /* 1500 when 0 */
} LlmCfg;

typedef struct {
	const LlmCfg *cfg;
	LlmChat *chat;
	/* the chat is shared with the UI thread: every change to it is made
	 * between lock and unlock, and changed() follows */
	void (*lock)(void *ctx), (*unlock)(void *ctx), (*changed)(void *ctx);
	void *ctx;
	volatile int cancel; /* set by the UI: the turn ends at the next byte or tool */
	/* out */
	int used; /* LLM_SERVER or LLM_MOCK */
	char why_mock[200];
} LlmRun;

/* One turn: the chat's last message is the user's. Returns when the model
 * answered without a tool call, failed, ran out of steps or was stopped
 * (each says so in a note). */
void llm_turn(LlmRun *r);

/* The system prompt used when the user has none: short on purpose. */
extern const char llm_default_system[];
/* s with {root} replaced (malloc'd) */
char *llm_expand_system(const char *s, const char *root);

/* ---- pieces, for the tests ---------------------------------------------------------------------- */

void llm_request(const LlmCfg *cfg, const LlmChat *c, JBuf *out);

/* An HTTP response read as it arrives: the head, then the body, chunked
 * or not, as server-sent events ("data: {...}" lines) or one JSON. */
typedef struct {
	int stage; /* 0 head, 1 body, 2 done */
	JBuf head;
	int status;
	bool chunked, sse;
	i64 chunk_left; /* -1: reading a chunk size line */
	JBuf szl;       /* the chunk size line */
	JBuf line;      /* the event line */
	JBuf body;      /* a body that is not events */
	bool done_seen; /* data: [DONE] */
} LlmHttp;

typedef void (*LlmEventFn)(void *ctx, const char *json, size_t n);
void llm_http_feed(LlmHttp *h, const char *s, size_t n, LlmEventFn fn, void *ctx);
void llm_http_free(LlmHttp *h);
/* One streamed event (or a whole completion) applied to message m:
 * content, reasoning, tool call pieces. false: an error event (m->text has it). */
bool llm_apply_event(LlmMsg *m, const char *json, size_t n, bool *finished);

#endif
