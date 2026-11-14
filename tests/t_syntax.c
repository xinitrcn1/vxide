/* Syntax lexer tests: golden span classes per language. */
#define _POSIX_C_SOURCE 200809L
#include "syntax.h"
#include "tap.h"

#include <time.h>

static u32 sp_start[512], sp_len[512];
static u8 sp_cls[512];
static u8 map[4096];
static const char *cur_line;

/* Lex one line and expand the spans into a per-byte class map. */
static u32 lex(int lang, u32 state, const char *line)
{
	SynSpans o = { sp_start, sp_len, sp_cls, 0, 512 };
	u32 n = (u32)strlen(line);
	u32 st = syn_line(lang, state, line, n, &o);
	memset(map, TK_TEXT, n);
	u32 end = 0;
	for (u32 i = 0; i < o.count; i++) {
		if (o.start[i] < end || o.start[i] + o.len[i] > n)
			fprintf(stderr, "bad span %u+%u in \"%s\"\n", o.start[i], o.len[i], line);
		CHECK(o.start[i] >= end && o.start[i] + o.len[i] <= n);
		memset(map + o.start[i], o.cls[i], o.len[i]);
		end = o.start[i] + o.len[i];
	}
	cur_line = line;
	return st;
}

/* Class of the nth occurrence of sub in the current line, or -1 when
 * its bytes have mixed classes. */
static int cls_nth(const char *sub, int nth)
{
	const char *p = cur_line;
	for (;;) {
		p = strstr(p, sub);
		if (!p)
			return -2;
		if (nth-- == 0)
			break;
		p++;
	}
	u32 a = (u32)(p - cur_line), l = (u32)strlen(sub);
	for (u32 i = 1; i < l; i++)
		if (map[a + i] != map[a])
			return -1;
	return map[a];
}

static const char *const names[] = {
	"TEXT", "COMMENT", "STRING", "ESCAPE", "NUMBER", "STORAGE", "CONTROL", "TYPE",
	"FUNCTION", "VARIABLE", "CONSTANT", "LANGCONST", "PREPROC", "OPERATOR", "LABEL", "INVALID",
};

static void expect(const char *sub, int nth, int cls, int lineno)
{
	int got = cls_nth(sub, nth);
	tap_n++;
	if (got != cls) {
		tap_fail++;
		fprintf(stderr, "t_syntax.c:%d: \"%s\" in \"%s\": got %s, want %s\n", lineno, sub, cur_line,
		        got >= 0 ? names[got] : got == -1 ? "(mixed)" : "(missing)", names[cls]);
	}
}
#define T(sub, cls) expect(sub, 0, cls, __LINE__)
#define TN(sub, nth, cls) expect(sub, nth, cls, __LINE__)

