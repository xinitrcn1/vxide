/* LSP client: JSON-RPC 2.0 over a child's stdio, non-blocking. */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"
#include "json.h"
#include "syntax.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { RQ_INIT = 1, RQ_SHUTDOWN, RQ_COMPLETION, RQ_HOVER, RQ_DEFINITION, RQ_SEMANTIC, RQ_REFERENCES };

typedef struct {
	char *b;
	u32 off, n, cap;
} RBuf;

struct Lsp {
	pid_t pid;
	int fd_in, fd_out, fd_err;
	bool out_eof, err_eof, reaped, exit_reported;
	bool init_done; /* initialize answered and `initialized` sent */
	bool utf8;
	bool stopping;
	char *root_uri;
	char *settings; /* initializationOptions / workspace/configuration reply (JSON), or NULL */

	JBuf out; /* framed bytes waiting to be written */
	u32 out_off;
	JBuf pre; /* framed notifications queued before initialize finished */
	JBuf msg; /* scratch for building one message body */

	RBuf in, err;
	i32 next_id;

	/* in-flight requests (SoA) */
	u32 rq_n, rq_cap;
	i32 *rq_id;
	u8 *rq_kind;
	char **rq_path;

	/* open documents (SoA), for UTF-16 conversion */
	u32 doc_n, doc_cap;
	char **doc_path;
	char **doc_text;
	u32 *doc_len;

	/* semantic token legend mapped to TK_* */
	u8 leg_cls[64];
	u32 leg_n;
	u32 ro_bit; /* "readonly" modifier bit in the server legend */

	/* event storage, reset by every lsp_next */
	Json js;
	Pool pool;
	LspDiags diags;
	u32 diags_cap;
	LspCompletion comp;
	u32 comp_cap;
	LspLocations locs;
	u32 locs_cap;
	LspSemantic sem;
	u32 sem_cap;
	char *scratch; /* file text read for conversions */
};

/* ---------- small helpers ---------- */

u32 lsp_utf16_to_byte(const char *s, u32 n, u32 col16)
{
	u32 i = 0, u = 0;
	while (i < n) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		u32 units = cp >= 0x10000 ? 2 : 1;
		if (u + units > col16)
			break;
		u += units;
		i += k;
	}
	return i;
}

u32 lsp_byte_to_utf16(const char *s, u32 n, u32 col)
{
	if (col > n)
		col = n;
	u32 i = 0, u = 0;
	while (i < col) {
		u32 cp;
		u32 k = utf8_decode(s + i, n - i, &cp);
		u += cp >= 0x10000 ? 2 : 1;
		i += k;
	}
	return u;
}

char *lsp_path_to_uri(const char *path)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(path);
	char *u = vx_alloc(8 + n * 3 + 1);
	char *o = u;
	memcpy(o, "file://", 7);
	o += 7;
	for (size_t i = 0; i < n; i++) {
		u8 c = (u8)path[i];
		bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		            (c >= '0' && c <= '9') || strchr("-._~/", c);
		if (keep && c) {
			*o++ = (char)c;
		} else {
			*o++ = '%';
			*o++ = hex[c >> 4];
			*o++ = hex[c & 15];
		}
	}
	*o = 0;
	return u;
}

