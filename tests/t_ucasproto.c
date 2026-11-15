/* ucasproto: requests and responses of ucas's JSON frontend (the shapes are
 * ucas's own answers, trimmed). */
#include "ucasproto.h"
#include "syntax.h"
#include "tap.h"

static UcasResp r;

static bool parse(const char *s) { return ucas_parse(&r, s, (u32)strlen(s)); }

int main(void)
{
	/* base64, with JSON's "\/" */
	u8 out[64];
	u32 n = ucas_b64_decode("R0lGODlh", 8, out);
	CHECK(n == 6 && !memcmp(out, "GIF89a", 6));
	n = ucas_b64_decode("aGk\\/Pz8=", 9, out); /* "hi?" "??" : '/' escaped */
	CHECK(n == 5 && !memcmp(out, "hi???", 5));
	n = ucas_b64_decode("YQ==", 4, out);
	CHECK(n == 1 && out[0] == 'a');

	/* a plot: text, structured kind, a GIF inline */
	CHECK(parse("{\"id\": 7, \"ok\": true, \"cmd\": \"ucas\", \"ucas\": \"plot sin x\", \"structured\": true, "
	            "\"result\": {\"kind\": \"plot\", \"image\": {\"width\": 640}}, \"notes\": [], \"conditions\": [], "
	            "\"checks\": [], \"files\": [{\"name\": \"plot.gif\", \"mime\": \"image/gif\", \"bytes\": 6, "
	            "\"base64\": \"R0lGODlh\"}], \"text\": \"plot of 1 function\\n  image: 640x440\\n\"}"));
	CHECK(r.id == 7 && r.ok);
	CHECK_STR(ucas_s(&r, r.kind), "plot");
	CHECK_STR(ucas_s(&r, r.ucas), "plot sin x");
	CHECK_STR(ucas_s(&r, r.text), "plot of 1 function\n  image: 640x440\n");
	CHECK(r.nfiles == 1 && r.files[0].n == 6 && !memcmp(r.files[0].data, "GIF89a", 6));
	CHECK_STR(ucas_s(&r, r.files[0].name), "plot.gif");
	CHECK_STR(ucas_s(&r, r.files[0].mime), "image/gif");
	CHECK(r.err_code == UCAS_NONE);

	/* proof, checks, notes, conditions */
	CHECK(parse("{\"id\": 2, \"ok\": true, \"proof\": {\"kind\": \"sat\", \"text\": \"proved by SAT: 745 AIG nodes\"}, "
	            "\"checks\": [{\"kind\": \"derivative\", \"points\": 401, \"passed\": true, \"text\": \"d/dx\"}, "
	            "{\"kind\": \"numeric_integral\", \"passed\": false, \"text\": \"GK\"}], "
	            "\"notes\": [{\"kind\": \"info\", \"text\": \"x/x = 1 where x != 0\"}], \"conditions\": [\"x != 0\"], "
	            "\"files\": []}"));
	CHECK(r.nfiles == 0); /* the last response's file was freed */
	CHECK(r.proof == UCAS_PROOF_SAT);
	CHECK_STR(ucas_s(&r, r.proof_text), "proved by SAT: 745 AIG nodes");
	CHECK(r.nchecks == 2 && r.check_passed[0] && !r.check_passed[1]);
	CHECK_STR(ucas_s(&r, r.check_kind[1]), "numeric_integral");
	CHECK(r.nnotes == 1);
	CHECK_STR(ucas_s(&r, r.note_text[0]), "x/x = 1 where x != 0");
	CHECK(r.nconds == 1);
	CHECK_STR(ucas_s(&r, r.cond[0]), "x != 0");

	/* an ambiguity: its key and readings */
	CHECK(parse("{\"id\": 4, \"ok\": false, \"cmd\": \"ucas\", \"ucas\": \"k = 3 ^ 5 & 1\", \"text\": \"\", "
	            "\"error\": {\"code\": \"ambiguous\", \"message\": \"\\\"^\\\" between integers\", \"key\": \"caret\", "
	            "\"readings\": [{\"index\": 1, \"reading\": \"xor (bitwise exclusive or)\", \"default\": true}, "
	            "{\"index\": 2, \"reading\": \"power\", \"default\": false}], \"how\": \"repeat ...\"}}"));
	CHECK(!r.ok);
	CHECK_STR(ucas_s(&r, r.err_code), "ambiguous");
	CHECK_STR(ucas_s(&r, r.err_msg), "\"^\" between integers");
	CHECK_STR(ucas_s(&r, r.err_key), "caret");
	CHECK(r.nread == 2 && r.read_index[1] == 2 && r.read_default[0] && !r.read_default[1]);
	CHECK_STR(ucas_s(&r, r.read_text[1]), "power");

	/* a parse error's position */
	CHECK(parse("{\"id\": 5, \"ok\": false, \"error\": {\"code\": \"parse_error\", \"message\": \"expected )\", "
	            "\"pos\": {\"line\": 1, \"col\": 9}}}"));
	CHECK(r.err_line == 1 && r.err_col == 9);

	/* state */
	CHECK(parse("{\"id\": 6, \"ok\": true, \"cmd\": \"state\", \"result\": {\"kind\": \"state\", \"functions\": "
	            "[{\"name\": \"f\", \"params\": [{\"name\": \"x\", \"type\": \"u8\"}], \"body\": \"3x\", "
	            "\"signature\": \"f(x: u8)\"}], \"models\": [\"gt1030\", \"zen4\"], \"active_model\": \"gt1030\"}}"));
	CHECK(r.nfuncs == 1);
	CHECK_STR(ucas_s(&r, r.func_sig[0]), "f(x: u8)");
	CHECK_STR(ucas_s(&r, r.func_body[0]), "3x");
	CHECK_STR(ucas_s(&r, r.model), "gt1030");

	/* english: what it was read as */
	CHECK(parse("{\"id\": 8, \"ok\": true, \"cmd\": \"english\", \"result\": {\"kind\": \"english\", "
	            "\"statements\": [\"area x^2 from 0 to 4\"], \"understood\": true}}"));
	CHECK(r.understood && r.nstmts == 1);
	CHECK_STR(ucas_s(&r, r.stmt[0]), "area x^2 from 0 to 4");

	CHECK(!parse("not json"));
	CHECK(!parse("[1, 2]"));

	/* requests */
	JBuf b = {0};
	ucas_request(&b, 3, "ucas", "source", "f(x: u8) = \"x\"\nrange f", 22, NULL, 0, NULL, NULL, 0, 86400);
	CHECK_STR(b.b, "{\"id\": 3, \"cmd\": \"ucas\", \"args\": {\"source\": \"f(x: u8) = \\\"x\\\"\\nrange f\", "
	               "\"timeout_s\": 86400}}\n");
	jb_reset(&b);
	ucas_request(&b, 4, "ucas", "source", "k = 3 ^ 5 & 1", 13, NULL, 0, NULL, "caret", 2, 0);
	CHECK_STR(b.b, "{\"id\": 4, \"cmd\": \"ucas\", \"args\": {\"source\": \"k = 3 ^ 5 & 1\", \"assume\": {\"caret\": 2}}}\n");
	jb_reset(&b);
	ucas_request(&b, 5, "state", NULL, NULL, 0, NULL, 0, NULL, NULL, 0, 0);
	CHECK_STR(b.b, "{\"id\": 5, \"cmd\": \"state\", \"args\": {}}\n");
	jb_reset(&b);
	ucas_request(&b, 6, "english", "text", "simplify", 8, "uint f(uint x)\n{ return x; }", 28, "glsl", NULL, 0, 60);
	CHECK_STR(b.b, "{\"id\": 6, \"cmd\": \"english\", \"args\": {\"text\": \"simplify\", \"code\": \"uint f(uint x)\\n{ "
	               "return x; }\", \"lang\": \"glsl\", \"timeout_s\": 60}}\n");
	jb_reset(&b);
	ucas_request(&b, 7, "english", "text", "simplify", 8, "x", 1, NULL, NULL, 0, 0);
	CHECK_STR(b.b, "{\"id\": 7, \"cmd\": \"english\", \"args\": {\"text\": \"simplify\", \"code\": \"x\"}}\n");
	jb_free(&b);

	/* code blocks */
	UcasSeg g[8];
#define SPLIT(s) ucas_split((s), (u32)strlen(s), g, 8)
#define SEQ(p, n, lit) ((n) == strlen(lit) && !memcmp((p), (lit), (n)))
	CHECK(SPLIT("range (x: u8) / 3") == 1 && !g[0].code && SEQ(g[0].stmt, g[0].stmt_n, "range (x: u8) / 3"));
	CHECK(SPLIT("simplify\n```glsl\nuint f(uint x) { return x; }\n```\n") == 1);
	CHECK(SEQ(g[0].stmt, g[0].stmt_n, "simplify") && SEQ(g[0].code, g[0].code_n, "uint f(uint x) { return x; }"));
	CHECK(SEQ(g[0].tag, g[0].tag_n, "glsl") && !strcmp(g[0].ucas_lang, "glsl") && g[0].closed);
	/* statements before it, the statement on the line before, text after */
	CHECK(SPLIT("f(x: u8) = x + 1\nrange f\n\nanimate exec of this seed 1\n  ```comp\nvoid main() {}\n\nx;\n  ```\nplot f") == 3);
	CHECK(!g[0].code && SEQ(g[0].stmt, g[0].stmt_n, "f(x: u8) = x + 1\nrange f\n\n"));
	CHECK(SEQ(g[1].stmt, g[1].stmt_n, "animate exec of this seed 1") && g[1].lang == LANG_GLSL);
	CHECK(SEQ(g[1].code, g[1].code_n, "void main() {}\n\nx;"));
	CHECK(!g[2].code && SEQ(g[2].stmt, g[2].stmt_n, "plot f"));
	/* only a block: simplify; SPIR-V and C tags; untagged; unclosed */
	CHECK(SPLIT("```spv\n%1 = OpUndef %uint\n```") == 1 && SEQ(g[0].stmt, g[0].stmt_n, "simplify"));
	CHECK(g[0].lang == LANG_SPIRV && !strcmp(g[0].ucas_lang, "spv"));
	CHECK(SPLIT("c optimize nofile\n```c\nint f(int x) { return x * 2; }\n```") == 1 && !strcmp(g[0].ucas_lang, "c"));
	CHECK(SPLIT("simplify\n```\n#version 450\n```") == 1 && g[0].ucas_lang == NULL && g[0].tag_n == 0);
	CHECK(SPLIT("simplify\n```frag\nvoid main() {}") == 1 && !g[0].closed && SEQ(g[0].code, g[0].code_n, "void main() {}"));
	CHECK(SPLIT("simplify\n```glsl\n```") == 1 && g[0].code && g[0].code_n == 0);
	/* two blocks */
	CHECK(SPLIT("simplify\n```glsl\na\n```\nsimplify\n```spv\nb\n```") == 2 && SEQ(g[1].code, g[1].code_n, "b"));
	CHECK(SPLIT("\n\n") == 0);
	const char *tg;
	u32 tn;
	CHECK(ucas_fence_open("```glsl", 7, &tg, &tn) && tn == 4 && !ucas_fence_open("``", 2, &tg, &tn));
	CHECK(!ucas_fence_open("```c foo", 8, &tg, &tn) && ucas_fence_close("  ```  ", 7) && !ucas_fence_close("```c", 4));

	/* English or statements; open brackets */
	CHECK(ucas_looks_english("the area under x^2 from 0 to 4", 30));
	CHECK(!ucas_looks_english("range (x: u8) / 3", 17));
	CHECK(!ucas_looks_english("avg(x: u32, y: u32) = (x & y) + 1", 34));
	CHECK(ucas_open_depth("f(x: u8) = (x +", 15) == 1);
	CHECK(ucas_open_depth("f(x: u8) = x", 12) == 0);

	ucas_resp_free(&r);
	return tap_done();
}