static void test_c(void)
{
	u32 s;
	s = lex(LANG_C, 0, "#include <stdio.h>");
	T("#include", TK_CONTROL);
	T("<stdio.h>", TK_STRING);
	CHECK(s == 0);

	lex(LANG_C, 0, "  #  include \"vx.h\" // hi");
	T("#  include", TK_CONTROL);
	T("\"vx.h\"", TK_STRING);
	T("// hi", TK_COMMENT);

	lex(LANG_C, 0, "#define MAX(a, b) ((a) > (b) ? (a) : (b))");
	T("#define", TK_CONTROL);
	T("MAX", TK_PREPROC);
	T("a", TK_VARIABLE);
	TN("b", 0, TK_VARIABLE);
	T(">", TK_OPERATOR);

	s = lex(LANG_C, 0, "#define FOO 1 + \\");
	T("FOO", TK_PREPROC);
	T("1", TK_NUMBER);
	T("\\", TK_ESCAPE);
	CHECK(s != 0);
	s = lex(LANG_C, s, "  bar(2)");
	T("bar", TK_FUNCTION);
	T("2", TK_NUMBER);
	T("(", TK_PREPROC);
	CHECK(s == 0);

	lex(LANG_C, 0, "#if defined(FOO) && __has_include(<x.h>)");
	T("defined", TK_CONTROL);
	T("FOO", TK_PREPROC);
	T("&&", TK_OPERATOR);

	lex(LANG_C, 0, "#error \"no\" support");
	T("\"no\" support", TK_STRING);
	lex(LANG_C, 0, "#embed \"blob.bin\" limit(4)");
	T("#embed", TK_CONTROL);
	T("\"blob.bin\"", TK_STRING);
	lex(LANG_C, 0, "#elifdef X");
	T("#elifdef", TK_CONTROL);
	lex(LANG_C, 0, "#pragma once");
	T("once", TK_VARIABLE);

	lex(LANG_C, 0, "static const struct Foo *foo_new(size_t n, uint32_t x);");
	T("static", TK_STORAGE);
	T("const", TK_STORAGE);
	T("struct", TK_STORAGE);
	T("Foo", TK_TYPE);
	T("foo_new", TK_FUNCTION);
	T("size_t", TK_TYPE);
	T("uint32_t", TK_TYPE);
	T("n,", TK_TEXT);

	lex(LANG_C, 0, "my_type_t v; FILE *f; enum color { RED };");
	T("my_type_t", TK_TYPE);
	T("color", TK_TYPE);
	T("FILE", TK_TEXT);

	lex(LANG_C, 0, "\tif (x) return y; else goto done;");
	T("if", TK_CONTROL);
	T("return", TK_CONTROL);
	T("else", TK_CONTROL);
	T("goto", TK_CONTROL);
	T("done", TK_LABEL);
	lex(LANG_C, 0, "switch (k) { case 1: break; default: continue; }");
	T("switch", TK_CONTROL);
	T("case", TK_CONTROL);
	T("default", TK_CONTROL);

	lex(LANG_C, 0, "p->len = s.n + sizeof(int) + alignof(T);");
	T("p", TK_VARIABLE);
	T("len", TK_VARIABLE);
	T("s", TK_VARIABLE);
	TN("n", 1, TK_VARIABLE);
	T("sizeof", TK_STORAGE);
	T("alignof", TK_STORAGE);
	T("=", TK_OPERATOR);

	lex(LANG_C, 0, "obj.method(1);");
	T("method", TK_FUNCTION);

	/* C23 */
	lex(LANG_C, 0, "constexpr int x = 1'000'000 + 0b1010 + 0x1.8p+3 + 10uwb;");
	T("constexpr", TK_STORAGE);
	T("1'000'000", TK_NUMBER);
	T("0b1010", TK_NUMBER);
	T("0x1.8p+3", TK_NUMBER);
	T("10uwb", TK_NUMBER);
	lex(LANG_C, 0, "_BitInt(7) b = nullptr; bool t = true; typeof_unqual(x) y;");
	T("_BitInt", TK_STORAGE);
	T("nullptr", TK_LANGCONST);
	T("bool", TK_STORAGE);
	T("true", TK_LANGCONST);
	T("typeof_unqual", TK_STORAGE);
	lex(LANG_C, 0, "[[nodiscard]] static_assert(N > 0); thread_local _Atomic long z = 1.5e-3f;");
	T("nodiscard", TK_TYPE);
	T("static_assert", TK_STORAGE);
	T("thread_local", TK_STORAGE);
	T("_Atomic", TK_STORAGE);
	T("1.5e-3f", TK_NUMBER);
	lex(LANG_C, 0, "char8_t c = u8'a'; const char *s = u8\"x\" L\"w\";");
	T("u8'a'", TK_STRING);
	T("u8\"x\"", TK_STRING);
	T("L\"w\"", TK_STRING);

	/* strings: escapes and printf placeholders */
	lex(LANG_C, 0, "printf(\"%-08.3lf|%zu\\n\\x41\\q%%\", v);");
	T("printf", TK_FUNCTION);
	T("%-08.3lf", TK_VARIABLE);
	T("%zu", TK_VARIABLE);
	T("\\n", TK_ESCAPE);
	T("\\x41", TK_ESCAPE);
	T("\\q", TK_INVALID);
	T("%%", TK_VARIABLE);
	T("|", TK_STRING);
	lex(LANG_C, 0, "c = '\\0';");
	T("'", TK_STRING);
	T("\\0", TK_ESCAPE);

	/* multi-line block comment */
	s = lex(LANG_C, 0, "int a; /* start");
	T("/* start", TK_COMMENT);
	T("int", TK_STORAGE);
	CHECK(s != 0);
	s = lex(LANG_C, s, "middle if return");
	T("middle if return", TK_COMMENT);
	s = lex(LANG_C, s, "end */ int b;");
	T("end */", TK_COMMENT);
	T("int", TK_STORAGE);
	CHECK(s == 0);

	/* doxygen */
	s = lex(LANG_C, 0, "/** Frobnicate. @param n count @return 0");
	T("@param", TK_STORAGE);
	T("n", TK_COMMENT); /* the 'n' in Frobnicate */
	T(" count", TK_COMMENT);
	T("@return", TK_STORAGE);
	TN("n ", 0, -1);
	s = lex(LANG_C, s, " * \\brief x */");
	T("\\brief", TK_STORAGE);
	CHECK(s == 0);
	lex(LANG_C, 0, "/* @param not doc */");
	T("@param", TK_COMMENT);

	/* string continued with backslash */
	s = lex(LANG_C, 0, "const char *m = \"abc\\");
	CHECK(s != 0);
	s = lex(LANG_C, s, "def\" ; int z;");
	T("def\"", TK_STRING);
	T("int", TK_STORAGE);
	CHECK(s == 0);

	/* #if 0 dead region */
	s = lex(LANG_C, 0, "#if 0");
	T("#if", TK_CONTROL);
	CHECK(s != 0);
	s = lex(LANG_C, s, "int dead(void);");
	T("int dead(void);", TK_COMMENT);
	s = lex(LANG_C, s, "#ifdef X");
	T("#ifdef X", TK_COMMENT);
	s = lex(LANG_C, s, "#endif");
	T("#endif", TK_COMMENT);
	s = lex(LANG_C, s, "#else");
	T("#else", TK_CONTROL);
	CHECK(s == 0);
	s = lex(LANG_C, s, "int live;");
	T("int", TK_STORAGE);

	/* line comment continued */
	s = lex(LANG_C, 0, "// comment \\");
	CHECK(s != 0);
	s = lex(LANG_C, s, "still comment");
	T("still comment", TK_COMMENT);
	CHECK(s == 0);
}

