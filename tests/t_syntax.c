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
	"HEADING", "BOLD", "ITALIC", "STRIKE", "RAW", "LINK", "LIST", "QUOTE", "TAG", "TAGPUNCT", "ATTR", "REGEX",
};
_Static_assert(sizeof names / sizeof names[0] == TK_COUNT, "names[] matches TK_*");

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


static void test_sh(void)
{
	u32 s;
	s = lex(LANG_SH, 0, "#!/bin/sh");
	T("#!/bin/sh", TK_COMMENT);
	lex(LANG_SH, 0, "if [ -f \"$HOME/.rc\" ]; then echo hi; fi # done");
	T("if", TK_CONTROL);
	T("-f", TK_OPERATOR);
	T("$HOME", TK_VARIABLE);
	T("/.rc", TK_STRING);
	T("then", TK_CONTROL);
	T("echo", TK_FUNCTION);
	T("hi", TK_STRING);
	T("fi", TK_CONTROL);
	T("# done", TK_COMMENT);
	lex(LANG_SH, 0, "FOO=1 BAR=\"x\" make -j4 --prefix=/usr all 2>&1 | tee log");
	T("FOO", TK_VARIABLE);
	TN("=", 0, TK_OPERATOR);
	T("1", TK_NUMBER);
	T("BAR", TK_VARIABLE);
	T("\"x\"", TK_STRING);
	T("make", TK_FUNCTION);
	T("-j4", TK_LANGCONST);
	T("--prefix=/usr", TK_LANGCONST);
	T("all", TK_STRING);
	T("2>&1", TK_OPERATOR);
	T("|", TK_OPERATOR);
	T("tee", TK_FUNCTION);
	lex(LANG_SH, 0, "local x=${1:-def} n=$# st=$?");
	T("local", TK_STORAGE);
	T("x", TK_VARIABLE);
	T("${1:-def}", TK_VARIABLE);
	T("$#", TK_LANGCONST);
	T("$?", TK_LANGCONST);
	lex(LANG_SH, 0, "greet() {");
	T("greet", TK_FUNCTION);
	lex(LANG_SH, 0, "function build {");
	T("function", TK_STORAGE);
	T("build", TK_FUNCTION);
	lex(LANG_SH, 0, "for f in *.c; do cc \"$f\" -o \"${f%.c}\"; done");
	T("for", TK_CONTROL);
	T("in", TK_CONTROL);
	T("do", TK_CONTROL);
	T("cc", TK_FUNCTION);
	T("$f", TK_VARIABLE);
	T("${f%.c}", TK_VARIABLE);
	T("done", TK_CONTROL);
	lex(LANG_SH, 0, "echo \"n=$(wc -l <\"$f\") \\$x\" 'raw $x' $'a\\tb'");
	T("wc", TK_FUNCTION);
	T("-l", TK_LANGCONST);
	T("\\$", TK_ESCAPE);
	T("'raw $x'", TK_STRING);
	T("\\t", TK_ESCAPE);
	lex(LANG_SH, 0, "n=$((a + 2 * $b))");
	T("a", TK_VARIABLE);
	T("2", TK_NUMBER);
	T("$b", TK_VARIABLE);
	T("+", TK_OPERATOR);
	/* heredocs */
	s = lex(LANG_SH, 0, "cat <<EOF > out.txt");
	T("<<", TK_OPERATOR);
	T("cat", TK_FUNCTION);
	CHECK((s & 7) == 4);
	u32 h = s;
	s = lex(LANG_SH, h, "body $x line");
	T("body $x line", TK_STRING);
	CHECK(s == h);
	s = lex(LANG_SH, h, "EOFX");
	CHECK(s == h);
	s = lex(LANG_SH, h, "EOF");
	CHECK(s == 0);
	s = lex(LANG_SH, 0, "\tcat <<-'END'");
	CHECK((s & 7) == 4 && (s & 0x18) == 0x18);
	s = lex(LANG_SH, s, "\t\tEND");
	CHECK(s == 0);
	/* strings spanning lines */
	s = lex(LANG_SH, 0, "msg=\"first");
	CHECK(s == 2);
	s = lex(LANG_SH, s, "second $v\" ; ls");
	T("second", TK_STRING);
	T("$v", TK_VARIABLE);
	T("ls", TK_FUNCTION);
	CHECK(s == 0);
	s = lex(LANG_SH, 0, "x='a");
	CHECK(s == 1);
	s = lex(LANG_SH, s, "b' # c");
	T("b'", TK_STRING);
	T("# c", TK_COMMENT);
	CHECK(s == 0);
	lex(LANG_SH, 0, "case \"$1\" in");
	T("case", TK_CONTROL);
	T("in", TK_CONTROL);
	lex(LANG_SH, 0, "export PATH=/bin:$PATH; return 1");
	T("export", TK_STORAGE);
	T("PATH", TK_VARIABLE);
	T("$PATH", TK_VARIABLE);
	T("return", TK_CONTROL);
	lex(LANG_SH, 0, "test \"x#y\" && true");
	T("x#y", TK_STRING);
	T("&&", TK_OPERATOR);
	T("true", TK_FUNCTION);
	(void)s;
}