static int hexd(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *lsp_uri_to_path(const char *uri, size_t n)
{
	if (n >= 7 && memcmp(uri, "file://", 7) == 0) {
		uri += 7;
		n -= 7;
		if (n >= 9 && memcmp(uri, "localhost", 9) == 0) {
			uri += 9;
			n -= 9;
		}
	}
	char *p = vx_alloc(n + 1), *o = p;
	for (size_t i = 0; i < n; i++) {
		if (uri[i] == '%' && i + 2 < n) {
			int a = hexd(uri[i + 1]);
			int b = hexd(uri[i + 2]);
			if (a >= 0 && b >= 0) {
				*o++ = (char)(a * 16 + b);
				i += 2;
				continue;
			}
		}
		*o++ = uri[i];
	}
	*o = 0;
	return p;
}

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_cloexec(int fd)
{
	int fl = fcntl(fd, F_GETFD);
	if (fl >= 0)
		fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static void rbuf_compact(RBuf *r)
{
	if (r->off == 0)
		return;
	memmove(r->b, r->b + r->off, r->n - r->off);
	r->n -= r->off;
	r->off = 0;
}

/* Read all available bytes; returns false on EOF/error. */
static bool rbuf_fill(RBuf *r, int fd)
{
	rbuf_compact(r);
	for (;;) {
		VX_GROW(r->b, r->cap, r->n + 65536);
		ssize_t k = read(fd, r->b + r->n, r->cap - r->n);
		if (k > 0) {
			r->n += (u32)k;
			continue;
		}
		if (k == 0)
			return false;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
}

/* ---------- documents ---------- */

static i32 doc_find(Lsp *l, const char *path)
{
	for (u32 i = 0; i < l->doc_n; i++)
		if (strcmp(l->doc_path[i], path) == 0)
			return (i32)i;
	return -1;
}

static void doc_set(Lsp *l, const char *path, const char *text, size_t n)
{
	i32 d = doc_find(l, path);
	if (d < 0) {
		if (l->doc_n == l->doc_cap) {
			u32 c = l->doc_cap ? l->doc_cap * 2 : 16;
			l->doc_path = vx_realloc(l->doc_path, c * sizeof(char *));
			l->doc_text = vx_realloc(l->doc_text, c * sizeof(char *));
			l->doc_len = vx_realloc(l->doc_len, c * sizeof(u32));
			l->doc_cap = c;
		}
		d = (i32)l->doc_n++;
		l->doc_path[d] = vx_strdup(path);
		l->doc_text[d] = NULL;
	}
	free(l->doc_text[d]);
	l->doc_text[d] = vx_strndup(text, n);
	l->doc_len[d] = (u32)n;
}

static void doc_del(Lsp *l, const char *path)
{
	i32 d = doc_find(l, path);
	if (d < 0)
		return;
	free(l->doc_path[d]);
	free(l->doc_text[d]);
	u32 last = --l->doc_n;
	l->doc_path[d] = l->doc_path[last];
	l->doc_text[d] = l->doc_text[last];
	l->doc_len[d] = l->doc_len[last];
}

/* Text of a document: open copy, else the file on disk (kept in scratch). */
static const char *doc_text(Lsp *l, const char *path, u32 *n)
{
	i32 d = doc_find(l, path);
	if (d >= 0) {
		*n = l->doc_len[d];
		return l->doc_text[d];
	}
	free(l->scratch);
	size_t len = 0;
	l->scratch = vx_read_file(path, &len);
	*n = (u32)len;
	return l->scratch;
}

/* Walks lines forward; restarts when asked for an earlier line. */
typedef struct {
	const char *t;
	u32 n, line, off;
} LineCur;

static void lc_seek(LineCur *c, u32 line, const char **s, u32 *len)
{
	if (!c->t) {
		*s = "";
		*len = 0;
		return;
	}
	if (line < c->line) {
		c->line = 0;
		c->off = 0;
	}
	while (c->line < line && c->off < c->n) {
		const char *nl = memchr(c->t + c->off, '\n', c->n - c->off);
		c->off = nl ? (u32)(nl - c->t) + 1 : c->n;
		c->line++;
	}
	const char *nl = memchr(c->t + c->off, '\n', c->n - c->off);
	*s = c->t + c->off;
	*len = (nl ? (u32)(nl - c->t) : c->n) - c->off;
	if (c->line < line)
		*len = 0;
}

static u32 col_in(LineCur *c, u32 line, u32 col16)
{
	const char *s;
	u32 n;
	lc_seek(c, line, &s, &n);
	return lsp_utf16_to_byte(s, n, col16);
}

static u32 col_out(Lsp *l, const char *path, u32 line, u32 col)
{
	if (l->utf8)
		return col;
	LineCur c = {0};
	c.t = doc_text(l, path, &c.n);
	const char *s;
	u32 n;
	lc_seek(&c, line, &s, &n);
	return lsp_byte_to_utf16(s, n, col);
}

/* ---------- output ---------- */

static void flush_out(Lsp *l)
{
	while (l->fd_in >= 0 && l->out_off < l->out.n) {
		ssize_t k = write(l->fd_in, l->out.b + l->out_off, l->out.n - l->out_off);
		if (k > 0) {
			l->out_off += (u32)k;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		/* EPIPE etc: the server is gone */
		close(l->fd_in);
		l->fd_in = -1;
		break;
	}
	if (l->out_off == l->out.n) {
		l->out.n = 0;
		l->out_off = 0;
	} else if (l->out_off > 1 << 20) {
		memmove(l->out.b, l->out.b + l->out_off, l->out.n - l->out_off);
		l->out.n -= l->out_off;
		l->out_off = 0;
	}
}

static void frame(JBuf *dst, const JBuf *body)
{
	char hdr[64];
	int h = snprintf(hdr, sizeof hdr, "Content-Length: %u\r\n\r\n", body->n);
	jb_rawn(dst, hdr, (size_t)h);
	jb_rawn(dst, body->b, body->n);
}

/* Send l->msg now, or queue it until initialization when early. */
static void send_msg(Lsp *l, bool early_ok)
{
	if (l->init_done || early_ok)
		frame(&l->out, &l->msg);
	else
		frame(&l->pre, &l->msg);
	flush_out(l);
}

static void msg_begin_notify(Lsp *l, const char *method)
{
	jb_reset(&l->msg);
	jb_raw(&l->msg, "{\"jsonrpc\":\"2.0\",\"method\":");
	jb_str(&l->msg, method);
	jb_raw(&l->msg, ",\"params\":");
}

/* Request ids are unique across every server, so a reply's id says which
 * request it answers whichever server it came from. */
static i32 g_next_id = 1;

static i32 msg_begin_request(Lsp *l, const char *method, u8 kind, const char *path)
{
	i32 id = g_next_id++;
	l->next_id = g_next_id;
	if (l->rq_n == l->rq_cap) {
		u32 c = l->rq_cap ? l->rq_cap * 2 : 16;
		l->rq_id = vx_realloc(l->rq_id, c * sizeof(i32));
		l->rq_kind = vx_realloc(l->rq_kind, c);
		l->rq_path = vx_realloc(l->rq_path, c * sizeof(char *));
		l->rq_cap = c;
	}
	l->rq_id[l->rq_n] = id;
	l->rq_kind[l->rq_n] = kind;
	l->rq_path[l->rq_n] = path ? vx_strdup(path) : NULL;
	l->rq_n++;
	jb_reset(&l->msg);
	jb_raw(&l->msg, "{\"jsonrpc\":\"2.0\",\"id\":");
	jb_int(&l->msg, id);
	jb_raw(&l->msg, ",\"method\":");
	jb_str(&l->msg, method);
	jb_raw(&l->msg, ",\"params\":");
	return id;
}

static void jb_uri(JBuf *b, const char *path)
{
	char *u = lsp_path_to_uri(path);
	jb_str(b, u);
	free(u);
}

/* ---------- spawn ---------- */

static const char *k_token_types[] = {
	"namespace", "type", "class", "enum", "interface", "struct", "typeParameter",
	"parameter", "variable", "property", "enumMember", "event", "function",
	"method", "macro", "keyword", "modifier", "comment", "string", "number",
	"regexp", "operator", "decorator", "concept",
};
static const char *k_token_mods[] = {
	"declaration", "definition", "readonly", "static", "deprecated", "abstract",
	"async", "modification", "documentation", "defaultLibrary",
};

static void send_initialize(Lsp *l, const char *root)
{
	msg_begin_request(l, "initialize", RQ_INIT, NULL);
	JBuf *m = &l->msg;
	jb_raw(m, "{\"processId\":");
	jb_int(m, (i64)getpid());
	jb_raw(m, ",\"clientInfo\":{\"name\":\"vxide\",\"version\":\"0.1\"},\"rootPath\":");
	jb_str(m, root);
	jb_raw(m, ",\"rootUri\":");
	jb_str(m, l->root_uri);
	jb_raw(m, ",\"workspaceFolders\":[{\"uri\":");
	jb_str(m, l->root_uri);
	jb_raw(m, ",\"name\":");
	const char *base = strrchr(root, '/');
	jb_str(m, base && base[1] ? base + 1 : root);
	jb_raw(m, "}],");
	if (l->settings) {
		jb_raw(m, "\"initializationOptions\":");
		jb_raw(m, l->settings);
		jb_raw(m, ",");
	}
	jb_raw(m, "\"capabilities\":{"
	          "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
	          /* clangd < 16 reads its extension from capabilities */
	          "\"offsetEncoding\":[\"utf-8\",\"utf-16\"],"
	          "\"workspace\":{\"configuration\":true,\"workspaceFolders\":true},"
	          "\"window\":{\"workDoneProgress\":false},"
	          "\"textDocument\":{"
	          "\"synchronization\":{\"dynamicRegistration\":false,\"didSave\":true,"
	          "\"willSave\":false,\"willSaveWaitUntil\":false},"
	          "\"publishDiagnostics\":{\"relatedInformation\":false},"
	          "\"completion\":{\"contextSupport\":true,\"completionItem\":{"
	          "\"snippetSupport\":false,\"insertReplaceSupport\":true,"
	          "\"labelDetailsSupport\":false}},"
	          "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
	          "\"definition\":{\"linkSupport\":true},"
	          "\"declaration\":{\"linkSupport\":true},"
	          "\"typeDefinition\":{\"linkSupport\":true},"
	          "\"references\":{},"
	          "\"semanticTokens\":{\"requests\":{\"full\":true,\"range\":false},"
	          "\"formats\":[\"relative\"],\"overlappingTokenSupport\":false,"
	          "\"multilineTokenSupport\":false,\"tokenTypes\":[");
	for (size_t i = 0; i < VX_COUNT(k_token_types); i++) {
		if (i)
			jb_raw(m, ",");
		jb_str(m, k_token_types[i]);
	}
	jb_raw(m, "],\"tokenModifiers\":[");
	for (size_t i = 0; i < VX_COUNT(k_token_mods); i++) {
		if (i)
			jb_raw(m, ",");
		jb_str(m, k_token_mods[i]);
	}
	jb_raw(m, "]}}}}}");
	send_msg(l, true);
}

Lsp *lsp_start(const char *root, char *const argv[]) { return lsp_start_ex(root, argv, NULL); }

Lsp *lsp_start_ex(const char *root, char *const argv[], const char *settings)
{
	if (!argv || !argv[0])
		return NULL;
	struct sigaction sa;
	if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
		/* A dead server must not kill the editor on write. */
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sa, NULL);
	}
	int pin[2], pout[2], perr[2], pexec[2];
	if (pipe(pin))
		return NULL;
	if (pipe(pout)) {
		close(pin[0]);
		close(pin[1]);
		return NULL;
	}
	if (pipe(perr)) {
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return NULL;
	}
	if (pipe(pexec)) {
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		close(perr[0]);
		close(perr[1]);
		return NULL;
	}
	set_cloexec(pin[1]);
	set_cloexec(pout[0]);
	set_cloexec(perr[0]);
	set_cloexec(pexec[0]);
	set_cloexec(pexec[1]);
	pid_t pid = fork();
	if (pid < 0) {
		int fds[] = {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1], pexec[0], pexec[1]};
		for (size_t i = 0; i < VX_COUNT(fds); i++)
			close(fds[i]);
		return NULL;
	}
	if (pid == 0) {
		dup2(pin[0], 0);
		dup2(pout[1], 1);
		dup2(perr[1], 2);
		if (pin[0] > 2)
			close(pin[0]);
		if (pout[1] > 2)
			close(pout[1]);
		if (perr[1] > 2)
			close(perr[1]);
		if (root && chdir(root) != 0) {
			/* keep going from the current directory */
		}
		execvp(argv[0], argv);
		int e = errno;
		ssize_t w = write(pexec[1], &e, sizeof e);
		(void)w;
		_exit(127);
	}
	close(pin[0]);
	close(pout[1]);
	close(perr[1]);
	close(pexec[1]);
	int e = 0;
	ssize_t k;
	do
		k = read(pexec[0], &e, sizeof e);
	while (k < 0 && errno == EINTR);
	close(pexec[0]);
	if (k > 0) { /* exec failed */
		close(pin[1]);
		close(pout[0]);
		close(perr[0]);
		waitpid(pid, NULL, 0);
		return NULL;
	}
	Lsp *l = vx_alloc(sizeof *l);
	l->pid = pid;
	l->fd_in = pin[1];
	l->fd_out = pout[0];
	l->fd_err = perr[0];
	set_nonblock(l->fd_in);
	set_nonblock(l->fd_out);
	set_nonblock(l->fd_err);
	l->next_id = 1;
	l->utf8 = false;
	l->root_uri = lsp_path_to_uri(root ? root : ".");
	l->settings = settings ? vx_strdup(settings) : NULL;
	send_initialize(l, root ? root : ".");
	return l;
}

int lsp_fd(Lsp *l)
{
	if (!l || l->out_eof)
		return -1;
	return l->fd_out;
}

bool lsp_ready(Lsp *l) { return l && l->init_done && !l->out_eof; }
bool lsp_utf8(Lsp *l) { return l && l->utf8; }

static void try_reap(Lsp *l)
{
	if (l->reaped || l->pid <= 0)
		return;
	if (waitpid(l->pid, NULL, WNOHANG) == l->pid)
		l->reaped = true;
}

void lsp_pump(Lsp *l)
{
	if (!l)
		return;
	flush_out(l);
	if (!l->out_eof && !rbuf_fill(&l->in, l->fd_out))
		l->out_eof = true;
	if (!l->err_eof && !rbuf_fill(&l->err, l->fd_err))
		l->err_eof = true;
	if (l->out_eof)
		try_reap(l);
}

/* ---------- event tables ---------- */

#define RE(p, c) (p) = vx_realloc((p), (size_t)(c) * sizeof(*(p)))

static void diags_reserve(Lsp *l, u32 n)
{
	if (n <= l->diags_cap)
		return;
	u32 c = n < 64 ? 64 : n;
	LspDiags *d = &l->diags;
	RE(d->line0, c); RE(d->col0, c); RE(d->line1, c); RE(d->col1, c);
	RE(d->severity, c); RE(d->message, c); RE(d->source, c);
	l->diags_cap = c;
}

static void comp_reserve(Lsp *l, u32 n)
{
	if (n <= l->comp_cap)
		return;
	u32 c = n < 64 ? 64 : n;
	LspCompletion *d = &l->comp;
	RE(d->label, c); RE(d->detail, c); RE(d->insert, c); RE(d->kind, c);
	RE(d->line0, c); RE(d->col0, c); RE(d->line1, c); RE(d->col1, c);
	RE(d->has_range, c);
	l->comp_cap = c;
}

static void locs_reserve(Lsp *l, u32 n)
{
	if (n <= l->locs_cap)
		return;
	u32 c = n < 16 ? 16 : n;
	LspLocations *d = &l->locs;
	RE(d->path, c); RE(d->line, c); RE(d->col, c);
	l->locs_cap = c;
}

static void sem_reserve(Lsp *l, u32 n)
{
	if (n <= l->sem_cap)
		return;
	u32 c = n < 256 ? 256 : n;
	LspSemantic *d = &l->sem;
	RE(d->line, c); RE(d->col, c); RE(d->len, c); RE(d->cls, c); RE(d->mods, c);
	l->sem_cap = c;
}

static void ev_init(Lsp *l, LspEvent *ev, int type, i32 req)
{
	memset(ev, 0, sizeof *ev);
	ev->type = type;
	ev->req = req;
	ev->pool = &l->pool;
	ev->diags = &l->diags;
	ev->completion = &l->comp;
	ev->locations = &l->locs;
	ev->semantic = &l->sem;
	l->diags.count = l->comp.count = l->locs.count = l->sem.count = 0;
}

/* Pool strings are referenced by pointer in the event; the pool must not
 * reallocate after a pointer is taken, so pointers are resolved last. */

/* ---------- message handling ---------- */

static u8 map_token_type(const Json *j, u32 t)
{
	static const struct {
		const char *name;
		u8 cls;
	} m[] = {
		{"function", TK_FUNCTION}, {"method", TK_FUNCTION},
		{"variable", TK_VARIABLE}, {"parameter", TK_VARIABLE},
		{"property", TK_VARIABLE}, {"type", TK_TYPE},
		{"class", TK_TYPE},        {"struct", TK_TYPE},
		{"typeParameter", TK_TYPE}, {"concept", TK_TYPE},
		{"interface", TK_TYPE},    {"enum", TK_TYPE},
		{"namespace", TK_TYPE},    {"enumMember", TK_CONSTANT},
		{"macro", TK_PREPROC},     {"keyword", TK_STORAGE},
		{"modifier", TK_STORAGE},  {"comment", TK_COMMENT},
		{"string", TK_STRING},     {"number", TK_NUMBER},
		{"operator", TK_OPERATOR},
		/* rust-analyzer's own types */
		{"builtinType", TK_TYPE}, {"typeAlias", TK_TYPE},
		{"union", TK_TYPE},       {"selfTypeKeyword", TK_TYPE},
		{"selfKeyword", TK_STORAGE}, {"lifetime", TK_STORAGE},
		{"boolean", TK_STORAGE},  {"character", TK_STRING},
	};
	for (size_t i = 0; i < VX_COUNT(m); i++)
		if (json_eq(j, t, m[i].name))
			return m[i].cls;
	return TK_TEXT;
}

static void handle_init_result(Lsp *l, u32 res)
{
	const Json *j = &l->js;
	u32 enc = json_get(j, res, "positionEncoding");
	if (enc == JSON_NIL)
		enc = json_path(j, res, "capabilities.positionEncoding");
	if (enc == JSON_NIL)
		enc = json_get(j, res, "offsetEncoding");
	l->utf8 = json_eq(j, enc, "utf-8");
	u32 types = json_path(j, res, "capabilities.semanticTokensProvider.legend.tokenTypes");
	l->leg_n = 0;
	if (json_type(j, types) == JS_ARR) {
		u32 t = json_first(j, types);
		for (u32 i = 0; i < j->size[types] && l->leg_n < VX_COUNT(l->leg_cls); i++) {
			l->leg_cls[l->leg_n++] = map_token_type(j, t);
			t = j->next[t];
		}
	}
	u32 mods = json_path(j, res, "capabilities.semanticTokensProvider.legend.tokenModifiers");
	l->ro_bit = 0;
	if (json_type(j, mods) == JS_ARR) {
		u32 t = json_first(j, mods);
		for (u32 i = 0; i < j->size[mods] && i < 32; i++) {
			if (json_eq(j, t, "readonly"))
				l->ro_bit = 1u << i;
			t = j->next[t];
		}
	}
	msg_begin_notify(l, "initialized");
	jb_raw(&l->msg, "{}}");
	send_msg(l, true);
	l->init_done = true;
	if (l->pre.n) {
		jb_rawn(&l->out, l->pre.b, l->pre.n);
		jb_reset(&l->pre);
	}
	flush_out(l);
}

static void reply_server_request(Lsp *l, u32 id, u32 method)
{
	const Json *j = &l->js;
	JBuf *m = &l->msg;
	jb_reset(m);
	jb_raw(m, "{\"jsonrpc\":\"2.0\",\"id\":");
	u32 s = j->start[id], e = j->end[id];
	if (j->type[id] == JS_STR) {
		s--;
		e++;
	}
	jb_rawn(m, j->src + s, e - s);
	jb_raw(m, ",\"result\":");
	if (json_eq(j, method, "workspace/configuration")) {
		u32 items = json_path(j, 0, "params.items");
		u32 n = json_type(j, items) == JS_ARR ? j->size[items] : 0;
		jb_raw(m, "[");
		for (u32 i = 0; i < n; i++) {
			if (i)
				jb_raw(m, ",");
			jb_raw(m, l->settings ? l->settings : "null");
		}
		jb_raw(m, "]");
	} else if (json_eq(j, method, "workspace/workspaceFolders")) {
		jb_raw(m, "[{\"uri\":");
		jb_str(m, l->root_uri);
		jb_raw(m, ",\"name\":\"root\"}]");
	} else {
		jb_raw(m, "null");
	}
	jb_raw(m, "}");
	send_msg(l, true);
}

/* Read a {line,character} into byte coordinates using cursor c. */
static void read_pos(Lsp *l, LineCur *c, u32 pos, u32 *line, u32 *col)
{
	const Json *j = &l->js;
	*line = (u32)json_int(j, json_get(j, pos, "line"), 0);
	u32 ch = (u32)json_int(j, json_get(j, pos, "character"), 0);
	*col = l->utf8 ? ch : col_in(c, *line, ch);
}

static u32 uri_pool(Lsp *l, u32 tok)
{
	const Json *j = &l->js;
	if (json_type(j, tok) != JS_STR)
		return pool_add(&l->pool, "", 0);
	u32 raw = json_str_pool(j, tok, &l->pool);
	char *p = lsp_uri_to_path(pool_str(&l->pool, raw), strlen(pool_str(&l->pool, raw)));
	u32 off = pool_add(&l->pool, p, (u32)strlen(p));
	free(p);
	return off;
}

static void cursor_for(Lsp *l, LineCur *c, const char *path)
{
	memset(c, 0, sizeof *c);
	if (!l->utf8)
		c->t = doc_text(l, path, &c->n);
}

/* Event path/text are stored as pool offsets and resolved at the end. */
typedef struct {
	u32 path, text;
	bool has_path, has_text;
} EvStr;

static void ev_finish(Lsp *l, LspEvent *ev, const EvStr *s)
{
	ev->path = s->has_path ? pool_str(&l->pool, s->path) : NULL;
	ev->text = s->has_text ? pool_str(&l->pool, s->text) : NULL;
}

static bool handle_diagnostics(Lsp *l, LspEvent *ev, EvStr *es)
{
	const Json *j = &l->js;
	u32 params = json_get(j, 0, "params");
	ev_init(l, ev, LSP_EV_DIAGNOSTICS, -1);
	es->path = uri_pool(l, json_get(j, params, "uri"));
	es->has_path = true;
	char *path = vx_strdup(pool_str(&l->pool, es->path));
	LineCur c;
	cursor_for(l, &c, path);
	u32 arr = json_get(j, params, "diagnostics");
	u32 n = json_type(j, arr) == JS_ARR ? j->size[arr] : 0;
	diags_reserve(l, n);
	LspDiags *d = &l->diags;
	u32 t = json_first(j, arr);
	for (u32 i = 0; i < n; i++, t = j->next[t]) {
		u32 range = json_get(j, t, "range");
		read_pos(l, &c, json_get(j, range, "start"), &d->line0[i], &d->col0[i]);
		read_pos(l, &c, json_get(j, range, "end"), &d->line1[i], &d->col1[i]);
		i64 sev = json_int(j, json_get(j, t, "severity"), LSP_SEV_ERROR);
		d->severity[i] = (u8)(sev >= 1 && sev <= 4 ? sev : LSP_SEV_ERROR);
		d->message[i] = json_str_pool(j, json_get(j, t, "message"), &l->pool);
		d->source[i] = json_str_pool(j, json_get(j, t, "source"), &l->pool);
	}
	d->count = n;
	free(path);
	return true;
}

/* Trim leading whitespace and clangd's bullet (U+2022) from a label. */
static u32 trim_label(Lsp *l, u32 off)
{
	const char *s = pool_str(&l->pool, off);
	u32 k = 0;
	for (;;) {
		if (s[k] == ' ' || s[k] == '\t')
			k++;
		else if ((u8)s[k] == 0xE2 && (u8)s[k + 1] == 0x80 && (u8)s[k + 2] == 0xA2)
			k += 3;
		else
			break;
	}
	return off + k;
}

static void handle_completion(Lsp *l, u32 res, const char *path)
{
	const Json *j = &l->js;
	u32 items = res;
	if (json_type(j, res) == JS_OBJ)
		items = json_get(j, res, "items");
	u32 n = json_type(j, items) == JS_ARR ? j->size[items] : 0;
	comp_reserve(l, n);
	LspCompletion *c = &l->comp;
	LineCur cur;
	cursor_for(l, &cur, path);
	u32 t = json_first(j, items);
	for (u32 i = 0; i < n; i++, t = j->next[t]) {
		c->label[i] = trim_label(l, json_str_pool(j, json_get(j, t, "label"), &l->pool));
		u32 det = json_get(j, t, "detail");
		c->detail[i] = json_str_pool(j, det, &l->pool);
		c->kind[i] = (u8)json_int(j, json_get(j, t, "kind"), 0);
		u32 te = json_get(j, t, "textEdit");
		u32 ins = json_get(j, t, "insertText");
		c->has_range[i] = 0;
		c->line0[i] = c->col0[i] = c->line1[i] = c->col1[i] = 0;
		if (te != JSON_NIL) {
			u32 range = json_get(j, te, "range");
			if (range == JSON_NIL)
				range = json_get(j, te, "insert");
			if (range != JSON_NIL) {
				read_pos(l, &cur, json_get(j, range, "start"), &c->line0[i], &c->col0[i]);
				read_pos(l, &cur, json_get(j, range, "end"), &c->line1[i], &c->col1[i]);
				c->has_range[i] = 1;
			}
			c->insert[i] = json_str_pool(j, json_get(j, te, "newText"), &l->pool);
		} else if (ins != JSON_NIL) {
			c->insert[i] = json_str_pool(j, ins, &l->pool);
		} else {
			c->insert[i] = c->label[i];
		}
	}
	c->count = n;
}

static void hover_append(Lsp *l, JBuf *b, u32 t)
{
	const Json *j = &l->js;
	u32 v = JSON_NIL;
	if (json_type(j, t) == JS_STR)
		v = t;
	else if (json_type(j, t) == JS_OBJ)
		v = json_get(j, t, "value");
	if (json_type(j, v) != JS_STR)
		return;
	u32 n = j->end[v] - j->start[v];
	char *tmp = vx_alloc(n + 1);
	u32 m = json_unescape(j->src + j->start[v], n, tmp);
	if (b->n && m)
		jb_raw(b, "\n\n");
	jb_rawn(b, tmp, m);
	free(tmp);
}

static u32 handle_hover(Lsp *l, u32 res)
{
	const Json *j = &l->js;
	JBuf b = {0};
	u32 contents = json_get(j, res, "contents");
	if (json_type(j, contents) == JS_ARR) {
		u32 t = json_first(j, contents);
		for (u32 i = 0; i < j->size[contents]; i++, t = j->next[t])
			hover_append(l, &b, t);
	} else if (contents != JSON_NIL) {
		hover_append(l, &b, contents);
	}
	u32 off = pool_add(&l->pool, b.b ? b.b : "", b.n);
	jb_free(&b);
	return off;
}

static void add_location(Lsp *l, u32 t)
{
	const Json *j = &l->js;
	u32 uri = json_get(j, t, "uri");
	u32 range = json_get(j, t, "range");
	if (uri == JSON_NIL) { /* LocationLink */
		uri = json_get(j, t, "targetUri");
		range = json_get(j, t, "targetSelectionRange");
		if (range == JSON_NIL)
			range = json_get(j, t, "targetRange");
	}
	if (uri == JSON_NIL)
		return;
	LspLocations *L = &l->locs;
	locs_reserve(l, L->count + 1);
	u32 i = L->count++;
	L->path[i] = uri_pool(l, uri);
	char *p = vx_strdup(pool_str(&l->pool, L->path[i]));
	LineCur c;
	cursor_for(l, &c, p);
	read_pos(l, &c, json_get(j, range, "start"), &L->line[i], &L->col[i]);
	free(p);
}

static void handle_definition(Lsp *l, u32 res)
{
	const Json *j = &l->js;
	if (json_type(j, res) == JS_ARR) {
		u32 t = json_first(j, res);
		for (u32 i = 0; i < j->size[res]; i++, t = j->next[t])
			add_location(l, t);
	} else if (json_type(j, res) == JS_OBJ) {
		add_location(l, res);
	}
}

static void handle_semantic(Lsp *l, u32 res, const char *path)
{
	const Json *j = &l->js;
	u32 data = json_get(j, res, "data");
	u32 n = json_type(j, data) == JS_ARR ? j->size[data] / 5 : 0;
	sem_reserve(l, n);
	LspSemantic *s = &l->sem;
	LineCur cur;
	cursor_for(l, &cur, path);
	u32 t = json_first(j, data);
	u32 line = 0, col = 0;
	for (u32 i = 0; i < n; i++) {
		u32 v[5];
		for (int k = 0; k < 5; k++) {
			v[k] = (u32)json_int(j, t, 0);
			t = j->next[t];
		}
		if (v[0]) {
			line += v[0];
			col = v[1];
		} else {
			col += v[1];
		}
		s->line[i] = line;
		if (l->utf8) {
			s->col[i] = col;
			s->len[i] = v[2];
		} else {
			u32 b0 = col_in(&cur, line, col);
			u32 b1 = col_in(&cur, line, col + v[2]);
			s->col[i] = b0;
			s->len[i] = b1 - b0;
		}
		s->cls[i] = v[3] < l->leg_n ? l->leg_cls[v[3]] : TK_TEXT;
		s->mods[i] = v[4];
		/* Dark+ colours readonly variables as variable.other.constant. */
		if (s->cls[i] == TK_VARIABLE && (v[4] & l->ro_bit))
			s->cls[i] = TK_CONSTANT;
	}
	s->count = n;
}

static i32 rq_take(Lsp *l, i32 id, u8 *kind, char **path)
{
	for (u32 i = 0; i < l->rq_n; i++) {
		if (l->rq_id[i] != id)
			continue;
		*kind = l->rq_kind[i];
		*path = l->rq_path[i];
		u32 last = --l->rq_n;
		l->rq_id[i] = l->rq_id[last];
		l->rq_kind[i] = l->rq_kind[last];
		l->rq_path[i] = l->rq_path[last];
		return 0;
	}
	return -1;
}

/* Handle one parsed message. Returns true when it produced an event. */
static bool handle_message(Lsp *l, LspEvent *ev)
{
	const Json *j = &l->js;
	if (json_type(j, 0) != JS_OBJ)
		return false;
	u32 id = json_get(j, 0, "id");
	u32 method = json_get(j, 0, "method");
	EvStr es = {0};
	if (method != JSON_NIL && id != JSON_NIL) {
		reply_server_request(l, id, method);
		return false;
	}
	if (method != JSON_NIL) {
		if (json_eq(j, method, "textDocument/publishDiagnostics")) {
			handle_diagnostics(l, ev, &es);
			ev_finish(l, ev, &es);
			return true;
		}
		if (json_eq(j, method, "window/logMessage") ||
		    json_eq(j, method, "window/showMessage")) {
			ev_init(l, ev, LSP_EV_LOG, -1);
			es.text = json_str_pool(j, json_path(j, 0, "params.message"), &l->pool);
			es.has_text = true;
			ev_finish(l, ev, &es);
			return true;
		}
		return false; /* $/progress and friends */
	}
	if (id == JSON_NIL)
		return false;
	i32 rid = (i32)json_int(j, id, -1);
	u8 kind;
	char *path;
	if (rq_take(l, rid, &kind, &path))
		return false;
	u32 res = json_get(j, 0, "result");
	bool ok = json_get(j, 0, "error") == JSON_NIL;
	bool produced = true;
	if (path) {
		es.path = pool_add(&l->pool, path, (u32)strlen(path));
		es.has_path = true;
	}
	switch (kind) {
	case RQ_INIT:
		ev_init(l, ev, LSP_EV_READY, rid);
		if (ok)
			handle_init_result(l, res);
		else {
			/* initialize failed: report it, the server is useless */
			ev->type = LSP_EV_LOG;
			es.text = json_str_pool(j, json_path(j, 0, "error.message"), &l->pool);
			es.has_text = true;
		}
		break;
	case RQ_SHUTDOWN:
		produced = false;
		break;
	case RQ_COMPLETION:
		ev_init(l, ev, LSP_EV_COMPLETION, rid);
		if (ok)
			handle_completion(l, res, path);
		break;
	case RQ_HOVER:
		ev_init(l, ev, LSP_EV_HOVER, rid);
		es.text = ok ? handle_hover(l, res) : pool_add(&l->pool, "", 0);
		es.has_text = true;
		break;
	case RQ_DEFINITION:
	case RQ_REFERENCES:
		ev_init(l, ev, LSP_EV_LOCATION, rid);
		if (ok)
			handle_definition(l, res);
		break;
	case RQ_SEMANTIC:
		ev_init(l, ev, LSP_EV_SEMANTIC, rid);
		if (ok)
			handle_semantic(l, res, path);
		break;
	default:
		produced = false;
	}
	free(path);
	if (produced)
		ev_finish(l, ev, &es);
	return produced;
}

/* Extract one framed message from l->in; returns body or NULL. */
static const char *next_frame(Lsp *l, u32 *len)
{
	RBuf *r = &l->in;
	for (;;) {
		const char *s = r->b + r->off;
		u32 avail = r->n - r->off;
		if (avail < 4)
			return NULL;
		u32 h = 0;
		bool found = false;
		for (; h + 3 < avail; h++) {
			if (s[h] == '\r' && s[h + 1] == '\n' && s[h + 2] == '\r' && s[h + 3] == '\n') {
				found = true;
				break;
			}
		}
		if (!found)
			return NULL;
		/* parse headers */
		long clen = -1;
		u32 p = 0;
		while (p < h) {
			const char *line = s + p;
			const char *eol = memchr(line, '\r', h - p);
			u32 ll = eol ? (u32)(eol - line) : h - p;
			static const char key[] = "content-length:";
			if (ll >= sizeof key - 1) {
				bool match = true;
				for (u32 k = 0; k < sizeof key - 1; k++) {
					char c = line[k];
					if (c >= 'A' && c <= 'Z')
						c = (char)(c - 'A' + 'a');
					if (c != key[k]) {
						match = false;
						break;
					}
				}
				if (match) {
					clen = 0;
					for (u32 k = sizeof key - 1; k < ll; k++)
						if (line[k] >= '0' && line[k] <= '9')
							clen = clen * 10 + (line[k] - '0');
				}
			}
			p += ll + 2;
		}
		if (clen < 0) { /* garbage: drop the header block */
			r->off += h + 4;
			continue;
		}
		if (avail < h + 4 + (u32)clen)
			return NULL;
		*len = (u32)clen;
		r->off += h + 4 + (u32)clen;
		return s + h + 4;
	}
}

bool lsp_next(Lsp *l, LspEvent *ev)
{
	if (!l)
		return false;
	pool_clear(&l->pool);
	u32 len;
	const char *body;
	while ((body = next_frame(l, &len)) != NULL) {
		if (json_parse(&l->js, body, len))
			continue;
		if (handle_message(l, ev))
			return true;
	}
	/* stderr lines */
	RBuf *e = &l->err;
	if (e->off < e->n) {
		const char *s = e->b + e->off;
		const char *nl = memchr(s, '\n', e->n - e->off);
		if (nl || l->err_eof) {
			u32 n = nl ? (u32)(nl - s) : e->n - e->off;
			ev_init(l, ev, LSP_EV_LOG, -1);
			u32 off = pool_add(&l->pool, s, n && s[n - 1] == '\r' ? n - 1 : n);
			e->off += n + (nl ? 1 : 0);
			ev->text = pool_str(&l->pool, off);
			return true;
		}
	}
	if (l->out_eof && !l->exit_reported) {
		l->exit_reported = true;
		try_reap(l);
		ev_init(l, ev, LSP_EV_EXIT, -1);
		return true;
	}
	return false;
}

/* ---------- document sync & requests ---------- */

void lsp_did_open(Lsp *l, const char *path, const char *lang_id, i32 version,
                  const char *text, size_t n)
{
	if (!l)
		return;
	doc_set(l, path, text, n);
	msg_begin_notify(l, "textDocument/didOpen");
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, ",\"languageId\":");
	jb_str(&l->msg, lang_id ? lang_id : "plaintext");
	jb_raw(&l->msg, ",\"version\":");
	jb_int(&l->msg, version);
	jb_raw(&l->msg, ",\"text\":");
	jb_strn(&l->msg, text, n);
	jb_raw(&l->msg, "}}}");
	send_msg(l, false);
}