static void test_asm(void)
{
	u32 s;
	lex(LANG_ASM_GAS, 0, "main:\tmovq %rsp, %rbp  # frame");
	T("main", TK_FUNCTION);
	T("movq", TK_STORAGE);
	T("%rsp", TK_VARIABLE);
	T("%rbp", TK_VARIABLE);
	T("# frame", TK_COMMENT);
	lex(LANG_ASM_GAS, 0, "\t.globl\tmain");
	T(".globl", TK_CONTROL);
	T("main", TK_TEXT);
	lex(LANG_ASM_GAS, 0, "\taddl $0x10, -8(%rbp); jne .L3");
	T("addl", TK_STORAGE);
	T("$0x10", TK_NUMBER);
	T("-", TK_OPERATOR);
	T("8", TK_NUMBER);
	T("jne", TK_CONTROL);
	T(".L3", TK_TEXT);
	lex(LANG_ASM_GAS, 0, ".L3: vfmadd231ps %zmm1, %zmm2, %zmm3{%k1}");
	T(".L3", TK_FUNCTION);
	T("vfmadd231ps", TK_STORAGE);
	T("%zmm3", TK_VARIABLE);
	T("%k1", TK_VARIABLE);
	lex(LANG_ASM_GAS, 0, "\trep movsb; call puts@PLT; ret");
	T("rep", TK_STORAGE);
	T("movsb", TK_STORAGE);
	T("call", TK_CONTROL);
	T("ret", TK_CONTROL);
	lex(LANG_ASM_GAS, 0, "\tcmovge %eax, %r10d; sete %al; vpternlogd $0x96, %zmm0, %zmm1, %zmm2");
	T("cmovge", TK_STORAGE);
	T("%r10d", TK_VARIABLE);
	T("sete", TK_STORAGE);
	T("vpternlogd", TK_STORAGE);
	lex(LANG_ASM_GAS, 0, "\t.asciz \"hi\\n\"");
	T(".asciz", TK_CONTROL);
	T("\"hi", TK_STRING);
	T("\\n", TK_ESCAPE);
	lex(LANG_ASM_GAS, 0, "#include <asm/unistd.h>");
	T("#include", TK_CONTROL);
	T("<asm/unistd.h>", TK_STRING);
	s = lex(LANG_ASM_GAS, 0, "/* multi");
	CHECK(s == 1);
	s = lex(LANG_ASM_GAS, s, "line */ nop");
	T("line */", TK_COMMENT);
	T("nop", TK_STORAGE);
	CHECK(s == 0);
	lex(LANG_ASM_GAS, 0, "1:\tdec %ecx; jnz 1b; fldt (%rsp); kmovw %k1, %eax");
	T("1", TK_FUNCTION);
	T("1b", TK_NUMBER);
	T("fldt", TK_STORAGE);
	T("kmovw", TK_STORAGE);

	lex(LANG_ASM_INTEL, 0, "section .text");
	T("section", TK_CONTROL);
	lex(LANG_ASM_INTEL, 0, "global _start ; entry");
	T("global", TK_CONTROL);
	T("; entry", TK_COMMENT);
	lex(LANG_ASM_INTEL, 0, "_start: mov rax, qword [rbp - 8] ; load");
	T("_start", TK_FUNCTION);
	T("mov", TK_STORAGE);
	T("rax", TK_VARIABLE);
	T("qword", TK_STORAGE);
	T("rbp", TK_VARIABLE);
	T("8", TK_NUMBER);
	lex(LANG_ASM_INTEL, 0, "msg db \"Hello\", 0Ah, 0");
	T("msg", TK_FUNCTION);
	T("db", TK_CONTROL);
	T("\"Hello\"", TK_STRING);
	T("0Ah", TK_NUMBER);
	lex(LANG_ASM_INTEL, 0, "  times 510-($-$$) db 0");
	T("times", TK_CONTROL);
	T("$$", TK_LANGCONST);
	T("db", TK_CONTROL);
	lex(LANG_ASM_INTEL, 0, "%define SIZE 16");
	T("%define", TK_CONTROL);
	lex(LANG_ASM_INTEL, 0, "  vaddps ymm0, ymm1, YMM2 ; avx");
	T("vaddps", TK_STORAGE);
	T("YMM2", TK_VARIABLE);
	lex(LANG_ASM_INTEL, 0, "  JMP short .done");
	T("JMP", TK_CONTROL);
	T("short", TK_STORAGE);
	lex(LANG_ASM_INTEL, 0, "  mov eax, 1011b ; xor r15b, r15b");
	T("1011b", TK_NUMBER);
}

