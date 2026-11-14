#include "json.h"
#include "tap.h"

static Json j;

static int parse(const char *s) { return json_parse(&j, s, (u32)strlen(s)); }

int main(void)
{
	/* scalars */
	CHECK(parse("42") == 0 && j.n == 1 && j.type[0] == JS_NUM);
	CHECK(json_int(&j, 0, 0) == 42);
	CHECK(parse(" -17 ") == 0 && json_int(&j, 0, 0) == -17);
	CHECK(parse("1.5e2") == 0 && json_num(&j, 0, 0) == 150.0);
	CHECK(parse("true") == 0 && json_bool(&j, 0, false));
	CHECK(parse("null") == 0 && j.type[0] == JS_NULL);

	/* nesting and navigation */
	const char *doc = "{\"a\":1,\"b\":[true,{\"c\":\"x\"},[]],\"d\":{\"e\":{\"f\":-3}},\"g\":\"h\"}";
	CHECK(parse(doc) == 0);
	CHECK(j.type[0] == JS_OBJ && j.size[0] == 4);
	CHECK(json_int(&j, json_get(&j, 0, "a"), 0) == 1);
	u32 b = json_get(&j, 0, "b");
	CHECK(json_type(&j, b) == JS_ARR && j.size[b] == 3);
	CHECK(json_type(&j, json_at(&j, b, 0)) == JS_TRUE);
	CHECK(json_eq(&j, json_get(&j, json_at(&j, b, 1), "c"), "x"));
	CHECK(json_type(&j, json_at(&j, b, 2)) == JS_ARR);
	CHECK(json_at(&j, b, 3) == JSON_NIL);
	CHECK(json_int(&j, json_path(&j, 0, "d.e.f"), 0) == -3);
	CHECK(json_path(&j, 0, "d.x.f") == JSON_NIL);
	CHECK(json_eq(&j, json_get(&j, 0, "g"), "h"));
	CHECK(json_get(&j, 0, "zz") == JSON_NIL);
	CHECK(j.next[b] == json_get(&j, 0, "d") - 1); /* key "d" follows b's subtree */
	CHECK(json_type(&j, json_at(&j, 0, 3)) == JS_STR); /* object member values */

	/* unescape */
	CHECK(parse("\"a\\n\\t\\\"\\\\\\/\\u00e9\\ud83d\\ude00z\"") == 0);
	Pool p = {0};
	u32 off = json_str_pool(&j, 0, &p);
	CHECK_STR(pool_str(&p, off), "a\n\t\"\\/\xc3\xa9\xf0\x9f\x98\x80z");
	CHECK(json_eq(&j, 0, "a\n\t\"\\/\xc3\xa9\xf0\x9f\x98\x80z"));
	/* lone surrogate -> U+FFFD */
	char out[32];
	json_unescape("\\ud800x", 7, out);
	CHECK_STR(out, "\xef\xbf\xbdx");
	/* key with escapes */
	CHECK(parse("{\"k\\u0065y\":5}") == 0 && json_int(&j, json_get(&j, 0, "key"), 0) == 5);

	/* malformed */
	const char *bad[] = {
		"", "{", "[1,]", "{\"a\"}", "{\"a\":}", "[1 2]", "\"abc", "tru", "01",
		"1.", "-", "{\"a\":1,}", "\"\\x\"", "\"\\u12g4\"", "[1]]", "{a:1}",
		"\"tab\there\"",
	};
	for (size_t i = 0; i < VX_COUNT(bad); i++) {
		int r = parse(bad[i]);
		if (r != -1)
			fprintf(stderr, "accepted bad input: %s\n", bad[i]);
		CHECK(r == -1);
	}
	/* deep nesting is bounded */
	char deep[2000];
	memset(deep, '[', 1000);
	memset(deep + 1000, ']', 1000);
	CHECK(json_parse(&j, deep, 2000) == -1);
	memset(deep, '[', 100);
	memset(deep + 100, ']', 100);
	CHECK(json_parse(&j, deep, 200) == 0 && j.n == 100);

	/* writer round trip */
	JBuf w = {0};
	jb_raw(&w, "{\"s\":");
	jb_str(&w, "q\"b\\n\n\x01\xc3\xa9");
	jb_raw(&w, ",\"i\":");
	jb_int(&w, -123456789012LL);
	jb_raw(&w, "}");
	CHECK_STR(w.b, "{\"s\":\"q\\\"b\\\\n\\n\\u0001\xc3\xa9\",\"i\":-123456789012}");
	CHECK(json_parse(&j, w.b, w.n) == 0);
	CHECK(json_eq(&j, json_get(&j, 0, "s"), "q\"b\\n\n\x01\xc3\xa9"));
	CHECK(json_int(&j, json_get(&j, 0, "i"), 0) == -123456789012LL);
	jb_free(&w);
	pool_free(&p);
	json_free(&j);
	return tap_done();
}
