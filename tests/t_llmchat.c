/* The LLM agent's chats and turns: a chat's JSON, the request, a chunked
 * event stream read a byte at a time, the mock model's turns through the
 * harness, and whole turns against a stand-in OpenAI-compatible server
 * (a tool call streamed in pieces, its answer sent back, an error). */
#define _DEFAULT_SOURCE
#include "llmchat.h"
#include "tap.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static void nolock(void *c) { (void)c; }
static int nchanged;
static void changed(void *c)
{
	(void)c;
	nchanged++;
}

static void put(const char *dir, const char *name, const char *text)
{
	char p[600];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "w");
	fputs(text, f);
	fclose(f);
}

static bool has(const JBuf *b, const char *s) { return b->b && strstr(b->b, s); }

/* ---- a stand-in server: answers each connection with the next script ---- */

static const char *answers[4];
static int nanswers, port;
static JBuf got[4]; /* the requests' bodies */
static int lfd;

static void *serve(void *arg)
{
	(void)arg;
	for (int i = 0; i < nanswers; i++) {
		int c = accept(lfd, NULL, NULL);
		if (c < 0)
			break;
		/* the head, then Content-Length bytes */
		JBuf req = {0};
		char b[4096];
		long want = -1;
		for (;;) {
			ssize_t n = recv(c, b, sizeof b, 0);
			if (n <= 0)
				break;
			jb_rawn(&req, b, (size_t)n);
			char *e = strstr(req.b, "\r\n\r\n");
			if (e && want < 0) {
				char *cl = strcasestr(req.b, "content-length:");
				want = cl ? atol(cl + 15) : 0;
			}
			if (e && (long)(req.n - (u32)(e + 4 - req.b)) >= want) {
				jb_raw(&got[i], e + 4);
				break;
			}
		}
		jb_free(&req);
		send(c, answers[i], strlen(answers[i]), 0);
		close(c);
	}
	return NULL;
}

static pthread_t serve_start(void)
{
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(lfd, (struct sockaddr *)&a, sizeof a);
	socklen_t al = sizeof a;
	getsockname(lfd, (struct sockaddr *)&a, &al);
	port = ntohs(a.sin_port);
	listen(lfd, 4);
	pthread_t th;
	pthread_create(&th, NULL, serve, NULL);
	return th;
}

/* the body as chunks of a few bytes: events split anywhere */
static char *chunked(const char *head, const char *body)
{
	JBuf o = {0};
	jb_raw(&o, head);
	size_t n = strlen(body);
	for (size_t i = 0; i < n;) {
		size_t k = VX_MIN((size_t)7 + i % 5, n - i);
		char h[16];
		snprintf(h, sizeof h, "%zx\r\n", k);
		jb_raw(&o, h);
		jb_rawn(&o, body + i, k);
		jb_raw(&o, "\r\n");
		i += k;
	}
	jb_raw(&o, "0\r\n\r\n");
	return o.b;
}

#define SSE_HEAD "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"