static void test_lisp(void)
{
	u32 s;
	lex(LANG_LISP, 0, "(defun square (x) \"Square X.\" (* x x)) ; sq");
	T("defun", TK_STORAGE);
	T("square", TK_FUNCTION);
	T("\"Square X.\"", TK_STRING);
	T("*", TK_FUNCTION);
	T("; sq", TK_COMMENT);
	lex(LANG_LISP, 0, "(let ((a 1/2) (b -3.5e2)) (if (null a) nil t))");
	T("let", TK_CONTROL);
	T("1/2", TK_NUMBER);
	T("-3.5e2", TK_NUMBER);
	T("if", TK_CONTROL);
	T("null", TK_FUNCTION);
	T("nil", TK_LANGCONST);
	T(" t)", -1);
	lex(LANG_LISP, 0, "(format t \"~a\\n\" :key '(1 2) #'car `(a ,b ,@c) #\\Space #x1F)");
	T("format", TK_FUNCTION);
	T(":key", TK_CONSTANT);
	T("'", TK_OPERATOR);
	T("#'", TK_OPERATOR);
	T("`", TK_OPERATOR);
	T(",@", TK_OPERATOR);
	T("#\\Space", TK_LANGCONST);
	T("#x1F", TK_NUMBER);
	T("\\n", TK_ESCAPE);
	lex(LANG_LISP, 0, "'(foo bar)");
	T("foo", TK_TEXT);
	lex(LANG_LISP, 0, "(defvar *count* 0) (defstruct point x y) (lambda (&optional z) z)");
	T("*count*", TK_VARIABLE);
	T("point", TK_TYPE);
	T("lambda", TK_CONTROL);
	T("&optional", TK_STORAGE);
	lex(LANG_LISP, 0, "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) #t");
	T("define", TK_STORAGE);
	T("fact", TK_FUNCTION);
	T("=", TK_FUNCTION);
	T("#t", TK_LANGCONST);
	lex(LANG_LISP, 0, "(defn greet [name] (str \"hi \" name))");
	T("defn", TK_STORAGE);
	T("greet", TK_FUNCTION);
	T("str", TK_FUNCTION);

	s = lex(LANG_LISP, 0, "(a #| outer #| inner |# still");
	T("#| outer #| inner |# still", TK_COMMENT);
	CHECK(s != 0);
	s = lex(LANG_LISP, s, "comment |# (b)");
	T("comment |#", TK_COMMENT);
	T("b", TK_FUNCTION);
	CHECK(s == 0);
	s = lex(LANG_LISP, 0, "(setq s \"multi");
	CHECK(s != 0);
	s = lex(LANG_LISP, s, "line\") (foo)");
	T("line\"", TK_STRING);
	T("foo", TK_FUNCTION);
	CHECK(s == 0);
}