void lsp_did_change(Lsp *l, const char *path, i32 version, const char *text, size_t n)
{
	if (!l)
		return;
	doc_set(l, path, text, n);
	msg_begin_notify(l, "textDocument/didChange");
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, ",\"version\":");
	jb_int(&l->msg, version);
	jb_raw(&l->msg, "},\"contentChanges\":[{\"text\":");
	jb_strn(&l->msg, text, n);
	jb_raw(&l->msg, "}]}}");
	send_msg(l, false);
}

static void doc_notify(Lsp *l, const char *method, const char *path)
{
	msg_begin_notify(l, method);
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, "}}}");
	send_msg(l, false);
}

void lsp_did_save(Lsp *l, const char *path)
{
	if (l)
		doc_notify(l, "textDocument/didSave", path);
}

void lsp_did_close(Lsp *l, const char *path)
{
	if (!l)
		return;
	doc_notify(l, "textDocument/didClose", path);
	doc_del(l, path);
}

static i32 pos_request(Lsp *l, const char *method, u8 kind, const char *path, u32 line, u32 col)
{
	if (!lsp_ready(l) || l->fd_in < 0)
		return -1;
	u32 c = col_out(l, path, line, col);
	i32 id = msg_begin_request(l, method, kind, path);
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, "},\"position\":{\"line\":");
	jb_int(&l->msg, line);
	jb_raw(&l->msg, ",\"character\":");
	jb_int(&l->msg, c);
	jb_raw(&l->msg, "}}}");
	send_msg(l, false);
	return id;
}

