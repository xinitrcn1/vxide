/* LSP client tests: UTF-16 helpers, URIs, and a live session with clangd. */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"
#include "syntax.h"
#include "tap.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *k_src =
	"struct point { int x; int y; };\n"                 /* 0 */
	"static int add(int a, int b) { return a + b; }\n"  /* 1 */
	"int main(void) {\n"                                /* 2 */
	"\tstruct point p = {1, 2}; /* \xc3\xa9t\xc3\xa9 */\n" /* 3 */
	"\tint r = add(p.x, p.y);\n"                        /* 4 */
	"\tundeclared_thing = r;\n"                         /* 5 */
	"\treturn p.y;\n"                                   /* 6 */
	"}\n";

static void unit_tests(void)
{
	/* a (1 byte, 1 unit), e-acute (2, 1), U+1F600 (4, 2), b */
	const char *s = "a\xc3\xa9\xf0\x9f\x98\x80" "b";
	u32 n = (u32)strlen(s);
	CHECK(lsp_byte_to_utf16(s, n, 0) == 0);
	CHECK(lsp_byte_to_utf16(s, n, 1) == 1);
	CHECK(lsp_byte_to_utf16(s, n, 3) == 2);
	CHECK(lsp_byte_to_utf16(s, n, 7) == 4);
	CHECK(lsp_byte_to_utf16(s, n, 8) == 5);
	CHECK(lsp_byte_to_utf16(s, n, 99) == 5);
	CHECK(lsp_utf16_to_byte(s, n, 0) == 0);
	CHECK(lsp_utf16_to_byte(s, n, 2) == 3);
	CHECK(lsp_utf16_to_byte(s, n, 3) == 3); /* middle of a surrogate pair */
	CHECK(lsp_utf16_to_byte(s, n, 4) == 7);
	CHECK(lsp_utf16_to_byte(s, n, 5) == 8);
	CHECK(lsp_utf16_to_byte(s, n, 50) == 8);

	char *u = lsp_path_to_uri("/tmp/a b/c%d/\xc3\xa9.c");
	CHECK_STR(u, "file:///tmp/a%20b/c%25d/%C3%A9.c");
	char *p = lsp_uri_to_path(u, strlen(u));
	CHECK_STR(p, "/tmp/a b/c%d/\xc3\xa9.c");
	free(u);
	free(p);
	p = lsp_uri_to_path("file://localhost/x/y", 20);
	CHECK_STR(p, "/x/y");
	free(p);
}

/* Pump until an event of `type` (and request id `req` if >= 0) arrives. */
static bool wait_ev(Lsp *l, LspEvent *ev, int type, i32 req, int ms)
{
	u64 end = vx_now_ns() + (u64)ms * 1000000ull;
	while (vx_now_ns() < end) {
		lsp_pump(l);
		while (lsp_next(l, ev)) {
			if (ev->type == LSP_EV_EXIT)
				return type == LSP_EV_EXIT;
			if (ev->type == type && (req < 0 || ev->req == req))
				return true;
		}
		int fd = lsp_fd(l);
		struct pollfd p = {.fd = fd, .events = POLLIN};
		poll(&p, fd >= 0 ? 1 : 0, 50);
	}
	return false;
}