static void test_detect(void)
{
	CHECK(syn_lang_for_path("/a/b/x.c", NULL, 0) == LANG_C);
	CHECK(syn_lang_for_path("x.H", NULL, 0) == LANG_C);
	CHECK(syn_lang_for_path("x.lisp", NULL, 0) == LANG_LISP);
	CHECK(syn_lang_for_path("init.el", NULL, 0) == LANG_LISP);
	CHECK(syn_lang_for_path("core.clj", NULL, 0) == LANG_LISP);
	CHECK(syn_lang_for_path("Makefile", NULL, 0) == LANG_NONE);
	CHECK(syn_lang_for_path(".gitignore", NULL, 0) == LANG_NONE);
	const char *gas = "\t.text\n\t.globl main\nmain:\n\tmovl $1, %eax\n";
	const char *nasm = "bits 64\nsection .text\nglobal _start\n_start:\n\tmov rax, [rsp]\n";
	const char *intel_gas = ".intel_syntax noprefix\nmain:\n mov eax, 1\n";
	CHECK(syn_lang_for_path("a.s", gas, strlen(gas)) == LANG_ASM_GAS);
	CHECK(syn_lang_for_path("a.S", nasm, strlen(nasm)) == LANG_ASM_INTEL);
	CHECK(syn_lang_for_path("a.s", intel_gas, strlen(intel_gas)) == LANG_ASM_INTEL);
	CHECK(syn_lang_for_path("a.asm", nasm, strlen(nasm)) == LANG_ASM_INTEL);
	CHECK(syn_lang_for_path("a.asm", gas, strlen(gas)) == LANG_ASM_GAS);
	CHECK(syn_lang_for_path("a.asm", NULL, 0) == LANG_ASM_INTEL);
	CHECK(syn_lang_for_path("a.s", NULL, 0) == LANG_ASM_GAS);
	CHECK_STR(syn_line_comment(LANG_C), "//");
	CHECK_STR(syn_line_comment(LANG_ASM_GAS), "#");
	CHECK_STR(syn_line_comment(LANG_ASM_INTEL), ";");
	CHECK_STR(syn_line_comment(LANG_LISP), ";");
	CHECK_STR(syn_lang_lsp_id(LANG_C), "c");
	CHECK(syn_lang_lsp_id(LANG_NONE) == NULL);
	/* Spans beyond the capacity are dropped, state still tracked. */
	u32 st[2], ln[2];
	u8 cl[2];
	SynSpans o = { st, ln, cl, 0, 2 };
	const char *l = "int a = 1 + 2; /* x";
	u32 s = syn_line(LANG_C, 0, l, (u32)strlen(l), &o);
	CHECK(o.count == 2);
	CHECK(s != 0);
	CHECK(syn_line(LANG_C, 0, l, (u32)strlen(l), NULL) == s);
}

static void bench(void)
{
	static const char *const lines[] = {
		"static int foo_bar(struct Foo *f, size_t n) /* doc */",
		"{",
		"\tfor (size_t i = 0; i < n; i++) {",
		"\t\tif (f->items[i].value > 0x7FFF && !done)",
		"\t\t\tprintf(\"%s: %d\\n\", f->name, (int)i); // trace",
		"\t}",
		"#define LIMIT(x) ((x) < 0 ? 0 : (x))",
		"\treturn 1'000 + LIMIT(n) * sizeof(uint32_t);",
		"}",
		"",
	};
	enum { N = 100000 };
	size_t bytes = 0;
	u32 sp_s[256], sp_l[256];
	u8 sp_c[256];
	SynSpans o = { sp_s, sp_l, sp_c, 0, 256 };
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	u32 s = 0;
	for (int r = 0; r < 5; r++)
		for (u32 i = 0; i < N; i++) {
			const char *l = lines[i % VX_COUNT(lines)];
			u32 n = (u32)strlen(l);
			s = syn_line(LANG_C, s, l, n, &o);
			bytes += n + 1;
		}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
	fprintf(stderr, "bench: C lexer %.1f MB/s (%zu bytes, %.3f s)\n", (double)bytes / sec / 1e6, bytes, sec);
}

int main(void)
{
	test_c();
	test_asm();
	test_lisp();
	test_detect();
	bench();
	return tap_done();
}
