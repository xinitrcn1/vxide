/* Binary files: detection, ELF/PE/Mach-O parsing, disassembly, inspector. */
#define _POSIX_C_SOURCE 200809L
#include "binfile.h"
#include "tap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[] = "/tmp/vxide-bin-XXXXXX";

static u8 *load(const char *path, size_t *n)
{
	char *b = vx_read_file(path, n);
	return (u8 *)b;
}

static u32 sec(const BinImage *img, const char *name)
{
	for (u32 i = 0; i < img->ns; i++)
		if (!strcmp(bin_str(img, img->s_name[i]), name))
			return i;
	return UINT32_MAX;
}

static u32 sym(const BinImage *img, const char *name)
{
	for (u32 i = 0; i < img->nsym; i++)
		if (!strcmp(bin_str(img, img->y_name[i]), name))
			return i;
	return UINT32_MAX;
}

static bool sh(const char *cmd)
{
	char b[2048];
	snprintf(b, sizeof b, "cd %s && %s >/dev/null 2>&1", dir, cmd);
	return system(b) == 0;
}

/* A call from main to helper must decode with a resolved target. */
static void check_call(const char *path, const char *fmt, const char *arch, bool want_call)
{
	size_t n;
	u8 *d = load(path, &n);
	CHECK(d != NULL);
	if (!d)
		return;
	BinImage img;
	CHECK(bin_parse(d, n, &img) == 0);
	CHECK_STR(img.format_name, fmt);
	CHECK_STR(img.arch_name, arch);
	u32 t = sec(&img, ".text");
	CHECK(t != UINT32_MAX && (img.s_flags[t] & BS_CODE));
	u32 ro = sec(&img, ".rodata");
	CHECK(ro != UINT32_MAX && (img.s_flags[ro] & BS_DATA) && (img.s_flags[ro] & BS_READONLY));
	u32 dt = sec(&img, ".data");
	CHECK(dt != UINT32_MAX && (img.s_flags[dt] & BS_WRITABLE));
	CHECK(sec(&img, ".bss") == UINT32_MAX); /* no file data */
	u32 hs = sym(&img, "helper"), ms = sym(&img, "main");
	CHECK(hs != UINT32_MAX && ms != UINT32_MAX && img.y_func[hs]);
	BinDisasm ds;
	CHECK(bin_disasm(&img, t, d, n, &ds) == 0);
	bool label = false, call = false;
	for (u32 i = 0; i < ds.n; i++) {
		const char *tx = ds.pool.b + ds.text[i];
		if (ds.kind[i] == BR_LABEL && !strcmp(tx, "main"))
			label = true;
		if (ds.kind[i] == BR_INSN && hs != UINT32_MAX && ds.target[i] == img.y_addr[hs] &&
		    (!strncmp(tx, "call", 4) || !strncmp(tx, "bl", 2)))
			call = true;
	}
	CHECK(label);
	if (want_call)
		CHECK(call);
	u64 delta;
	CHECK(bin_symbol_at(&img, img.y_addr[ms] + 1, &delta) == ms && delta == 1);
	bin_disasm_free(&ds);
	bin_free(&img);
	free(d);
}

static void put32(u8 *b, u32 off, u32 v)
{
	for (int i = 0; i < 4; i++)
		b[off + i] = (u8)(v >> (8 * i));
}
static void put16(u8 *b, u32 off, u16 v)
{
	b[off] = (u8)v;
	b[off + 1] = (u8)(v >> 8);
}
static void put64(u8 *b, u32 off, u64 v)
{
	put32(b, off, (u32)v);
	put32(b, off + 4, (u32)(v >> 32));
}