i32 lsp_completion(Lsp *l, const char *path, u32 line, u32 col)
{
	return pos_request(l, "textDocument/completion", RQ_COMPLETION, path, line, col);
}

i32 lsp_hover(Lsp *l, const char *path, u32 line, u32 col)
{
	return pos_request(l, "textDocument/hover", RQ_HOVER, path, line, col);
}

i32 lsp_definition(Lsp *l, const char *path, u32 line, u32 col)
{
	return pos_request(l, "textDocument/definition", RQ_DEFINITION, path, line, col);
}

i32 lsp_declaration(Lsp *l, const char *path, u32 line, u32 col)
{
	return pos_request(l, "textDocument/declaration", RQ_DEFINITION, path, line, col);
}

i32 lsp_type_definition(Lsp *l, const char *path, u32 line, u32 col)
{
	return pos_request(l, "textDocument/typeDefinition", RQ_DEFINITION, path, line, col);
}

i32 lsp_references(Lsp *l, const char *path, u32 line, u32 col)
{
	if (!lsp_ready(l) || l->fd_in < 0)
		return -1;
	u32 c = col_out(l, path, line, col);
	i32 id = msg_begin_request(l, "textDocument/references", RQ_REFERENCES, path);
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, "},\"position\":{\"line\":");
	jb_int(&l->msg, line);
	jb_raw(&l->msg, ",\"character\":");
	jb_int(&l->msg, c);
	jb_raw(&l->msg, "},\"context\":{\"includeDeclaration\":true}}}");
	send_msg(l, false);
	return id;
}

