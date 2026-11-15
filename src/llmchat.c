/* The LLM agent's chats and turns (see llmchat.h).
 *
 * The server is spoken to as an OpenAI-compatible chat completion
 * endpoint, POST <server>/v1/chat/completions with "stream": true, which is
 * what llama.cpp's llama-server serves (its tool calls need --jinja). The
 * HTTP is the little this needs: one request per connection, a chunked or
 * plain body, server-sent events. No TLS: the server is local.
 *
 * The mock model stands in while no server answers. It is not a language
 * model: it reads the question for a command in backticks, a word to
 * search for, or a file of the project, calls the matching tool through the
 * same harness, and writes up the tool's answer in Markdown, streamed like
 * a real one, so the whole turn (request, tools, answer) is exercised. */
#define _DEFAULT_SOURCE
#include "llmchat.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

const char llm_default_system[] =
    "You are a coding agent working in the project at {root}, inside the vxide editor.\n"
    "Look before you answer: ls, search and read. Change files with edit or write; run builds and tests with exec. "
    "Paths are relative to the project; nothing outside it can be reached.\n"
    "Be brief. Answer in Markdown, with code in fenced blocks tagged with their language.\n";

i64 llm_now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (i64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void sleep_ms(int ms)
{
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

char *llm_expand_system(const char *s, const char *root)
{
	JBuf b = {0};
	for (const char *p = s; *p;) {
		const char *q = strstr(p, "{root}");
		if (!q) {
			jb_raw(&b, p);
			break;
		}
		jb_rawn(&b, p, (size_t)(q - p));
		jb_raw(&b, root ? root : ".");
		p = q + 6;
	}
	jb_rawn(&b, "", 0);
	return b.b;
}

/* ---- chats --------------------------------------------------------------------------------------- */

void llm_chat_init(LlmChat *c)
{
	memset(c, 0, sizeof *c);
	c->created = c->updated = llm_now_ms();
	time_t t = (time_t)(c->created / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	char d[24];
	strftime(d, sizeof d, "%Y%m%d-%H%M%S", &tm);
	snprintf(c->id, sizeof c->id, "%s-%03d", d, (int)(c->created % 1000));
}

static void msg_free(LlmMsg *m)
{
	jb_free(&m->text);
	jb_free(&m->think);
	jb_free(&m->call_id);
	jb_free(&m->name);
	for (u32 i = 0; i < m->ncalls; i++) {
		jb_free(&m->calls[i].id);
		jb_free(&m->calls[i].name);
		jb_free(&m->calls[i].args);
	}
	free(m->calls);
}

void llm_chat_free(LlmChat *c)
{
	for (u32 i = 0; i < c->n; i++)
		msg_free(&c->m[i]);
	free(c->m);
	jb_free(&c->title);
	memset(c, 0, sizeof *c);
}

LlmMsg *llm_msg_add(LlmChat *c, int role, const char *text, size_t n)
{
	VX_GROW(c->m, c->cap, c->n + 1);
	LlmMsg *m = &c->m[c->n++];
	memset(m, 0, sizeof *m);
	m->role = (u8)role;
	m->t0 = m->t1 = llm_now_ms();
	jb_rawn(&m->text, text ? text : "", text ? n : 0);
	m->done = role != LLM_ASSISTANT;
	c->updated = m->t0;
	return m;
}

LlmCall *llm_call_add(LlmMsg *m)
{
	VX_GROW(m->calls, m->calls_cap, m->ncalls + 1);
	LlmCall *k = &m->calls[m->ncalls++];
	memset(k, 0, sizeof *k);
	jb_rawn(&k->id, "", 0);
	jb_rawn(&k->name, "", 0);
	jb_rawn(&k->args, "", 0);
	return k;
}

void llm_chat_set_title(LlmChat *c)
{
	for (u32 i = 0; i < c->n; i++) {
		if (c->m[i].role != LLM_USER)
			continue;
		const char *s = c->m[i].text.b ? c->m[i].text.b : "";
		while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '#')
			s++;
		size_t l = strcspn(s, "\n");
		jb_reset(&c->title);
		if (l > 60) {
			/* at a word */
			size_t k = 60;
			while (k > 30 && s[k] != ' ')
				k--;
			while (k > 0 && (s[k] & 0xC0) == 0x80)
				k--;
			jb_rawn(&c->title, s, k);
			jb_raw(&c->title, "...");
		} else
			jb_rawn(&c->title, s, l);
		return;
	}
}

static const char *role_name(int r)
{
	return r == LLM_USER ? "user" : r == LLM_ASSISTANT ? "assistant" : r == LLM_TOOL ? "tool" : "note";
}

static void jb_text(JBuf *o, const JBuf *s) { jb_strn(o, s->b ? s->b : "", s->n); }

void llm_chat_json(const LlmChat *c, JBuf *o)
{
	jb_raw(o, "{\"id\":");
	jb_str(o, c->id);
	jb_raw(o, ",\"title\":");
	jb_text(o, &c->title);
	jb_raw(o, ",\"created\":");
	jb_int(o, c->created);
	jb_raw(o, ",\"updated\":");
	jb_int(o, c->updated);
	jb_raw(o, ",\"model\":");
	jb_str(o, c->model);
	jb_raw(o, ",\"messages\":[");
	for (u32 i = 0; i < c->n; i++) {
		const LlmMsg *m = &c->m[i];
		jb_raw(o, i ? ",\n{\"role\":" : "\n{\"role\":");
		jb_str(o, role_name(m->role));
		jb_raw(o, ",\"content\":");
		jb_text(o, &m->text);
		if (m->think.n) {
			jb_raw(o, ",\"reasoning\":");
			jb_text(o, &m->think);
		}
		if (m->ncalls) {
			jb_raw(o, ",\"tool_calls\":[");
			for (u32 k = 0; k < m->ncalls; k++) {
				jb_raw(o, k ? ",{\"id\":" : "{\"id\":");
				jb_text(o, &m->calls[k].id);
				jb_raw(o, ",\"name\":");
				jb_text(o, &m->calls[k].name);
				jb_raw(o, ",\"arguments\":");
				jb_text(o, &m->calls[k].args);
				jb_raw(o, "}");
			}
			jb_raw(o, "]");
		}
		if (m->role == LLM_TOOL) {
			jb_raw(o, ",\"tool_call_id\":");
			jb_text(o, &m->call_id);
			jb_raw(o, ",\"name\":");
			jb_text(o, &m->name);
		}
		if (m->error)
			jb_raw(o, ",\"error\":true");
		jb_raw(o, ",\"t0\":");
		jb_int(o, m->t0);
		jb_raw(o, ",\"t1\":");
		jb_int(o, m->t1);
		jb_raw(o, "}");
	}
	jb_raw(o, "]}\n");
}

/* append string token t, unescaped */
static void jb_tok(JBuf *o, const Json *j, u32 t)
{
	if (json_type(j, t) != JS_STR) {
		jb_rawn(o, "", 0);
		return;
	}
	u32 n = j->end[t] - j->start[t];
	VX_GROW(o->b, o->cap, o->n + n + 1);
	o->n += json_unescape(j->src + j->start[t], n, o->b + o->n);
	o->b[o->n] = 0;
}

bool llm_chat_parse(LlmChat *c, const char *s, size_t n)
{
	memset(c, 0, sizeof *c);
	Json j = {0};
	if (json_parse(&j, s, (u32)n) != 0 || json_type(&j, 0) != JS_OBJ) {
		json_free(&j);
		return false;
	}
	JBuf t = {0};
	jb_tok(&t, &j, json_get(&j, 0, "id"));
	snprintf(c->id, sizeof c->id, "%s", t.b ? t.b : "");
	jb_reset(&t);
	jb_tok(&t, &j, json_get(&j, 0, "model"));
	snprintf(c->model, sizeof c->model, "%s", t.b ? t.b : "");
	jb_free(&t);
	jb_tok(&c->title, &j, json_get(&j, 0, "title"));
	c->created = json_int(&j, json_get(&j, 0, "created"), 0);
	c->updated = json_int(&j, json_get(&j, 0, "updated"), c->created);
	u32 ms = json_get(&j, 0, "messages");
	for (u32 i = 0; json_type(&j, ms) == JS_ARR && i < j.size[ms]; i++) {
		u32 o = json_at(&j, ms, i);
		u32 r = json_get(&j, o, "role");
		int role = json_eq(&j, r, "user") ? LLM_USER : json_eq(&j, r, "assistant") ? LLM_ASSISTANT
		                                           : json_eq(&j, r, "tool")        ? LLM_TOOL
		                                                                           : LLM_NOTE;
		LlmMsg *m = llm_msg_add(c, role, NULL, 0);
		m->done = true;
		jb_reset(&m->text);
		jb_tok(&m->text, &j, json_get(&j, o, "content"));
		if (json_get(&j, o, "reasoning") != JSON_NIL)
			jb_tok(&m->think, &j, json_get(&j, o, "reasoning"));
		u32 tc = json_get(&j, o, "tool_calls");
		for (u32 k = 0; json_type(&j, tc) == JS_ARR && k < j.size[tc]; k++) {
			u32 co = json_at(&j, tc, k);
			LlmCall *cl = llm_call_add(m);
			jb_tok(&cl->id, &j, json_get(&j, co, "id"));
			jb_tok(&cl->name, &j, json_get(&j, co, "name"));
			jb_tok(&cl->args, &j, json_get(&j, co, "arguments"));
		}
		if (role == LLM_TOOL) {
			jb_tok(&m->call_id, &j, json_get(&j, o, "tool_call_id"));
			jb_tok(&m->name, &j, json_get(&j, o, "name"));
		}
		m->error = json_bool(&j, json_get(&j, o, "error"), false);
		m->t0 = json_int(&j, json_get(&j, o, "t0"), 0);
		m->t1 = json_int(&j, json_get(&j, o, "t1"), m->t0);
	}
	c->updated = json_int(&j, json_get(&j, 0, "updated"), c->created);
	json_free(&j);
	return c->id[0] != 0;
}

static void mkdirs(const char *path)
{
	char d[4096];
	snprintf(d, sizeof d, "%s", path);
	for (char *s = d + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(d, 0755);
			*s = '/';
		}
	mkdir(d, 0755);
}

bool llm_chat_save(const LlmChat *c, const char *dir)
{
	mkdirs(dir);
	char p[4096], tmp[4200];
	snprintf(p, sizeof p, "%s/%s.json", dir, c->id);
	snprintf(tmp, sizeof tmp, "%s.tmp", p);
	JBuf b = {0};
	llm_chat_json(c, &b);
	bool ok = vx_write_file(tmp, b.b, b.n) == 0 && rename(tmp, p) == 0;
	jb_free(&b);
	return ok;
}

bool llm_chat_load(LlmChat *c, const char *path)
{
	size_t n = 0;
	char *s = vx_read_file(path, &n);
	if (!s)
		return false;
	bool ok = llm_chat_parse(c, s, n);
	free(s);
	if (!ok)
		llm_chat_free(c);
	return ok;
}

/* ---- the request ------------------------------------------------------------------------------------ */

void llm_request(const LlmCfg *cfg, const LlmChat *c, JBuf *o)
{
	jb_raw(o, "{");
	if (cfg->model[0]) {
		jb_raw(o, "\"model\":");
		jb_str(o, cfg->model);
		jb_raw(o, ",");
	}
	jb_raw(o, "\"stream\":true,\"cache_prompt\":true,\"messages\":[");
	bool first = true;
	if (cfg->system && *cfg->system) {
		jb_raw(o, "{\"role\":\"system\",\"content\":");
		jb_str(o, cfg->system);
		jb_raw(o, "}");
		first = false;
	}
	for (u32 i = 0; i < c->n; i++) {
		const LlmMsg *m = &c->m[i];
		if (m->role == LLM_NOTE || (m->role == LLM_ASSISTANT && !m->text.n && !m->ncalls))
			continue;
		jb_raw(o, first ? "{\"role\":" : ",{\"role\":");
		first = false;
		jb_str(o, role_name(m->role));
		jb_raw(o, ",\"content\":");
		jb_text(o, &m->text);
		if (m->role == LLM_ASSISTANT && m->ncalls) {
			jb_raw(o, ",\"tool_calls\":[");
			for (u32 k = 0; k < m->ncalls; k++) {
				jb_raw(o, k ? ",{\"id\":" : "{\"id\":");
				jb_text(o, &m->calls[k].id);
				jb_raw(o, ",\"type\":\"function\",\"function\":{\"name\":");
				jb_text(o, &m->calls[k].name);
				jb_raw(o, ",\"arguments\":");
				jb_text(o, &m->calls[k].args);
				jb_raw(o, "}}");
			}
			jb_raw(o, "]");
		}
		if (m->role == LLM_TOOL) {
			jb_raw(o, ",\"tool_call_id\":");
			jb_text(o, &m->call_id);
		}
		jb_raw(o, "}");
	}
	jb_raw(o, "],\"tools\":");
	llmt_schema(&cfg->tools, o);
	jb_raw(o, "}");
}

/* ---- the response ------------------------------------------------------------------------------------- */

bool llm_apply_event(LlmMsg *m, const char *s, size_t n, bool *finished)
{
	Json j = {0};
	if (json_parse(&j, s, (u32)n) != 0 || json_type(&j, 0) != JS_OBJ) {
		json_free(&j);
		return true; /* a keep-alive or something else: ignored */
	}
	u32 e = json_get(&j, 0, "error");
	if (e != JSON_NIL) {
		jb_reset(&m->text);
		u32 msg = json_type(&j, e) == JS_OBJ ? json_get(&j, e, "message") : e;
		jb_tok(&m->text, &j, msg);
		if (!m->text.n)
			jb_raw(&m->text, "the server answered with an error");
		json_free(&j);
		return false;
	}
	u32 ch = json_at(&j, json_get(&j, 0, "choices"), 0);
	u32 d = json_get(&j, ch, "delta");
	if (d == JSON_NIL)
		d = json_get(&j, ch, "message"); /* not streamed */
	if (d != JSON_NIL) {
		jb_tok(&m->text, &j, json_get(&j, d, "content"));
		u32 r = json_get(&j, d, "reasoning_content");
		if (r == JSON_NIL)
			r = json_get(&j, d, "reasoning");
		if (json_type(&j, r) == JS_STR)
			jb_tok(&m->think, &j, r);
		u32 tc = json_get(&j, d, "tool_calls");
		for (u32 k = 0; json_type(&j, tc) == JS_ARR && k < j.size[tc]; k++) {
			u32 co = json_at(&j, tc, k);
			i64 idx = json_int(&j, json_get(&j, co, "index"), (i64)k);
			if (idx < 0 || idx > 64)
				continue;
			while (m->ncalls <= (u32)idx)
				llm_call_add(m);
			LlmCall *cl = &m->calls[idx];
			jb_tok(&cl->id, &j, json_get(&j, co, "id"));
			u32 fn = json_get(&j, co, "function");
			jb_tok(&cl->name, &j, json_get(&j, fn, "name"));
			u32 a = json_get(&j, fn, "arguments");
			if (json_type(&j, a) == JS_STR)
				jb_tok(&cl->args, &j, a);
			else if (json_type(&j, a) == JS_OBJ) /* some servers send an object */
				jb_rawn(&cl->args, j.src + j.start[a], j.end[a] - j.start[a]);
		}
	}
	u32 fr = json_get(&j, ch, "finish_reason");
	if (json_type(&j, fr) == JS_STR && finished)
		*finished = true;
	json_free(&j);
	return true;
}

static void body_bytes(LlmHttp *h, const char *s, size_t n, LlmEventFn fn, void *ctx)
{
	if (!h->sse) {
		jb_rawn(&h->body, s, n);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] != '\n') {
			jb_rawn(&h->line, s + i, 1);
			continue;
		}
		char *l = h->line.b ? h->line.b : "";
		u32 ln = h->line.n;
		if (ln && l[ln - 1] == '\r')
			ln--;
		if (ln >= 5 && !strncmp(l, "data:", 5)) {
			const char *v = l + 5;
			u32 vn = ln - 5;
			while (vn && *v == ' ')
				v++, vn--;
			if (vn == 6 && !strncmp(v, "[DONE]", 6))
				h->done_seen = true;
			else if (vn)
				fn(ctx, v, vn);
		}
		jb_reset(&h->line);
	}
}