static const char ev_calls[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":null}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"The user wants \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"the files.\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
    "\"function\":{\"name\":\"ls\",\"arguments\":\"{\\\"pa\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"th\\\":\\\".\\\"}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

static const char ev_answer[] =
    "data: {\"choices\":[{\"delta\":{\"content\":\"There is **one** file: \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"`a.txt` \\u2014 caf\\u00e9.\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

static void ev_into(void *ctx, const char *json, size_t n)
{
	bool fin = false;
	llm_apply_event(ctx, json, n, &fin);
}

static LlmRun run_for(LlmCfg *cfg, LlmChat *c, const char *q)
{
	llm_msg_add(c, LLM_USER, q, strlen(q));
	LlmRun r = {0};
	r.cfg = cfg;
	r.chat = c;
	r.lock = r.unlock = nolock;
	r.changed = changed;
	llm_turn(&r);
	return r;
}

static const LlmMsg *last(const LlmChat *c, int role)
{
	for (u32 i = c->n; i-- > 0;)
		if (c->m[i].role == role)
			return &c->m[i];
	return NULL;
}

int main(void)
{
	char dir[] = "/tmp/vxide-llmc-XXXXXX";
	if (!mkdtemp(dir))
		return tap_done();

	/* ---- a chat's JSON: everything comes back ---- */
	LlmChat c;
	llm_chat_init(&c);
	CHECK(strlen(c.id) == 19);
	llm_msg_add(&c, LLM_USER, "What \"is\" here?\nline two", 24);
	LlmMsg *m = llm_msg_add(&c, LLM_ASSISTANT, "Let me look.", 12);
	jb_raw(&m->think, "hmm");
	LlmCall *k = llm_call_add(m);
	jb_raw(&k->id, "c1");
	jb_raw(&k->name, "ls");
	jb_raw(&k->args, "{\"path\":\".\"}");
	m = llm_msg_add(&c, LLM_TOOL, "a.txt 3\n", 8);
	jb_raw(&m->call_id, "c1");
	jb_raw(&m->name, "ls");
	m = llm_msg_add(&c, LLM_NOTE, "Stopped.", 8);
	m->error = true;
	llm_chat_set_title(&c);
	CHECK_STR(c.title.b, "What \"is\" here?");
	char sdir[700];
	snprintf(sdir, sizeof sdir, "%s/state/chats", dir);
	CHECK(llm_chat_save(&c, sdir));
	char fp[900];
	snprintf(fp, sizeof fp, "%s/%s.json", sdir, c.id);
	LlmChat d;
	CHECK(llm_chat_load(&d, fp));
	CHECK(d.n == 4 && !strcmp(d.id, c.id) && !strcmp(d.title.b, c.title.b));
	CHECK(d.m[0].role == LLM_USER && !strcmp(d.m[0].text.b, "What \"is\" here?\nline two"));
	CHECK(d.m[1].ncalls == 1 && !strcmp(d.m[1].calls[0].args.b, "{\"path\":\".\"}") && !strcmp(d.m[1].think.b, "hmm"));
	CHECK(d.m[2].role == LLM_TOOL && !strcmp(d.m[2].call_id.b, "c1") && !strcmp(d.m[2].name.b, "ls"));
	CHECK(d.m[3].role == LLM_NOTE && d.m[3].error && d.m[3].done);

	/* ---- the request: system, messages (no notes), tool calls, tools ---- */
	LlmCfg cfg = {0};
	snprintf(cfg.server, sizeof cfg.server, "http://127.0.0.1:1");
	char *sys = llm_expand_system(llm_default_system, dir);
	CHECK(strstr(sys, dir) && !strstr(sys, "{root}") && strlen(sys) < 500);
	cfg.system = sys;
	cfg.tools.root = dir;
	JBuf rq = {0};
	llm_request(&cfg, &d, &rq);
	Json j = {0};
	CHECK(json_parse(&j, rq.b, rq.n) == 0);
	u32 msgs = json_get(&j, 0, "messages");
	CHECK(json_type(&j, msgs) == JS_ARR && j.size[msgs] == 4); /* system + 3 */
	CHECK(json_eq(&j, json_path(&j, json_at(&j, msgs, 0), "role"), "system"));
	CHECK(json_eq(&j, json_path(&j, json_at(&j, msgs, 2), "tool_calls"), "") == false);
	CHECK(json_eq(&j, json_get(&j, json_at(&j, json_get(&j, json_at(&j, msgs, 2), "tool_calls"), 0), "type"),
	              "function"));
	CHECK(json_eq(&j, json_get(&j, json_at(&j, msgs, 3), "tool_call_id"), "c1"));
	CHECK(json_type(&j, json_get(&j, 0, "tools")) == JS_ARR && json_bool(&j, json_get(&j, 0, "stream"), false));
	CHECK(!has(&rq, "Stopped."));
	llm_chat_free(&d);
	llm_chat_free(&c);

	/* ---- a chunked event stream, a byte at a time ---- */
	{
		char *resp = chunked(SSE_HEAD, ev_calls);
		LlmHttp h = {0};
		LlmMsg mm = {0};
		for (size_t i = 0; resp[i]; i++)
			llm_http_feed(&h, resp + i, 1, ev_into, &mm);
		CHECK(h.status == 200 && h.sse && h.chunked && h.done_seen && h.stage == 2);
		CHECK(mm.ncalls == 1 && !strcmp(mm.calls[0].name.b, "ls") && !strcmp(mm.calls[0].args.b, "{\"path\":\".\"}"));
		CHECK(!strcmp(mm.think.b, "The user wants the files.") && !mm.text.n);
		llm_http_free(&h);
		free(resp);
		LlmMsg e = {0};
		CHECK(!llm_apply_event(&e, "{\"error\":{\"code\":500,\"message\":\"no tools\"}}",
		                       strlen("{\"error\":{\"code\":500,\"message\":\"no tools\"}}"), NULL));
		CHECK_STR(e.text.b, "no tools");
		jb_free(&e.text);
	}

	/* ---- the mock model, through the harness ---- */
	char proj[700];
	snprintf(proj, sizeof proj, "%s/proj", dir);
	char cmd[1600];
	snprintf(cmd, sizeof cmd, "mkdir -p %s/src", proj);
	(void)system(cmd);
	put(proj, "README.md", "# Demo\n\nA tiny demo project that answers questions.\nIt has one program.\n\n## Build\n");
	put(proj, "src/main.c",
	    "#include <stdio.h>\n\nstatic int answer(void)\n{\n\treturn 42;\n}\n\nint main(void)\n{\n"
	    "\tprintf(\"%d\\n\", answer());\n\treturn 0;\n}\n");
	cfg.tools.root = proj;
	cfg.backend = LLM_MOCK;
	LlmChat mc;
	llm_chat_init(&mc);
	LlmRun r = run_for(&cfg, &mc, "What is this project?");
	CHECK(r.used == LLM_MOCK);
	/* user, assistant (ls + read), tool, tool, assistant */
	CHECK(mc.n == 5 && mc.m[1].ncalls == 2 && !strcmp(mc.m[1].calls[0].name.b, "ls") &&
	      !strcmp(mc.m[1].calls[1].name.b, "read"));
	const LlmMsg *a = last(&mc, LLM_ASSISTANT);
	CHECK(a && a->done && !a->ncalls && has(&a->text, "| `src/` | folder |") && has(&a->text, "> A tiny demo project"));
	CHECK(has(&a->text, "mock model") && !strcmp(mc.model, "mock"));
	r = run_for(&cfg, &mc, "What does src/main.c do?");
	a = last(&mc, LLM_ASSISTANT);
	CHECK(has(&a->text, "`src/main.c` has **12 lines** of c") && has(&a->text, "- `static int answer(void)` (line 3)"));
	CHECK(has(&a->text, "```c\n#include <stdio.h>"));
	r = run_for(&cfg, &mc, "where is answer defined?");
	a = last(&mc, LLM_ASSISTANT);
	CHECK(has(&a->text, "`answer` appears **3 times**") && has(&a->text, "- [src/main.c:3](src/main.c:3)"));
	r = run_for(&cfg, &mc, "please run `echo hi; exit 2`");
	a = last(&mc, LLM_ASSISTANT);
	CHECK(has(&a->text, "**exit 2**") && has(&a->text, "```text\nhi\n```"));
	const LlmMsg *t = last(&mc, LLM_TOOL);
	CHECK(t && t->done && !strcmp(t->name.b, "exec") && has(&t->text, "[exit 2]"));
	/* a mock in a read-only harness does not run commands */
	cfg.tools.read_only = true;
	r = run_for(&cfg, &mc, "run `touch x`");
	CHECK(strcmp(last(&mc, LLM_TOOL)->name.b, "exec") != 0);
	cfg.tools.read_only = false;
	llm_chat_free(&mc);

	/* ---- AUTO without a server: the mock, and why ---- */
	cfg.backend = LLM_AUTO;
	llm_chat_init(&mc);
	r = run_for(&cfg, &mc, "hello");
	CHECK(r.used == LLM_MOCK && strstr(r.why_mock, "127.0.0.1:1"));
	llm_chat_free(&mc);

	/* ---- a server: a tool call streamed in pieces, then the answer ---- */
	put(proj, "a.txt", "abc");
	answers[0] = chunked(SSE_HEAD, ev_calls);
	answers[1] = chunked(SSE_HEAD, ev_answer);
	answers[2] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 53\r\n\r\n"
	             "{\"error\":{\"code\":500,\"message\":\"tools need --jinja\"}}";
	nanswers = 3;
	pthread_t th = serve_start();
	snprintf(cfg.server, sizeof cfg.server, "http://127.0.0.1:%d/v1/", port);
	snprintf(cfg.model, sizeof cfg.model, "qwen");
	llm_chat_init(&mc);
	r = run_for(&cfg, &mc, "Which files are there?");
	CHECK(r.used == LLM_SERVER && !strcmp(mc.model, "qwen"));
	CHECK(mc.n == 4 && mc.m[1].ncalls == 1 && !strcmp(mc.m[1].calls[0].id.b, "c1"));
	CHECK(mc.m[2].role == LLM_TOOL && has(&mc.m[2].text, "a.txt 3"));
	a = last(&mc, LLM_ASSISTANT);
	CHECK(a && !strcmp(a->text.b, "There is **one** file: `a.txt` \xE2\x80\x94 caf\xC3\xA9."));
	CHECK(has(&got[0], "\"model\":\"qwen\"") && has(&got[0], "\"stream\":true"));
	CHECK(has(&got[1], "\"tool_call_id\":\"c1\"") && has(&got[1], "a.txt 3") && has(&got[1], "\"tool_calls\""));
	/* the server's error, as a note */
	r = run_for(&cfg, &mc, "again");
	const LlmMsg *n = last(&mc, LLM_NOTE);
	CHECK(n && n->error && has(&n->text, "HTTP 500: tools need --jinja"));
	pthread_join(th, NULL);
	close(lfd);
	CHECK(nchanged > 10);
	llm_chat_free(&mc);

	free(sys);
	jb_free(&rq);
	json_free(&j);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)system(cmd);
	return tap_done();
}