i32 lsp_semantic_tokens(Lsp *l, const char *path)
{
	if (!lsp_ready(l) || l->fd_in < 0)
		return -1;
	i32 id = msg_begin_request(l, "textDocument/semanticTokens/full", RQ_SEMANTIC, path);
	jb_raw(&l->msg, "{\"textDocument\":{\"uri\":");
	jb_uri(&l->msg, path);
	jb_raw(&l->msg, "}}}");
	send_msg(l, false);
	return id;
}

/* ---------- shutdown ---------- */

static bool wait_exit(Lsp *l, int ms)
{
	u64 end = vx_now_ns() + (u64)ms * 1000000ull;
	while (!l->reaped) {
		try_reap(l);
		if (l->reaped)
			break;
		u64 now = vx_now_ns();
		if (now >= end)
			return false;
		/* keep draining so the child never blocks on a full pipe */
		lsp_pump(l);
		l->in.off = l->in.n;
		l->err.off = l->err.n;
		struct pollfd p[2];
		int np = 0;
		if (!l->out_eof)
			p[np++] = (struct pollfd){.fd = l->fd_out, .events = POLLIN};
		if (!l->err_eof)
			p[np++] = (struct pollfd){.fd = l->fd_err, .events = POLLIN};
		int left = (int)((end - now) / 1000000ull);
		poll(p, (nfds_t)np, np ? VX_MIN(left, 20) + 1 : 10);
	}
	return true;
}