void llm_http_feed(LlmHttp *h, const char *s, size_t n, LlmEventFn fn, void *ctx)
{
	size_t i = 0;
	while (i < n && h->stage == 0) {
		jb_rawn(&h->head, s + i, 1);
		i++;
		if (h->head.n >= 4 && !memcmp(h->head.b + h->head.n - 4, "\r\n\r\n", 4)) {
			h->stage = 1;
			sscanf(h->head.b, "HTTP/%*s %d", &h->status);
			for (char *p = h->head.b; *p; p++)
				*p = (char)tolower((unsigned char)*p);
			h->chunked = strstr(h->head.b, "transfer-encoding: chunked") != NULL;
			h->sse = strstr(h->head.b, "text/event-stream") != NULL;
			h->chunk_left = -1;
			jb_reset(&h->line);
		}
	}
	if (h->stage != 1 || i >= n)
		return;
	if (!h->chunked) {
		body_bytes(h, s + i, n - i, fn, ctx);
		return;
	}
	while (i < n && h->stage == 1) {
		if (h->chunk_left < 0) {
			/* a size line, or the empty line that ends a chunk */
			if (s[i] == '\n') {
				if (h->szl.n) {
					h->chunk_left = (i64)strtoll(h->szl.b, NULL, 16);
					if (h->chunk_left == 0)
						h->stage = 2;
				}
				jb_reset(&h->szl);
			} else if (s[i] != '\r')
				jb_rawn(&h->szl, s + i, 1);
			i++;
			continue;
		}
		size_t k = (size_t)VX_MIN((i64)(n - i), h->chunk_left);
		body_bytes(h, s + i, k, fn, ctx);
		i += k;
		h->chunk_left -= (i64)k;
		if (h->chunk_left == 0)
			h->chunk_left = -1;
	}
}