static void test_pe(void)
{
	static u8 b[0x400];
	memset(b, 0, sizeof b);
	b[0] = 'M';
	b[1] = 'Z';
	put32(b, 0x3c, 0x40);
	memcpy(b + 0x40, "PE\0\0", 4);
	u32 coff = 0x44;
	put16(b, coff, 0x8664);
	put16(b, coff + 2, 1);
	put16(b, coff + 16, 240);
	u32 opt = coff + 20;
	put16(b, opt, 0x20b);
	put32(b, opt + 16, 0x1000);
	put64(b, opt + 24, 0x140000000ull);
	put32(b, opt + 108, 16);
	u32 s = opt + 240;
	memcpy(b + s, ".text", 5);
	put32(b, s + 8, 5);
	put32(b, s + 12, 0x1000);
	put32(b, s + 16, 0x200);
	put32(b, s + 20, 0x200);
	put32(b, s + 36, 0x60000020);
	memcpy(b + 0x200, "\x55\x48\x89\xe5\xc3", 5);
	BinImage img;
	CHECK(bin_parse(b, sizeof b, &img) == 0);
	CHECK_STR(img.format_name, "PE32+");
	CHECK_STR(img.arch_name, "x86-64");
	CHECK(img.ns == 1 && img.s_addr[0] == 0x140001000ull && img.s_size[0] == 5 && (img.s_flags[0] & BS_CODE));
	CHECK(img.entry == 0x140001000ull);
	BinDisasm ds;
	CHECK(bin_disasm(&img, 0, b, sizeof b, &ds) == 0);
	u32 first = 0;
	while (first < ds.n && ds.kind[first] != BR_INSN)
		first++;
	CHECK(first < ds.n && !strcmp(ds.pool.b + ds.text[first], "push rbp"));
	CHECK(ds.n >= 3 && !strcmp(ds.pool.b + ds.text[ds.n - 1], "ret"));
	bin_disasm_free(&ds);
	bin_free(&img);
	/* "MZ" without a PE header is not an executable image */
	memcpy(b + 0x40, "XX", 2);
	CHECK(bin_parse(b, sizeof b, &img) == 1);
	bin_free(&img);
}

static void test_macho(void)
{
	static u8 b[0x200];
	memset(b, 0, sizeof b);
	put32(b, 0, 0xfeedfacf);
	put32(b, 4, 0x01000007);
	put32(b, 16, 2);          /* ncmds */
	put32(b, 20, 72 + 80 + 24); /* sizeofcmds */
	u32 lc = 32;
	put32(b, lc, 0x19);
	put32(b, lc + 4, 72 + 80);
	memcpy(b + lc + 8, "__TEXT", 6);
	put64(b, lc + 24, 0x100000000ull);
	put32(b, lc + 60, 5); /* initprot r-x */
	put32(b, lc + 64, 1); /* nsects */
	u32 sp = lc + 72;
	memcpy(b + sp, "__text", 6);
	memcpy(b + sp + 16, "__TEXT", 6);
	put64(b, sp + 32, 0x100000100ull);
	put64(b, sp + 40, 4);
	put32(b, sp + 48, 0x100);
	put32(b, sp + 64, 0x80000400);
	lc += 72 + 80;
	put32(b, lc, 2); /* LC_SYMTAB */
	put32(b, lc + 4, 24);
	put32(b, lc + 8, 0x180);  /* symoff */
	put32(b, lc + 12, 1);     /* nsyms */
	put32(b, lc + 16, 0x1a0); /* stroff */
	put32(b, lc + 20, 16);    /* strsize */
	memcpy(b + 0x100, "\x31\xc0\xc3\x90", 4);
	put32(b, 0x180, 1);  /* strx */
	b[0x184] = 0x0f;     /* N_SECT | N_EXT */
	b[0x185] = 1;        /* section 1 */
	put64(b, 0x188, 0x100000100ull);
	memcpy(b + 0x1a1, "_main", 6);
	BinImage img;
	CHECK(bin_parse(b, sizeof b, &img) == 0);
	CHECK_STR(img.format_name, "Mach-O 64");
	CHECK(img.ns == 1 && !strcmp(bin_str(&img, img.s_name[0]), "__TEXT,__text") && (img.s_flags[0] & BS_CODE));
	CHECK(img.nsym == 1 && !strcmp(bin_str(&img, img.y_name[0]), "_main") && img.y_func[0]);
	BinDisasm ds;
	CHECK(bin_disasm(&img, 0, b, sizeof b, &ds) == 0);
	CHECK(ds.n >= 2 && ds.kind[0] == BR_LABEL && !strcmp(ds.pool.b + ds.text[1], "xor eax, eax"));
	bin_disasm_free(&ds);
	bin_free(&img);
	/* a Java class file (same magic as fat Mach-O) is not an image */
	u8 java[16] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 52};
	CHECK(bin_parse(java, sizeof java, &img) == 1);
	bin_free(&img);
}