static void test_perl(void)
{
	u32 s;
	lex(LANG_PERL, 0, "use strict; my ($x, @y, %h) = (1_000, 0x1F, 0b101); # c");
	T("use", TK_CONTROL);
	T("my", TK_STORAGE);
	T("$x", TK_VARIABLE);
	T("@y", TK_VARIABLE);
	T("%h", TK_VARIABLE);
	T("1_000", TK_NUMBER);
	T("0x1F", TK_NUMBER);
	T("0b101", TK_NUMBER);
	T("# c", TK_COMMENT);
	lex(LANG_PERL, 0, "sub greet { print \"hi $name\\n\" if $#ARGV > 0; }");
	T("sub", TK_STORAGE);
	T("greet", TK_FUNCTION);
	T("print", TK_FUNCTION);
	T("\"hi ", TK_STRING);
	T("$name", TK_VARIABLE);
	T("\\n", TK_ESCAPE);
	T("if", TK_CONTROL);
	T("$#ARGV", TK_VARIABLE);
	lex(LANG_PERL, 0, "$s =~ s{foo(\\d+)}{bar$1}gx; $t =~ tr/a-z/A-Z/;");
	T("s{foo(", TK_REGEX);
	T("\\d", TK_ESCAPE);
	T("}{bar", TK_REGEX);
	T("$1", TK_VARIABLE);
	T("gx", TK_CONTROL);
	T("tr/a-z/A-Z/", TK_REGEX);
	T("=~", TK_OPERATOR);
	lex(LANG_PERL, 0, "my @w = qw(a b c); my $q = q{it's {nested}}; if ($x =~ /^ab+c$/i) {}");
	T("qw(a b c)", TK_STRING);
	T("q{it's {nested}}", TK_STRING);
	T("/^ab+c$/", TK_REGEX);
	CHECK(map[strstr(cur_line, "/i)") - cur_line + 1] == TK_CONTROL);
	lex(LANG_PERL, 0, "my $r = $a / $b / 2; my %h = (key => 1, s => 2, y => 3);");
	TN(" / ", 0, TK_TEXT);
	TN(" / ", 1, TK_TEXT);
	T("key", TK_TEXT);
	T(" s ", TK_TEXT);
	T("=>", TK_OPERATOR);
	lex(LANG_PERL, 0, "split /,/, $line; join ',', @list; $h{y} = 1; $_ = 3;");
	T("split", TK_FUNCTION);
	T("/,/", TK_REGEX);
	T("',", TK_STRING);
	T("{y}", TK_TEXT);
	T("$_", TK_VARIABLE);
	lex(LANG_PERL, 0, "package Foo::Bar; return __PACKAGE__ unless -e $f and not $g;");
	T("package", TK_STORAGE);
	T("Foo::Bar", TK_TYPE);
	T("__PACKAGE__", TK_LANGCONST);
	T("unless", TK_CONTROL);
	T("-e", TK_OPERATOR);
	T("and", TK_OPERATOR);
	T("not", TK_OPERATOR);
	/* POD */
	s = lex(LANG_PERL, 0, "=head1 NAME");
	T("=head1 NAME", TK_COMMENT);
	CHECK(s == 1);
	s = lex(LANG_PERL, s, "my $x = 1;");
	T("my $x = 1;", TK_COMMENT);
	s = lex(LANG_PERL, s, "=cut");
	CHECK(s == 0);
	/* multi-line strings and regexes */
	s = lex(LANG_PERL, 0, "my $t = qq{a $x");
	CHECK((s & 7) == 2);
	s = lex(LANG_PERL, s, "  {b} c} . $y;");
	T("{b} c}", TK_STRING);
	T("$y", TK_VARIABLE);
	CHECK(s == 0);
	s = lex(LANG_PERL, 0, "$x =~ s{a}");
	CHECK((s & 7) == 2);
	s = lex(LANG_PERL, s, "  {b}g;");
	T("{b}", TK_REGEX);
	T("g", TK_CONTROL);
	CHECK(s == 0);
	/* heredocs */
	s = lex(LANG_PERL, 0, "print <<\"EOT\", <<'RAW';");
	T("<<\"EOT\"", TK_STRING);
	CHECK((s & 7) == 3);
	u32 h = s;
	s = lex(LANG_PERL, h, "Hello $name");
	T("Hello ", TK_STRING);
	T("$name", TK_VARIABLE);
	CHECK(s == h);
	s = lex(LANG_PERL, h, "EOT");
	CHECK(s == 0);
	s = lex(LANG_PERL, 0, "my $t = <<~END;");
	s = lex(LANG_PERL, s, "    indented");
	s = lex(LANG_PERL, s, "    END");
	CHECK(s == 0);
	/* __END__ */
	s = lex(LANG_PERL, 0, "__END__");
	CHECK(s == 4);
	s = lex(LANG_PERL, s, "some data $x");
	T("some data $x", TK_TEXT);
	CHECK(s == 4);
}