static void clangd_tests(const char *server)
{
	char dir[] = "/tmp/vxide-lsp-XXXXXX";
	if (!mkdtemp(dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	char path[256], flags[256];
	snprintf(path, sizeof path, "%s/main.c", dir);
	snprintf(flags, sizeof flags, "%s/compile_flags.txt", dir);
	vx_write_file(flags, "-std=c23\n", 9);
	vx_write_file(path, k_src, strlen(k_src));

	char *argv[] = {(char *)server, "--log=error", NULL};
	Lsp *l = lsp_start(dir, argv);
	CHECK(l != NULL);
	if (!l)
		return;
	LspEvent ev;
	/* didOpen before READY is queued and flushed after initialize */
	lsp_did_open(l, path, "c", 1, k_src, strlen(k_src));
	CHECK(wait_ev(l, &ev, LSP_EV_READY, -1, 20000));
	CHECK(lsp_ready(l));
	fprintf(stderr, "%s: utf8=%d\n", server, lsp_utf8(l));

	/* diagnostics: an error on line 5 */
	bool got = false;
	u64 end = vx_now_ns() + 20000000000ull;
	while (!got && vx_now_ns() < end) {
		if (!wait_ev(l, &ev, LSP_EV_DIAGNOSTICS, -1, 20000))
			break;
		CHECK_STR(ev.path, path);
		for (u32 i = 0; i < ev.diags->count; i++) {
			fprintf(stderr, "diag %u:%u sev %u: %s [%s]\n", ev.diags->line0[i],
			        ev.diags->col0[i], ev.diags->severity[i],
			        pool_str(ev.pool, ev.diags->message[i]),
			        pool_str(ev.pool, ev.diags->source[i]));
			if (ev.diags->severity[i] == LSP_SEV_ERROR && ev.diags->line0[i] == 5 &&
			    ev.diags->col0[i] == 1)
				got = true;
		}
	}
	CHECK(got);

	/* completion after "p." on line 6 */
	i32 rq = lsp_completion(l, path, 6, 10);
	CHECK(rq > 0);
	if (wait_ev(l, &ev, LSP_EV_COMPLETION, rq, 20000)) {
		bool x = false, y = false;
		for (u32 i = 0; i < ev.completion->count; i++) {
			const char *lab = pool_str(ev.pool, ev.completion->label[i]);
			const char *ins = pool_str(ev.pool, ev.completion->insert[i]);
			fprintf(stderr, "completion '%s' -> '%s' kind %u range %u\n", lab, ins,
			        ev.completion->kind[i], ev.completion->has_range[i]);
			if (strcmp(lab, "x") == 0 && strcmp(ins, "x") == 0)
				x = true;
			if (strcmp(lab, "y") == 0) {
				y = true;
				if (ev.completion->has_range[i]) {
					CHECK(ev.completion->line0[i] == 6);
					CHECK(ev.completion->col0[i] == 10);
				}
			}
		}
		CHECK(x && y);
	} else {
		CHECK(!"completion reply");
	}

	/* hover on "add" in line 4 */
	rq = lsp_hover(l, path, 4, 9);
	CHECK(rq > 0);
	if (wait_ev(l, &ev, LSP_EV_HOVER, rq, 20000)) {
		fprintf(stderr, "hover: %s\n", ev.text);
		CHECK(ev.text && strstr(ev.text, "add") != NULL);
		CHECK_STR(ev.path, path);
	} else {
		CHECK(!"hover reply");
	}

	/* definition of add -> line 1 col 11 */
	rq = lsp_definition(l, path, 4, 9);
	if (wait_ev(l, &ev, LSP_EV_LOCATION, rq, 20000)) {
		CHECK(ev.locations->count >= 1);
		if (ev.locations->count) {
			CHECK_STR(pool_str(ev.pool, ev.locations->path[0]), path);
			CHECK(ev.locations->line[0] == 1);
			CHECK(ev.locations->col[0] == 11);
		}
	} else {
		CHECK(!"definition reply");
	}

	/* semantic tokens: add is a function at 1:11 and 4:9 */
	rq = lsp_semantic_tokens(l, path);
	if (wait_ev(l, &ev, LSP_EV_SEMANTIC, rq, 20000)) {
		bool f1 = false, f4 = false, var = false;
		for (u32 i = 0; i < ev.semantic->count; i++) {
			u32 ln = ev.semantic->line[i], c = ev.semantic->col[i];
			if (ln == 1 && c == 11 && ev.semantic->len[i] == 3 && ev.semantic->cls[i] == TK_FUNCTION)
				f1 = true;
			if (ln == 4 && c == 9 && ev.semantic->cls[i] == TK_FUNCTION)
				f4 = true;
			if (ln == 4 && c == 13 && ev.semantic->cls[i] == TK_VARIABLE)
				var = true; /* p */
		}
		fprintf(stderr, "semantic tokens: %u\n", ev.semantic->count);
		CHECK(f1 && f4 && var);
	} else {
		CHECK(!"semantic reply");
	}

	/* edit: fix the error, diagnostics on line 5 go away */
	const char *fixed = "int main(void) { return 0; }\n";
	lsp_did_change(l, path, 2, fixed, strlen(fixed));
	got = false;
	end = vx_now_ns() + 20000000000ull;
	while (!got && vx_now_ns() < end) {
		if (!wait_ev(l, &ev, LSP_EV_DIAGNOSTICS, -1, 20000))
			break;
		if (ev.diags->count == 0)
			got = true;
	}
	CHECK(got);
	lsp_did_save(l, path);
	lsp_did_close(l, path);

	lsp_stop(l);
	/* the child must be reaped: no children left */
	CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

	unlink(path);
	unlink(flags);
	char cache[300];
	snprintf(cache, sizeof cache, "rm -rf '%s'", dir);
	if (system(cache) != 0)
		fprintf(stderr, "cleanup failed\n");
}

static void frame_to(FILE *f, const char *body)
{
	fprintf(f, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
}

/* A scripted server that answers in UTF-16: positions must be converted. */
static void utf16_tests(void)
{
	char dir[] = "/tmp/vxide-lsp16-XXXXXX";
	if (!mkdtemp(dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	char a[300], b[300], doc[300], cmd[1024];
	snprintf(a, sizeof a, "%s/a", dir);
	snprintf(b, sizeof b, "%s/b", dir);
	snprintf(doc, sizeof doc, "%s/f.c", dir);
	char *uri = lsp_path_to_uri(doc);
	FILE *f = fopen(a, "wb");
	frame_to(f, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{"
	            "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":"
	            "[\"variable\",\"function\"],\"tokenModifiers\":[]}}}}}");
	char body[1024];
	snprintf(body, sizeof body,
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
	         "\"params\":{\"uri\":\"%s\",\"diagnostics\":[{\"range\":{\"start\":"
	         "{\"line\":0,\"character\":4},\"end\":{\"line\":0,\"character\":5}},"
	         "\"severity\":2,\"message\":\"w\\u00e9\"}]}}",
	         uri);
	frame_to(f, body);
	/* a server->client request that must be answered */
	frame_to(f, "{\"jsonrpc\":\"2.0\",\"id\":\"cfg\",\"method\":\"workspace/configuration\","
	            "\"params\":{\"items\":[{},{}]}}");
	fclose(f);
	f = fopen(b, "wb");
	frame_to(f, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"data\":[0,4,2,1,0,1,0,1,0,0]}}");
	fclose(f);
	snprintf(cmd, sizeof cmd, "cat '%s'; sleep 1; cat '%s'; exec sleep 5", a, b);
	char *argv[] = {"sh", "-c", cmd, NULL};
	Lsp *l = lsp_start(dir, argv);
	CHECK(l != NULL);
	if (!l)
		return;
	/* x, e-acute, U+1F600, y, z; then a */
	const char *text = "x\xc3\xa9\xf0\x9f\x98\x80yz\na\n";
	lsp_did_open(l, doc, "c", 1, text, strlen(text));
	LspEvent ev;
	CHECK(wait_ev(l, &ev, LSP_EV_READY, -1, 5000));
	CHECK(!lsp_utf8(l));
	CHECK(wait_ev(l, &ev, LSP_EV_DIAGNOSTICS, -1, 5000));
	CHECK_STR(ev.path, doc);
	CHECK(ev.diags->count == 1);
	if (ev.diags->count == 1) {
		CHECK(ev.diags->col0[0] == 7 && ev.diags->col1[0] == 8);
		CHECK(ev.diags->severity[0] == LSP_SEV_WARNING);
		CHECK_STR(pool_str(ev.pool, ev.diags->message[0]), "w\xc3\xa9");
	}
	i32 rq = lsp_semantic_tokens(l, doc);
	CHECK(rq == 2);
	CHECK(wait_ev(l, &ev, LSP_EV_SEMANTIC, rq, 5000));
	CHECK(ev.semantic->count == 2);
	if (ev.semantic->count == 2) {
		CHECK(ev.semantic->line[0] == 0 && ev.semantic->col[0] == 7 && ev.semantic->len[0] == 2);
		CHECK(ev.semantic->cls[0] == TK_FUNCTION);
		CHECK(ev.semantic->line[1] == 1 && ev.semantic->col[1] == 0 && ev.semantic->len[1] == 1);
		CHECK(ev.semantic->cls[1] == TK_VARIABLE);
	}
	lsp_stop(l);
	CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
	free(uri);
	unlink(a);
	unlink(b);
	rmdir(dir);
}

int main(void)
{
	unit_tests();
	utf16_tests();

	/* spawn failure is reported as NULL */
	char *bad[] = {"/nonexistent/vxide-no-such-server", NULL};
	CHECK(lsp_start("/tmp", bad) == NULL);

	const char *servers[] = {"clangd", "clangd23"};
	for (size_t i = 0; i < VX_COUNT(servers); i++) {
		char probe[64];
		snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1", servers[i]);
		if (system(probe) != 0) {
			fprintf(stderr, "skip: %s not found\n", servers[i]);
			continue;
		}
		clangd_tests(servers[i]);
	}
	return tap_done();
}