void lsp_stop(Lsp *l)
{
	if (!l)
		return;
	l->stopping = true;
	if (l->fd_in >= 0 && !l->out_eof) {
		i32 id = msg_begin_request(l, "shutdown", RQ_SHUTDOWN, NULL);
		jb_raw(&l->msg, "null}");
		send_msg(l, true);
		/* wait for the reply (up to 300 ms) */
		u64 end = vx_now_ns() + 300000000ull;
		bool got = false;
		while (!got && !l->out_eof && vx_now_ns() < end) {
			struct pollfd p = {.fd = l->fd_out, .events = POLLIN};
			poll(&p, 1, 20);
			lsp_pump(l);
			u32 len;
			const char *body;
			while ((body = next_frame(l, &len)) != NULL) {
				if (json_parse(&l->js, body, len) == 0 &&
				    json_int(&l->js, json_get(&l->js, 0, "id"), -1) == id &&
				    json_get(&l->js, 0, "method") == JSON_NIL)
					got = true;
			}
			l->err.off = l->err.n;
		}
		jb_reset(&l->msg);
		jb_raw(&l->msg, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
		send_msg(l, true);
		flush_out(l);
	}
	if (l->fd_in >= 0) {
		close(l->fd_in);
		l->fd_in = -1;
	}
	if (!wait_exit(l, 500)) {
		kill(l->pid, SIGTERM);
		if (!wait_exit(l, 200)) {
			kill(l->pid, SIGKILL);
			waitpid(l->pid, NULL, 0);
			l->reaped = true;
		}
	}
	close(l->fd_out);
	close(l->fd_err);
	for (u32 i = 0; i < l->rq_n; i++)
		free(l->rq_path[i]);
	free(l->rq_id);
	free(l->rq_kind);
	free(l->rq_path);
	for (u32 i = 0; i < l->doc_n; i++) {
		free(l->doc_path[i]);
		free(l->doc_text[i]);
	}
	free(l->doc_path);
	free(l->doc_text);
	free(l->doc_len);
	jb_free(&l->out);
	jb_free(&l->pre);
	jb_free(&l->msg);
	free(l->in.b);
	free(l->err.b);
	free(l->root_uri);
	free(l->settings);
	free(l->scratch);
	json_free(&l->js);
	pool_free(&l->pool);
	LspDiags *d = &l->diags;
	free(d->line0); free(d->col0); free(d->line1); free(d->col1);
	free(d->severity); free(d->message); free(d->source);
	LspCompletion *c = &l->comp;
	free(c->label); free(c->detail); free(c->insert); free(c->kind);
	free(c->line0); free(c->col0); free(c->line1); free(c->col1); free(c->has_range);
	free(l->locs.path); free(l->locs.line); free(l->locs.col);
	LspSemantic *s = &l->sem;
	free(s->line); free(s->col); free(s->len); free(s->cls); free(s->mods);
	free(l);
}