static void test_troff(void)
{
	u32 s;
	lex(LANG_TROFF, 0, ".TH LS 1 \"2024-01-01\" \"GNU\"");
	T(".TH", TK_CONTROL);
	T("1", TK_NUMBER);
	T("\"2024-01-01\"", TK_STRING);
	lex(LANG_TROFF, 0, ".\\\" a comment");
	T(".\\\" a comment", TK_COMMENT);
	lex(LANG_TROFF, 0, ".SH DESCRIPTION");
	T(".SH", TK_CONTROL);
	T("DESCRIPTION", TK_HEADING);
	lex(LANG_TROFF, 0, ".B bold words");
	T("bold", TK_BOLD);
	T("words", TK_BOLD);
	lex(LANG_TROFF, 0, "Use \\fBls\\fR \\-l with \\*(lq quotes \\*[rq] and \\n(.l \\(em \\s+2big\\s0 \\\" note");
	T("\\fB", TK_ESCAPE);
	T("\\fR", TK_ESCAPE);
	T("\\-", TK_ESCAPE);
	T("\\*(lq", TK_VARIABLE);
	T("\\*[rq]", TK_VARIABLE);
	T("\\n(.l", TK_VARIABLE);
	T("\\(em", TK_ESCAPE);
	T("\\s+2", TK_ESCAPE);
	T("\\s0", TK_ESCAPE);
	T("\\\" note", TK_COMMENT);
	T("ls", TK_TEXT);
	lex(LANG_TROFF, 0, ".Op Fl a Ar file");
	T(".Op", TK_CONTROL);
	T("Fl", TK_STORAGE);
	T("Ar", TK_STORAGE);
	T("file", TK_TEXT);
	lex(LANG_TROFF, 0, "'br");
	T("'br", TK_CONTROL);
	s = lex(LANG_TROFF, 0, ".ig");
	CHECK(s == 1);
	s = lex(LANG_TROFF, s, "ignored text");
	T("ignored text", TK_COMMENT);
	s = lex(LANG_TROFF, s, "..");
	CHECK(s == 0);
}