static u32 rnd(void)
{
	static u32 s = 0x9e3779b9;
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

/* Mutations and truncations of real binaries never crash (run under ASan). */
static void fuzz(const char *path, int rounds)
{
	size_t n;
	u8 *orig = load(path, &n);
	if (!orig)
		return;
	u8 *d = vx_alloc(n);
	for (int it = 0; it < rounds; it++) {
		memcpy(d, orig, n);
		size_t len = (it % 3 == 0) ? rnd() % n : n;
		int muts = 1 + (int)(rnd() % 16);
		for (int k = 0; k < muts; k++) {
			/* bias towards the headers where the structure lives */
			size_t at = (rnd() % 4) ? rnd() % VX_MIN(n, (size_t)4096) : rnd() % n;
			d[at] = (u8)rnd();
		}
		BinImage img;
		if (bin_parse(d, len, &img) >= 0)
			for (u32 s = 0; s < img.ns && s < 4; s++)
				if (img.s_flags[s] & BS_CODE) {
					BinDisasm ds;
					bin_disasm(&img, s, d, len, &ds);
					bin_disasm_free(&ds);
				}
		bin_free(&img);
	}
	/* random garbage behind each magic */
	static const char *const magics[] = {"\x7f" "ELF\2\1\1", "\x7f" "ELF\1\2\1", "MZ", "\xcf\xfa\xed\xfe",
	                                     "\xce\xfa\xed\xfe", "\xca\xfe\xba\xbe", "\xca\xfe\xba\xbf"};
	for (int it = 0; it < rounds; it++) {
		size_t len = 4 + rnd() % 4096;
		u8 *g = vx_alloc(len);
		for (size_t k = 0; k < len; k++)
			g[k] = (u8)rnd();
		const char *m = magics[it % VX_COUNT(magics)];
		memcpy(g, m, VX_MIN(strlen(m) ? strlen(m) : 4, len));
		if ((it % VX_COUNT(magics)) >= 3)
			memcpy(g, m, 4);
		BinImage img;
		bin_parse(g, len, &img);
		for (u32 s = 0; s < img.ns && s < 2; s++) {
			BinDisasm ds;
			bin_disasm(&img, s, g, len, &ds);
			bin_disasm_free(&ds);
		}
		bin_free(&img);
		free(g);
	}
	free(d);
	free(orig);
	CHECK(1);
}

static const char *field(const BinInspect *o, const char *name)
{
	for (u32 i = 0; i < o->n; i++)
		if (!strcmp(o->name[i], name))
			return o->value[i];
	return NULL;
}

static void test_inspect(void)
{
	BinInspect o;
	const u8 a[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	bin_inspect(a, 8, false, &o);
	CHECK_STR(field(&o, "Int16"), "513");
	CHECK_STR(field(&o, "UInt24"), "197121");
	CHECK_STR(field(&o, "UInt32"), "67305985");
	CHECK_STR(field(&o, "Binary (8 bit)"), "00000001");
	bin_inspect(a, 8, true, &o);
	CHECK_STR(field(&o, "Int16"), "258");
	CHECK_STR(field(&o, "UInt64"), "72623859790382856");
	bin_inspect(a, 1, false, &o);
	CHECK(field(&o, "Int16") == NULL && field(&o, "Int8") != NULL);
	const u8 leb[] = {0xE5, 0x8E, 0x26};
	bin_inspect(leb, 3, false, &o);
	CHECK_STR(field(&o, "LEB128"), "624485 (3 bytes)");
	const u8 sleb[] = {0xC0, 0xBB, 0x78};
	bin_inspect(sleb, 3, false, &o);
	CHECK_STR(field(&o, "SLEB128"), "-123456 (3 bytes)");
	const u8 half[] = {0x00, 0x3C};
	bin_inspect(half, 2, false, &o);
	CHECK_STR(field(&o, "Float16 (half)"), "1");
	const u8 f32[] = {0x00, 0x00, 0x80, 0x3f};
	bin_inspect(f32, 4, false, &o);
	CHECK_STR(field(&o, "Float32 (single)"), "1");
	const u8 f32be[] = {0x3f, 0x80, 0x00, 0x00};
	bin_inspect(f32be, 4, true, &o);
	CHECK_STR(field(&o, "Float32 (single)"), "1");
	const u8 z[16] = {0};
	bin_inspect(z, 16, false, &o);
	CHECK_STR(field(&o, "time_t (32 bit)"), "1970-01-01 00:00:00 UTC");
	CHECK_STR(field(&o, "GUID"), "{00000000-0000-0000-0000-000000000000}");
	const u8 e[] = {0xC3, 0xA9};
	bin_inspect(e, 2, false, &o);
	CHECK_STR(field(&o, "UTF-8 code point"), "U+00E9 (2 bytes)");
	CHECK_STR(field(&o, "UTF-16 code point"), "U+A9C3");
}

static void test_detect(void)
{
	CHECK(!bin_is_binary("hello\nworld\n", strlen("hello\nworld\n")));
	CHECK(!bin_is_binary("h\xc3\xa9llo w\xc3\xb6rld\n", strlen("h\xc3\xa9llo w\xc3\xb6rld\n")));
	CHECK(!bin_is_binary("", strlen("")));
	CHECK(!bin_is_binary("BMW is a car\n", strlen("BMW is a car\n")));
	CHECK(!bin_is_binary("MZ: a text file\n", strlen("MZ: a text file\n")));
	CHECK(!bin_is_binary("tab\tsep\r\n\x1b[1mbold\x1b[0m\n", strlen("tab\tsep\r\n\x1b[1mbold\x1b[0m\n")));
	CHECK(bin_is_binary("\x7f" "ELF\2\1\1", strlen("\x7f" "ELF\2\1\1")));
	CHECK(bin_is_binary("abc\0def", 7));
	CHECK(bin_is_binary("\x89PNG\r\n\x1a\nxxxx", strlen("\x89PNG\r\n\x1a\nxxxx")));
	CHECK(bin_is_binary("%PDF-1.7\n", strlen("%PDF-1.7\n")));
	CHECK(bin_is_binary("\x1f\x8b\x08", strlen("\x1f\x8b\x08")));
	CHECK(bin_is_binary("\x01\x02\x03\x04\x05\x06\x07\x08 text", strlen("\x01\x02\x03\x04\x05\x06\x07\x08 text")));
	/* Latin-1 everywhere: >10% invalid UTF-8 */
	CHECK(bin_is_binary("\xe9\xe8\xe0\xf9\xe9\xe8 a", strlen("\xe9\xe8\xe0\xf9\xe9\xe8 a")));
	CHECK(bin_is_binary("MZ\x90\0\3\0", 6));
}

static void speed(void)
{
	const char *p = "/usr/local/llvm19/lib/libLLVM-19.so";
	int fd = open(p, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "speed: %s missing, skipped\n", p);
		return;
	}
	struct stat st;
	fstat(fd, &st);
	u8 *d = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (d == MAP_FAILED)
		return;
	BinImage img;
	CHECK(bin_parse(d, (size_t)st.st_size, &img) == 0);
	u32 t = sec(&img, ".text");
	if (t != UINT32_MAX) {
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		BinDisasm ds;
		CHECK(bin_disasm(&img, t, d, (size_t)st.st_size, &ds) == 0);
		clock_gettime(CLOCK_MONOTONIC, &b);
		double s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
		fprintf(stderr, "libLLVM-19.so .text: %.1f MB, %u rows (%u symbols) in %.2f s = %.1f MB/s\n",
		        (double)img.s_size[t] / 1e6, ds.n, img.nsym, s, (double)img.s_size[t] / 1e6 / s);
		bin_disasm_free(&ds);
	}
	bin_free(&img);
	munmap(d, (size_t)st.st_size);
}

int main(int argc, char **argv)
{
	test_detect();
	test_inspect();
	test_pe();
	test_macho();
	CHECK(mkdtemp(dir) != NULL);
	char path[512];
	snprintf(path, sizeof path, "%s/p.c", dir);
	const char *src = "const char msg[] = \"hello\";\n"
	                  "int counter = 3;\n"
	                  "static char buf[4096];\n"
	                  "__attribute__((noinline)) int helper(int x) { buf[x] = (char)x; return x * 3 + msg[x]; }\n"
	                  "int main(void) { return helper(counter) + buf[1]; }\n";
	vx_write_file(path, src, strlen(src));
	if (sh("cc -O1 -o p64 p.c")) {
		snprintf(path, sizeof path, "%s/p64", dir);
		check_call(path, "ELF64", "x86-64", true);
		fuzz(path, argc > 1 ? atoi(argv[1]) : 3000);
	}
	if (sh("cc -m32 -O1 -o p32 p.c")) {
		snprintf(path, sizeof path, "%s/p32", dir);
		check_call(path, "ELF32", "x86", true);
	} else {
		fprintf(stderr, "cc -m32 unavailable: 32-bit ELF test skipped\n");
	}
	if (sh("cc --target=aarch64-unknown-freebsd -O1 -c -o pa64.o p.c")) {
		snprintf(path, sizeof path, "%s/pa64.o", dir);
		/* relocatable: calls are relocations, so no resolved target check */
		check_call(path, "ELF64", "AArch64", false);
		size_t n;
		u8 *d = load(path, &n);
		BinImage img;
		bin_parse(d, n, &img);
		BinDisasm ds;
		bin_disasm(&img, sec(&img, ".text"), d, n, &ds);
		bool ret = false;
		for (u32 i = 0; i < ds.n; i++)
			ret |= !strcmp(ds.pool.b + ds.text[i], "ret");
		CHECK(ret);
		bin_disasm_free(&ds);
		bin_free(&img);
		free(d);
	}
	speed();
	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