void llm_http_free(LlmHttp *h)
{
	jb_free(&h->head);
	jb_free(&h->line);
	jb_free(&h->szl);
	jb_free(&h->body);
	memset(h, 0, sizeof *h);
}

/* ---- the server ------------------------------------------------------------------------------------------ */

typedef struct {
	char host[200], port[16], prefix[200];
} Url;

static bool url_parse(const char *s, Url *u)
{
	memset(u, 0, sizeof *u);
	if (!strncmp(s, "http://", 7))
		s += 7;
	else if (strstr(s, "://"))
		return false; /* https and the rest: not here */
	size_t hl = strcspn(s, ":/");
	if (!hl || hl >= sizeof u->host)
		return false;
	memcpy(u->host, s, hl);
	s += hl;
	snprintf(u->port, sizeof u->port, "80");
	if (*s == ':') {
		size_t pl = strcspn(s + 1, "/");
		if (!pl || pl >= sizeof u->port)
			return false;
		memcpy(u->port, s + 1, pl);
		u->port[pl] = 0;
		s += 1 + pl;
	}
	snprintf(u->prefix, sizeof u->prefix, "%s", s);
	size_t pl = strlen(u->prefix);
	while (pl && u->prefix[pl - 1] == '/')
		u->prefix[--pl] = 0;
	/* "http://host:8080/v1" is the same server */
	if (pl >= 3 && !strcmp(u->prefix + pl - 3, "/v1"))
		u->prefix[pl - 3] = 0;
	return true;
}