static void test_md(void)
{
	u32 s;
	lex(LANG_MARKDOWN, 0, "# Title *x*");
	T("# Title *x*", TK_HEADING);
	lex(LANG_MARKDOWN, 0, "Some **bold** and *it* and _it2_ and ~~gone~~ and `code` here.");
	T("**bold**", TK_BOLD);
	T("*it*", TK_ITALIC);
	T("_it2_", TK_ITALIC);
	T("~~gone~~", TK_STRIKE);
	T("`code`", TK_RAW);
	T("Some", TK_TEXT);
	lex(LANG_MARKDOWN, 0, "snake_case_word stays and 2*3*4 too");
	T("snake_case_word", TK_TEXT);
	lex(LANG_MARKDOWN, 0, "- item [link](http://x.org \"t\") <b class=\"a\">x</b>");
	T("-", TK_LIST);
	T("link", TK_STRING);
	T("http://x.org", TK_LINK);
	T("<", TK_TAGPUNCT);
	TN("b", 0, TK_TAG);
	T("class", TK_ATTR);
	T("\"a\"", TK_STRING);
	lex(LANG_MARKDOWN, 0, "12. step see https://example.com/a.");
	T("12.", TK_LIST);
	T("https://example.com/a", TK_LINK);
	lex(LANG_MARKDOWN, 0, "> quoted \\*not italic\\*");
	T(">", TK_QUOTE);
	T("\\*", TK_ESCAPE);
	/* setext heading needs a paragraph above */
	s = lex(LANG_MARKDOWN, 0, "Para text");
	CHECK(s & (1u << 12));
	lex(LANG_MARKDOWN, s, "=====");
	T("=====", TK_HEADING);
	lex(LANG_MARKDOWN, 0, "---");
	T("---", TK_TEXT);
	/* fenced code with an embedded lexer */
	s = lex(LANG_MARKDOWN, 0, "```c");
	CHECK((s & 3) == 1);
	s = lex(LANG_MARKDOWN, s, "int x = 1; /* open");
	T("int", TK_STORAGE);
	T("1", TK_NUMBER);
	CHECK((s & 3) == 1 && (s >> 12) != 0);
	s = lex(LANG_MARKDOWN, s, "still comment */ return;");
	T("still comment */", TK_COMMENT);
	T("return", TK_CONTROL);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
	s = lex(LANG_MARKDOWN, 0, "~~~bash");
	s = lex(LANG_MARKDOWN, s, "echo $HOME");
	T("echo", TK_FUNCTION);
	T("$HOME", TK_VARIABLE);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK((s & 3) == 1); /* a ``` does not close a ~~~ fence */
	s = lex(LANG_MARKDOWN, s, "~~~~");
	CHECK(s == 0);
	s = lex(LANG_MARKDOWN, 0, "```unknownlang");
	s = lex(LANG_MARKDOWN, s, "plain *stuff*");
	T("plain *stuff*", TK_TEXT);
	s = lex(LANG_MARKDOWN, s, "```");
	/* html comments across lines */
	s = lex(LANG_MARKDOWN, 0, "text <!-- hidden");
	T("<!-- hidden", TK_COMMENT);
	CHECK(s == 2);
	s = lex(LANG_MARKDOWN, s, "more --> **after**");
	T("more -->", TK_COMMENT);
	T("**after**", TK_BOLD);
	CHECK((s & 3) == 0);
}

