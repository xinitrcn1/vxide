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
	"PROPERTY", "INSERTED", "DELETED", "CHANGED", "DIFFHDR", "UNQUOTED",
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
	CHECK(syn_lang_for_path("Makefile", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path(".gitignore", NULL, 0) == LANG_IGNORE);
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
	CHECK(syn_lang_for_path("script", bang_py, strlen(bang_py)) == LANG_PYTHON);
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


static void test_cmake(void)
{
	u32 s;
	s = lex(LANG_CMAKE, 0, "cmake_minimum_required(VERSION 3.20) # min");
	T("cmake_minimum_required", TK_FUNCTION);
	T("VERSION", TK_CONSTANT);
	T("3.20", TK_NUMBER);
	T("# min", TK_COMMENT);
	CHECK(s == 0);
	lex(LANG_CMAKE, 0, "IF(WIN32) elseif (x) endif()");
	T("IF", TK_CONTROL);
	T("elseif", TK_CONTROL);
	T("endif", TK_CONTROL);
	T("WIN32", TK_CONSTANT);
	lex(LANG_CMAKE, 0, "target_link_libraries(app PUBLIC ${LIB_${KIND}} $ENV{HOME} ON off)");
	T("target_link_libraries", TK_FUNCTION);
	T("app", TK_TEXT);
	T("PUBLIC", TK_CONSTANT);
	T("${LIB_", TK_VARIABLE);
	T("${KIND}", TK_VARIABLE);
	T("$ENV{HOME}", TK_VARIABLE);
	T("ON", TK_LANGCONST);
	T("off", TK_LANGCONST);
	lex(LANG_CMAKE, 0, "set(F $<$<CONFIG:Debug>:-g> \"a\\tb ${X}\")");
	T("set", TK_FUNCTION);
	T("$<", TK_PREPROC);
	T("$<CONFIG", TK_PREPROC);
	T("\"a", TK_STRING);
	T("\\t", TK_ESCAPE);
	T("${X}", TK_VARIABLE);
	/* bracket comment and bracket argument across lines */
	s = lex(LANG_CMAKE, 0, "#[==[ start");
	T("#[==[ start", TK_COMMENT);
	CHECK(s != 0);
	s = lex(LANG_CMAKE, s, "still ]] inside");
	T("still ]] inside", TK_COMMENT);
	s = lex(LANG_CMAKE, s, "end ]==] project(x)");
	T("end ]==]", TK_COMMENT);
	T("project", TK_FUNCTION);
	CHECK((s & 3u) == 0);
	s = lex(LANG_CMAKE, 0, "message([[multi");
	T("[[multi", TK_STRING);
	s = lex(LANG_CMAKE, s, "line]])");
	T("line]]", TK_STRING);
	CHECK(s == 0);
	/* quoted string spanning lines, inside a command */
	s = lex(LANG_CMAKE, 0, "message(\"one");
	s = lex(LANG_CMAKE, s, "two\" X)");
	T("two\"", TK_STRING);
	T("X", TK_TEXT);
	CHECK(s == 0);
	/* a command name inside arguments is not a command */
	lex(LANG_CMAKE, 0, "foo(bar(x))");
	T("foo", TK_FUNCTION);
	T("bar", TK_TEXT);
}

static void test_make(void)
{
	u32 s;
	/* BSD make */
	lex(LANG_MAKE, 0, ".include <bsd.prog.mk>");
	T(".include", TK_CONTROL);
	T("<bsd.prog.mk>", TK_STRING);
	lex(LANG_MAKE, 0, ".if defined(DEBUG) && ${CC} == \"clang\"");
	T(".if", TK_CONTROL);
	T("defined", TK_FUNCTION);
	T("&&", TK_OPERATOR);
	T("${CC}", TK_VARIABLE);
	T("==", TK_OPERATOR);
	T("\"clang\"", TK_STRING);
	lex(LANG_MAKE, 0, ".  for f in ${SRCS:M*.c}");
	T(".", TK_CONTROL);
	T("for", TK_CONTROL);
	TN("f", 1, TK_VARIABLE); /* the loop variable, not the f of "for" */
	T("in", TK_CONTROL);
	T("${SRCS", TK_VARIABLE);
	T(":M", TK_FUNCTION);
	T("*.c", TK_TEXT);
	T("}", TK_VARIABLE);
	lex(LANG_MAKE, 0, ".endfor");
	T(".endfor", TK_CONTROL);
	lex(LANG_MAKE, 0, "OBJS= ${SRCS:S/.c/.o/g:T} ${.TARGET}");
	T("OBJS", TK_VARIABLE);
	T("=", TK_OPERATOR);
	T(":S", TK_FUNCTION);
	T(":T", TK_FUNCTION);
	T("${.TARGET}", TK_LANGCONST);
	lex(LANG_MAKE, 0, "HASH!= git rev-parse HEAD");
	T("HASH", TK_VARIABLE);
	T("!=", TK_OPERATOR);
	lex(LANG_MAKE, 0, ".PHONY: all clean");
	T(".PHONY", TK_FUNCTION);
	/* GNU make */
	lex(LANG_MAKE, 0, "OBJS := $(patsubst %.c,%.o,$(SRCS)) # objects");
	T("OBJS", TK_VARIABLE);
	T(":=", TK_OPERATOR);
	T("$(patsubst", TK_FUNCTION);
	T("$(SRCS)", TK_VARIABLE);
	T("# objects", TK_COMMENT);
	lex(LANG_MAKE, 0, "CFLAGS += -O2");
	T("CFLAGS", TK_VARIABLE);
	T("+=", TK_OPERATOR);
	lex(LANG_MAKE, 0, "ifeq ($(CC),gcc)");
	T("ifeq", TK_CONTROL);
	T("$(CC)", TK_VARIABLE);
	lex(LANG_MAKE, 0, "else ifdef FOO");
	T("else", TK_CONTROL);
	T("ifdef", TK_CONTROL);
	lex(LANG_MAKE, 0, "%.o: %.c $(DEPS)");
	TN("%", 0, TK_VARIABLE);
	T(".o", TK_FUNCTION);
	T("$(DEPS)", TK_VARIABLE);
	lex(LANG_MAKE, 0, "all: vxide");
	T("all", TK_FUNCTION);
	/* recipe: prefix, shell, make refs painted over */
	s = lex(LANG_MAKE, 0, "\t@echo \"built $@\" && $(CC) -o $@ $^ $(MAKE)");
	T("@", TK_CONTROL);
	T("echo", TK_FUNCTION);
	T("$@", TK_LANGCONST);
	T("$(CC)", TK_VARIABLE);
	T("$^", TK_LANGCONST);
	T("$(MAKE)", TK_LANGCONST);
	CHECK(s == 0);
	/* recipe continuation keeps the shell going */
	s = lex(LANG_MAKE, 0, "\tif [ -f x ]; then \\");
	T("if", TK_CONTROL);
	CHECK((s & 7u) == 2u);
	s = lex(LANG_MAKE, s, "\t  rm -f x; fi");
	T("rm", TK_FUNCTION);
	T("fi", TK_CONTROL);
	CHECK(s == 0);
	/* continued assignment */
	s = lex(LANG_MAKE, 0, "SRCS = a.c \\");
	s = lex(LANG_MAKE, s, "  $(EXTRA) b.c");
	T("$(EXTRA)", TK_VARIABLE);
	CHECK(s == 0);
	/* define body */
	s = lex(LANG_MAKE, 0, "define RULE");
	T("define", TK_CONTROL);
	T("RULE", TK_VARIABLE);
	s = lex(LANG_MAKE, s, "$(1): ; echo $(2)");
	T("$(1)", TK_VARIABLE);
	s = lex(LANG_MAKE, s, "endef");
	T("endef", TK_CONTROL);
	CHECK(s == 0);
	lex(LANG_MAKE, 0, "-include $(wildcard *.d)");
	T("-include", TK_CONTROL);
	T("$(wildcard", TK_FUNCTION);
}

static void test_json(void)
{
	u32 s;
	lex(LANG_JSON, 0, "{\"name\": \"vx\\nide\", \"n\": -1.5e3, \"ok\": true, \"x\": null}");
	T("\"name\"", TK_PROPERTY);
	T("\"vx", TK_STRING);
	T("\\n", TK_ESCAPE);
	T("-1.5e3", TK_NUMBER);
	T("true", TK_LANGCONST);
	T("null", TK_LANGCONST);
	T("\"ok\"", TK_PROPERTY);
	lex(LANG_JSON, 0, "  \"bad\": \"\\q\",");
	T("\\q", TK_INVALID);
	lex(LANG_JSON, 0, "[\"a\", \"b\"]");
	T("\"a\"", TK_STRING);
	lex(LANG_JSON, 0, "\"key\"   :");
	T("\"key\"", TK_PROPERTY);
	s = lex(LANG_JSONC, 0, "{ // comment");
	T("// comment", TK_COMMENT);
	s = lex(LANG_JSONC, 0, "/* block");
	CHECK(s == 1);
	s = lex(LANG_JSONC, s, "end */ \"k\": 1");
	T("end */", TK_COMMENT);
	T("\"k\"", TK_PROPERTY);
	CHECK(s == 0);
	lex(LANG_JSON, 0, "\"\\u00e9\": 1");
	T("\\u00e9", TK_ESCAPE);
}

static void test_ignore(void)
{
	lex(LANG_IGNORE, 0, "# build output");
	T("# build output", TK_COMMENT);
	lex(LANG_IGNORE, 0, "!keep/**/*.o");
	T("!", TK_OPERATOR);
	TN("*", 0, TK_REGEX);
	T("keep", TK_TEXT);
	lex(LANG_IGNORE, 0, "build/");
	T("/", TK_OPERATOR);
	lex(LANG_IGNORE, 0, "\\#notcomment [ab].txt");
	T("\\#", TK_ESCAPE);
	lex(LANG_IGNORE, 0, "*.[oa]");
	T("[oa]", TK_REGEX);
	lex(LANG_IGNORE, 0, "*.sh text eol=lf -diff");
	T("text", TK_PROPERTY);
	T("eol", TK_PROPERTY);
	T("lf", TK_STRING);
	T("-", TK_OPERATOR);
	T("diff", TK_PROPERTY);
}

static void test_diff(void)
{
	/* git format-patch mail headers */
	lex(LANG_DIFF, 0, "From 5368c1466f621135a67f99055b5861a54b4e02a3 Mon Sep 17 00:00:00 2001");
	T("From", TK_DIFFHDR);
	T("5368c1466f621135a67f99055b5861a54b4e02a3", TK_NUMBER);
	lex(LANG_DIFF, 0, "Subject: [PATCH] fix");
	T("Subject:", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "From: Bob <b@x>");
	T("From:", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "diff --git a/x.c b/x.c");
	T("diff --git a/x.c b/x.c", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "--- a/x.c");
	T("--- a/x.c", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "+++ b/x.c");
	T("+++ b/x.c", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "@@ -1,3 +1,4 @@ int main(void)");
	T("@@", TK_TEXT);
	lex(LANG_DIFF, 0, "+\tadded();");
	T("+\tadded();", TK_INSERTED);
	lex(LANG_DIFF, 0, "-\tremoved();");
	T("-\tremoved();", TK_DELETED);
	lex(LANG_DIFF, 0, "! changed");
	T("! changed", TK_CHANGED);
	lex(LANG_DIFF, 0, " context");
	T("context", TK_TEXT);
	lex(LANG_DIFF, 0, "index 1a2b3c4..5d6e7f8 100644");
	T("index", TK_TEXT);
	lex(LANG_DIFF, 0, "commit 2724520aabbccdd (HEAD -> main)");
	T("commit", TK_DIFFHDR);
	T("2724520aabbccdd", TK_NUMBER);
	lex(LANG_DIFF, 0, "Author: Ada <a@x>");
	T("Author:", TK_DIFFHDR);
	lex(LANG_DIFF, 0, "---");
	T("---", TK_TEXT);
}

static void test_git_msgs(void)
{
	u32 s;
	s = lex(LANG_GIT_COMMIT, 0, "This subject line is definitely longer than fifty characters total");
	T("This subject line is definitely longer than fifty ", TK_TEXT); /* 50 characters */
	T("characters total", TK_INVALID);
	s = lex(LANG_GIT_COMMIT, s, "");
	s = lex(LANG_GIT_COMMIT, s, "A body line that is also quite long but is not a subject line at all");
	T("not a subject", TK_TEXT);
	s = lex(LANG_GIT_COMMIT, s, "# Please enter the commit message");
	T("# Please enter the commit message", TK_COMMENT);
	s = lex(LANG_GIT_COMMIT, s, "#\tmodified:   src/x.c");
	T("modified:   src/x.c", TK_CHANGED);
	s = lex(LANG_GIT_COMMIT, s, "#\tnew file:   y.c");
	T("new file:   y.c", TK_INSERTED);
	s = lex(LANG_GIT_COMMIT, s, "#\tdeleted:    z.c");
	T("deleted:    z.c", TK_DELETED);
	s = lex(LANG_GIT_COMMIT, s, "#\tbothmodified: w.c");
	T("bothmodified", TK_STORAGE);
	T("w.c", TK_STRING);
	s = lex(LANG_GIT_COMMIT, s, "diff --git a/x b/x");
	T("diff --git a/x b/x", TK_DIFFHDR);
	s = lex(LANG_GIT_COMMIT, s, "+added");
	T("+added", TK_INSERTED);
	s = lex(LANG_GIT_COMMIT, s, "# not a comment inside the diff");
	T("# not a comment inside the diff", TK_COMMENT);
	CHECK(s == 1);

	lex(LANG_GIT_REBASE, 0, "pick 1a2b3c4 Add the thing");
	T("pick", TK_PROPERTY);
	T("1a2b3c4", TK_NUMBER);
	T("Add the thing", TK_TEXT);
	lex(LANG_GIT_REBASE, 0, "f 5d6e7f8 fixup! Add");
	T("f", TK_PROPERTY);
	T("5d6e7f8", TK_NUMBER);
	lex(LANG_GIT_REBASE, 0, "fixup -C 9abcdef msg");
	T("-C", TK_PROPERTY);
	T("9abcdef", TK_NUMBER);
	lex(LANG_GIT_REBASE, 0, "exec make test");
	T("exec", TK_PROPERTY);
	T("make", TK_FUNCTION);
	lex(LANG_GIT_REBASE, 0, "# Rebase 1a2..3b4 onto 1a2");
	T("# Rebase", TK_COMMENT);
	lex(LANG_GIT_REBASE, 0, "break");
	T("break", TK_PROPERTY);
}

static void test_detect3(void)
{
	CHECK(syn_lang_for_path("/p/CMakeLists.txt", NULL, 0) == LANG_CMAKE);
	CHECK(syn_lang_for_path("cmake/Find.cmake", NULL, 0) == LANG_CMAKE);
	CHECK(syn_lang_for_path("config.cmake.in", NULL, 0) == LANG_CMAKE);
	CHECK(syn_lang_for_path("/x/Makefile", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path("BSDmakefile", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path("GNUmakefile", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path("Makefile.inc", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path("rules.mk", NULL, 0) == LANG_MAKE);
	CHECK(syn_lang_for_path("build.mak", NULL, 0) == LANG_MAKE);
	const char *bsd = "# x\nPROG= foo\n.include <bsd.prog.mk>\n";
	CHECK(syn_lang_for_path("/src/usr.bin/foo/Makefile.depend", bsd, strlen(bsd)) == LANG_MAKE);
	CHECK(syn_lang_for_path("/src/mkfile", bsd, strlen(bsd)) == LANG_MAKE);
	CHECK(syn_lang_for_path("a.json", NULL, 0) == LANG_JSON);
	CHECK(syn_lang_for_path("x/.vscode/settings.json", NULL, 0) == LANG_JSONC);
	CHECK(syn_lang_for_path("tsconfig.base.json", NULL, 0) == LANG_JSONC);
	CHECK(syn_lang_for_path("a.jsonc", NULL, 0) == LANG_JSONC);
	CHECK(syn_lang_for_path("p.code-workspace", NULL, 0) == LANG_JSONC);
	CHECK(syn_lang_for_path("compile_commands.json", NULL, 0) == LANG_JSON);
	CHECK(syn_lang_for_path("/r/.gitignore", NULL, 0) == LANG_IGNORE);
	CHECK(syn_lang_for_path(".dockerignore", NULL, 0) == LANG_IGNORE);
	CHECK(syn_lang_for_path(".gitattributes", NULL, 0) == LANG_IGNORE);
	CHECK(syn_lang_for_path("/r/.git/info/exclude", NULL, 0) == LANG_IGNORE);
	CHECK(syn_lang_for_path("fix.patch", NULL, 0) == LANG_DIFF);
	CHECK(syn_lang_for_path("a.diff", NULL, 0) == LANG_DIFF);
	const char *dg = "diff --git a/x b/x\n";
	CHECK(syn_lang_for_path("noext", dg, strlen(dg)) == LANG_DIFF);
	CHECK(syn_lang_for_path("/r/.git/COMMIT_EDITMSG", NULL, 0) == LANG_GIT_COMMIT);
	CHECK(syn_lang_for_path("MERGE_MSG", NULL, 0) == LANG_GIT_COMMIT);
	CHECK(syn_lang_for_path("/r/.git/rebase-merge/git-rebase-todo", NULL, 0) == LANG_GIT_REBASE);
	const char *sh = "#!/bin/sh\n.include x\n";
	CHECK(syn_lang_for_path("script", sh, strlen(sh)) == LANG_SH);
	CHECK(syn_lang_for_path("notes", "hello\n", 6) == LANG_NONE);
	CHECK_STR(syn_lang_name(LANG_MAKE), "Makefile");
	CHECK_STR(syn_line_comment(LANG_JSONC), "//");
	CHECK(syn_line_comment(LANG_JSON) == NULL);
	CHECK(syn_line_comment(LANG_DIFF) == NULL);
	CHECK_STR(syn_line_comment(LANG_CMAKE), "#");
	CHECK(LANG_SPIRV == 32); /* Markdown keeps a fence language in 5 bits: up to GLSL (31) */
	/* Markdown fences embed the new languages (language id > 15) */
	u32 s = lex(LANG_MARKDOWN, 0, "```jsonc");
	s = lex(LANG_MARKDOWN, s, "{\"a\": 1} // c");
	T("\"a\"", TK_PROPERTY);
	T("// c", TK_COMMENT);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
	s = lex(LANG_MARKDOWN, 0, "```makefile");
	s = lex(LANG_MARKDOWN, s, "all: x");
	T("all", TK_FUNCTION);
	s = lex(LANG_MARKDOWN, s, "````"); /* longer fence closes */
	CHECK((s & 3u) == 0);
}

static void bench3(void)
{
	static const char *const mk[] = {"OBJS := $(patsubst %.c,%.o,$(SRCS))", "\t$(CC) $(CFLAGS) -c $< -o $@",
	                                 ".for f in ${SRCS:M*.c}", "all: ${PROG}"};
	static const char *const js[] = {"  \"name\": \"vxide\",", "  \"deps\": [1, 2, 3, true, null],",
	                                 "  \"nested\": {\"a\": \"b\\n\"},", "}"};
	static const char *const cm[] = {"target_link_libraries(app PUBLIC ${LIBS})", "if(WIN32)",
	                                 "  set(X \"${Y}/z\")", "endif()"};
	bench2(LANG_MAKE, "make", mk, 4);
	bench2(LANG_JSON, "json", js, 4);
	bench2(LANG_CMAKE, "cmake", cm, 4);
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

static void test_python(void)
{
	u32 s;
	lex(LANG_PYTHON, 0, "def area(self, w: int, h=2, *args, **kw) -> float:");
	T("def", TK_STORAGE);
	T("area", TK_FUNCTION);
	T("self", TK_VARIABLE);
	T("w", TK_VARIABLE);
	T("int", TK_TYPE);
	T("h", TK_VARIABLE);
	T("2", TK_NUMBER);
	T("args", TK_VARIABLE);
	T("kw", TK_VARIABLE);
	T("float", TK_TYPE);
	lex(LANG_PYTHON, 0, "    return self.w * len(xs) if x is not None else 0x1F");
	T("return", TK_CONTROL);
	T("self", TK_LANGCONST);
	T("len", TK_FUNCTION);
	T("if", TK_CONTROL);
	T("is", TK_STORAGE);
	T("not", TK_STORAGE);
	T("None", TK_LANGCONST);
	T("0x1F", TK_NUMBER);
	lex(LANG_PYTHON, 0, "class Shape(Base, metaclass=Meta):  # base");
	T("class", TK_STORAGE);
	T("Shape", TK_TYPE);
	T("Base", TK_TYPE);
	T("# base", TK_COMMENT);
	lex(LANG_PYTHON, 0, "@functools.lru_cache(maxsize=None)");
	T("@functools.lru_cache", TK_FUNCTION);
	T("maxsize", TK_VARIABLE);
	lex(LANG_PYTHON, 0, "from os import path as p");
	T("from", TK_CONTROL);
	T("import", TK_CONTROL);
	T("as", TK_CONTROL);
	lex(LANG_PYTHON, 0, "x = f\"v={x!r:>{w}} {{lit}}\" + 'a\\n' + rb'\\d+' + R'\\d' + 1_000.5e3j");
	T("f", TK_STORAGE);
	T("v=", TK_STRING);
	TN("{", 0, TK_STORAGE);
	TN("x", 1, TK_TEXT);
	T("!r:>", TK_STORAGE);
	T("{{", TK_ESCAPE);
	T("\\n", TK_ESCAPE);
	T("rb", TK_STORAGE);
	T("+'", TK_REGEX);
	T("R", TK_STORAGE);
	T("1_000.5e3", TK_NUMBER);
	T("j", TK_STORAGE);
	lex(LANG_PYTHON, 0, "print(\"%s: {0}\" % name, end='')");
	T("print", TK_FUNCTION);
	T("%s", TK_STORAGE);
	T("{0}", TK_STORAGE);
	T("end", TK_VARIABLE);
	/* triple-quoted docstring across lines */
	s = lex(LANG_PYTHON, 0, "    \"\"\"Docstring");
	T("Docstring", TK_STRING);
	CHECK(s != 0);
	s = lex(LANG_PYTHON, s, "still \\t inside");
	T("still", TK_STRING);
	T("\\t", TK_ESCAPE);
	s = lex(LANG_PYTHON, s, "end\"\"\" + raise_it");
	T("end\"\"\"", TK_STRING);
	T("raise_it", TK_TEXT);
	CHECK(s == 0);
	/* def parameters across lines */
	s = lex(LANG_PYTHON, 0, "def g(a,");
	s = lex(LANG_PYTHON, s, "      b=1):");
	T("b", TK_VARIABLE);
	s = lex(LANG_PYTHON, s, "    pass");
	T("pass", TK_CONTROL);
	CHECK(s == 0);
	lex(LANG_PYTHON, 0, "async def run(): await x; raise ValueError(__name__) from __init__");
	TN("async", 0, TK_STORAGE);
	T("await", TK_CONTROL);
	T("ValueError", TK_TYPE);
	T("__name__", TK_VARIABLE);
	T("__init__", TK_FUNCTION);
	lex(LANG_PYTHON, 0, "match cmd:");
	T("match", TK_CONTROL);
	lex(LANG_PYTHON, 0, "match = re.match(p, s)");
	TN("match", 0, TK_TEXT);
	lex(LANG_PYTHON, 0, "lambda: True and False or ...");
	T("lambda", TK_STORAGE);
	T("True", TK_LANGCONST);
	T("and", TK_STORAGE);
}

static void test_yaml(void)
{
	u32 s;
	lex(LANG_YAML, 0, "name: build # ci");
	T("name", TK_TAG);
	T("build", TK_UNQUOTED);
	T("# ci", TK_COMMENT);
	lex(LANG_YAML, 0, "  - key: \"a\\tb\"");
	T("key", TK_TAG);
	T("\\t", TK_ESCAPE);
	T("\"a", TK_STRING);
	lex(LANG_YAML, 0, "on: [push, 'pull request']");
	T("on", TK_TAG);
	T("push", TK_UNQUOTED);
	T("'pull request'", TK_UNQUOTED);
	lex(LANG_YAML, 0, "base: &defaults {retries: 3, ok: true, none: ~, v: 1.5e3}");
	T("&", TK_CONTROL);
	T("defaults", TK_VARIABLE);
	T("retries", TK_TAG);
	T("3", TK_NUMBER);
	T("true", TK_LANGCONST);
	T("~", TK_LANGCONST);
	T("1.5e3", TK_NUMBER);
	lex(LANG_YAML, 0, "job: *defaults");
	T("*", TK_CONTROL);
	T("defaults", TK_VARIABLE);
	lex(LANG_YAML, 0, "when: !!str 2024-01-01");
	T("!!str", TK_STORAGE);
	T("2024-01-01", TK_UNQUOTED);
	lex(LANG_YAML, 0, "\"quoted key\": x");
	T("\"quoted key\"", TK_TAG);
	lex(LANG_YAML, 0, "%YAML 1.2");
	T("%YAML", TK_STORAGE);
	lex(LANG_YAML, 0, "--- # doc");
	T("# doc", TK_COMMENT);
	T("---", TK_TEXT);
	/* block scalar continues while indented, ends on dedent */
	s = lex(LANG_YAML, 0, "  run: |-");
	T("|", TK_CONTROL);
	T("-", TK_STORAGE);
	CHECK((s & 3) == 1);
	s = lex(LANG_YAML, s, "    make: all # not a key");
	T("make: all # not a key", TK_UNQUOTED);
	s = lex(LANG_YAML, s, "");
	s = lex(LANG_YAML, s, "      deeper");
	T("deeper", TK_UNQUOTED);
	s = lex(LANG_YAML, s, "  next: 1");
	T("next", TK_TAG);
	CHECK((s & 3) == 0);
	/* multi-line quoted string */
	s = lex(LANG_YAML, 0, "msg: \"two");
	s = lex(LANG_YAML, s, "  lines\" # c");
	T("lines\"", TK_STRING);
	T("# c", TK_COMMENT);
	CHECK(s == 0);
}

static void test_codeowners(void)
{
	lex(LANG_CODEOWNERS, 0, "# owners");
	T("# owners", TK_COMMENT);
	lex(LANG_CODEOWNERS, 0, "/src/**/*.c  @alice @org/core-team dev@example.com # c");
	TN("/", 0, TK_OPERATOR);
	TN("*", 0, TK_REGEX);
	T("@alice", TK_VARIABLE);
	T("@org/core-team", TK_VARIABLE);
	T("dev@example.com", TK_STRING);
	T("# c", TK_COMMENT);
	lex(LANG_CODEOWNERS, 0, "^[Docs][2] @writers");
	T("^", TK_OPERATOR);
	T("[Docs]", TK_HEADING);
	T("[2]", TK_NUMBER);
	T("@writers", TK_VARIABLE);
	lex(LANG_CODEOWNERS, 0, "docs/ @a");
	TN("/", 0, TK_OPERATOR);
}

static void test_detect4(void)
{
	CHECK(syn_lang_for_path("a/b.py", NULL, 0) == LANG_PYTHON);
	CHECK(syn_lang_for_path("stubs.pyi", NULL, 0) == LANG_PYTHON);
	CHECK(syn_lang_for_path("/x/SConstruct", NULL, 0) == LANG_PYTHON);
	const char *py = "#!/usr/bin/env python3\nprint(1)\n";
	CHECK(syn_lang_for_path("tool", py, strlen(py)) == LANG_PYTHON);
	const char *py2 = "#!/usr/local/bin/python3.12 -u\n";
	CHECK(syn_lang_for_path("bin/run", py2, strlen(py2)) == LANG_PYTHON);
	CHECK(syn_lang_for_path("ci.yml", NULL, 0) == LANG_YAML);
	CHECK(syn_lang_for_path("/r/.github/workflows/build.yaml", NULL, 0) == LANG_YAML);
	CHECK(syn_lang_for_path("/r/.clang-format", NULL, 0) == LANG_YAML);
	CHECK(syn_lang_for_path("/r/.clangd", NULL, 0) == LANG_YAML);
	CHECK(syn_lang_for_path("/r/.github/CODEOWNERS", NULL, 0) == LANG_CODEOWNERS);
	CHECK(syn_lang_for_path("CODEOWNERS", NULL, 0) == LANG_CODEOWNERS);
	CHECK(!strcmp(syn_lang_name(LANG_PYTHON), "Python"));
	CHECK(!strcmp(syn_line_comment(LANG_YAML), "#"));
	CHECK(!strcmp(syn_lang_lsp_id(LANG_PYTHON), "python"));
	/* fenced code in Markdown */
	u32 s = lex(LANG_MARKDOWN, 0, "```python");
	s = lex(LANG_MARKDOWN, s, "def f(): pass");
	T("def", TK_STORAGE);
	TN("f", 1, TK_FUNCTION);
	s = lex(LANG_MARKDOWN, s, "```");
	s = lex(LANG_MARKDOWN, s, "```yaml");
	s = lex(LANG_MARKDOWN, s, "k: v");
	T("k", TK_TAG);
	(void)s;
	CHECK(LANG_GLSL < 32);
}

static void test_java(void)
{
	u32 s;
	lex(LANG_JAVA, 0, "import java.util.List;");
	T("import", TK_STORAGE);
	T("java.util.List", TK_TEXT);
	lex(LANG_JAVA, 0, "@Override public sealed class Foo<T> extends Bar implements Baz permits Q {");
	T("@Override", TK_TYPE);
	T("public", TK_STORAGE);
	T("sealed", TK_STORAGE);
	T("class", TK_STORAGE);
	T("Foo", TK_TYPE);
	T("T", TK_TYPE);
	T("extends", TK_STORAGE);
	T("Bar", TK_TYPE);
	T("permits", TK_STORAGE);
	lex(LANG_JAVA, 0, "non-sealed interface I {}");
	T("non-sealed", TK_STORAGE);
	lex(LANG_JAVA, 0, "    private static final Map<String, List<Integer>> m = new HashMap<>();");
	T("private", TK_STORAGE);
	T("Map", TK_TYPE);
	T("String", TK_TYPE);
	T("List", TK_TYPE);
	T("new", TK_CONTROL);
	T("HashMap", TK_TYPE);
	lex(LANG_JAVA, 0, "int x = 0x1F_FFL + 1_000 + 3.5e-2f + 0b1010; char c = '\\n';");
	T("int", TK_TYPE);
	T("0x1F_FFL", TK_NUMBER);
	T("1_000", TK_NUMBER);
	T("3.5e-2f", TK_NUMBER);
	T("0b1010", TK_NUMBER);
	T("\\n", TK_ESCAPE);
	lex(LANG_JAVA, 0, "if (o instanceof String s && s.length() > 0) return this.name; else yield null;");
	T("if", TK_CONTROL);
	T("instanceof", TK_STORAGE);
	T("length", TK_FUNCTION);
	T("return", TK_CONTROL);
	T("this", TK_LANGCONST);
	T("name", TK_VARIABLE);
	T("yield", TK_CONTROL);
	T("null", TK_LANGCONST);
	lex(LANG_JAVA, 0, "var list = items.stream().map(x -> x * 2).toList(); // done");
	T("var", TK_STORAGE);
	T("stream", TK_FUNCTION);
	T("->", TK_STORAGE);
	T("// done", TK_COMMENT);
	lex(LANG_JAVA, 0, "String var = \"a\\tb\";");
	T("var", TK_TEXT); /* not `var name` */
	T("\\t", TK_ESCAPE);
	/* text block across lines */
	s = lex(LANG_JAVA, 0, "String q = \"\"\"");
	T("\"\"\"", TK_STRING);
	CHECK(s == 3);
	s = lex(LANG_JAVA, s, "    SELECT * FROM t \\n");
	T("SELECT * FROM t ", TK_STRING);
	T("\\n", TK_ESCAPE);
	CHECK(s == 3);
	s = lex(LANG_JAVA, s, "    \"\"\"; int y;");
	T("\"\"\"", TK_STRING);
	T("int", TK_TYPE);
	CHECK(s == 0);
	/* javadoc */
	s = lex(LANG_JAVA, 0, "/** Adds two.");
	T("Adds two.", TK_COMMENT);
	CHECK(s == 2);
	s = lex(LANG_JAVA, s, " * @param num the first {@link Foo}");
	T("@param", TK_STORAGE);
	T("num", TK_VARIABLE);
	T("the first", TK_COMMENT);
	T("@link", TK_STORAGE);
	s = lex(LANG_JAVA, s, " */ record Point(int x, int y) {}");
	T("*/", TK_COMMENT);
	T("record", TK_STORAGE);
	T("Point", TK_TYPE);
	CHECK(s == 0);
	s = lex(LANG_JAVA, 0, "/* plain");
	CHECK(s == 1);
	lex(LANG_JAVA, 0, "@interface Marker {}");
	T("@interface", TK_STORAGE);
	T("Marker", TK_TYPE);
	lex(LANG_JAVA, 0, "try { throw new IllegalStateException(\"x\"); } catch (Exception e) {}");
	T("try", TK_CONTROL);
	T("throw", TK_CONTROL);
	T("IllegalStateException", TK_TYPE);
	T("catch", TK_CONTROL);
	T("Exception", TK_TYPE);
	lex(LANG_JAVA, 0, "a < b && c > d");
	T("<", TK_OPERATOR);
	T(">", TK_OPERATOR);
}

static void test_kotlin(void)
{
	u32 s;
	lex(LANG_KOTLIN, 0, "package com.example.app");
	T("package", TK_STORAGE);
	lex(LANG_KOTLIN, 0, "@JvmStatic suspend fun <T> List<T>.firstOr(x: T): T? = if (isEmpty()) x else this[0]");
	T("@JvmStatic", TK_TYPE);
	T("suspend", TK_STORAGE);
	T("fun", TK_STORAGE);
	T("firstOr", TK_FUNCTION);
	T("List", TK_TYPE);
	TN("T", 1, TK_TYPE); /* List<T> receiver */
	T("isEmpty", TK_FUNCTION);
	T("if", TK_CONTROL);
	T("else", TK_CONTROL);
	T("this", TK_LANGCONST);
	lex(LANG_KOTLIN, 0, "data class User(val name: String, var age: Int = 0x1F_00) : Base()");
	T("data", TK_STORAGE);
	T("class", TK_STORAGE);
	T("User", TK_TYPE);
	T("val", TK_STORAGE);
	T("name", TK_VARIABLE);
	T("String", TK_TYPE);
	T("0x1F_00", TK_NUMBER);
	T("Base", TK_TYPE);
	lex(LANG_KOTLIN, 0, "val msg = \"hi $name, ${user.age + 1} \\t done\"");
	T("msg", TK_VARIABLE);
	T("hi ", TK_STRING);
	T("$", TK_STORAGE);
	T("name", TK_VARIABLE);
	T("${", TK_STORAGE);
	T("age", TK_VARIABLE);
	T("1", TK_NUMBER);
	T("\\t", TK_ESCAPE);
	lex(LANG_KOTLIN, 0, "when (x) { is Int -> 1L; !in 0..9 -> 2u; else -> null }");
	T("when", TK_CONTROL);
	T("is", TK_STORAGE);
	T("1L", TK_NUMBER);
	T("!in", TK_STORAGE);
	T("2u", TK_NUMBER);
	T("null", TK_LANGCONST);
	/* nested block comments */
	s = lex(LANG_KOTLIN, 0, "/* outer /* inner */ still");
	T("still", TK_COMMENT);
	CHECK((s & 3u) == 1 && (s >> 2) == 1);
	s = lex(LANG_KOTLIN, s, "comment */ val y = 2");
	T("comment */", TK_COMMENT);
	T("val", TK_STORAGE);
	CHECK(s == 0);
	/* raw string across lines, with a template */
	s = lex(LANG_KOTLIN, 0, "val sql = \"\"\"");
	CHECK((s & 3u) == 2);
	s = lex(LANG_KOTLIN, s, "  select $col from t");
	T("select ", TK_STRING);
	T("col", TK_VARIABLE);
	s = lex(LANG_KOTLIN, s, "\"\"\".trimIndent()");
	T("\"\"\"", TK_STRING);
	T("trimIndent", TK_FUNCTION);
	CHECK(s == 0);
	lex(LANG_KOTLIN, 0, "object Registry : Map<String, Int> by HashMap()");
	T("object", TK_STORAGE);
	T("Registry", TK_TYPE);
	T("by", TK_STORAGE);
	lex(LANG_KOTLIN, 0, "x.by(1) // c");
	T("by", TK_FUNCTION); /* a method call, not the keyword */
	T("// c", TK_COMMENT);
}

static void test_toml(void)
{
	u32 s;
	lex(LANG_TOML, 0, "[package] # main");
	T("[package]", TK_TAG);
	T("# main", TK_COMMENT);
	lex(LANG_TOML, 0, "[[bin]]");
	T("[[bin]]", TK_TAG);
	lex(LANG_TOML, 0, "[dependencies.serde]");
	T("[dependencies.serde]", TK_TAG);
	lex(LANG_TOML, 0, "name = \"vx\\tide\"");
	T("name", TK_PROPERTY);
	T("\\t", TK_ESCAPE);
	lex(LANG_TOML, 0, "site.\"google.com\" = true");
	T("site", TK_PROPERTY);
	T("\"google.com\"", TK_PROPERTY);
	T("true", TK_LANGCONST);
	lex(LANG_TOML, 0, "path = 'C:\\Users\\x'");
	T("'C:\\Users\\x'", TK_STRING);
	lex(LANG_TOML, 0, "n = +1_000  f = 6.02e+23  h = 0xDEAD_beef  x = -inf");
	T("+1_000", TK_NUMBER);
	T("6.02e+23", TK_NUMBER);
	T("0xDEAD_beef", TK_NUMBER);
	T("-inf", TK_NUMBER);
	lex(LANG_TOML, 0, "dob = 1979-05-27T07:32:00-08:00");
	T("dob", TK_PROPERTY);
	T("1979-05-27T07:32:00-08:00", TK_NUMBER);
	lex(LANG_TOML, 0, "point = { x = 1, y = \"b\" }");
	T("point", TK_PROPERTY);
	T("x", TK_PROPERTY);
	T("y", TK_PROPERTY);
	T("\"b\"", TK_STRING);
	/* multi-line array: values are not keys */
	s = lex(LANG_TOML, 0, "deps = [");
	CHECK(s == 0);
	lex(LANG_TOML, 0, "  \"serde\", # json");
	T("\"serde\"", TK_STRING);
	T("# json", TK_COMMENT);
	/* multi-line strings */
	s = lex(LANG_TOML, 0, "desc = \"\"\"first \\");
	T("desc", TK_PROPERTY);
	T("\"\"\"first ", TK_STRING);
	CHECK(s == 1);
	s = lex(LANG_TOML, s, "second line");
	T("second line", TK_STRING);
	CHECK(s == 1);
	s = lex(LANG_TOML, s, "end\"\"\" # ok");
	T("end\"\"\"", TK_STRING);
	T("# ok", TK_COMMENT);
	CHECK(s == 0);
	s = lex(LANG_TOML, 0, "re = '''");
	CHECK(s == 2);
	s = lex(LANG_TOML, s, "\\d+ ''' ");
	T("\\d+ '''", TK_STRING);
	CHECK(s == 0);
}

static void test_detect5(void)
{
	CHECK(syn_lang_for_path("src/Main.java", NULL, 0) == LANG_JAVA);
	CHECK(syn_lang_for_path("A.jav", NULL, 0) == LANG_JAVA);
	CHECK(syn_lang_for_path("app/Main.kt", NULL, 0) == LANG_KOTLIN);
	CHECK(syn_lang_for_path("build.gradle.kts", NULL, 0) == LANG_KOTLIN);
	CHECK(syn_lang_for_path("/r/Cargo.toml", NULL, 0) == LANG_TOML);
	CHECK(syn_lang_for_path("Cargo.lock", NULL, 0) == LANG_TOML);
	CHECK(syn_lang_for_path("Pipfile", NULL, 0) == LANG_TOML);
	CHECK(syn_lang_for_path("poetry.lock", NULL, 0) == LANG_TOML);
	CHECK(syn_lang_for_path("uv.lock", NULL, 0) == LANG_TOML);
	CHECK(!strcmp(syn_lang_name(LANG_JAVA), "Java"));
	CHECK(!strcmp(syn_lang_name(LANG_KOTLIN), "Kotlin"));
	CHECK(!strcmp(syn_lang_name(LANG_TOML), "TOML"));
	CHECK(!strcmp(syn_line_comment(LANG_JAVA), "//") && !strcmp(syn_line_comment(LANG_KOTLIN), "//"));
	CHECK(!strcmp(syn_line_comment(LANG_TOML), "#"));
	CHECK(!strcmp(syn_lang_lsp_id(LANG_KOTLIN), "kotlin"));
	CHECK(LANG_GLSL < 32);
	/* fenced code in Markdown */
	u32 s = lex(LANG_MARKDOWN, 0, "```java");
	s = lex(LANG_MARKDOWN, s, "public class A {}");
	T("public", TK_STORAGE);
	T("A", TK_TYPE);
	s = lex(LANG_MARKDOWN, s, "```");
	s = lex(LANG_MARKDOWN, s, "```kotlin");
	s = lex(LANG_MARKDOWN, s, "fun f() = 1");
	T("fun", TK_STORAGE);
	TN("f", 1, TK_FUNCTION);
	s = lex(LANG_MARKDOWN, s, "```");
	s = lex(LANG_MARKDOWN, s, "```toml");
	s = lex(LANG_MARKDOWN, s, "[a]");
	T("[a]", TK_TAG);
	s = lex(LANG_MARKDOWN, s, "```");
	(void)s;
	/* states stay within the embeddable width */
	CHECK(lex(LANG_KOTLIN, 1u | (250u << 2), "/* /* x") < (1u << SYN_STATE_BITS));
}

static void test_cpp(void)
{
	u32 s;
	CHECK(LANG_CPP == 23 && LANG_RUST == 24);
	s = lex(LANG_CPP, 0, "#include <vector> // std");
	T("#include", TK_CONTROL);
	T("<vector>", TK_STRING);
	T("// std", TK_COMMENT);
	CHECK(s == 0);
	lex(LANG_CPP, 0, "#embed \"blob.bin\" limit(4)");
	T("#embed", TK_CONTROL);
	T("\"blob.bin\"", TK_STRING);
	lex(LANG_CPP, 0, "namespace vx::detail {");
	T("namespace", TK_STORAGE);
	T("vx", TK_TYPE);
	T("detail", TK_TYPE);
	lex(LANG_CPP, 0, "template<typename T, class... Us> requires std::integral<T>");
	T("template", TK_STORAGE);
	T("typename", TK_STORAGE);
	TN("T", 0, TK_TYPE);
	T("class", TK_STORAGE);
	T("Us", TK_TYPE);
	T("requires", TK_STORAGE);
	T("std", TK_TYPE);
	T("integral", TK_TYPE);
	T("<", TK_TEXT);
	lex(LANG_CPP, 0, "class Widget final : public Base, private virtual Mixin {");
	T("class", TK_STORAGE);
	T("Widget", TK_TYPE);
	T("final", TK_STORAGE);
	T("public", TK_STORAGE);
	T("Base", TK_TYPE);
	T("virtual", TK_STORAGE);
	T("Mixin", TK_TYPE);
	lex(LANG_CPP, 0, "public:");
	T("public", TK_STORAGE);
	lex(LANG_CPP, 0, "  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return n_; }");
	T("nodiscard", TK_TYPE);
	T("constexpr", TK_STORAGE);
	T("auto", TK_STORAGE);
	T("size", TK_FUNCTION);
	T("const", TK_STORAGE);
	T("noexcept", TK_STORAGE);
	T("size_t", TK_TYPE);
	T("return", TK_CONTROL);
	lex(LANG_CPP, 0, "std::vector<int> v = std::make_unique<Foo>(1'000'000, 0x1p-3, 42_km);");
	T("vector", TK_TYPE);
	T("int", TK_STORAGE);
	T("make_unique", TK_FUNCTION);
	T("Foo", TK_TYPE);
	T("1'000'000", TK_NUMBER);
	T("0x1p-3", TK_NUMBER);
	T("42_km", TK_NUMBER);
	lex(LANG_CPP, 0, "const std::string& name = u8\"h\\u00e9\" \"x\"sv;");
	T("const", TK_STORAGE);
	T("string", TK_TYPE);
	T("u8\"h", TK_STRING);
	T("\\u00e9", TK_ESCAPE);
	T("sv", TK_STORAGE);
	lex(LANG_CPP, 0, "auto c = U'\\x{1F600}' + L'a' + u'b';");
	T("U'", TK_STRING);
	T("\\x{1F600}", TK_ESCAPE);
	T("L'a'", TK_STRING);
	T("u'b'", TK_STRING);
	lex(LANG_CPP, 0, "Widget* w = new Widget(); delete w; this->x = nullptr;");
	TN("Widget", 0, TK_TYPE);
	T("new", TK_CONTROL);
	TN("Widget", 1, TK_FUNCTION);
	T("delete", TK_CONTROL);
	T("this", TK_LANGCONST);
	T("x", TK_VARIABLE);
	T("nullptr", TK_LANGCONST);
	lex(LANG_CPP, 0, "using namespace std; using Vec = std::vector<float>;");
	TN("using", 0, TK_CONTROL);
	T("namespace", TK_STORAGE);
	TN("std", 0, TK_TYPE);
	T("Vec", TK_TYPE);
	lex(LANG_CPP, 0, "bool operator==(const A&) const = default; auto operator<=>(B) const;");
	T("operator", TK_CONTROL);
	T("==", TK_CONTROL);
	T("<=>", TK_CONTROL);
	T("A", TK_TYPE);
	T("default", TK_CONTROL);
	lex(LANG_CPP, 0, "long double operator\"\"_km(long double x);");
	T("\"\"_km", TK_FUNCTION);
	lex(LANG_CPP, 0, "co_await sched(); co_yield v; co_return x and y;");
	T("co_await", TK_CONTROL);
	T("co_yield", TK_CONTROL);
	T("co_return", TK_CONTROL);
	T("and", TK_STORAGE);
	lex(LANG_CPP, 0, "consteval int sq(int x) { return x * x; } constinit int g = sq(3);");
	T("consteval", TK_STORAGE);
	T("constinit", TK_STORAGE);
	lex(LANG_CPP, 0, "template <class T> concept Hashable = requires(T a) { std::hash<T>{}(a); };");
	T("concept", TK_STORAGE);
	T("Hashable", TK_TYPE);
	T("hash", TK_TYPE);
	lex(LANG_CPP, 0, "int f(int x) pre(x > 0) post(r: r >= 0) { contract_assert(x != 0); }");
	T("pre", TK_STORAGE);
	T("post", TK_STORAGE);
	T("contract_assert", TK_STORAGE);
	lex(LANG_CPP, 0, "if (ok) pre(x); int pre = 0; obj.post(1);");
	T("pre", TK_FUNCTION);
	TN("pre", 1, TK_TEXT);
	T("post", TK_FUNCTION);
	lex(LANG_CPP, 0, "    pre(n > 0)");
	T("pre", TK_STORAGE);
	lex(LANG_CPP, 0, "auto first = Ts...[0]; auto [a, b] = pair;");
	T("Ts", TK_TEXT);
	T("0", TK_NUMBER);
	lex(LANG_CPP, 0, "export module vx.core; import <iostream>; import std;");
	T("export", TK_STORAGE);
	T("module", TK_STORAGE);
	T("import", TK_TEXT); /* not at the statement start */
	lex(LANG_CPP, 0, "import <iostream>;");
	T("import", TK_CONTROL);
	T("<iostream>", TK_STRING);
	lex(LANG_CPP, 0, "auto p = static_cast<int>(x) + sizeof...(Ts) + alignof(T);");
	T("static_cast", TK_STORAGE);
	T("sizeof", TK_STORAGE);
	T("alignof", TK_STORAGE);
	lex(LANG_CPP, 0, "for (auto& [k, v] : map) if (a < b && c > d) break;");
	T("for", TK_CONTROL);
	T("<", TK_OPERATOR);
	T(">", TK_OPERATOR);
	T("break", TK_CONTROL);
	/* raw strings, single line, with delimiter and suffix */
	lex(LANG_CPP, 0, "auto r = R\"(a\\n\"b)\" + LR\"xy(q)\")xy\"_s;");
	T("R\"(a\\n\"b)\"", TK_STRING);
	T("LR\"xy(q)\")xy\"", TK_STRING);
	T("_s", TK_STORAGE);
	/* raw string spanning lines; only )sql" closes it */
	s = lex(LANG_CPP, 0, "const char *q = R\"sql(SELECT \"x\"");
	T("R\"sql(SELECT \"x\"", TK_STRING);
	CHECK((s & 7u) == 7u);
	s = lex(LANG_CPP, s, "  FROM t)\" WHERE a = 1)x\" // no");
	T("FROM t)\" WHERE a = 1)x\" // no", TK_STRING);
	CHECK((s & 7u) == 7u);
	s = lex(LANG_CPP, s, ")sql\"; int after = 2;");
	T(")sql\"", TK_STRING);
	T("int", TK_STORAGE);
	T("2", TK_NUMBER);
	CHECK(s == 0);
	s = lex(LANG_CPP, 0, "auto e = u8R\"(");
	CHECK(s != 0 && s < (1u << SYN_STATE_BITS));
	s = lex(LANG_CPP, s, ")\"; // done");
	T("// done", TK_COMMENT);
	CHECK(s == 0);
	/* block comments, continued strings and #if 0 as in C */
	s = lex(LANG_CPP, 0, "int a; /* open");
	s = lex(LANG_CPP, s, "  still */ class B;");
	T("still */", TK_COMMENT);
	T("B", TK_TYPE);
	CHECK(s == 0);
	s = lex(LANG_CPP, 0, "#if 0");
	s = lex(LANG_CPP, s, "class Dead {};");
	T("class Dead {};", TK_COMMENT);
	s = lex(LANG_CPP, s, "#endif");
	CHECK(s == 0);
	s = lex(LANG_CPP, 0, "#define MAX(a, b) \\");
	s = lex(LANG_CPP, s, "  ((a) > (b) ? (a) : (b))");
	T("a", TK_PREPROC);
	CHECK(s == 0);
}

static void test_rust(void)
{
	u32 s;
	lex(LANG_RUST, 0, "use std::collections::{HashMap, self};");
	T("use", TK_STORAGE);
	T("std", TK_TYPE);
	T("collections", TK_TYPE);
	T("HashMap", TK_TYPE);
	T("self", TK_LANGCONST);
	lex(LANG_RUST, 0, "pub fn parse<'a>(input: &'a str, n: u8) -> Result<Vec<u64>, Error> {");
	T("pub", TK_STORAGE);
	T("fn", TK_STORAGE);
	T("parse", TK_FUNCTION);
	TN("a", 1, TK_TYPE); /* <'a> */
	T("input", TK_VARIABLE);
	T("str", TK_TYPE);
	T("u8", TK_TYPE);
	T("Result", TK_TYPE);
	T("u64", TK_TYPE);
	lex(LANG_RUST, 0, "    let mut x = 1_000u32 + 0xffi64 as u32 + 2.5e3f64;");
	T("let", TK_STORAGE);
	T("mut", TK_STORAGE);
	T("x", TK_VARIABLE);
	T("1_000", TK_NUMBER);
	T("u32", TK_TYPE);
	T("0xff", TK_NUMBER);
	T("i64", TK_TYPE);
	T("as", TK_STORAGE);
	T("2.5e3", TK_NUMBER);
	T("f64", TK_TYPE);
	lex(LANG_RUST, 0, "    let c = 'x'; let e = '\\n'; let u = '\\u{1F600}'; let b = b'a'; let z = 'é';");
	T("'x'", TK_STRING);
	T("\\n", TK_ESCAPE);
	T("\\u{1F600}", TK_ESCAPE);
	T("b'a'", TK_STRING);
	T("'é'", TK_STRING);
	lex(LANG_RUST, 0, "impl<'de> Visitor<'de> for V { fn f(&'static self) {} }");
	T("impl", TK_STORAGE);
	T("de", TK_TYPE);
	T("static", TK_TYPE); /* lifetime name */
	T("self", TK_LANGCONST);
	T("Visitor", TK_TYPE);
	lex(LANG_RUST, 0, "    println!(\"{} {x:?}\\t\", v.len()); MyMacro!(); if lhs != rhs { return; }");
	T("println!", TK_FUNCTION);
	T("\"{} {x:?}", TK_STRING);
	T("\\t", TK_ESCAPE);
	T("len", TK_FUNCTION);
	T("MyMacro!", TK_TYPE);
	T("if", TK_CONTROL);
	T("lhs", TK_VARIABLE);
	T("rhs", TK_VARIABLE);
	T("return", TK_CONTROL);
	lex(LANG_RUST, 0, "let p = point.x; let s = Some(3).unwrap_or(None); const MAX_N: usize = 4;");
	T("point", TK_VARIABLE);
	T("x;", TK_TEXT);
	T("Some", TK_TYPE);
	T("unwrap_or", TK_FUNCTION);
	T("None", TK_TYPE);
	T("const", TK_STORAGE);
	T("MAX_N", TK_TEXT);
	T("usize", TK_TYPE);
	lex(LANG_RUST, 0, "#[derive(Debug, Clone)] #![allow(dead_code)] struct S { a: bool }");
	T("derive", TK_TEXT);
	T("Debug", TK_TYPE);
	T("allow", TK_TEXT);
	T("dead_code", TK_TEXT);
	T("struct", TK_STORAGE);
	T("bool", TK_TYPE);
	lex(LANG_RUST, 0, "mod net; match v { Ok(x) => x, Err(_) => loop { break } }");
	T("mod", TK_STORAGE);
	T("net", TK_TEXT);
	T("match", TK_CONTROL);
	T("Ok", TK_TYPE);
	T("loop", TK_CONTROL);
	lex(LANG_RUST, 0, "let v = it.collect::<Vec<_>>().await; let t = true; let r#type = 1;");
	T("collect", TK_FUNCTION);
	T("await", TK_CONTROL);
	T("true", TK_LANGCONST);
	T("r#type", TK_VARIABLE);
	lex(LANG_RUST, 0, "macro_rules! square { ($x:expr) => { $x * $x }; }");
	T("macro_rules!", TK_FUNCTION);
	T("square", TK_FUNCTION);
	lex(LANG_RUST, 0, "let b = b\"by\\x7f\"; let c = c\"hi\"; let br = br\"\\d\"; /// doc");
	T("b\"by", TK_STRING);
	T("\\x7f", TK_ESCAPE);
	T("c\"hi\"", TK_STRING);
	T("br\"\\d\"", TK_STRING);
	T("/// doc", TK_COMMENT);
	/* raw strings with hashes, one line and spanning lines */
	lex(LANG_RUST, 0, "let j = r#\"{\"a\": \"b\"}\"#; let k = r\"x\";");
	T("r#\"{\"a\": \"b\"}\"#", TK_STRING);
	T("r\"x\"", TK_STRING);
	s = lex(LANG_RUST, 0, "let q = r##\"line one \"# still");
	T("r##\"line one \"# still", TK_STRING);
	CHECK((s & 3u) == 3u && ((s >> 2) & 0xFFu) == 2u);
	s = lex(LANG_RUST, s, "two \"# \"## + 1;");
	T("two \"# \"##", TK_STRING);
	T("1", TK_NUMBER);
	CHECK(s == 0);
	/* plain strings span lines too */
	s = lex(LANG_RUST, 0, "let m = \"multi");
	CHECK((s & 3u) == 2u);
	s = lex(LANG_RUST, s, "line\\n\"; let after = 1;");
	T("line", TK_STRING);
	T("\\n", TK_ESCAPE);
	T("after", TK_VARIABLE);
	CHECK(s == 0);
	/* nested block comments keep their depth */
	s = lex(LANG_RUST, 0, "x /* outer /* inner */ still");
	T("/* outer /* inner */ still", TK_COMMENT);
	CHECK((s & 3u) == 1u && ((s >> 2) & 0xFFu) == 1u);
	s = lex(LANG_RUST, 0, "/* a /* b");
	CHECK((s & 3u) == 1u && ((s >> 2) & 0xFFu) == 2u);
	s = lex(LANG_RUST, s, "*/ c /* d */ e");
	T("*/ c /* d */ e", TK_COMMENT);
	CHECK(((s >> 2) & 0xFFu) == 1u);
	s = lex(LANG_RUST, s, "*/ fn g() {}");
	T("*/", TK_COMMENT);
	T("fn", TK_STORAGE);
	T("g", TK_FUNCTION);
	CHECK(s == 0);
	/* attributes and use declarations across lines */
	s = lex(LANG_RUST, 0, "#[cfg(all(");
	s = lex(LANG_RUST, s, "  unix, feature = \"x\"))]");
	T("unix", TK_TEXT);
	T("\"x\"", TK_STRING);
	s = lex(LANG_RUST, s, "fn h() {}");
	T("h", TK_FUNCTION);
	CHECK(s == 0);
	s = lex(LANG_RUST, 0, "use std::{");
	s = lex(LANG_RUST, s, "    io, fs::File,");
	T("io", TK_TEXT);
	T("fs", TK_TYPE);
	T("File", TK_TYPE);
	s = lex(LANG_RUST, s, "};");
	CHECK(s == 0);
	s = lex(LANG_RUST, s, "let io = 1;");
	T("io", TK_VARIABLE);
}

static void test_detect6(void)
{
	static const char *const cppx[] = {"a.cpp", "a.cc", "a.cxx", "a.c++", "a.hpp", "a.hh", "a.hxx",
	                                   "a.h++", "a.ipp", "a.tpp", "a.inl", "a.ixx", "a.cppm", "A.CPP"};
	for (size_t i = 0; i < VX_COUNT(cppx); i++)
		CHECK(syn_lang_for_path(cppx[i], NULL, 0) == LANG_CPP);
	CHECK(syn_lang_for_path("src/lib.rs", NULL, 0) == LANG_RUST);
	CHECK(syn_lang_for_path("x.c", NULL, 0) == LANG_C);
	CHECK(syn_lang_for_path("x.h", NULL, 0) == LANG_C);
	const char *ch = "#ifndef X_H\n#define X_H\n/* class Foo would be C++\n namespace x */\nstruct s { int class_; };\n"
	                 "#ifdef __cplusplus\nextern \"C\" {\n#endif\n#include <stdio.h>\n";
	CHECK(syn_lang_for_path("x.h", ch, strlen(ch)) == LANG_C);
	const char *h1 = "#pragma once\n#include <vector>\n";
	CHECK(syn_lang_for_path("x.h", h1, strlen(h1)) == LANG_CPP);
	const char *h2 = "// util\nnamespace vx {\nint f();\n}\n";
	CHECK(syn_lang_for_path("x.h", h2, strlen(h2)) == LANG_CPP);
	const char *h3 = "template <typename T>\nT max(T a, T b);\n";
	CHECK(syn_lang_for_path("x.h", h3, strlen(h3)) == LANG_CPP);
	const char *h4 = "class Foo {\npublic:\n  Foo();\n};\n";
	CHECK(syn_lang_for_path("x.h", h4, strlen(h4)) == LANG_CPP);
	CHECK_STR(syn_lang_name(LANG_CPP), "C++");
	CHECK_STR(syn_lang_name(LANG_RUST), "Rust");
	CHECK_STR(syn_lang_lsp_id(LANG_CPP), "cpp");
	CHECK_STR(syn_lang_lsp_id(LANG_RUST), "rust");
	CHECK_STR(syn_lang_lsp_id(LANG_C), "c");
	CHECK_STR(syn_line_comment(LANG_CPP), "//");
	CHECK_STR(syn_line_comment(LANG_RUST), "//");
	CHECK(LANG_GLSL < 32);
	/* fenced code in Markdown, raw string state carried in the fence */
	u32 s = lex(LANG_MARKDOWN, 0, "```cpp");
	s = lex(LANG_MARKDOWN, s, "auto s = R\"(");
	s = lex(LANG_MARKDOWN, s, "class X )\" ;");
	T("class X )\"", TK_STRING);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
	s = lex(LANG_MARKDOWN, 0, "```rust");
	s = lex(LANG_MARKDOWN, s, "/* /* x */");
	s = lex(LANG_MARKDOWN, s, "y */ fn main() {}");
	T("y */", TK_COMMENT);
	T("main", TK_FUNCTION);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
	/* states stay within the embeddable width */
	CHECK(lex(LANG_RUST, (1u << 10) | (31u << 11) | (1u << 16) | 3u | (255u << 2), "r########\"") < (1u << SYN_STATE_BITS));
	CHECK(lex(LANG_CPP, 0, "R\"0123456789abcdef(") < (1u << SYN_STATE_BITS));
}

static void test_csharp(void)
{
	u32 s;
	CHECK(LANG_CSHARP == 25 && LANG_QML == 26 && LANG_QMAKE == 27 && LANG_GDSCRIPT == 28 && LANG_GODOT_RES == 29 &&
	      LANG_XML == 30 && LANG_GLSL == 31 && LANG_SPIRV == 32);
	/* using directives, namespaces, aliases */
	lex(LANG_CSHARP, 0, "using System.Collections.Generic;");
	T("using", TK_CONTROL);
	T("System", TK_TYPE);
	T("Generic", TK_TYPE);
	lex(LANG_CSHARP, 0, "global using static System.Math; using Foo = Bar.Baz;");
	T("global", TK_STORAGE);
	T("static", TK_STORAGE);
	T("Math", TK_TYPE);
	T("Foo", TK_TYPE);
	T("Baz", TK_TYPE);
	lex(LANG_CSHARP, 0, "namespace App.Core;");
	T("namespace", TK_STORAGE);
	T("App", TK_TYPE);
	T("Core", TK_TYPE);
	lex(LANG_CSHARP, 0, "using (var f = File.Open(p)) { }");
	T("using", TK_CONTROL);
	T("var", TK_STORAGE);
	T("f", TK_VARIABLE);
	T("File", TK_VARIABLE);
	T("Open", TK_FUNCTION);
	/* declarations: modifiers, generics, base lists, constraints */
	lex(LANG_CSHARP, 0, "public sealed partial class Repo<T> : IRepo<T>, IDisposable where T : class, new()");
	T("public", TK_STORAGE);
	T("sealed", TK_STORAGE);
	T("partial", TK_STORAGE);
	TN("class", 0, TK_STORAGE);
	T("Repo", TK_TYPE);
	TN("T", 0, TK_TYPE);
	T("IRepo", TK_TYPE);
	T("IDisposable", TK_TYPE);
	T("where", TK_STORAGE);
	TN("T", 2, TK_TYPE);
	TN("class", 1, TK_STORAGE);
	T("new", TK_STORAGE);
	lex(LANG_CSHARP, 0, "file record struct Point(int X, int Y) : IShape;");
	T("file", TK_STORAGE);
	T("record", TK_STORAGE);
	T("struct", TK_STORAGE);
	T("Point", TK_TYPE);
	TN("int", 1, TK_STORAGE);
	T("X", TK_VARIABLE);
	T("IShape", TK_TYPE);
	lex(LANG_CSHARP, 0, "public required string Name { get; init; } = \"\";");
	T("required", TK_STORAGE);
	T("string", TK_STORAGE);
	T("Name", TK_VARIABLE);
	T("get", TK_STORAGE);
	T("init", TK_STORAGE);
	lex(LANG_CSHARP, 0, "private readonly Dictionary<string, List<int>> _map = new();");
	T("Dictionary", TK_TYPE);
	T("List", TK_TYPE);
	T("int", TK_STORAGE);
	T("_map", TK_VARIABLE);
	T("new", TK_STORAGE);
	lex(LANG_CSHARP, 0, "public async Task<int> GetAsync(int id, CancellationToken ct = default) => await Get<int>(id);");
	T("async", TK_STORAGE);
	T("Task", TK_TYPE);
	T("GetAsync", TK_FUNCTION);
	T("id", TK_VARIABLE);
	T("CancellationToken", TK_TYPE);
	T("ct", TK_VARIABLE);
	T("default", TK_STORAGE);
	T("await", TK_STORAGE);
	T("Get", TK_FUNCTION);
	T("=>", TK_OPERATOR);
	lex(LANG_CSHARP, 0, "delegate void Handler(object sender); scoped ref Span<byte> b = ref x;");
	T("delegate", TK_STORAGE);
	T("void", TK_STORAGE);
	T("Handler", TK_TYPE);
	T("object", TK_STORAGE);
	T("sender", TK_VARIABLE);
	T("scoped", TK_STORAGE);
	TN("ref", 0, TK_STORAGE);
	T("Span", TK_TYPE);
	T("byte", TK_STORAGE);
	/* contextual keywords stay names elsewhere */
	lex(LANG_CSHARP, 0, "var var = file + record; get = value; where.x = select;");
	TN("var", 0, TK_STORAGE);
	TN("var", 1, TK_VARIABLE);
	T("file", TK_VARIABLE);
	T("record", TK_VARIABLE);
	T("get", TK_VARIABLE);
	T("where", TK_VARIABLE);
	T("select", TK_VARIABLE);
	/* statements, patterns, casts */
	lex(LANG_CSHARP, 0, "if (obj is not null and Foo fx) return (int)fx.Count;");
	T("if", TK_CONTROL);
	T("is", TK_STORAGE);
	T("not", TK_STORAGE);
	T("null", TK_LANGCONST);
	T("and", TK_STORAGE);
	T("Foo", TK_TYPE);
	T("fx", TK_VARIABLE);
	T("return", TK_CONTROL);
	T("int", TK_STORAGE);
	T("Count", TK_VARIABLE);
	lex(LANG_CSHARP, 0, "var n = (List<Item>)obj; if (ok) Run(); var s = x as string;");
	T("List", TK_TYPE);
	T("Item", TK_TYPE);
	T("ok", TK_VARIABLE);
	T("Run", TK_FUNCTION);
	T("as", TK_STORAGE);
	lex(LANG_CSHARP, 0, "foreach (var item in items) { yield return item; } x = y switch { > 0 or < -1 => 1, _ => 0 };");
	T("foreach", TK_CONTROL);
	T("in", TK_CONTROL);
	T("yield", TK_CONTROL);
	T("switch", TK_CONTROL);
	TN("or", 1, TK_STORAGE);
	T("_", TK_LANGCONST);
	lex(LANG_CSHARP, 0, "switch (k) { case Foo x when x.Ok: break; default: goto case 1; }");
	T("case", TK_CONTROL);
	T("Foo", TK_TYPE);
	T("when", TK_CONTROL);
	T("default", TK_CONTROL);
	T("goto", TK_CONTROL);
	lex(LANG_CSHARP, 0, "var t = typeof(Dictionary<,>); var z = default(T); var nm = nameof(x); checked { }");
	T("typeof", TK_STORAGE);
	T("Dictionary", TK_TYPE);
	T("default", TK_STORAGE);
	T("T", TK_TYPE);
	T("nameof", TK_STORAGE);
	T("checked", TK_CONTROL);
	lex(LANG_CSHARP, 0, "var p = point with { X = 1 }; Foo? maybe = null; Foo[] arr = [a, b]; this.x = base.y;");
	T("with", TK_STORAGE);
	TN("Foo", 0, TK_TYPE);
	TN("Foo", 1, TK_TYPE);
	T("this", TK_LANGCONST);
	T("base", TK_LANGCONST);
	lex(LANG_CSHARP, 0, "var q = from c in list where c.Age > 1 orderby c.Name descending select c;");
	T("from", TK_STORAGE);
	T("in", TK_STORAGE);
	T("where", TK_STORAGE);
	T("orderby", TK_STORAGE);
	T("descending", TK_STORAGE);
	T("select", TK_STORAGE);
	lex(LANG_CSHARP, 0, "var x = new Foo.Bar<int>(1); throw new InvalidOperationException();");
	T("Foo", TK_TYPE);
	T("Bar", TK_TYPE);
	T("throw", TK_CONTROL);
	T("InvalidOperationException", TK_TYPE);
	lex(LANG_CSHARP, 0, "lock (o) { } fixed (byte* p = buf) { } unsafe { } int @class = 1;");
	T("lock", TK_CONTROL);
	T("fixed", TK_CONTROL);
	T("unsafe", TK_CONTROL);
	T("@class", TK_VARIABLE);
	/* numbers */
	lex(LANG_CSHARP, 0, "x = 0xFF_FFu + 0b1010_1010 + 1_000UL + 3.14e-2f + 1.5m + .5d + 10L; r = 1..^2;");
	T("0xFF_FFu", TK_NUMBER);
	T("0b1010_1010", TK_NUMBER);
	T("1_000UL", TK_NUMBER);
	T("3.14e-2f", TK_NUMBER);
	T("1.5m", TK_NUMBER);
	T(".5d", TK_NUMBER);
	T("10L", TK_NUMBER);
	T("..", TK_OPERATOR);
	/* strings and chars */
	lex(LANG_CSHARP, 0, "s = \"a\\tb\\u0041\\x7\"; c = '\\n'; d = 'x'; e = '\\'';");
	T("\"a", TK_STRING);
	T("\\t", TK_ESCAPE);
	T("\\u0041", TK_ESCAPE);
	T("\\x7", TK_ESCAPE);
	T("\\n", TK_ESCAPE);
	T("'x'", TK_STRING);
	lex(LANG_CSHARP, 0, "p = @\"C:\\tmp\\\"\"q\"\"\"; ok = 1;");
	T("@\"C:\\tmp\\", TK_STRING);
	TN("\"\"", 0, TK_ESCAPE);
	T("ok", TK_VARIABLE);
	lex(LANG_CSHARP, 0, "t = $\"Hi {name,10:N2} {{x}} {(a ? b : c)} {Get(\"k\")}\";");
	T("$\"Hi ", TK_STRING);
	T("name", TK_VARIABLE);
	T("10", TK_NUMBER);
	T(":N2", TK_STRING);
	T("{{", TK_ESCAPE);
	T("}}", TK_ESCAPE);
	T("a", TK_VARIABLE);
	T("Get", TK_FUNCTION);
	T("\"k\"", TK_STRING);
	lex(LANG_CSHARP, 0, "u = @$\"{a}\\n\" + $@\"{b}\";");
	T("@$\"", TK_STRING);
	T("a", TK_VARIABLE);
	T("\\n\"", TK_STRING);
	T("b", TK_VARIABLE);
	lex(LANG_CSHARP, 0, "r = \"\"\"say \"hi\" \"\"\"; e = \"\"; r2 = $$\"\"\"{\"a\": {{v}}}\"\"\";");
	T("\"\"\"say \"hi\" \"\"\"", TK_STRING);
	T("e", TK_VARIABLE);
	T("{\"a\": ", TK_STRING);
	T("v", TK_VARIABLE);
	T("}\"\"\"", TK_STRING);
	/* multi-line: raw strings keep their quote and '$' counts */
	s = lex(LANG_CSHARP, 0, "var raw = $$\"\"\"\"");
	CHECK((s & 3u) == 3u && ((s >> 3) & 31u) == 4u && ((s >> 8) & 7u) == 2u);
	s = lex(LANG_CSHARP, s, "  text \"\"\" {x} {{y}}");
	T("  text \"\"\" {x} ", TK_STRING);
	T("y", TK_VARIABLE);
	CHECK((s & 3u) == 3u);
	s = lex(LANG_CSHARP, s, "  \"\"\"\"; int z = 1;");
	T("  \"\"\"\"", TK_STRING);
	T("int", TK_STORAGE);
	CHECK(s == 0);
	/* verbatim strings span lines; interpolated ones keep the flag */
	s = lex(LANG_CSHARP, 0, "var v = $@\"multi {x}");
	CHECK((s & 3u) == 2u && (s & 4u));
	s = lex(LANG_CSHARP, s, "\"\"q\"\" {y} line\"; z = 2;");
	T("\"\"", TK_ESCAPE);
	T("y", TK_VARIABLE);
	T("z", TK_VARIABLE);
	CHECK(s == 0);
	s = lex(LANG_CSHARP, 0, "var w = @\"no {x}");
	CHECK(s == 2u);
	s = lex(LANG_CSHARP, s, "{y}\";");
	T("{y}\"", TK_STRING);
	CHECK(s == 0);
	/* a regular string ends at the line */
	s = lex(LANG_CSHARP, 0, "var bad = \"open");
	CHECK(s == 0);
	/* block comments */
	s = lex(LANG_CSHARP, 0, "int a; /* block");
	CHECK(s == 1u);
	s = lex(LANG_CSHARP, s, "still */ int b;");
	T("still */", TK_COMMENT);
	T("int", TK_STORAGE);
	CHECK(s == 0);
	/* doc comments, attributes, preprocessor */
	lex(LANG_CSHARP, 0, "/// <param name=\"x\">The <see cref=\"T\"/> &amp; more</param>");
	T("/// ", TK_COMMENT);
	T("<", TK_TAGPUNCT);
	T("param", TK_TAG);
	T("name", TK_ATTR);
	T("\"x\"", TK_STRING);
	T("The ", TK_COMMENT);
	T("/>", TK_TAGPUNCT);
	T("&amp;", TK_LANGCONST);
	T("</", TK_TAGPUNCT);
	lex(LANG_CSHARP, 0, "// plain <b>not</b> xml");
	T("// plain <b>not</b> xml", TK_COMMENT);
	lex(LANG_CSHARP, 0, "[assembly: InternalsVisibleTo(\"Tests\")]");
	T("assembly", TK_STORAGE);
	T("InternalsVisibleTo", TK_TYPE);
	lex(LANG_CSHARP, 0, "[Serializable, Obsolete(\"x\"), Foo.Bar] void M([NotNull] string text, int[] nums) { nums[0] = 1; }");
	T("Serializable", TK_TYPE);
	T("Obsolete", TK_TYPE);
	T("Foo", TK_TYPE);
	T("Bar", TK_TYPE);
	T("M", TK_FUNCTION);
	T("NotNull", TK_TYPE);
	T("text", TK_VARIABLE);
	T("nums", TK_VARIABLE);
	lex(LANG_CSHARP, 0, "#if DEBUG && !TRACE // why");
	T("#if", TK_PREPROC);
	T("DEBUG", TK_VARIABLE);
	T("&&", TK_OPERATOR);
	T("TRACE", TK_VARIABLE);
	T("// why", TK_COMMENT);
	lex(LANG_CSHARP, 0, "  #region Helper methods  ");
	T("#region", TK_PREPROC);
	T("Helper methods", TK_STRING);
	lex(LANG_CSHARP, 0, "#nullable enable");
	T("#nullable", TK_PREPROC);
	T("enable", TK_PREPROC);
	lex(LANG_CSHARP, 0, "#pragma warning disable CS0168");
	T("warning", TK_PREPROC);
	lex(LANG_CSHARP, 0, "#define X");
	T("X", TK_VARIABLE);
	/* states stay within the embeddable width */
	CHECK(lex(LANG_CSHARP, 0, "x = $$$$$$$$$$\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"") < (1u << SYN_STATE_BITS));
	/* deep interpolation nesting is bounded */
	char deep[2048];
	u32 dl = 0;
	for (int k = 0; k < 200; k++) {
		memcpy(deep + dl, "$\"{", 3);
		dl += 3;
	}
	deep[dl] = 0;
	lex(LANG_CSHARP, 0, deep);
}

static void test_qml(void)
{
	u32 s;
	lex(LANG_QML, 0, "import QtQuick.Controls 2.15 as QQC");
	T("import", TK_CONTROL);
	T("QtQuick", TK_TYPE);
	T("Controls", TK_TYPE);
	T("2.15", TK_NUMBER);
	T("as", TK_CONTROL);
	T("QQC", TK_TYPE);
	lex(LANG_QML, 0, "import \"util.js\" as Util");
	T("\"util.js\"", TK_STRING);
	T("Util", TK_TYPE);
	lex(LANG_QML, 0, "pragma Singleton");
	T("pragma", TK_CONTROL);
	T("Singleton", TK_TYPE);
	lex(LANG_QML, 0, "Item {");
	T("Item", TK_TYPE);
	lex(LANG_QML, 0, "    id: root");
	T("id", TK_STORAGE);
	T("root", TK_VARIABLE);
	lex(LANG_QML, 0, "    readonly property int count: 3 * 2");
	T("readonly", TK_STORAGE);
	T("property", TK_STORAGE);
	T("int", TK_TYPE);
	T("count", TK_VARIABLE);
	T("3", TK_NUMBER);
	lex(LANG_QML, 0, "required property list<Item> kids; default property alias content: box.data");
	T("required", TK_STORAGE);
	T("list", TK_TYPE);
	T("Item", TK_TYPE);
	T("kids", TK_VARIABLE);
	T("default", TK_STORAGE);
	T("alias", TK_STORAGE);
	T("content", TK_VARIABLE);
	T("data", TK_VARIABLE);
	lex(LANG_QML, 0, "signal clicked(int x, y: real)");
	T("signal", TK_STORAGE);
	T("clicked", TK_FUNCTION);
	T("int", TK_TYPE);
	T("x", TK_VARIABLE);
	T("y", TK_VARIABLE);
	T("real", TK_TYPE);
	lex(LANG_QML, 0, "Layout.fillWidth: true; anchors.fill: parent");
	T("Layout", TK_TYPE);
	T("fillWidth", TK_VARIABLE);
	T("true", TK_LANGCONST);
	T("anchors", TK_VARIABLE);
	T("fill", TK_VARIABLE);
	T("parent", TK_VARIABLE);
	lex(LANG_QML, 0, "onClicked: console.log(`n=${count + 1}`, /ab+c/gi.test(name), a / b)");
	T("onClicked", TK_FUNCTION);
	T("console", TK_VARIABLE);
	T("log", TK_FUNCTION);
	T("`n=", TK_STRING);
	T("${", TK_STORAGE);
	T("count", TK_VARIABLE);
	T("1", TK_NUMBER);
	T("}", TK_STORAGE);
	T("/ab+c/gi", TK_REGEX);
	T("test", TK_FUNCTION);
	TN("/", 2, TK_OPERATOR);
	lex(LANG_QML, 0, "Component.onCompleted: { var x = Text.AlignLeft; if (x) return MAX_N }");
	T("Component", TK_TYPE);
	T("onCompleted", TK_FUNCTION);
	T("var", TK_STORAGE);
	T("Text", TK_TYPE);
	T("AlignLeft", TK_CONSTANT);
	T("if", TK_CONTROL);
	T("return", TK_CONTROL);
	T("MAX_N", TK_CONSTANT);
	lex(LANG_QML, 0, "Behavior on opacity { NumberAnimation { duration: 200 } }");
	T("Behavior", TK_TYPE);
	T("on", TK_STORAGE);
	T("opacity", TK_VARIABLE);
	T("NumberAnimation", TK_TYPE);
	T("duration", TK_VARIABLE);
	lex(LANG_QML, 0, "QQC.Button { text: qsTr(\"Hi\\n\") }");
	T("QQC", TK_TYPE);
	T("Button", TK_TYPE);
	T("qsTr", TK_FUNCTION);
	T("\\n", TK_ESCAPE);
	lex(LANG_QML, 0, "function doIt(a, b) { let r = new Date(); return typeof a === 'x' ? null : r }");
	T("function", TK_STORAGE);
	T("doIt", TK_FUNCTION);
	T("let", TK_STORAGE);
	T("new", TK_STORAGE);
	T("Date", TK_TYPE);
	T("typeof", TK_STORAGE);
	T("'x'", TK_STRING);
	T("null", TK_LANGCONST);
	lex(LANG_QML, 0, "component Tag: Rectangle { color: \"red\" }");
	T("component", TK_STORAGE);
	T("Tag", TK_TYPE);
	T("Rectangle", TK_TYPE);
	T("color", TK_VARIABLE);
	lex(LANG_QML, 0, "enum Mode { Idle, Busy }");
	T("enum", TK_STORAGE);
	T("Mode", TK_TYPE);
	lex(LANG_QML, 0, "onPressed: (mouse) => { for (const p of pts) mouse.accepted = false }");
	T("onPressed", TK_FUNCTION);
	T("=>", TK_STORAGE);
	T("for", TK_CONTROL);
	T("const", TK_STORAGE);
	T("of", TK_STORAGE);
	T("accepted", TK_VARIABLE);
	/* switch/default stay JavaScript */
	lex(LANG_QML, 0, "switch (k) { case 1: break; default: x = 0 }");
	T("switch", TK_CONTROL);
	T("case", TK_CONTROL);
	T("default", TK_CONTROL);
	/* block comments and template strings span lines */
	s = lex(LANG_QML, 0, "/* multi");
	CHECK(s == 1u);
	s = lex(LANG_QML, s, "   line */ width: 10");
	T("   line */", TK_COMMENT);
	T("width", TK_VARIABLE);
	CHECK(s == 0);
	s = lex(LANG_QML, 0, "property string t: `first ${a}");
	CHECK(s == 2u);
	T("a", TK_VARIABLE);
	s = lex(LANG_QML, s, "second ${b} \\` end` + x");
	T("second ", TK_STRING);
	T("b", TK_VARIABLE);
	T("\\`", TK_ESCAPE);
	T("x", TK_VARIABLE);
	CHECK(s == 0);
}

static void test_qmake(void)
{
	u32 s;
	lex(LANG_QMAKE, 0, "QT += core gui widgets  # comment");
	T("QT", TK_VARIABLE);
	T("+=", TK_OPERATOR);
	T("core gui widgets", TK_TEXT);
	T("# comment", TK_COMMENT);
	lex(LANG_QMAKE, 0, "TEMPLATE = app");
	T("TEMPLATE", TK_VARIABLE);
	T("=", TK_OPERATOR);
	lex(LANG_QMAKE, 0, "CONFIG -= x; DEFINES *= A; LIST ~= s/a/b/");
	T("-=", TK_OPERATOR);
	lex(LANG_QMAKE, 0, "DEFINES ~= s/FOO/BAR/");
	T("~=", TK_OPERATOR);
	lex(LANG_QMAKE, 0, "QMAKE_CXXFLAGS *= -O2");
	T("QMAKE_CXXFLAGS", TK_VARIABLE);
	T("*=", TK_OPERATOR);
	lex(LANG_QMAKE, 0, "win32:CONFIG += console");
	T("win32", TK_TYPE);
	T(":", TK_OPERATOR);
	T("CONFIG", TK_VARIABLE);
	lex(LANG_QMAKE, 0, "unix:!macx|freebsd {");
	T("unix", TK_TYPE);
	T("!", TK_OPERATOR);
	T("macx", TK_TYPE);
	T("|", TK_OPERATOR);
	T("freebsd", TK_TYPE);
	lex(LANG_QMAKE, 0, "LIBS += -L$$PWD/lib $${EXTRA} $$[QT_INSTALL_LIBS] $$(HOME) $(MAKEFILE)");
	T("$$PWD", TK_VARIABLE);
	T("/lib", TK_TEXT);
	T("$${EXTRA}", TK_VARIABLE);
	T("$$[QT_INSTALL_LIBS]", TK_VARIABLE);
	T("$$(HOME)", TK_VARIABLE);
	T("$(MAKEFILE)", TK_VARIABLE);
	lex(LANG_QMAKE, 0, "contains(QT_ARCH, x86_64):message(\"64 bit $$TARGET \\\"q\\\"\")");
	T("contains", TK_FUNCTION);
	T("QT_ARCH", TK_TEXT);
	T("message", TK_FUNCTION);
	T("\"64 bit ", TK_STRING);
	T("$$TARGET", TK_VARIABLE);
	T("\\\"", TK_ESCAPE);
	lex(LANG_QMAKE, 0, "} else:unix {");
	T("else", TK_CONTROL);
	T("unix", TK_TYPE);
	lex(LANG_QMAKE, 0, "target.path = $$[QT_INSTALL_BINS]");
	T("target.path", TK_VARIABLE);
	lex(LANG_QMAKE, 0, "FILES = $$files(*.cpp, true) contains");
	T("$$files", TK_FUNCTION);
	T("contains", TK_TEXT);
	lex(LANG_QMAKE, 0, "for(f, FILES) { !exists($$f): error(missing) }");
	T("for", TK_CONTROL);
	T("exists", TK_FUNCTION);
	T("$$f", TK_VARIABLE);
	T("error", TK_FUNCTION);
	lex(LANG_QMAKE, 0, "defineTest(check) {");
	T("defineTest", TK_CONTROL);
	/* line continuation keeps listing values */
	s = lex(LANG_QMAKE, 0, "SOURCES += main.cpp \\");
	T("\\", TK_ESCAPE);
	CHECK(s == 1u);
	s = lex(LANG_QMAKE, s, "    win32:a.cpp \\");
	T("win32:a.cpp", TK_TEXT);
	CHECK(s == 1u);
	s = lex(LANG_QMAKE, s, "    b.cpp");
	CHECK(s == 0);
	s = lex(LANG_QMAKE, s, "HEADERS = x.h # not continued \\");
	CHECK(s == 0);
}

static void test_gdscript(void)
{
	u32 s;
	lex(LANG_GDSCRIPT, 0, "@tool");
	T("@tool", TK_FUNCTION);
	lex(LANG_GDSCRIPT, 0, "class_name Player extends CharacterBody2D");
	T("class_name", TK_STORAGE);
	T("Player", TK_TYPE);
	T("extends", TK_STORAGE);
	T("CharacterBody2D", TK_TYPE);
	lex(LANG_GDSCRIPT, 0, "## Doc comment");
	T("## Doc comment", TK_COMMENT);
	lex(LANG_GDSCRIPT, 0, "signal health_changed(old: int, new_value)");
	T("signal", TK_STORAGE);
	T("health_changed", TK_FUNCTION);
	T("old", TK_VARIABLE);
	T("int", TK_TYPE);
	lex(LANG_GDSCRIPT, 0, "enum State { IDLE, RUN = 2 }");
	T("enum", TK_STORAGE);
	T("State", TK_TYPE);
	T("IDLE", TK_CONSTANT);
	lex(LANG_GDSCRIPT, 0, "const SPEED := 300.0");
	T("const", TK_STORAGE);
	T("SPEED", TK_CONSTANT);
	T("300.0", TK_NUMBER);
	lex(LANG_GDSCRIPT, 0, "@export_range(0, 10) var speed: float = 1.5e2");
	T("@export_range", TK_FUNCTION);
	T("var", TK_STORAGE);
	T("speed", TK_VARIABLE);
	T("float", TK_TYPE);
	T("1.5e2", TK_NUMBER);
	lex(LANG_GDSCRIPT, 0, "@onready var sprite: Sprite2D = $Body/Sprite2D");
	T("@onready", TK_FUNCTION);
	T("Sprite2D", TK_TYPE);
	T("$", TK_CONTROL);
	T("Body/Sprite2D", TK_ESCAPE);
	lex(LANG_GDSCRIPT, 0, "var hud = %HUD; var n = $\"Quoted Node\"; var m = a % b");
	T("%", TK_CONTROL);
	T("HUD", TK_ESCAPE);
	T("\"Quoted Node\"", TK_STRING);
	TN("%", 1, TK_OPERATOR);
	lex(LANG_GDSCRIPT, 0, "var path := ^\"../Node\"; var sn = &\"jump\"");
	T("^", TK_CONSTANT);
	T("\"../Node\"", TK_STRING);
	T("&", TK_CONSTANT);
	T("\"jump\"", TK_STRING);
	lex(LANG_GDSCRIPT, 0, "\tset(value):");
	T("set", TK_STORAGE);
	lex(LANG_GDSCRIPT, 0, "\tget:");
	T("get", TK_STORAGE);
	lex(LANG_GDSCRIPT, 0, "\tdict.get(\"k\")");
	T("get", TK_FUNCTION);
	lex(LANG_GDSCRIPT, 0, "func _ready() -> void:");
	T("func", TK_STORAGE);
	T("_ready", TK_FUNCTION);
	T("void", TK_STORAGE);
	lex(LANG_GDSCRIPT, 0, "\tif not is_on_floor() and hp % 2 == 0: velocity = Vector2(0, -SPEED)");
	T("if", TK_CONTROL);
	T("not", TK_STORAGE);
	T("is_on_floor", TK_FUNCTION);
	T("and", TK_STORAGE);
	T("%", TK_OPERATOR);
	T("Vector2", TK_TYPE);
	T("SPEED", TK_CONSTANT);
	lex(LANG_GDSCRIPT, 0, "\tawait get_tree().create_timer(1.0).timeout; super._ready(); self.x = null");
	T("await", TK_CONTROL);
	T("get_tree", TK_FUNCTION);
	T("timeout", TK_VARIABLE);
	T("super", TK_LANGCONST);
	T("self", TK_LANGCONST);
	T("null", TK_LANGCONST);
	lex(LANG_GDSCRIPT, 0, "\tprint(\"hp=%d\\n\" % hp, r\"raw\\n\")");
	T("print", TK_FUNCTION);
	T("%d", TK_STORAGE);
	T("\\n", TK_ESCAPE);
	TN("r", 1, TK_STORAGE);
	T("\"raw\\n\"", TK_STRING);
	lex(LANG_GDSCRIPT, 0, "\tmatch state: State.IDLE when hp > 0: pass");
	T("match", TK_CONTROL);
	T("when", TK_CONTROL);
	T("pass", TK_CONTROL);
	lex(LANG_GDSCRIPT, 0, "for i in range(3): x += 0x1F + 0b10 + 1_000");
	T("for", TK_CONTROL);
	T("in", TK_STORAGE);
	T("range", TK_FUNCTION);
	T("0x1F", TK_NUMBER);
	T("0b10", TK_NUMBER);
	T("1_000", TK_NUMBER);
	/* triple-quoted strings span lines (raw ones too) */
	s = lex(LANG_GDSCRIPT, 0, "var doc = \"\"\"multi");
	CHECK(s == 2u);
	s = lex(LANG_GDSCRIPT, s, "line 'q' \\t");
	T("\\t", TK_ESCAPE);
	CHECK(s == 2u);
	s = lex(LANG_GDSCRIPT, s, "\"\"\" + x");
	T("x", TK_VARIABLE);
	CHECK(s == 0);
	s = lex(LANG_GDSCRIPT, 0, "var r = r'''raw \\d");
	CHECK(s == (1u | 4u));
	s = lex(LANG_GDSCRIPT, s, "\\n'''");
	T("\\n'''", TK_STRING);
	CHECK(s == 0);
}

static void test_godot_res(void)
{
	u32 s;
	lex(LANG_GODOT_RES, 0, "[gd_scene load_steps=3 format=3 uid=\"uid://b1x\"]");
	T("[gd_scene", TK_TAG);
	T("load_steps", TK_ATTR);
	T("3", TK_NUMBER);
	T("uid", TK_ATTR);
	T("\"uid://b1x\"", TK_STRING);
	T("]", TK_TAG);
	lex(LANG_GODOT_RES, 0, "[node name=\"P\" type=\"Node2D\" groups=[\"a\", \"b\"] instance=ExtResource(\"1\")] ; c");
	T("name", TK_ATTR);
	T("groups", TK_ATTR);
	T("\"b\"", TK_STRING);
	T("ExtResource", TK_FUNCTION);
	TN("]", 1, TK_TAG);
	T("; c", TK_COMMENT);
	lex(LANG_GODOT_RES, 0, "; comment line");
	T("; comment line", TK_COMMENT);
	lex(LANG_GODOT_RES, 0, "[application]");
	T("[application]", TK_TAG);
	lex(LANG_GODOT_RES, 0, "config/features=PackedStringArray(\"4.2\", \"Forward Plus\")");
	T("config/features", TK_PROPERTY);
	T("PackedStringArray", TK_TYPE);
	T("\"4.2\"", TK_STRING);
	lex(LANG_GODOT_RES, 0, "position = Vector2(10.5, -2e3)");
	T("position", TK_PROPERTY);
	T("Vector2", TK_TYPE);
	T("10.5", TK_NUMBER);
	T("-2e3", TK_NUMBER);
	lex(LANG_GODOT_RES, 0, "metadata/_edit_lock_ = true");
	T("metadata/_edit_lock_", TK_PROPERTY);
	T("true", TK_LANGCONST);
	lex(LANG_GODOT_RES, 0, "script = SubResource(\"x\"); v = inf");
	T("SubResource", TK_FUNCTION);
	lex(LANG_GODOT_RES, 0, "tags = [&\"y\", ^\"z\", null, \"a\\tb\"]");
	T("&", TK_CONSTANT);
	T("^", TK_CONSTANT);
	T("null", TK_LANGCONST);
	T("\\t", TK_ESCAPE);
	lex(LANG_GODOT_RES, 0, "config_version=5");
	T("config_version", TK_PROPERTY);
	T("5", TK_NUMBER);
	/* multi-line string values */
	s = lex(LANG_GODOT_RES, 0, "text = \"line one");
	T("\"line one", TK_STRING);
	CHECK(s == 1u);
	s = lex(LANG_GODOT_RES, s, "[not a header] = \\\"two\\\"");
	T("[not a header] = ", TK_STRING);
	T("\\\"", TK_ESCAPE);
	CHECK(s == 1u);
	s = lex(LANG_GODOT_RES, s, "end\" ; c");
	T("end\"", TK_STRING);
	T("; c", TK_COMMENT);
	CHECK(s == 0);
	/* dictionary entries over several lines are values, not keys */
	s = lex(LANG_GODOT_RES, 0, "\"a\": 1,");
	T("\"a\"", TK_STRING);
	T("1", TK_NUMBER);
	CHECK(s == 0);
}

static void test_xml(void)
{
	u32 s;
	lex(LANG_XML, 0, "<?xml version=\"1.0\" encoding='UTF-8'?>");
	T("<?", TK_TAGPUNCT);
	T("xml", TK_TAG);
	T("version", TK_ATTR);
	T("\"1.0\"", TK_UNQUOTED);
	T("'UTF-8'", TK_UNQUOTED);
	T("?>", TK_TAGPUNCT);
	lex(LANG_XML, 0, "<ui:root xmlns:ui=\"urn:x\">text &amp; &#65; &#x41; & bare</ui:root>");
	T("<", TK_TAGPUNCT);
	TN("ui", 0, TK_TAG);
	T("root", TK_TAG);
	T("xmlns", TK_ATTR);
	TN("ui", 1, TK_ATTR);
	T(":", TK_TEXT);
	T("text ", TK_TEXT);
	T("&amp;", TK_LANGCONST);
	T("&#65;", TK_LANGCONST);
	T("&#x41;", TK_LANGCONST);
	T("&", TK_LANGCONST);
	TN("&", 3, TK_INVALID);
	T("</", TK_TAGPUNCT);
	lex(LANG_XML, 0, "<br/><a href=\"x?a=1&amp;b\">a < b</a>");
	T("/>", TK_TAGPUNCT);
	T("\"x?a=1", TK_UNQUOTED);
	T("a < b", TK_TEXT);
	/* comments span lines */
	s = lex(LANG_XML, 0, "<a/> <!-- a comment");
	T("<!-- a comment", TK_COMMENT);
	CHECK(s == 1u);
	s = lex(LANG_XML, s, "  <b>still</b> --> <c/>");
	T("  <b>still</b> -->", TK_COMMENT);
	T("c", TK_TAG);
	CHECK(s == 0);
	/* CDATA spans lines */
	s = lex(LANG_XML, 0, "<x><![CDATA[ raw <stuff>");
	T("<![CDATA[ raw <stuff>", TK_UNQUOTED);
	CHECK(s == 2u);
	s = lex(LANG_XML, s, "  more ]]></x>");
	T("  more ]]>", TK_UNQUOTED);
	T("x", TK_TAG);
	CHECK(s == 0);
	/* attributes and values across lines */
	s = lex(LANG_XML, 0, "<widget class=\"QPushButton\"");
	CHECK(s == 3u);
	s = lex(LANG_XML, s, "        name=\"multi");
	T("name", TK_ATTR);
	CHECK(s == 4u);
	s = lex(LANG_XML, s, "line &lt; v\" other='q");
	T("line ", TK_UNQUOTED);
	T("&lt;", TK_LANGCONST);
	T("other", TK_ATTR);
	CHECK(s == 5u);
	s = lex(LANG_XML, s, "x'>text</widget>");
	T("x'", TK_UNQUOTED);
	T(">", TK_TAGPUNCT);
	T("widget", TK_TAG);
	CHECK(s == 0);
	/* processing instructions across lines close on ?> */
	s = lex(LANG_XML, 0, "<?xml-stylesheet type=\"text/xsl\"");
	CHECK(s == (3u | 8u));
	s = lex(LANG_XML, s, "  href=\"a.xsl\"?>");
	T("?>", TK_TAGPUNCT);
	CHECK(s == 0);
	/* DOCTYPE with an internal subset */
	s = lex(LANG_XML, 0, "<!DOCTYPE note SYSTEM \"note.dtd\" [");
	T("<!", TK_TAGPUNCT);
	T("DOCTYPE", TK_STORAGE);
	T("note", TK_LANGCONST);
	T("SYSTEM", TK_STORAGE);
	T("\"note.dtd\"", TK_STRING);
	CHECK(s == (6u | 16u));
	s = lex(LANG_XML, s, "  <!ENTITY writer \"Me\"> %pe; <!-- c");
	T("ENTITY", TK_STORAGE);
	T("\"Me\"", TK_STRING);
	T(">", TK_TAGPUNCT);
	T("%pe;", TK_LANGCONST);
	CHECK(s == (1u | 16u));
	s = lex(LANG_XML, s, "--> ]>");
	T("-->", TK_COMMENT);
	CHECK(s == 0);
	s = lex(LANG_XML, 0, "<!DOCTYPE html>");
	T("html", TK_LANGCONST);
	CHECK(s == 0);
	/* an unclosed tag gives way to the next '<' */
	s = lex(LANG_XML, 0, "<a <b>");
	T("b", TK_TAG);
	CHECK(s == 0);
}

/* GLSL (the C lexer with GLSL's words) and SPIR-V assembly: ucas's code blocks */
static void test_shaders(void)
{
	CHECK(syn_lang_for_path("a/b.comp", NULL, 0) == LANG_GLSL);
	CHECK(syn_lang_for_path("x.frag", NULL, 0) == LANG_GLSL && syn_lang_for_path("x.glsl", NULL, 0) == LANG_GLSL);
	CHECK(syn_lang_for_path("x.spvasm", NULL, 0) == LANG_SPIRV);
	CHECK(syn_lang_for_path("x.cs", NULL, 0) == LANG_CSHARP);
	CHECK(syn_block_lang("comp", 4) == LANG_GLSL && syn_block_lang("GLSL", 4) == LANG_GLSL);
	CHECK(syn_block_lang("spv", 3) == LANG_SPIRV && syn_block_lang("c", 1) == LANG_C);
	CHECK(syn_block_lang("rust", 4) == LANG_RUST && syn_block_lang("nope", 4) == LANG_NONE);
	CHECK(syn_fence_lang("glsl", 4) == LANG_GLSL);  /* fits Markdown's 5 bits */
	CHECK(syn_fence_lang("spv", 3) == LANG_NONE);   /* 32: does not */
	CHECK(!strcmp(syn_lang_name(LANG_GLSL), "GLSL") && !strcmp(syn_line_comment(LANG_SPIRV), ";"));

	lex(LANG_GLSL, 0, "layout(std430, binding = 0) buffer B { uvec4 v[]; };");
	T("layout", TK_STORAGE);
	T("buffer", TK_STORAGE);
	T("uvec4", TK_STORAGE);
	TN("0", 1, TK_NUMBER); /* the first is std430's */
	lex(LANG_GLSL, 0, "void main() { if (x > 1u) discard; v[0] = gl_GlobalInvocationID.x; }");
	T("main", TK_FUNCTION);
	T("if", TK_CONTROL);
	T("discard", TK_CONTROL);
	T("gl_GlobalInvocationID", TK_LANGCONST);
	u32 s = lex(LANG_GLSL, 0, "#version 450 /* a");
	T("#version", TK_CONTROL); /* a directive, as C's #include */
	lex(LANG_GLSL, s, "comment */ mat3 m;");
	T("comment", TK_COMMENT);
	T("mat3", TK_STORAGE);
	lex(LANG_GLSL, 0, "struct Light { vec3 dir; };");
	T("struct", TK_STORAGE);
	T("Light", TK_TYPE);
	/* C keeps its own words: no GLSL types there */
	lex(LANG_C, 0, "vec3 v; uint32_t gl_x;");
	T("vec3", TK_TEXT);
	T("gl_x", TK_TEXT);

	s = lex(LANG_SPIRV, 0, "; SPIR-V");
	T("; SPIR-V", TK_COMMENT);
	CHECK(s == 0);
	lex(LANG_SPIRV, 0, "     %uint_300 = OpConstant %uint 300");
	T("%uint_300", TK_VARIABLE);
	T("OpConstant", TK_STORAGE);
	TN("300", 1, TK_NUMBER);
	T("=", TK_OPERATOR);
	lex(LANG_SPIRV, 0, "%24 = OpExtInst %uint %2 UMin %20 %uint_300 ; clamp");
	T("OpExtInst", TK_FUNCTION);
	T("UMin", TK_CONSTANT);
	T("; clamp", TK_COMMENT);
	lex(LANG_SPIRV, 0, "OpBranchConditional %c %a %b");
	T("OpBranchConditional", TK_CONTROL);
	lex(LANG_SPIRV, 0, "%v4 = OpTypeVector %float 4");
	T("OpTypeVector", TK_TYPE);
	lex(LANG_SPIRV, 0, "OpEntryPoint GLCompute %main \"main\" %gid");
	T("OpEntryPoint", TK_PREPROC);
	T("GLCompute", TK_CONSTANT);
	T("\"main\"", TK_STRING);
	lex(LANG_SPIRV, 0, "%f = OpConstant %float -1.5e-3");
	T("-1.5e-3", TK_NUMBER);
	/* a fence in Markdown: GLSL inside */
	s = lex(LANG_MARKDOWN, 0, "```glsl");
	lex(LANG_MARKDOWN, s, "uniform vec4 c;");
	T("uniform", TK_STORAGE);
}

static void test_detect7(void)
{
	CHECK(syn_lang_for_path("src/Program.cs", NULL, 0) == LANG_CSHARP);
	CHECK(syn_lang_for_path("build.csx", NULL, 0) == LANG_CSHARP);
	CHECK(syn_lang_for_path("A.CS", NULL, 0) == LANG_CSHARP);
	CHECK(syn_lang_for_path("qml/Main.qml", NULL, 0) == LANG_QML);
	static const char *const pro[] = {"app.pro", "common.pri", "feature.prf", "libQt6Core.prl"};
	for (size_t i = 0; i < VX_COUNT(pro); i++)
		CHECK(syn_lang_for_path(pro[i], NULL, 0) == LANG_QMAKE);
	CHECK(syn_lang_for_path("player.gd", NULL, 0) == LANG_GDSCRIPT);
	static const char *const gres[] = {"main.tscn", "theme.tres", "x.godot", "icon.svg.import", "/g/project.godot"};
	for (size_t i = 0; i < VX_COUNT(gres); i++)
		CHECK(syn_lang_for_path(gres[i], NULL, 0) == LANG_GODOT_RES);
	static const char *const xml[] = {"a.xml", "form.ui", "res.qrc", "App.csproj", "Directory.Build.props",
	                                  "x.targets", "icon.svg", "s.xsd", "t.xsl", "Info.plist", "Strings.resx"};
	for (size_t i = 0; i < VX_COUNT(xml); i++)
		CHECK(syn_lang_for_path(xml[i], NULL, 0) == LANG_XML);
	const char *ts = "<?xml version=\"1.0\"?>\n<!DOCTYPE TS>\n<TS version=\"2.1\">\n";
	CHECK(syn_lang_for_path("app_de.ts", ts, strlen(ts)) == LANG_XML);
	CHECK(syn_lang_for_path("app_de.ts", NULL, 0) == LANG_XML);
	const char *tsc = "export const x: number = 1;\n";
	CHECK(syn_lang_for_path("index.ts", tsc, strlen(tsc)) == LANG_NONE);
	/* an XML head names the language of unknown files */
	const char *xh = "\xEF\xBB\xBF\n  <?xml version=\"1.0\"?>\n<root/>\n";
	CHECK(syn_lang_for_path("config", xh, strlen(xh)) == LANG_XML);
	CHECK(syn_lang_for_path("data.weird", xh, strlen(xh)) == LANG_XML);
	CHECK(syn_lang_for_path("data.weird", "hello", 5) == LANG_NONE);
	CHECK(syn_lang_for_path("x.c", xh, strlen(xh)) == LANG_C);
	CHECK_STR(syn_lang_name(LANG_CSHARP), "C#");
	CHECK_STR(syn_lang_name(LANG_QML), "QML");
	CHECK_STR(syn_lang_name(LANG_QMAKE), "qmake");
	CHECK_STR(syn_lang_name(LANG_GDSCRIPT), "GDScript");
	CHECK_STR(syn_lang_name(LANG_GODOT_RES), "Godot Resource");
	CHECK_STR(syn_lang_name(LANG_XML), "XML");
	CHECK_STR(syn_lang_lsp_id(LANG_CSHARP), "csharp");
	CHECK_STR(syn_lang_lsp_id(LANG_QML), "qml");
	CHECK_STR(syn_lang_lsp_id(LANG_GDSCRIPT), "gdscript");
	CHECK_STR(syn_lang_lsp_id(LANG_XML), "xml");
	CHECK(syn_lang_lsp_id(LANG_QMAKE) == NULL);
	CHECK(syn_lang_lsp_id(LANG_GODOT_RES) == NULL);
	CHECK_STR(syn_line_comment(LANG_CSHARP), "//");
	CHECK_STR(syn_line_comment(LANG_QML), "//");
	CHECK_STR(syn_line_comment(LANG_QMAKE), "#");
	CHECK_STR(syn_line_comment(LANG_GDSCRIPT), "#");
	CHECK_STR(syn_line_comment(LANG_GODOT_RES), ";");
	CHECK(syn_line_comment(LANG_XML) == NULL);
	CHECK(LANG_GLSL < 32);
	/* Markdown fences embed the new languages and carry their states */
	u32 s = lex(LANG_MARKDOWN, 0, "```csharp");
	s = lex(LANG_MARKDOWN, s, "var r = $$\"\"\"\"");
	s = lex(LANG_MARKDOWN, s, "class X \"\"\" {{v}}");
	T("class X \"\"\" ", TK_STRING);
	T("v", TK_VARIABLE);
	s = lex(LANG_MARKDOWN, s, "\"\"\"\"; int z;");
	T("int", TK_STORAGE);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
	static const struct {
		const char *fence, *line, *sub;
		int cls;
	} fences[] = {
	    {"```cs", "public int X;", "public", TK_STORAGE},
	    {"```c#", "namespace N;", "N", TK_TYPE},
	    {"```qml", "Item { id: r }", "Item", TK_TYPE},
	    {"```gdscript", "func go(): pass", "go", TK_FUNCTION},
	    {"```gd", "var x = $Node", "Node", TK_ESCAPE},
	    {"```xml", "<a b=\"c\"/>", "b", TK_ATTR},
	    {"```qmake", "QT += core", "QT", TK_VARIABLE},
	    {"```tscn", "[node name=\"x\"]", "name", TK_ATTR},
	};
	for (size_t i = 0; i < VX_COUNT(fences); i++) {
		s = lex(LANG_MARKDOWN, 0, fences[i].fence);
		CHECK(s != 0);
		s = lex(LANG_MARKDOWN, s, fences[i].line);
		T(fences[i].sub, fences[i].cls);
	}
	s = lex(LANG_MARKDOWN, 0, "```xml");
	s = lex(LANG_MARKDOWN, s, "<!-- open");
	s = lex(LANG_MARKDOWN, s, "still -->");
	T("still -->", TK_COMMENT);
	s = lex(LANG_MARKDOWN, s, "```");
	CHECK(s == 0);
}

/* Random lines over each new lexer: spans stay ordered and in bounds (lex
 * checks) and states stay within SYN_STATE_BITS (for Markdown: the fenced
 * state it carries above bit 12). */
static void test_fuzz_new(void)
{
	static const char alpha[] = "\"'`$@{}[]()<>/\\*#;:=!?&%^.,+-_ \tabcxyzABC019"
	                            "\"\"\"$$$@@{{}}<!--]]>?>/*///";
	static const int langs[] = {LANG_CSHARP, LANG_QML, LANG_QMAKE, LANG_GDSCRIPT, LANG_GODOT_RES, LANG_XML, LANG_MARKDOWN};
	u32 rng = 12345;
	char line[200];
	int bad = 0;
	for (size_t l = 0; l < VX_COUNT(langs); l++) {
		u32 s = 0;
		for (int it = 0; it < 4000; it++) {
			rng = rng * 1103515245u + 12345u;
			u32 len = (rng >> 16) % (sizeof line - 1);
			for (u32 k = 0; k < len; k++) {
				rng = rng * 1103515245u + 12345u;
				line[k] = alpha[(rng >> 16) % (sizeof alpha - 1)];
			}
			line[len] = 0;
			if (it == 0 && langs[l] == LANG_MARKDOWN)
				strcpy(line, "```cs");
			if (langs[l] == LANG_MARKDOWN && (s >> 12) >= (1u << SYN_STATE_BITS))
				bad++;
			s = lex(langs[l], s, line);
			if (langs[l] != LANG_MARKDOWN && s >= (1u << SYN_STATE_BITS))
				bad++;
		}
	}
	CHECK(bad == 0);
}

static void test_sail(void)
{
	u32 s;
	lex(LANG_SAIL, 0, "val MinMax : forall 'n. (bool, bits('n)) -> bits(32)");
	T("val", TK_STORAGE);
	T("MinMax", TK_FUNCTION);
	T("forall", TK_STORAGE);
	T("'n", TK_TYPE);
	T("bool", TK_TYPE);
	T("bits", TK_TYPE);
	T("32", TK_NUMBER);
	T("->", TK_OPERATOR);
	lex(LANG_SAIL, 0, "function clause decode (0b0110 @ _ as op) if SEE < 1 = execute_X(op, true)");
	T("function", TK_STORAGE);
	T("clause", TK_STORAGE);
	T("decode", TK_FUNCTION);
	T("0b0110", TK_NUMBER);
	T("as", TK_STORAGE);
	T("if", TK_CONTROL);
	T("execute_X", TK_FUNCTION);
	T("true", TK_LANGCONST);
	lex(LANG_SAIL, 0, "union clause ast = ADD : (bits(5), bitzero) // the ADD");
	T("union", TK_STORAGE);
	T("ast", TK_TYPE);
	T("bitzero", TK_LANGCONST);
	T("// the ADD", TK_COMMENT);
	lex(LANG_SAIL, 0, "register PC : bits(64)");
	T("register", TK_STORAGE);
	T("PC", TK_VARIABLE);
	lex(LANG_SAIL, 0, "enum Shade = {Shade_Dark, Shade_Light}");
	T("Shade", TK_TYPE);
	T("Shade_Dark", TK_TEXT);
	lex(LANG_SAIL, 0, "$include <prelude.sail>");
	T("$include", TK_PREPROC);
	T("<prelude.sail>", TK_STRING);
	lex(LANG_SAIL, 0, "$[sv_module] let x : bits(32) = 0x0000_FFFF; foreach (i from 0 to 3 by 1 in inc) ()");
	T("$[sv_module]", TK_PREPROC);
	T("let", TK_STORAGE);
	T("0x0000_FFFF", TK_NUMBER);
	T("foreach", TK_CONTROL);
	T("from", TK_CONTROL);
	T("inc", TK_LANGCONST);
	lex(LANG_SAIL, 0, "print_endline(\"pc=\\n\")");
	T("\"pc=", TK_STRING);
	T("\\n", TK_ESCAPE);
	/* nested block comments span lines */
	s = lex(LANG_SAIL, 0, "x /* a /* b */");
	T("/* a /* b */", TK_COMMENT);
	CHECK(s == 1);
	s = lex(LANG_SAIL, s, "still */ let");
	T("still */", TK_COMMENT);
	T("let", TK_STORAGE);
	CHECK(s == 0);
}

static void test_verilog(void)
{
	u32 s;
	lex(LANG_VERILOG, 0, "module top #(parameter W = 8) (input wire clk, output reg [W-1:0] q);");
	T("module", TK_STORAGE);
	T("top", TK_TYPE);
	T("parameter", TK_STORAGE);
	T("input", TK_STORAGE);
	T("wire", TK_STORAGE);
	T("reg", TK_STORAGE);
	T("8", TK_NUMBER);
	lex(LANG_VERILOG, 0, "always @(posedge clk) begin if (r == 4'sd3) q <= 8'hFF; else q <= 'b1x0z; end");
	T("always", TK_CONTROL);
	T("posedge", TK_CONTROL);
	T("begin", TK_CONTROL);
	T("4'sd3", TK_NUMBER);
	T("8'hFF", TK_NUMBER);
	T("'b1x0z", TK_NUMBER);
	T("<=", TK_OPERATOR);
	T("end", TK_CONTROL);
	lex(LANG_VERILOG, 0, "`timescale 1ns/1ps `define MAX(a,b) $display(\"%0d\\n\", r); #10");
	T("`timescale", TK_PREPROC);
	T("1ns", TK_NUMBER);
	T("`define", TK_PREPROC);
	T("$display", TK_FUNCTION);
	T("\\n", TK_ESCAPE);
	lex(LANG_VERILOG, 0, "fifo #(.DEPTH(4)) u_fifo (.clk(clk), .din(r));");
	T("fifo", TK_TYPE);
	T("DEPTH", TK_VARIABLE);
	T("u_fifo", TK_TEXT);
	TN("clk", 0, TK_VARIABLE);
	TN("clk", 1, TK_TEXT);
	lex(LANG_VERILOG, 0, "counter u0 (clk, q); assign y = f(a);");
	T("counter", TK_TYPE);
	T("u0", TK_TEXT);
	T("assign", TK_STORAGE);
	T("f", TK_FUNCTION);
	lex(LANG_VERILOG, 0, "function [7:0] add; endfunction // c");
	T("add", TK_FUNCTION);
	T("// c", TK_COMMENT);
	/* SystemVerilog words are not Verilog's */
	lex(LANG_VERILOG, 0, "logic x;");
	T("logic", TK_TEXT);
	lex(LANG_SYSTEMVERILOG, 0, "logic [7:0] x = '1; always_ff @(posedge clk) x <= pkg::f(x);");
	T("logic", TK_STORAGE);
	T("'1", TK_NUMBER);
	T("always_ff", TK_CONTROL);
	T("pkg", TK_TYPE);
	lex(LANG_SYSTEMVERILOG, 0, "class Pkt extends Base; function new(); super.new(); this.len = null; endfunction");
	T("class", TK_STORAGE);
	T("Pkt", TK_TYPE);
	T("Base", TK_TYPE);
	T("super", TK_LANGCONST);
	TN("new", 1, TK_FUNCTION);
	T("null", TK_LANGCONST);
	lex(LANG_SYSTEMVERILOG, 0, "function automatic int unsigned sum(input int a); return a; endfunction");
	T("sum", TK_FUNCTION);
	T("return", TK_CONTROL);
	s = lex(LANG_SYSTEMVERILOG, 0, "wire a; /* open");
	CHECK(s == 1);
	s = lex(LANG_SYSTEMVERILOG, s, "close */ wire b;");
	T("close */", TK_COMMENT);
	T("wire", TK_STORAGE);
	CHECK(s == 0);
}

static void test_tcl(void)
{
	u32 s;
	lex(LANG_TCL, 0, "proc greet {name {greeting \"Hi\"}} {");
	T("proc", TK_STORAGE);
	T("greet", TK_FUNCTION);
	T("name", TK_TEXT);
	T("\"Hi\"", TK_STRING);
	lex(LANG_TCL, 0, "    set msg \"$greeting, ${name}! [string toupper $name]\\n\"");
	T("set", TK_STORAGE);
	T("msg", TK_VARIABLE);
	T("$greeting", TK_VARIABLE);
	T("${name}", TK_VARIABLE);
	T("string", TK_FUNCTION);
	T("toupper", TK_TEXT);
	T("\\n", TK_ESCAPE);
	lex(LANG_TCL, 0, "if {$n > 0} { puts -nonewline stderr $n } elseif {$n == 0} { return 1.5e3 }");
	T("if", TK_CONTROL);
	T("$n", TK_VARIABLE);
	T("puts", TK_FUNCTION);
	T("-nonewline", TK_LANGCONST);
	T("stderr", TK_TEXT);
	T("elseif", TK_CONTROL);
	T("return", TK_CONTROL);
	T("1.5e3", TK_NUMBER);
	lex(LANG_TCL, 0, "global a b; incr ::ns::count 2; set l {x y}");
	T("global", TK_STORAGE);
	TN("a", 1, TK_VARIABLE);
	TN("b", 1, TK_VARIABLE);
	T("incr", TK_STORAGE);
	T("2", TK_NUMBER);
	T("x", TK_TEXT);
	lex(LANG_TCL, 0, "create_clock -period 10.0 [get_ports clk] ;# trailing");
	T("create_clock", TK_FUNCTION);
	T("get_ports", TK_FUNCTION);
	T("# trailing", TK_COMMENT);
	lex(LANG_TCL, 0, "puts \"a # not a comment\"");
	T("# not", TK_STRING);
	/* comments continue with a trailing backslash; strings span lines */
	s = lex(LANG_TCL, 0, "# a comment \\");
	CHECK(s == 2);
	s = lex(LANG_TCL, s, "  continued");
	T("continued", TK_COMMENT);
	CHECK(s == 0);
	s = lex(LANG_TCL, 0, "set s \"multi");
	CHECK(s == 1);
	s = lex(LANG_TCL, s, "line $x\" ; puts ok");
	T("line", TK_STRING);
	T("$x", TK_VARIABLE);
	T("puts", TK_FUNCTION);
	CHECK(s == 0);
	/* a continued command: no command word at the next line's start */
	s = lex(LANG_TCL, 0, "foo a \\");
	CHECK(s == 4);
	lex(LANG_TCL, s, "  b c");
	T("b", TK_TEXT);
}

static void test_pascal(void)
{
	u32 s;
	lex(LANG_PASCAL, 0, "program Squares(output); var i: integer;");
	T("program", TK_STORAGE);
	T("Squares", TK_TYPE);
	T("var", TK_STORAGE);
	T("integer", TK_TYPE);
	lex(LANG_PASCAL, 0, "BEGIN for i := 1 to 10 do WriteLn(i * i div 2) { c } END.");
	T("BEGIN", TK_CONTROL);
	T("for", TK_CONTROL);
	T(":=", TK_OPERATOR);
	T("WriteLn", TK_FUNCTION);
	T("div", TK_STORAGE);
	T("{ c }", TK_COMMENT);
	T("END", TK_CONTROL);
	lex(LANG_PASCAL, 0, "s := 'it''s' + #13#10 + #$0A; n := $FF + %101 + &17 + 1.5E3; p := nil");
	T("'it''s'", TK_STRING);
	T("#13#10", TK_ESCAPE);
	T("#$0A", TK_ESCAPE);
	T("$FF", TK_NUMBER);
	T("%101", TK_NUMBER);
	T("&17", TK_NUMBER);
	T("1.5E3", TK_NUMBER);
	T("nil", TK_LANGCONST);
	/* Object Pascal words are not ISO Pascal's */
	lex(LANG_PASCAL, 0, "try");
	T("try", TK_TEXT);
	lex(LANG_OBJPASCAL, 0, "{$mode objfpc}{$H+} TShape = class(TObject) TColor = (Red, Green); PInt = ^Integer;");
	T("{$mode objfpc}", TK_PREPROC);
	T("{$H+}", TK_PREPROC);
	T("TShape", TK_TYPE);
	T("class", TK_STORAGE);
	T("TObject", TK_TYPE);
	T("TColor", TK_TYPE);
	T("Red", TK_TEXT);
	T("PInt", TK_TYPE);
	lex(LANG_OBJPASCAL, 0, "constructor TShape.Create(const AName: string); override; // c");
	T("constructor", TK_STORAGE);
	T("TShape", TK_TYPE);
	T("Create", TK_FUNCTION);
	T("const", TK_STORAGE);
	T("string", TK_TYPE);
	T("override", TK_STORAGE);
	T("// c", TK_COMMENT);
	lex(LANG_OBJPASCAL, 0, "try raise Exception.Create('x') except on E: Exception do Self.Free; end;");
	T("try", TK_CONTROL);
	T("raise", TK_CONTROL);
	T("Exception", TK_TYPE);
	T("except", TK_CONTROL);
	T("Self", TK_LANGCONST);
	T("Free", TK_TEXT);
	lex(LANG_OBJPASCAL, 0, "if a = b then Read(x)");
	T("a", TK_TEXT);
	T("Read", TK_FUNCTION);
	s = lex(LANG_OBJPASCAL, 0, "x := 1; (* open");
	CHECK(s == 2);
	s = lex(LANG_OBJPASCAL, s, "still *) y");
	T("still *)", TK_COMMENT);
	CHECK(s == 0);
	s = lex(LANG_OBJPASCAL, 0, "{$IFDEF FPC");
	CHECK(s == 3);
	s = lex(LANG_OBJPASCAL, s, "  }");
	T("}", TK_PREPROC);
	CHECK(s == 0);
}

static void test_r(void)
{
	u32 s;
	lex(LANG_R, 0, "f <- function(x, na.rm = TRUE) {");
	T("f", TK_FUNCTION);
	T("<-", TK_OPERATOR);
	T("function", TK_STORAGE);
	T("na.rm", TK_VARIABLE);
	T("TRUE", TK_LANGCONST);
	lex(LANG_R, 0, "  if (is.null(x)) return(NA_real_) else y <- x %>% dplyr::filter(v > 1L) |> mean()");
	T("if", TK_CONTROL);
	T("is.null", TK_FUNCTION);
	T("return", TK_CONTROL);
	T("NA_real_", TK_LANGCONST);
	T("%>%", TK_OPERATOR);
	T("dplyr", TK_TYPE);
	T("filter", TK_FUNCTION);
	T("1L", TK_NUMBER);
	T("|>", TK_OPERATOR);
	lex(LANG_R, 0, "g = \\(z) z^2; x$name; 0x1F; 1e-3; 2i; .5; `odd name` <- Inf # c");
	T("g", TK_FUNCTION);
	T("\\", TK_STORAGE);
	T("name", TK_TEXT);
	T("0x1F", TK_NUMBER);
	T("1e-3", TK_NUMBER);
	T("2i", TK_NUMBER);
	T(".5", TK_NUMBER);
	T("odd", TK_TEXT);
	T("Inf", TK_LANGCONST);
	T("# c", TK_COMMENT);
	lex(LANG_R, 0, "#' @param x a vector");
	T("#'", TK_COMMENT);
	T("@param", TK_STORAGE);
	T("x a vector", TK_COMMENT);
	lex(LANG_R, 0, "s <- 'a\\tb'; r <- r\"(raw \\d)\"");
	T("'a", TK_STRING);
	T("\\t", TK_ESCAPE);
	T("r\"(raw \\d)\"", TK_STRING);
	s = lex(LANG_R, 0, "s <- \"multi");
	CHECK(s == 1);
	s = lex(LANG_R, s, "line\" ; for (i in 1:3) next");
	T("line\"", TK_STRING);
	T("for", TK_CONTROL);
	TN("in", 1, TK_CONTROL);
	T("next", TK_CONTROL);
	CHECK(s == 0);
	s = lex(LANG_R, 0, "x <- R'--[a ]\" ]-");
	CHECK((s & 3u) == 3);
	s = lex(LANG_R, s, "b]--' ; TRUE");
	T("b]--'", TK_STRING);
	T("TRUE", TK_LANGCONST);
	CHECK(s == 0);
}

static void test_ruby(void)
{
	u32 s;
	lex(LANG_RUBY, 0, "class Circle < Shapes::Base # c");
	T("class", TK_CONTROL);
	T("Circle", TK_TYPE);
	T("Shapes", TK_TYPE);
	T("Base", TK_TYPE);
	T("# c", TK_COMMENT);
	lex(LANG_RUBY, 0, "  attr_reader :radius; PI2 = 6.28; require 'json'");
	T("attr_reader", TK_FUNCTION);
	T(":radius", TK_LANGCONST);
	T("PI2", TK_CONSTANT);
	T("6.28", TK_NUMBER);
	T("'json'", TK_STRING);
	lex(LANG_RUBY, 0, "  def initialize(radius:, name: \"c\") = @radius = @@n = $stderr");
	T("def", TK_CONTROL);
	T("initialize", TK_FUNCTION);
	T("radius:", TK_LANGCONST);
	T("name:", TK_LANGCONST);
	T("@radius", TK_VARIABLE);
	T("@@n", TK_VARIABLE);
	T("$stderr", TK_VARIABLE);
	lex(LANG_RUBY, 0, "def self.unit = new; def ==(o) = o.is_a?(C); def name=(v); end");
	T("self", TK_LANGCONST);
	T("unit", TK_FUNCTION);
	T("==", TK_FUNCTION);
	T("is_a?", TK_FUNCTION);
	T("name=", TK_FUNCTION);
	T("end", TK_CONTROL);
	lex(LANG_RUBY, 0, "puts \"made #{name.upcase} #@r\\n\" unless x and not y");
	T("puts", TK_FUNCTION);
	T("\"made ", TK_STRING);
	T("#{", TK_STORAGE);
	T("#@r", TK_VARIABLE);
	T("\\n", TK_ESCAPE);
	T("unless", TK_CONTROL);
	T("and", TK_OPERATOR);
	T("not", TK_OPERATOR);
	lex(LANG_RUBY, 0, "x = %w[a b] + %i(c); re = /ab+c/i; q = a / b; c = ?a; h = { key: :sym, 1_000 => nil }");
	T("%w[a b]", TK_STRING);
	T("%i(c)", TK_LANGCONST);
	T("/ab+c/i", TK_REGEX);
	TN("/", 2, TK_OPERATOR); /* a / b */
	T("?a", TK_STRING);
	T("key:", TK_LANGCONST);
	T(":sym", TK_LANGCONST);
	T("1_000", TK_NUMBER);
	T("nil", TK_LANGCONST);
	lex(LANG_RUBY, 0, "puts /x/ if a.empty? ? 1 : 2");
	T("/x/", TK_REGEX);
	/* heredocs */
	s = lex(LANG_RUBY, 0, "text = <<~EOS.strip");
	T("<<~EOS", TK_STRING);
	CHECK((s & 7u) == 2);
	u32 hd = s;
	s = lex(LANG_RUBY, s, "  Hello #{x.first}");
	T("  Hello ", TK_STRING);
	T("#{", TK_STORAGE);
	CHECK(s == hd);
	s = lex(LANG_RUBY, s, "  EOS");
	T("EOS", TK_STRING);
	CHECK(s == 0);
	s = lex(LANG_RUBY, 0, "q = <<-'RAW'");
	s = lex(LANG_RUBY, s, "no #{interp}");
	T("no #{interp}", TK_STRING);
	s = lex(LANG_RUBY, s, "RAW");
	CHECK(s == 0);
	/* strings, %() and =begin span lines */
	s = lex(LANG_RUBY, 0, "long = \"spans");
	CHECK((s & 7u) == 3);
	s = lex(LANG_RUBY, s, "lines #{h[:k]}\" if ok");
	T("lines ", TK_STRING);
	T(":k", TK_LANGCONST);
	T("if", TK_CONTROL);
	CHECK(s == 0);
	s = lex(LANG_RUBY, 0, "w = %w(a (b");
	s = lex(LANG_RUBY, s, "c) d)");
	T("c) d)", TK_STRING);
	CHECK(s == 0);
	s = lex(LANG_RUBY, 0, "=begin");
	CHECK(s == 1);
	s = lex(LANG_RUBY, s, "doc");
	T("doc", TK_COMMENT);
	s = lex(LANG_RUBY, s, "=end");
	CHECK(s == 0);
	s = lex(LANG_RUBY, 0, "__END__");
	s = lex(LANG_RUBY, s, "data # here");
	T("data", TK_TEXT);
	CHECK(s == 4);
}

static void test_algol68(void)
{
	u32 s;
	lex(LANG_ALGOL68, 0, "PROC fact = (INT n) INT: IF n <= 1 THEN 1 ELSE n * fact (n - 1) FI; # c #");
	T("PROC", TK_STORAGE);
	TN("fact", 0, TK_FUNCTION);
	TN("fact", 1, TK_FUNCTION);
	T("INT", TK_STORAGE);
	T("IF", TK_CONTROL);
	T("THEN", TK_CONTROL);
	T("FI", TK_CONTROL);
	T("1", TK_NUMBER);
	T("# c #", TK_COMMENT);
	lex(LANG_ALGOL68, 0, "MODE NODE = STRUCT (INT value, REF NODE next); OP +:= = (REF INT a, INT b) REF INT: a := a + b;");
	T("MODE", TK_STORAGE);
	T("NODE", TK_TYPE);
	T("STRUCT", TK_STORAGE);
	T("REF", TK_STORAGE);
	T("value", TK_TEXT);
	T("+:=", TK_FUNCTION);
	lex(LANG_ALGOL68, 0, "PRIO MAX = 6; INT x := 16r1f; REAL r = 1.5e3; BOOL b = TRUE; FOR i TO UPB a DO SKIP OD");
	T("MAX", TK_FUNCTION);
	T("16r1f", TK_NUMBER);
	T("1.5e3", TK_NUMBER);
	T("TRUE", TK_LANGCONST);
	T("UPB", TK_STORAGE);
	T("SKIP", TK_LANGCONST);
	T("OD", TK_CONTROL);
	lex(LANG_ALGOL68, 0, "print ((\"it \"\"q\"\"\", new line)); 'begin' 'int' z := 1 'end'");
	T("print", TK_FUNCTION);
	T("\"it \"\"q\"\"\"", TK_STRING);
	T("'begin'", TK_CONTROL);
	T("'int'", TK_STORAGE);
	T("'end'", TK_CONTROL);
	lex(LANG_ALGOL68, 0, "PR echo PR x \xC2\xA2 cent \xC2\xA2 y");
	T("PR echo PR", TK_PREPROC);
	T("\xC2\xA2 cent \xC2\xA2", TK_COMMENT);
	/* comments span lines; CO closes CO, not COMMENT */
	s = lex(LANG_ALGOL68, 0, "x := 1; COMMENT a CO b");
	CHECK(s == 3);
	s = lex(LANG_ALGOL68, s, "c COMMENT BEGIN");
	T("c COMMENT", TK_COMMENT);
	T("BEGIN", TK_CONTROL);
	CHECK(s == 0);
	s = lex(LANG_ALGOL68, 0, "'co' quoted");
	CHECK(s == (2u | 8u));
	s = lex(LANG_ALGOL68, s, "CO still 'co' END");
	T("CO still 'co'", TK_COMMENT);
	T("END", TK_CONTROL);
	CHECK(s == 0);
	s = lex(LANG_ALGOL68, 0, "# open");
	CHECK(s == 1);
}

static void test_detect8(void)
{
	CHECK(LANG_SAIL == 33 && LANG_VERILOG == 34 && LANG_SYSTEMVERILOG == 35 && LANG_TCL == 36 && LANG_PASCAL == 37 &&
	      LANG_OBJPASCAL == 38 && LANG_R == 39 && LANG_RUBY == 40 && LANG_ALGOL68 == 41);
	CHECK(syn_lang_for_path("src/nx/gpu/shader/sail/sm53.sail", NULL, 0) == LANG_SAIL);
	CHECK(syn_lang_for_path("rtl/top.v", NULL, 0) == LANG_VERILOG);
	CHECK(syn_lang_for_path("defs.vh", NULL, 0) == LANG_VERILOG);
	CHECK(syn_lang_for_path("tb.sv", NULL, 0) == LANG_SYSTEMVERILOG);
	CHECK(syn_lang_for_path("pkg.SVH", NULL, 0) == LANG_SYSTEMVERILOG);
	static const char *const tcl[] = {"a.tcl", "w.tk", "pkgIndex.tcl", "x.tm", "top.sdc", "pins.xdc", "run.exp"};
	for (size_t i = 0; i < VX_COUNT(tcl); i++)
		CHECK(syn_lang_for_path(tcl[i], NULL, 0) == LANG_TCL);
	static const char tclsh[] = "#!/usr/bin/env tclsh8.6\nputs hi\n", wish[] = "#!/usr/local/bin/wish\n";
	CHECK(syn_lang_for_path("tool", tclsh, sizeof tclsh - 1) == LANG_TCL);
	CHECK(syn_lang_for_path("gui", wish, sizeof wish - 1) == LANG_TCL);
	static const char iso[] = "program p(output);\nbegin writeln('x') end.\n";
	static const char fpc[] = "{$mode objfpc}\nunit u;\ninterface\n";
	static const char dlp[] = "unit Shapes;\ninterface\ntype TShape = class\n";
	CHECK(syn_lang_for_path("p.pas", iso, sizeof iso - 1) == LANG_PASCAL);
	CHECK(syn_lang_for_path("u.pas", fpc, sizeof fpc - 1) == LANG_OBJPASCAL);
	CHECK(syn_lang_for_path("u.pp", dlp, sizeof dlp - 1) == LANG_OBJPASCAL);
	CHECK(syn_lang_for_path("p.p", NULL, 0) == LANG_PASCAL);
	CHECK(syn_lang_for_path("Project.dpr", NULL, 0) == LANG_OBJPASCAL);
	CHECK(syn_lang_for_path("app.lpr", NULL, 0) == LANG_OBJPASCAL);
	CHECK(syn_lang_for_path("analysis.R", NULL, 0) == LANG_R);
	CHECK(syn_lang_for_path("x.r", NULL, 0) == LANG_R);
	CHECK(syn_lang_for_path("/home/u/.Rprofile", NULL, 0) == LANG_R);
	static const char rs[] = "#!/usr/bin/env Rscript\n";
	CHECK(syn_lang_for_path("plot", rs, sizeof rs - 1) == LANG_R);
	static const char *const rb[] = {"a.rb", "Rakefile", "Gemfile", "x.gemspec", "config.ru", "t.rake", "Vagrantfile", ".irbrc"};
	for (size_t i = 0; i < VX_COUNT(rb); i++)
		CHECK(syn_lang_for_path(rb[i], NULL, 0) == LANG_RUBY);
	static const char rby[] = "#!/usr/bin/ruby3.3 -w\n";
	CHECK(syn_lang_for_path("bin/tool", rby, sizeof rby - 1) == LANG_RUBY);
	CHECK(syn_lang_for_path("Gemfile.lock", NULL, 0) != LANG_RUBY);
	CHECK(syn_lang_for_path("hello.a68", NULL, 0) == LANG_ALGOL68);
	static const char a68[] = "#!/usr/bin/a68g --script\n";
	CHECK(syn_lang_for_path("hello", a68, sizeof a68 - 1) == LANG_ALGOL68);
	CHECK(syn_lang_for_path("x.inc", NULL, 0) == LANG_ASM_INTEL); /* not Pascal */

	CHECK_STR(syn_lang_name(LANG_SAIL), "Sail");
	CHECK_STR(syn_lang_name(LANG_VERILOG), "Verilog");
	CHECK_STR(syn_lang_name(LANG_SYSTEMVERILOG), "SystemVerilog");
	CHECK_STR(syn_lang_name(LANG_TCL), "Tcl");
	CHECK_STR(syn_lang_name(LANG_PASCAL), "Pascal");
	CHECK_STR(syn_lang_name(LANG_OBJPASCAL), "Object Pascal");
	CHECK_STR(syn_lang_name(LANG_R), "R");
	CHECK_STR(syn_lang_name(LANG_RUBY), "Ruby");
	CHECK_STR(syn_lang_name(LANG_ALGOL68), "ALGOL 68");
	CHECK_STR(syn_lang_lsp_id(LANG_SYSTEMVERILOG), "systemverilog");
	CHECK_STR(syn_lang_lsp_id(LANG_RUBY), "ruby");
	CHECK_STR(syn_lang_lsp_id(LANG_R), "r");
	CHECK_STR(syn_line_comment(LANG_SAIL), "//");
	CHECK_STR(syn_line_comment(LANG_VERILOG), "//");
	CHECK_STR(syn_line_comment(LANG_OBJPASCAL), "//");
	CHECK_STR(syn_line_comment(LANG_TCL), "#");
	CHECK_STR(syn_line_comment(LANG_R), "#");
	CHECK_STR(syn_line_comment(LANG_RUBY), "#");
	CHECK(syn_line_comment(LANG_ALGOL68) == NULL); /* # comments need a closing # */
	/* not fence languages: the Markdown fence keeps 5 bits */
	CHECK(syn_fence_lang("ruby", 4) == LANG_NONE);
}

/* Random lines over the lexers added with Sail: spans in order and in
 * bounds, states within SYN_STATE_BITS. */
static void test_fuzz_more(void)
{
	static const char alpha[] = "\"'`$@{}[]()<>/\\*#;:=!?&%^.,+-_ \tabcxyzABC019"
	                            "\"\"\"'''(*{$*)}`#{}<<~EOS=begin=endCOCOMMENTPR\xC2\xA2r\"(-";
	static const int langs[] = {LANG_SAIL, LANG_VERILOG, LANG_SYSTEMVERILOG, LANG_TCL, LANG_PASCAL,
	                            LANG_OBJPASCAL, LANG_R, LANG_RUBY, LANG_ALGOL68};
	u32 rng = 777;
	char line[200];
	int bad = 0;
	for (size_t l = 0; l < VX_COUNT(langs); l++) {
		u32 s = 0;
		for (int it = 0; it < 6000; it++) {
			rng = rng * 1103515245u + 12345u;
			u32 len = (rng >> 16) % (sizeof line - 1);
			for (u32 k = 0; k < len; k++) {
				rng = rng * 1103515245u + 12345u;
				line[k] = alpha[(rng >> 16) % (sizeof alpha - 1)];
			}
			line[len] = 0;
			s = lex(langs[l], s, line);
			if (s >= (1u << SYN_STATE_BITS))
				bad++;
		}
	}
	CHECK(bad == 0);
}

static void test_haskell(void)
{
	u32 s;
	lex(LANG_HASKELL, 0, "{-# LANGUAGE GADTs #-} module Main (main) where");
	T("{-# LANGUAGE GADTs #-}", TK_PREPROC);
	T("module", TK_STORAGE);
	T("Main", TK_TYPE);
	T("where", TK_CONTROL);
	lex(LANG_HASKELL, 0, "import qualified Data.Map as M");
	T("import", TK_STORAGE);
	T("qualified", TK_STORAGE);
	T("Data.Map", TK_TYPE);
	T("as", TK_STORAGE);
	lex(LANG_HASKELL, 0, "f as = as -- qualified as");
	T("f", TK_FUNCTION);
	TN("as", 0, TK_TEXT); /* not an import line */
	T("-- qualified as", TK_COMMENT);
	lex(LANG_HASKELL, 0, "data Tree a = Leaf | Node (Tree a) a deriving (Show, Eq)");
	T("data", TK_STORAGE);
	T("Tree", TK_TYPE);
	T("Leaf", TK_TYPE);
	T("|", TK_OPERATOR);
	T("deriving", TK_STORAGE);
	lex(LANG_HASKELL, 0, "insert :: Ord a => a -> Tree a -> Tree a");
	T("insert", TK_FUNCTION);
	T("::", TK_OPERATOR);
	T("=>", TK_OPERATOR);
	lex(LANG_HASKELL, 0, "insert x Leaf = Node Leaf x Leaf");
	T("insert", TK_FUNCTION);
	lex(LANG_HASKELL, 0, "  let m = M.fromList [(1 :: Int, \"one\\n\"), (0x1F, 'c'), (2.5e-3, '\\n')]");
	T("let", TK_CONTROL);
	T("M", TK_TYPE);
	T("fromList", TK_TEXT);
	T("\"one", TK_STRING);
	T("\\n", TK_ESCAPE);
	T("0x1F", TK_NUMBER);
	T("'c'", TK_STRING);
	T("2.5e-3", TK_NUMBER);
	lex(LANG_HASKELL, 0, "  x' = a `elem` [True, False] --> y");
	T("x'", TK_FUNCTION);
	T("`elem`", TK_OPERATOR);
	T("True", TK_LANGCONST);
	T("-->", TK_OPERATOR);
	lex(LANG_HASKELL, 0, "  case xs of { [] -> if a then b else c }");
	T("case", TK_CONTROL);
	T("of", TK_CONTROL);
	T("if", TK_CONTROL);
	T("->", TK_OPERATOR);
	/* nested comments span lines */
	s = lex(LANG_HASKELL, 0, "x {- a {- b -}");
	CHECK(s == 1);
	s = lex(LANG_HASKELL, s, "still -} y");
	T("still -}", TK_COMMENT);
	T("y", TK_TEXT);
	CHECK(s == 0);
	s = lex(LANG_HASKELL, 0, "{-# INLINE");
	CHECK(s == 0x101);
	s = lex(LANG_HASKELL, s, "  f #-}");
	T("f #-}", TK_PREPROC);
	CHECK(s == 0);
	/* a string gap */
	s = lex(LANG_HASKELL, 0, "s = \"abc\\");
	CHECK(s == 0x200);
	s = lex(LANG_HASKELL, s, "    \\def\" ++ t");
	T("def\"", TK_STRING);
	T("++", TK_OPERATOR);
	CHECK(s == 0);
}

static void test_batch(void)
{
	lex(LANG_BATCH, 0, "@echo off & rem not a comment here");
	T("@", TK_OPERATOR);
	T("echo", TK_STORAGE);
	T("off", TK_TEXT);
	T("&", TK_OPERATOR);
	lex(LANG_BATCH, 0, "REM build script");
	T("REM build script", TK_COMMENT);
	lex(LANG_BATCH, 0, "  :: also a comment");
	T(":: also a comment", TK_COMMENT);
	lex(LANG_BATCH, 0, ":fail");
	T(":fail", TK_FUNCTION);
	lex(LANG_BATCH, 0, "set /a count=0");
	T("set", TK_STORAGE);
	T("/a", TK_LANGCONST);
	T("count", TK_VARIABLE);
	T("0", TK_NUMBER);
	lex(LANG_BATCH, 0, "if not exist \"%OUT%\\x\" mkdir \"%~dp0build\"");
	T("if", TK_CONTROL);
	T("not", TK_CONTROL);
	T("exist", TK_CONTROL);
	T("\"", TK_STRING);
	T("%OUT%", TK_VARIABLE);
	T("mkdir", TK_STORAGE);
	T("%~dp0", TK_VARIABLE);
	lex(LANG_BATCH, 0, "for %%f in (*.c) do cl /c %%~nxf !OBJ! >nul 2>&1 || goto :eof");
	T("for", TK_CONTROL);
	T("%%f", TK_VARIABLE);
	T("in", TK_CONTROL);
	T("do", TK_CONTROL);
	T("/c", TK_LANGCONST);
	T("%%~nxf", TK_VARIABLE);
	T("!OBJ!", TK_VARIABLE);
	T("2>&1", TK_OPERATOR);
	T("||", TK_OPERATOR);
	T("goto", TK_CONTROL);
	T(":eof", TK_FUNCTION);
	lex(LANG_BATCH, 0, "IF %1 GEQ 10 ECHO many if set ^& more %2");
	T("IF", TK_CONTROL);
	T("%1", TK_VARIABLE);
	T("GEQ", TK_OPERATOR);
	T("ECHO", TK_STORAGE);
	TN("if", 0, TK_TEXT); /* echo's text */
	T("^&", TK_ESCAPE);
	T("%2", TK_VARIABLE);
	lex(LANG_BATCH, 0, "if \"%x%\"==\"y\" (call :sub %*) else exit /b 1");
	T("==", TK_OPERATOR);
	T("call", TK_CONTROL);
	T(":sub", TK_FUNCTION);
	T("%*", TK_VARIABLE);
	T("else", TK_CONTROL);
	CHECK(lex(LANG_BATCH, 0, "echo 100%") == 0);
	T("100%", TK_TEXT);
}

static void test_powershell(void)
{
	u32 s;
	lex(LANG_POWERSHELL, 0, "function Get-Thing([string]$Name) { # c");
	T("function", TK_STORAGE);
	T("Get-Thing", TK_FUNCTION);
	T("string", TK_STORAGE);
	T("$Name", TK_VARIABLE);
	T("# c", TK_COMMENT);
	lex(LANG_POWERSHELL, 0, "$items = Get-ChildItem -Path $env:TEMP | Where-Object { $_.Length -gt 1mb -and $true }");
	T("$items", TK_VARIABLE);
	T("Get-ChildItem", TK_FUNCTION);
	T("-Path", TK_LANGCONST);
	T("$env:", TK_STORAGE);
	T("TEMP", TK_VARIABLE);
	T("Where-Object", TK_FUNCTION);
	T("$_", TK_VARIABLE);
	T("Length", TK_TEXT);
	T("-gt", TK_OPERATOR);
	T("1mb", TK_NUMBER);
	T("-and", TK_OPERATOR);
	T("$true", TK_LANGCONST);
	lex(LANG_POWERSHELL, 0, "foreach ($i in $a) { Write-Host \"N: $($i.Name) `n\" -f 1 } ; git status");
	T("foreach", TK_CONTROL);
	T("in", TK_CONTROL);
	T("\"N: ", TK_STRING);
	T("$(", TK_STORAGE);
	T("$i", TK_VARIABLE);
	T("`n", TK_ESCAPE);
	T("-f", TK_OPERATOR);
	T("git", TK_FUNCTION);
	T("status", TK_TEXT);
	lex(LANG_POWERSHELL, 0, "[CmdletBinding()] param([Parameter(Mandatory)][int[]]$S = @(1, 0x1F))");
	T("CmdletBinding", TK_FUNCTION);
	T("param", TK_CONTROL);
	T("Parameter", TK_FUNCTION);
	T("Mandatory", TK_VARIABLE);
	T("int", TK_STORAGE);
	T("0x1F", TK_NUMBER);
	lex(LANG_POWERSHELL, 0, "$h = @{ Key = 'it''s' }; [System.IO.Path]::Combine($a[0], \"b\"); & $cmd @splat");
	T("Key", TK_VARIABLE);
	T("'it''s'", TK_STRING);
	T("System.IO.Path", TK_STORAGE);
	T("Combine", TK_FUNCTION);
	TN("0", 0, TK_NUMBER);
	T("@splat", TK_VARIABLE);
	lex(LANG_POWERSHELL, 0, "class Shape { [int]$Size; static [void] Draw() { $this.Size = $null } }");
	T("class", TK_STORAGE);
	T("Shape", TK_TYPE);
	T("static", TK_STORAGE);
	T("void", TK_STORAGE);
	T("$this", TK_LANGCONST);
	T("$null", TK_LANGCONST);
	/* block comments with help keywords, here-strings and strings span lines */
	s = lex(LANG_POWERSHELL, 0, "x <#");
	CHECK(s == 1);
	s = lex(LANG_POWERSHELL, s, ".SYNOPSIS");
	T(".SYNOPSIS", TK_STORAGE);
	s = lex(LANG_POWERSHELL, s, "  done #> Get-Date");
	T("  done #>", TK_COMMENT);
	T("Get-Date", TK_FUNCTION);
	CHECK(s == 0);
	s = lex(LANG_POWERSHELL, 0, "$t = @\"");
	CHECK(s == 2);
	s = lex(LANG_POWERSHELL, s, "Hi $Name \"q\" $(Get-Date)");
	T("Hi ", TK_STRING);
	T("$Name", TK_VARIABLE);
	T("\"q\"", TK_STRING);
	T("Get-Date", TK_FUNCTION);
	s = lex(LANG_POWERSHELL, s, "\"@ | Out-File x");
	T("\"@", TK_STRING);
	T("Out-File", TK_FUNCTION);
	CHECK(s == 0);
	s = lex(LANG_POWERSHELL, 0, "$r = @'");
	CHECK(s == 3);
	s = lex(LANG_POWERSHELL, s, "no $expansion");
	T("no $expansion", TK_STRING);
	s = lex(LANG_POWERSHELL, s, "'@");
	CHECK(s == 0);
	s = lex(LANG_POWERSHELL, 0, "$m = \"multi");
	CHECK(s == 4);
	s = lex(LANG_POWERSHELL, s, "line\"; return");
	T("return", TK_CONTROL);
	CHECK(s == 0);
}

static void test_detect9(void)
{
	CHECK(LANG_HASKELL == 42 && LANG_BATCH == 43 && LANG_POWERSHELL == 44 && LANG_COUNT == 45);
	CHECK(syn_lang_for_path("src/Main.hs", NULL, 0) == LANG_HASKELL);
	CHECK(syn_lang_for_path("A.hs-boot", NULL, 0) == LANG_HASKELL);
	static const char ghc[] = "#!/usr/bin/env runghc\nmain = pure ()\n";
	CHECK(syn_lang_for_path("script", ghc, sizeof ghc - 1) == LANG_HASKELL);
	CHECK(syn_lang_for_path("build.bat", NULL, 0) == LANG_BATCH);
	CHECK(syn_lang_for_path("RUN.CMD", NULL, 0) == LANG_BATCH);
	CHECK(syn_lang_for_path("x.command", NULL, 0) == LANG_SH);
	static const char *const ps[] = {"a.ps1", "m.psm1", "d.psd1"};
	for (size_t i = 0; i < VX_COUNT(ps); i++)
		CHECK(syn_lang_for_path(ps[i], NULL, 0) == LANG_POWERSHELL);
	static const char pwsh[] = "#!/usr/bin/env pwsh\n";
	CHECK(syn_lang_for_path("tool", pwsh, sizeof pwsh - 1) == LANG_POWERSHELL);
	CHECK_STR(syn_lang_name(LANG_HASKELL), "Haskell");
	CHECK_STR(syn_lang_name(LANG_BATCH), "Batch");
	CHECK_STR(syn_lang_name(LANG_POWERSHELL), "PowerShell");
	CHECK_STR(syn_lang_lsp_id(LANG_HASKELL), "haskell");
	CHECK_STR(syn_lang_lsp_id(LANG_BATCH), "bat");
	CHECK_STR(syn_lang_lsp_id(LANG_POWERSHELL), "powershell");
	CHECK_STR(syn_line_comment(LANG_HASKELL), "--");
	CHECK_STR(syn_line_comment(LANG_BATCH), "REM");
	CHECK_STR(syn_line_comment(LANG_POWERSHELL), "#");
}

static void test_fuzz_more2(void)
{
	static const char alpha[] = "\"'`$@{}[]()<>/\\*#;:=!?&%^.,+-_ \tabcxyzABC019"
	                            "{-#-}--<##>@\"\"@@''@%%~!REM::$(";
	static const int langs[] = {LANG_HASKELL, LANG_BATCH, LANG_POWERSHELL};
	u32 rng = 4242;
	char line[200];
	int bad = 0;
	for (size_t l = 0; l < VX_COUNT(langs); l++) {
		u32 s = 0;
		for (int it = 0; it < 6000; it++) {
			rng = rng * 1103515245u + 12345u;
			u32 len = (rng >> 16) % (sizeof line - 1);
			for (u32 k = 0; k < len; k++) {
				rng = rng * 1103515245u + 12345u;
				line[k] = alpha[(rng >> 16) % (sizeof alpha - 1)];
			}
			line[len] = 0;
			s = lex(langs[l], s, line);
			if (s >= (1u << SYN_STATE_BITS))
				bad++;
		}
	}
	CHECK(bad == 0);
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
	test_cmake();
	test_make();
	test_json();
	test_ignore();
	test_diff();
	test_git_msgs();
	test_detect3();
	test_python();
	test_yaml();
	test_codeowners();
	test_detect4();
	test_java();
	test_kotlin();
	test_toml();
	test_detect5();
	test_cpp();
	test_rust();
	test_detect6();
	test_csharp();
	test_qml();
	test_qmake();
	test_gdscript();
	test_godot_res();
	test_xml();
	test_detect7();
	test_shaders();
	test_fuzz_new();
	test_sail();
	test_verilog();
	test_tcl();
	test_pascal();
	test_r();
	test_ruby();
	test_algol68();
	test_detect8();
	test_fuzz_more();
	test_haskell();
	test_batch();
	test_powershell();
	test_detect9();
	test_fuzz_more2();
	bench();
	bench_more();
	bench3();
	return tap_done();
}
