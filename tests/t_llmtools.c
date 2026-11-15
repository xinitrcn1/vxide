/* The LLM agent's built-in tools (after mcp-fbsd): answers, edits, the
 * project boundary, exec's exit status, timeout and cleaned output. */
#define _DEFAULT_SOURCE
#include "llmtools.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static JBuf out;

static bool call(const LlmTools *t, const char *name, const char *args)
{
	return llmt_call(t, name, args, strlen(args), &out);
}

static bool has(const char *s) { return out.b && strstr(out.b, s) != NULL; }

int main(void)
{
	char dir[] = "/tmp/vxide-llmt-XXXXXX";
	if (!mkdtemp(dir))
		return tap_done();
	LlmTools t = {.root = dir};

	/* the schema: OpenAI function tools, read-only hides the writers */
	JBuf s = {0};
	llmt_schema(&t, &s);
	CHECK(strstr(s.b, "\"name\":\"read\"") && strstr(s.b, "\"name\":\"exec\"") && strstr(s.b, "\"type\":\"function\""));
	Json j = {0};
	CHECK(json_parse(&j, s.b, s.n) == 0 && json_type(&j, 0) == JS_ARR && j.size[0] == 9);
	LlmTools ro = {.root = dir, .read_only = true};
	jb_reset(&s);
	llmt_schema(&ro, &s);
	CHECK(!strstr(s.b, "\"write\"") && !strstr(s.b, "\"exec\"") && strstr(s.b, "\"search\""));
	CHECK(!call(&ro, "write", "{\"path\":\"x\",\"content\":\"y\"}") && has("unknown tool"));

	/* write (parents made), read, partial read */
	CHECK(call(&t, "write", "{\"path\":\"src/a.c\",\"content\":\"one\\ntwo\\nthree\\n\"}"));
	CHECK_STR(out.b, "ok");
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\"}"));
	CHECK_STR(out.b, "one\ntwo\nthree\n");
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\",\"offset\":2,\"limit\":1,\"n\":true}"));
	CHECK_STR(out.b, "2\ttwo\n[lines 2-2 of 3]\n");
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\",\"offset\":-1}"));
	CHECK_STR(out.b, "three\n[lines 3-3 of 3]\n");
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\",\"offset\":\"2\",\"limit\":\"1\"}")); /* quoted numbers */
	CHECK_STR(out.b, "two\n[lines 2-2 of 3]\n");
	CHECK(!call(&t, "read", "{\"path\":\"nope.c\"}"));
	CHECK(call(&t, "write", "{\"path\":\"src/a.c\",\"content\":\"four\\n\",\"append\":true}"));
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\",\"offset\":4}") && has("four"));

	/* edit: exact, counted, several at once (all or none) */
	CHECK(call(&t, "edit", "{\"path\":\"src/a.c\",\"old\":\"two\",\"new\":\"2\"}"));
	CHECK(!call(&t, "edit", "{\"path\":\"src/a.c\",\"old\":\"o\",\"new\":\"0\"}") && has("matches"));
	CHECK(call(&t, "edit", "{\"path\":\"src/a.c\",\"old\":\"o\",\"new\":\"0\",\"count\":0}"));
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\"}"));
	CHECK_STR(out.b, "0ne\n2\nthree\nf0ur\n");
	CHECK(!call(&t, "edit", "{\"path\":\"src/a.c\",\"edits\":[{\"old\":\"2\",\"new\":\"two\"},{\"old\":\"zzz\",\"new\":\"\"}]}"));
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\"}") && has("\n2\n")); /* untouched */
	CHECK(call(&t, "edit", "{\"path\":\"src/a.c\",\"edits\":[{\"old\":\"2\",\"new\":\"two\"},{\"old\":\"0ne\",\"new\":\"one\"}]}"));
	CHECK(call(&t, "read", "{\"path\":\"src/a.c\",\"limit\":2}"));
	CHECK_STR(out.b, "one\ntwo\n[lines 1-2 of 4]\n");

	/* ls, stat, mkdir, mv, search */
	CHECK(call(&t, "mkdir", "{\"path\":\"docs/notes\"}"));
	CHECK(call(&t, "write", "{\"path\":\".hidden\",\"content\":\"x\"}"));
	CHECK(call(&t, "ls", "{\"path\":\".\"}"));
	CHECK_STR(out.b, ".hidden 1\ndocs/\nsrc/\n");
	CHECK(call(&t, "ls", "{\"path\":\"\",\"depth\":2,\"hidden\":false}"));
	CHECK_STR(out.b, "docs/\n  notes/\nsrc/\n  a.c 19\n");
	CHECK(call(&t, "stat", "{\"path\":\"src\"}") && !strncmp(out.b, "dir ", 4));
	CHECK(call(&t, "mv", "{\"from\":\"src/a.c\",\"to\":\"lib/b.c\"}"));
	CHECK(call(&t, "search", "{\"path\":\".\",\"glob\":\"*.c\"}"));
	CHECK_STR(out.b, "lib/b.c\n");
	CHECK(call(&t, "search", "{\"path\":\".\",\"re\":\"^t(wo|hree)\"}"));
	CHECK_STR(out.b, "lib/b.c:2:two\nlib/b.c:3:three\n");
	CHECK(call(&t, "search", "{\"path\":\".\",\"re\":\"F0U\",\"i\":true,\"fixed\":true}"));
	CHECK_STR(out.b, "lib/b.c:4:f0ur\n");
	CHECK(call(&t, "search", "{\"path\":\".\",\"re\":\"absent\"}") && has("no matches"));

	/* the project boundary: absolute paths outside, climbing, links out */
	CHECK(!call(&t, "read", "{\"path\":\"/etc/passwd\"}") && has("outside the project"));
	CHECK(!call(&t, "read", "{\"path\":\"../../etc/passwd\"}"));
	CHECK(!call(&t, "write", "{\"path\":\"new/../../x\",\"content\":\"\"}"));
	char lk[300];
	snprintf(lk, sizeof lk, "%s/out", dir);
	CHECK(symlink("/etc", lk) == 0);
	CHECK(!call(&t, "ls", "{\"path\":\"out\"}") && has("outside"));
	CHECK(!call(&t, "mv", "{\"from\":\".\",\"to\":\"x\"}"));
	char abs[300];
	snprintf(abs, sizeof abs, "{\"path\":\"%s/lib/b.c\",\"limit\":1}", dir);
	CHECK(call(&t, "read", abs) && has("one"));

	/* exec: in the project, exit status, stdin, cleaned output, timeout */
	CHECK(call(&t, "exec", "{\"cmd\":\"ls lib; echo err >&2; exit 3\"}"));
	CHECK_STR(out.b, "b.c\nerr\n[exit 3]\n");
	CHECK(call(&t, "exec", "{\"cmd\":\"tr a-z A-Z\",\"stdin\":\"hello\"}"));
	CHECK_STR(out.b, "HELLO\n[exit 0]\n");
	CHECK(call(&t, "exec", "{\"cmd\":\"printf '\\\\033[1;31mred\\\\033[0m\\\\nA 10%%\\\\rA 100%%\\\\n'\"}"));
	CHECK_STR(out.b, "red\nA 100%\n[exit 0]\n");
	CHECK(call(&t, "exec", "{\"cmd\":\"pwd\",\"cwd\":\"docs\"}") && has("/docs\n"));
	CHECK(call(&t, "exec", "{\"cmd\":\"echo $TERM $PAGER\"}") && has("dumb cat"));
	CHECK(!call(&t, "exec", "{\"cmd\":\"sleep 5\",\"timeout_ms\":200}") && has("[killed after 200 ms]"));
	CHECK(call(&t, "exec", "{\"cmd\":\"yes | head -c 200000\"}") && has("bytes cut") && out.n < 40000);
	CHECK(!call(&t, "exec", "{\"cmd\":\"\"}"));
	CHECK(!call(&t, "nope", "{}") && has("unknown tool"));
	CHECK(!call(&t, "read", "[1]") && has("not a JSON object"));

	/* titles for the transcript */
	char ttl[200];
	llmt_title("read", "{\"path\":\"src/main.c\"}", strlen("{\"path\":\"src/main.c\"}"), ttl, sizeof ttl);
	CHECK_STR(ttl, "read src/main.c");
	llmt_title("exec", "{\"cmd\":\"make\\nmake check\"}", strlen("{\"cmd\":\"make\\nmake check\"}"), ttl, sizeof ttl);
	CHECK_STR(ttl, "exec make ...");
	llmt_title("search", "{\"path\":\".\",\"re\":\"main\",\"glob\":\"*.c\"}", strlen("{\"path\":\".\",\"re\":\"main\",\"glob\":\"*.c\"}"), ttl, sizeof ttl);
	CHECK_STR(ttl, "search . /main/ in *.c");

	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)system(cmd);
	jb_free(&out);
	jb_free(&s);
	json_free(&j);
	return tap_done();
}