static void test_detect2(void)
{
	const char *bang_sh = "#!/bin/sh\necho hi\n";
	const char *bang_bash = "#!/usr/bin/env bash\nset -e\n";
	const char *bang_env = "#!/usr/bin/env -S perl -w\n";
	const char *bang_perl = "#!/usr/local/bin/perl5.36 -w\nuse strict;\n";
	const char *bang_py = "#!/usr/bin/env python3\n";
	CHECK(syn_lang_for_path("configure", bang_sh, strlen(bang_sh)) == LANG_SH);
	CHECK(syn_lang_for_path("tools/run", bang_bash, strlen(bang_bash)) == LANG_SH);
	CHECK(syn_lang_for_path("x", bang_env, strlen(bang_env)) == LANG_PERL);
	CHECK(syn_lang_for_path("script", bang_perl, strlen(bang_perl)) == LANG_PERL);
	CHECK(syn_lang_for_path("script", bang_py, strlen(bang_py)) == LANG_NONE);
	CHECK(syn_lang_for_path("configure", NULL, 0) == LANG_NONE);
	CHECK(syn_lang_for_path("a.sh", NULL, 0) == LANG_SH);
	CHECK(syn_lang_for_path("a.bash", NULL, 0) == LANG_SH);
	CHECK(syn_lang_for_path("/home/u/.bashrc", NULL, 0) == LANG_SH);
	CHECK(syn_lang_for_path(".zshrc", NULL, 0) == LANG_SH);
	CHECK(syn_lang_for_path("PKGBUILD", NULL, 0) == LANG_SH);
	CHECK(syn_lang_for_path("README.md", NULL, 0) == LANG_MARKDOWN);
	CHECK(syn_lang_for_path("x.markdown", NULL, 0) == LANG_MARKDOWN);
	CHECK(syn_lang_for_path("lib/A.pm", NULL, 0) == LANG_PERL);
	CHECK(syn_lang_for_path("a.pl", NULL, 0) == LANG_PERL);
	CHECK(syn_lang_for_path("a.pod", NULL, 0) == LANG_PERL);
	CHECK(syn_lang_for_path("t/basic.t", "use Test::More;\n", 16) == LANG_PERL);
	const char *th = ".\\\" comment\n.TH X 1\n";
	CHECK(syn_lang_for_path("x.t", th, strlen(th)) == LANG_TROFF);
	CHECK(syn_lang_for_path("ls.1", NULL, 0) == LANG_TROFF);
	CHECK(syn_lang_for_path("ssl.3ssl", NULL, 0) == LANG_TROFF);
	CHECK(syn_lang_for_path("x.3p", NULL, 0) == LANG_TROFF);
	CHECK(syn_lang_for_path("x.mdoc", NULL, 0) == LANG_TROFF);
	CHECK(syn_lang_for_path("x.ms", NULL, 0) == LANG_TROFF);
	CHECK(syn_lang_for_path("x.cgi", bang_sh, strlen(bang_sh)) == LANG_SH);
	CHECK(syn_lang_for_path("x.cgi", NULL, 0) == LANG_PERL);
	CHECK(syn_lang_for_path("x.c", bang_sh, strlen(bang_sh)) == LANG_C);
	CHECK_STR(syn_lang_name(LANG_SH), "Shell Script");
	CHECK_STR(syn_lang_name(LANG_TROFF), "Roff");
	CHECK_STR(syn_lang_lsp_id(LANG_SH), "shellscript");
	CHECK_STR(syn_lang_lsp_id(LANG_PERL), "perl");
	CHECK(syn_lang_lsp_id(LANG_TROFF) == NULL);
	CHECK_STR(syn_line_comment(LANG_TROFF), ".\\\"");
	CHECK(syn_line_comment(LANG_MARKDOWN) == NULL);
	CHECK_STR(syn_line_comment(LANG_PERL), "#");
}

static void bench2(int lang, const char *name, const char *const *lines, u32 nl)
{
	u32 sp_s[256], sp_l[256];
	u8 sp_c[256];
	SynSpans o = { sp_s, sp_l, sp_c, 0, 256 };
	struct timespec t0, t1;
	size_t bytes = 0;
	u32 s = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (u32 i = 0; i < 200000; i++) {
		const char *l = lines[i % nl];
		u32 n = (u32)strlen(l);
		s = syn_line(lang, s, l, n, &o);
		bytes += n + 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
	fprintf(stderr, "bench: %s lexer %.1f MB/s\n", name, (double)bytes / sec / 1e6);
}

static void bench_more(void)
{
	static const char *const sh[] = {"for f in *.c; do", "\tcc -O2 -c \"$f\" -o \"${f%.c}.o\" || exit 1",
	                                 "done # build", "X=$(ls | wc -l); echo \"n=$X\""};
	static const char *const pl[] = {"my ($a, $b) = @_;", "return $a =~ s/(\\d+)/<$1>/gr if $b;",
	                                 "print join(', ', map { $_ * 2 } @list), \"\\n\";", "}"};
	static const char *const md[] = {"## Heading", "Some **bold** text with `code` and [a link](http://x).",
	                                 "- item *one*", ""};
	bench2(LANG_SH, "sh", sh, 4);
	bench2(LANG_PERL, "perl", pl, 4);
	bench2(LANG_MARKDOWN, "markdown", md, 4);
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
	test_sh();
	test_perl();
	test_troff();
	test_md();
	test_detect2();
	bench();
	bench_more();
	return tap_done();
}