/* A connected socket, or -1 with why. */
static int connect_to(const Url *u, int timeout_ms, char *why, size_t wcap)
{
	struct addrinfo hints = {0}, *ai = NULL;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(u->host, u->port, &hints, &ai);
	if (rc != 0) {
		snprintf(why, wcap, "%s: %s", u->host, gai_strerror(rc));
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *a = ai; a && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
			snprintf(why, wcap, "%s", strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		struct pollfd p = {fd, POLLOUT, 0};
		int err = 0;
		socklen_t el = sizeof err;
		if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
			snprintf(why, wcap, "%s", err ? strerror(err) : "no answer");
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(ai);
	if (fd >= 0) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	}
	return fd;
}

typedef struct {
	LlmRun *r;
	u32 mi;
	bool failed, finished;
} Sink;

static void on_event(void *ctx, const char *json, size_t n)
{
	Sink *k = ctx;
	LlmRun *r = k->r;
	r->lock(r->ctx);
	if (!llm_apply_event(&r->chat->m[k->mi], json, n, &k->finished))
		k->failed = true;
	r->unlock(r->ctx);
	r->changed(r->ctx);
}

/* Ask the server; the answer streams into message mi. false: it failed
 * (why says so), or the turn was stopped. */
static bool server_reply(LlmRun *r, int fd, const Url *u, u32 mi, char *why, size_t wcap)
{
	JBuf body = {0}, req = {0};
	llm_request(r->cfg, r->chat, &body);
	char head[1024];
	int hn = snprintf(head, sizeof head,
	                  "POST %s/v1/chat/completions HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\n"
	                  "Accept: text/event-stream\r\nContent-Length: %u\r\nConnection: close\r\n%s%s%s\r\n",
	                  u->prefix, u->host, u->port, body.n, r->cfg->api_key[0] ? "Authorization: Bearer " : "",
	                  r->cfg->api_key, r->cfg->api_key[0] ? "\r\n" : "");
	jb_rawn(&req, head, (size_t)hn);
	jb_rawn(&req, body.b, body.n);
	jb_free(&body);
	bool ok = true;
	for (u32 off = 0; off < req.n && ok;) {
		struct pollfd p = {fd, POLLOUT, 0};
		poll(&p, 1, 100);
		ssize_t w = send(fd, req.b + off, req.n - off, MSG_NOSIGNAL);
		if (w > 0)
			off += (u32)w;
		else if (w < 0 && errno != EAGAIN && errno != EINTR) {
			snprintf(why, wcap, "sending the request: %s", strerror(errno));
			ok = false;
		}
		if (r->cancel)
			ok = false;
	}
	jb_free(&req);
	LlmHttp h = {0};
	Sink k = {r, mi, false, false};
	u64 idle_until = vx_now_ns() + 600ull * 1000000000ull; /* a model loading can take minutes */
	while (ok && !r->cancel && h.stage != 2 && !h.done_seen) {
		struct pollfd p = {fd, POLLIN, 0};
		int pr = poll(&p, 1, 100);
		if (pr == 0) {
			if (vx_now_ns() > idle_until) {
				snprintf(why, wcap, "the server stopped answering");
				ok = false;
			}
			continue;
		}
		char b[16384];
		ssize_t n = recv(fd, b, sizeof b, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			break;
		idle_until = vx_now_ns() + 600ull * 1000000000ull;
		llm_http_feed(&h, b, (size_t)n, on_event, &k);
	}
	if (ok && !r->cancel) {
		if (h.stage == 0) {
			snprintf(why, wcap, "the server closed the connection without an answer");
			ok = false;
		} else if (h.status != 200) {
			/* an error body: {"error":{"message":...}} or text */
			LlmMsg tmp = {0};
			if (h.body.n && !llm_apply_event(&tmp, h.body.b, h.body.n, NULL))
				snprintf(why, wcap, "HTTP %d: %s", h.status, tmp.text.b);
			else
				snprintf(why, wcap, "HTTP %d%s%.200s", h.status, h.body.n ? ": " : "", h.body.n ? h.body.b : "");
			jb_free(&tmp.text);
			jb_free(&tmp.think);
			ok = false;
		} else if (!h.sse && h.body.n) {
			on_event(&k, h.body.b, h.body.n); /* not streamed: one completion */
		}
		if (ok && k.failed) {
			r->lock(r->ctx);
			snprintf(why, wcap, "%s", r->chat->m[mi].text.b ? r->chat->m[mi].text.b : "error");
			jb_reset(&r->chat->m[mi].text);
			r->unlock(r->ctx);
			ok = false;
		}
	}
	llm_http_free(&h);
	return ok && !r->cancel;
}

/* ---- the mock model --------------------------------------------------------------------------------------- */

typedef struct {
	JBuf text;
	struct {
		const char *name;
		JBuf args;
	} call[2];
	int ncall;
} Plan;

static void plan_call(Plan *p, const char *name, const char *fmt_json)
{
	if (p->ncall >= 2)
		return;
	p->call[p->ncall].name = name;
	jb_raw(&p->call[p->ncall].args, fmt_json);
	p->ncall++;
}

static bool ci_has(const char *s, const char *w)
{
	size_t wl = strlen(w);
	for (; *s; s++)
		if (!strncasecmp(s, w, wl))
			return true;
	return false;
}

/* the text between the first pair of backticks (not a fence) */
static bool backticked(const char *s, char *out, size_t cap)
{
	const char *a = strchr(s, '`');
	while (a && a[1] == '`')
		a = strchr(a + 2, '`');
	if (!a)
		return false;
	const char *b = strchr(a + 1, '`');
	if (!b || b == a + 1)
		return false;
	snprintf(out, cap, "%.*s", (int)(b - a - 1), a + 1);
	return true;
}

static bool quoted(const char *s, char *out, size_t cap)
{
	const char *a = strpbrk(s, "\"'");
	if (!a)
		return false;
	const char *b = strchr(a + 1, *a);
	if (!b || b == a + 1)
		return false;
	snprintf(out, cap, "%.*s", (int)(b - a - 1), a + 1);
	return true;
}

/* the word after one of the search verbs ("find foo", "where is foo defined") */
static bool search_term(const char *s, char *out, size_t cap)
{
	if (backticked(s, out, cap) || quoted(s, out, cap))
		return true;
	static const char *const skip[] = {"for", "is", "are", "the", "a", "an", "all", "uses", "of", "usages", "calls", "to", "where", "defined", "declared"};
	static const char *const verbs[] = {"search", "find", "grep", "where", "look for", "locate"};
	for (size_t v = 0; v < VX_COUNT(verbs); v++) {
		for (const char *p = s; *p; p++) {
			if (strncasecmp(p, verbs[v], strlen(verbs[v])) || (p > s && isalnum((unsigned char)p[-1])))
				continue;
			const char *q = p + strlen(verbs[v]);
			for (;;) {
				while (*q && !isalnum((unsigned char)*q) && *q != '_')
					q++;
				size_t l = 0;
				while (q[l] && (isalnum((unsigned char)q[l]) || q[l] == '_' || q[l] == '.'))
					l++;
				if (!l)
					break;
				while (l && q[l - 1] == '.')
					l--;
				bool sk = false;
				for (size_t k = 0; k < VX_COUNT(skip); k++)
					sk |= strlen(skip[k]) == l && !strncasecmp(q, skip[k], l);
				if (!sk) {
					snprintf(out, cap, "%.*s", (int)l, q);
					return true;
				}
				q += l;
			}
		}
	}
	return false;
}

/* a word of the question that is a file or folder of the project */
static int project_path(const char *root, const char *s, char *out, size_t cap)
{
	for (const char *p = s; *p;) {
		while (*p && (isspace((unsigned char)*p) || strchr("`'\"(<[", *p)))
			p++;
		size_t l = 0;
		while (p[l] && !isspace((unsigned char)p[l]) && !strchr("`'\")>]", p[l]))
			l++;
		size_t k = l;
		while (k && strchr(".,:;?!", p[k - 1]))
			k--;
		if (k && (memchr(p, '.', k) || memchr(p, '/', k)) && k < 1000) {
			char full[2048];
			snprintf(full, sizeof full, "%s/%.*s", root, (int)k, p);
			struct stat st;
			if (strstr(full, "..") == NULL && stat(full, &st) == 0) {
				snprintf(out, cap, "%.*s", (int)k, p);
				return S_ISDIR(st.st_mode) ? 2 : 1;
			}
		}
		p += l ? l : 1;
	}
	return 0;
}

static void jstr_into(JBuf *o, const char *key, const char *v, bool comma)
{
	if (comma)
		jb_raw(o, ",");
	jb_str(o, key);
	jb_raw(o, ":");
	jb_str(o, v);
}

static void mock_plan(const LlmRun *r, const char *q, Plan *p)
{
	const char *root = r->cfg->tools.root ? r->cfg->tools.root : ".";
	char w[1024];
	JBuf a = {0};
	bool can_exec = !r->cfg->tools.read_only && !r->cfg->tools.no_exec;
	if (can_exec && backticked(q, w, sizeof w) &&
	    (ci_has(q, "run") || ci_has(q, "exec") || ci_has(q, "build") || ci_has(q, "test") || ci_has(q, "make"))) {
		jb_raw(&p->text, "Running `");
		jb_raw(&p->text, w);
		jb_raw(&p->text, "` in the project.\n");
		jb_raw(&a, "{");
		jstr_into(&a, "cmd", w, false);
		jb_raw(&a, ",\"timeout_ms\":60000}");
		plan_call(p, "exec", a.b);
	} else if ((ci_has(q, "search") || ci_has(q, "find") || ci_has(q, "grep") || ci_has(q, "where") ||
	            ci_has(q, "look for") || ci_has(q, "locate")) &&
	           search_term(q, w, sizeof w)) {
		jb_raw(&p->text, "Searching the project for `");
		jb_raw(&p->text, w);
		jb_raw(&p->text, "`.\n");
		jb_raw(&a, "{\"path\":\".\"");
		jstr_into(&a, "re", w, true);
		jb_raw(&a, ",\"fixed\":true,\"i\":true,\"max\":40}");
		plan_call(p, "search", a.b);
	} else {
		int k = project_path(root, q, w, sizeof w);
		if (k == 1) {
			jb_raw(&p->text, "Let me read `");
			jb_raw(&p->text, w);
			jb_raw(&p->text, "`.\n");
			jb_raw(&a, "{");
			jstr_into(&a, "path", w, false);
			jb_raw(&a, ",\"limit\":400}");
			plan_call(p, "read", a.b);
		} else if (k == 2) {
			jb_raw(&p->text, "Let me look inside `");
			jb_raw(&p->text, w);
			jb_raw(&p->text, "`.\n");
			jb_raw(&a, "{");
			jstr_into(&a, "path", w, false);
			jb_raw(&a, ",\"depth\":1}");
			plan_call(p, "ls", a.b);
		} else {
			jb_raw(&p->text, "I'll start with the project's top folder.\n");
			plan_call(p, "ls", "{\"path\":\".\",\"depth\":1}");
			static const char *const readmes[] = {"README.md", "README", "readme.md", "README.txt"};
			for (size_t i = 0; i < VX_COUNT(readmes); i++) {
				char f[4096];
				struct stat st;
				snprintf(f, sizeof f, "%s/%s", root, readmes[i]);
				if (stat(f, &st) == 0 && S_ISREG(st.st_mode)) {
					jb_reset(&a);
					jb_raw(&a, "{");
					jstr_into(&a, "path", readmes[i], false);
					jb_raw(&a, ",\"limit\":60}");
					plan_call(p, "read", a.b);
					break;
				}
			}
		}
	}
	jb_free(&a);
}

static const char *fence_lang(const char *path)
{
	const char *b = strrchr(path, '/');
	b = b ? b + 1 : path;
	if (!strcmp(b, "CMakeLists.txt"))
		return "cmake";
	if (!strcmp(b, "Makefile") || !strcmp(b, "GNUmakefile"))
		return "make";
	const char *e = strrchr(b, '.');
	if (!e)
		return "";
	e++;
	static const char *const map[][2] = {
	    {"c", "c"},       {"h", "c"},         {"cc", "cpp"},       {"cpp", "cpp"},   {"hpp", "cpp"}, {"cxx", "cpp"},
	    {"rs", "rust"},   {"py", "python"},   {"md", "markdown"}, {"sh", "sh"},     {"js", "js"},   {"ts", "ts"},
	    {"json", "json"}, {"toml", "toml"},   {"yml", "yaml"},    {"yaml", "yaml"}, {"java", "java"}, {"kt", "kotlin"},
	    {"glsl", "glsl"}, {"comp", "glsl"},   {"frag", "glsl"},   {"vert", "glsl"}, {"cmake", "cmake"}, {"go", "go"},
	    {"lisp", "lisp"}, {"el", "elisp"},    {"pl", "perl"},     {"S", "asm"},     {"s", "asm"},   {"asm", "asm"},
	};
	for (size_t i = 0; i < VX_COUNT(map); i++)
		if (!strcmp(e, map[i][0]))
			return map[i][1];
	return "";
}

/* the n-th line of s (and its length) */
static const char *nth_line(const char *s, u32 i, u32 *len)
{
	for (u32 k = 0; k < i && s; k++) {
		s = strchr(s, '\n');
		if (s)
			s++;
	}
	if (!s || !*s)
		return NULL;
	*len = (u32)strcspn(s, "\n");
	return s;
}

static void md_esc_cell(JBuf *o, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		jb_rawn(o, s[i] == '|' ? "\\|" : s + i, 1 + (s[i] == '|'));
}

static void write_ls(JBuf *o, const char *path, const char *out)
{
	u32 nd = 0, nf = 0, rows = 0, total = 0, len;
	for (u32 i = 0; nth_line(out, i, &len); i++)
		total++;
	for (u32 i = 0; i < total; i++) {
		const char *l = nth_line(out, i, &len);
		if (len && l[len - 1] == '/')
			nd++;
		else
			nf++;
	}
	char h[300];
	snprintf(h, sizeof h, "`%s` holds **%u folder%s and %u file%s**:\n\n| Name | Kind | Size |\n|---|---|---:|\n",
	         path, nd, nd == 1 ? "" : "s", nf, nf == 1 ? "" : "s");
	jb_raw(o, h);
	for (u32 i = 0; i < total && rows < 30; i++, rows++) {
		const char *l = nth_line(out, i, &len);
		const char *arrow = memmem(l, len, "@ -> ", 5);
		jb_raw(o, "| `");
		if (len && l[len - 1] == '/') {
			md_esc_cell(o, l, len);
			jb_raw(o, "` | folder | |\n");
		} else if (arrow) {
			md_esc_cell(o, l, (size_t)(arrow - l));
			jb_raw(o, "` | link | |\n");
		} else {
			const char *sp = NULL;
			for (u32 q = len; q-- > 0 && !sp;)
				if (l[q] == ' ')
					sp = l + q;
			size_t nl = sp ? (size_t)(sp - l) : len;
			md_esc_cell(o, l, nl);
			long long sz = sp ? atoll(sp + 1) : 0;
			char b[64];
			if (sz >= 1 << 20)
				snprintf(b, sizeof b, "` | file | %.1f MB |\n", (double)sz / (1 << 20));
			else if (sz >= 1024)
				snprintf(b, sizeof b, "` | file | %.1f KB |\n", (double)sz / 1024);
			else
				snprintf(b, sizeof b, "` | file | %lld B |\n", sz);
			jb_raw(o, b);
		}
	}
	if (total > rows) {
		char b[64];
		snprintf(b, sizeof b, "\n...and %u more.\n", total - rows);
		jb_raw(o, b);
	}
	jb_raw(o, "\n");
}

static void write_read(JBuf *o, const char *path, const char *out, bool readme)
{
	/* the file's text without the [lines ...] footer */
	u32 n = (u32)strlen(out), lines = 0, len;
	const char *foot = strstr(out, "[lines ");
	unsigned a = 0, b = 0, tot = 0;
	if (foot && sscanf(foot, "[lines %u-%u of %u]", &a, &b, &tot) == 3)
		n = (u32)(foot - out);
	for (u32 i = 0; i < n; i++)
		lines += out[i] == '\n';
	if (!tot)
		tot = lines;
	char h[400];
	if (readme) {
		/* its first paragraph, quoted */
		snprintf(h, sizeof h, "From `%s`:\n\n", path);
		jb_raw(o, h);
		bool started = false;
		for (u32 i = 0; i < lines; i++) {
			const char *l = nth_line(out, i, &len);
			if (!l)
				break;
			bool blank = !len || (len == 1 && l[0] == '\r');
			if (!started && (blank || l[0] == '#' || l[0] == '<' || l[0] == '[' || l[0] == '!'))
				continue;
			if (started && (blank || l[0] == '#' || !strncmp(l, "```", 3)))
				break;
			started = true;
			jb_raw(o, "> ");
			jb_rawn(o, l, len);
			jb_raw(o, "\n");
		}
		jb_raw(o, "\n");
		return;
	}
	const char *lang = fence_lang(path);
	snprintf(h, sizeof h, "`%s` has **%u line%s**%s%s%s.\n\n", path, tot, tot == 1 ? "" : "s", *lang ? " of " : "",
	         lang, *lang ? "" : "");
	jb_raw(o, h);
	/* what it defines: C-like functions, Rust and Python fns, Markdown headings */
	JBuf defs = {0};
	u32 nd = 0;
	for (u32 i = 0; i < lines && nd < 12; i++) {
		const char *l = nth_line(out, i, &len);
		if (!l || !len)
			continue;
		bool def = false;
		if (!strcmp(lang, "markdown"))
			def = l[0] == '#';
		else if (!strcmp(lang, "rust"))
			def = (!strncmp(l, "fn ", 3) || !strncmp(l, "pub fn ", 7) || !strncmp(l, "pub(crate) fn ", 14));
		else if (!strcmp(lang, "python"))
			def = !strncmp(l, "def ", 4) || !strncmp(l, "class ", 6);
		else if (!strcmp(lang, "c") || !strcmp(lang, "cpp")) {
			def = (isalpha((unsigned char)l[0]) || l[0] == '_') && memchr(l, '(', len) && l[len - 1] != ';' &&
			      strncmp(l, "typedef", 7) && strncmp(l, "return", 6) && strncmp(l, "else", 4) && strncmp(l, "#", 1);
			u32 nl;
			const char *nx = nth_line(out, i + 1, &nl);
			def = def && (l[len - 1] == '{' || l[len - 1] == ')' || (nx && nl && nx[0] == '{'));
		}
		if (!def)
			continue;
		u32 k = len;
		while (k && (l[k - 1] == '{' || l[k - 1] == ' ' || l[k - 1] == ':'))
			k--;
		jb_raw(&defs, "- `");
		jb_rawn(&defs, l, VX_MIN(k, 100u));
		char ln[32];
		snprintf(ln, sizeof ln, "` (line %u)\n", a ? a + i : i + 1);
		jb_raw(&defs, ln);
		nd++;
	}
	if (nd) {
		jb_raw(o, !strcmp(lang, "markdown") ? "Its sections:\n\n" : "What it defines:\n\n");
		jb_rawn(o, defs.b, defs.n);
		jb_raw(o, "\n");
	}
	jb_free(&defs);
	/* how it starts */
	jb_raw(o, "It starts:\n\n```");
	jb_raw(o, lang);
	jb_raw(o, "\n");
	u32 shown = 0;
	for (u32 i = 0; i < lines && shown < 14; i++, shown++) {
		const char *l = nth_line(out, i, &len);
		if (!l)
			break;
		jb_rawn(o, l, VX_MIN(len, 160u));
		jb_raw(o, "\n");
	}
	jb_raw(o, "```\n\n");
}

static void write_search(JBuf *o, const char *args, const char *out)
{
	Json j = {0};
	JBuf term = {0};
	if (json_parse(&j, args, (u32)strlen(args)) == 0)
		jb_tok(&term, &j, json_get(&j, 0, "re"));
	json_free(&j);
	u32 n = 0, len;
	if (strncmp(out, "no matches", 10))
		for (u32 i = 0;; i++) {
			const char *l = nth_line(out, i, &len);
			if (!l)
				break;
			n += l[0] != '[';
		}
	char h[400];
	if (!n) {
		snprintf(h, sizeof h, "Nothing in the project matches `%s`.\n\n", term.b ? term.b : "");
		jb_raw(o, h);
		jb_free(&term);
		return;
	}
	snprintf(h, sizeof h, "`%s` appears **%u time%s**:\n\n", term.b ? term.b : "", n, n == 1 ? "" : "s");
	jb_raw(o, h);
	u32 shown = 0;
	for (u32 i = 0; shown < 20; i++) {
		const char *l = nth_line(out, i, &len);
		if (!l)
			break;
		if (l[0] == '[')
			continue;
		/* path:line:text */
		const char *c1 = memchr(l, ':', len);
		const char *c2 = c1 ? memchr(c1 + 1, ':', (size_t)(l + len - c1 - 1)) : NULL;
		if (!c2) {
			jb_raw(o, "- ");
			jb_rawn(o, l, len);
			jb_raw(o, "\n");
			shown++;
			continue;
		}
		const char *t = c2 + 1;
		size_t tl = (size_t)(l + len - t);
		while (tl && (*t == ' ' || *t == '\t'))
			t++, tl--;
		jb_raw(o, "- [");
		jb_rawn(o, l, (size_t)(c2 - l));
		jb_raw(o, "](");
		jb_rawn(o, l, (size_t)(c2 - l));
		jb_raw(o, ") `");
		for (size_t k = 0; k < VX_MIN(tl, (size_t)90); k++)
			jb_rawn(o, t[k] == '`' ? "'" : t + k, 1);
		jb_raw(o, "`\n");
		shown++;
	}
	if (n > shown) {
		snprintf(h, sizeof h, "- ...and %u more\n", n - shown);
		jb_raw(o, h);
	}
	jb_raw(o, "\n");
	jb_free(&term);
}

static void write_exec(JBuf *o, const char *cmd, const char *out)
{
	const char *ex = strrchr(out, '[');
	char h[600];
	snprintf(h, sizeof h, "`%s` finished: **%.*s**.\n\n", cmd, ex ? (int)strcspn(ex + 1, "]") : 2, ex ? ex + 1 : "ok");
	jb_raw(o, h);
	u32 n = ex ? (u32)(ex - out) : (u32)strlen(out), lines = 0;
	for (u32 i = 0; i < n; i++)
		lines += out[i] == '\n';
	if (!n) {
		jb_raw(o, "It printed nothing.\n\n");
		return;
	}
	u32 from = lines > 30 ? lines - 30 : 0, len;
	if (from) {
		snprintf(h, sizeof h, "The last 30 of its %u lines:\n\n", lines);
		jb_raw(o, h);
	}
	jb_raw(o, "```text\n");
	for (u32 i = from; i < lines; i++) {
		const char *l = nth_line(out, i, &len);
		if (!l)
			break;
		jb_rawn(o, l, VX_MIN(len, 200u));
		jb_raw(o, "\n");
	}
	jb_raw(o, "```\n\n");
}

/* The answer once the tools have answered: each written up. */
static void mock_answer(const LlmRun *r, u32 user, JBuf *o)
{
	const LlmChat *c = r->chat;
	u32 ntools = 0;
	for (u32 i = user + 1; i < c->n; i++) {
		const LlmMsg *m = &c->m[i];
		if (m->role != LLM_TOOL)
			continue;
		ntools++;
		/* its call: the arguments */
		const char *args = "{}";
		for (u32 k = user + 1; k < i; k++)
			for (u32 q = 0; q < c->m[k].ncalls; q++)
				if (c->m[k].calls[q].id.b && m->call_id.b && !strcmp(c->m[k].calls[q].id.b, m->call_id.b))
					args = c->m[k].calls[q].args.b;
		const char *name = m->name.b ? m->name.b : "";
		const char *out = m->text.b ? m->text.b : "";
		Json j = {0};
		JBuf path = {0}, cmd = {0};
		if (json_parse(&j, args, (u32)strlen(args)) == 0) {
			jb_tok(&path, &j, json_get(&j, 0, "path"));
			jb_tok(&cmd, &j, json_get(&j, 0, "cmd"));
		}
		json_free(&j);
		const char *pth = path.b && *path.b ? path.b : ".";
		if (m->error && strcmp(name, "exec")) {
			jb_raw(o, "> **");
			jb_raw(o, name);
			jb_raw(o, " failed:** ");
			jb_raw(o, out);
			jb_raw(o, "\n\n");
		} else if (!strcmp(name, "ls"))
			write_ls(o, pth, out);
		else if (!strcmp(name, "read"))
			write_read(o, pth, out, ntools > 1 || strstr(pth, "README") || strstr(pth, "readme"));
		else if (!strcmp(name, "search"))
			write_search(o, args, out);
		else if (!strcmp(name, "exec"))
			write_exec(o, cmd.b ? cmd.b : "", out);
		else {
			jb_raw(o, "`");
			jb_raw(o, name);
			jb_raw(o, "` said: ");
			jb_raw(o, out);
			jb_raw(o, "\n\n");
		}
		jb_free(&path);
		jb_free(&cmd);
	}
	char h[600];
	snprintf(h, sizeof h,
	         "---\n*Written by the mock model: no model answers at %s. Start one, for example "
	         "`llama-server -m model.gguf --port 8080 --jinja`, and the next message goes to it.*\n",
	         r->cfg->server);
	jb_raw(o, h);
}

static void stream_text(LlmRun *r, u32 mi, const char *s, size_t n)
{
	int delay = r->cfg->mock_delay_ms;
	for (size_t i = 0; i < n && !r->cancel;) {
		size_t k = delay > 0 ? VX_MIN((size_t)6, n - i) : n - i;
		while (i + k < n && (s[i + k] & 0xC0) == 0x80)
			k++; /* whole characters */
		r->lock(r->ctx);
		jb_rawn(&r->chat->m[mi].text, s + i, k);
		r->unlock(r->ctx);
		r->changed(r->ctx);
		i += k;
		if (delay > 0)
			sleep_ms(delay);
	}
}

static void mock_reply(LlmRun *r, u32 mi)
{
	const LlmChat *c = r->chat;
	u32 user = 0;
	for (u32 i = 0; i < mi; i++)
		if (c->m[i].role == LLM_USER)
			user = i;
	bool answered = false;
	for (u32 i = user + 1; i < mi; i++)
		answered |= c->m[i].role == LLM_TOOL;
	if (r->cfg->mock_delay_ms > 0)
		sleep_ms(r->cfg->mock_delay_ms * 20); /* "thinking" */
	if (answered) {
		JBuf o = {0};
		mock_answer(r, user, &o);
		stream_text(r, mi, o.b, o.n);
		jb_free(&o);
		return;
	}
	Plan p = {0};
	mock_plan(r, c->m[user].text.b ? c->m[user].text.b : "", &p);
	stream_text(r, mi, p.text.b ? p.text.b : "", p.text.n);
	r->lock(r->ctx);
	LlmMsg *m = &r->chat->m[mi];
	for (int i = 0; i < p.ncall; i++) {
		LlmCall *k = llm_call_add(m);
		char id[48];
		snprintf(id, sizeof id, "mock-%u-%d", mi, i);
		jb_raw(&k->id, id);
		jb_raw(&k->name, p.call[i].name);
		jb_rawn(&k->args, p.call[i].args.b, p.call[i].args.n);
	}
	r->unlock(r->ctx);
	r->changed(r->ctx);
	jb_free(&p.text);
	for (int i = 0; i < 2; i++)
		jb_free(&p.call[i].args);
}

/* ---- the turn ------------------------------------------------------------------------------------------------ */

static void note(LlmRun *r, bool error, const char *fmt, const char *arg)
{
	char b[800];
	snprintf(b, sizeof b, fmt, arg);
	r->lock(r->ctx);
	LlmMsg *m = llm_msg_add(r->chat, LLM_NOTE, b, strlen(b));
	m->error = error;
	r->unlock(r->ctx);
	r->changed(r->ctx);
}

void llm_turn(LlmRun *r)
{
	const LlmCfg *cfg = r->cfg;
	Url u;
	bool have_url = url_parse(cfg->server, &u);
	int backend = cfg->backend;
	r->used = LLM_MOCK;
	r->why_mock[0] = 0;
	int steps = cfg->max_steps > 0 ? cfg->max_steps : 24;
	JBuf tout = {0};
	for (int step = 0; step < steps && !r->cancel; step++) {
		/* the model: connect each step (Connection: close) */
		int fd = -1;
		char why[400] = "";
		if (backend != LLM_MOCK) {
			if (!have_url)
				snprintf(why, sizeof why, "cannot read the server address %s", cfg->server);
			else
				fd = connect_to(&u, cfg->connect_ms > 0 ? cfg->connect_ms : 1500, why, sizeof why);
			if (fd < 0 && (backend == LLM_SERVER || step > 0)) {
				note(r, true, "The model server cannot be reached: %s", why);
				break;
			}
			if (fd < 0)
				snprintf(r->why_mock, sizeof r->why_mock, "%s: %s", cfg->server, why);
		}
		bool server = fd >= 0;
		if (step == 0)
			backend = server ? LLM_SERVER : LLM_MOCK; /* the whole turn with one */
		r->used = backend;
		r->lock(r->ctx);
		LlmMsg *m = llm_msg_add(r->chat, LLM_ASSISTANT, NULL, 0);
		u32 mi = r->chat->n - 1;
		snprintf(r->chat->model, sizeof r->chat->model, "%s",
		         server ? (cfg->model[0] ? cfg->model : cfg->server) : "mock");
		(void)m;
		r->unlock(r->ctx);
		r->changed(r->ctx);
		bool ok = true;
		if (server) {
			ok = server_reply(r, fd, &u, mi, why, sizeof why);
			close(fd);
		} else
			mock_reply(r, mi);
		r->lock(r->ctx);
		m = &r->chat->m[mi];
		m->done = true;
		m->t1 = llm_now_ms();
		/* calls without a name cannot be run: dropped */
		u32 k = 0;
		for (u32 i = 0; i < m->ncalls; i++) {
			if (m->calls[i].name.n) {
				LlmCall t = m->calls[i];
				m->calls[i] = m->calls[k];
				m->calls[k++] = t;
				if (!m->calls[k - 1].id.n) {
					char id[48];
					snprintf(id, sizeof id, "call-%u-%u", mi, k - 1);
					jb_raw(&m->calls[k - 1].id, id);
				}
			}
		}
		for (u32 i = k; i < m->ncalls; i++) {
			jb_free(&m->calls[i].id);
			jb_free(&m->calls[i].name);
			jb_free(&m->calls[i].args);
		}
		m->ncalls = k;
		u32 ncalls = m->ncalls;
		r->unlock(r->ctx);
		r->changed(r->ctx);
		if (r->cancel)
			break;
		if (!ok) {
			note(r, true, "The model failed: %s", why);
			break;
		}
		if (!ncalls) {
			jb_free(&tout);
			return;
		}
		/* the tools, one after the other */
		for (u32 i = 0; i < ncalls && !r->cancel; i++) {
			r->lock(r->ctx);
			LlmCall *cl = &r->chat->m[mi].calls[i];
			char *name = vx_strdup(cl->name.b), *args = vx_strndup(cl->args.b ? cl->args.b : "", cl->args.n);
			LlmMsg *t = llm_msg_add(r->chat, LLM_TOOL, NULL, 0);
			jb_raw(&t->call_id, cl->id.b);
			jb_raw(&t->name, name);
			t->done = false;
			u32 ti = r->chat->n - 1;
			r->unlock(r->ctx);
			r->changed(r->ctx);
			bool tok = llmt_call(&cfg->tools, name, args, strlen(args), &tout);
			r->lock(r->ctx);
			t = &r->chat->m[ti];
			jb_reset(&t->text);
			jb_rawn(&t->text, tout.b, tout.n);
			t->error = !tok;
			t->done = true;
			t->t1 = llm_now_ms();
			r->unlock(r->ctx);
			r->changed(r->ctx);
			free(name);
			free(args);
		}
		if (step + 1 == steps) {
			char sn[16];
			snprintf(sn, sizeof sn, "%d", steps);
			note(r, true, "Stopped after %s model calls in one turn (the limit).", sn);
		}
	}
	if (r->cancel)
		note(r, false, "%s", "Stopped.");
	jb_free(&tout);
}
