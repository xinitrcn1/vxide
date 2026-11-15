#define _DEFAULT_SOURCE /* gmtime_r, sysconf(_SC_NPROCESSORS_ONLN) */
#include "binfile.h"

#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
/* capstone.h has an enumerator above INT_MAX (CS_MODE_BIG_ENDIAN) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <capstone/capstone.h>
#pragma GCC diagnostic pop

/* ---- detection --------------------------------------------------------------------- */

bool bin_is_binary(const char *head, size_t n)
{
	if (!n)
		return false;
	static const struct {
		const char *m;
		u8 len;
		u8 at;
	} magic[] = {
	    {"\x7f" "ELF", 4, 0},         {"MZ", 2, 0},
	    {"\xfe\xed\xfa\xce", 4, 0},   {"\xce\xfa\xed\xfe", 4, 0},
	    {"\xfe\xed\xfa\xcf", 4, 0},   {"\xcf\xfa\xed\xfe", 4, 0},
	    {"\xca\xfe\xba\xbe", 4, 0},   {"\xca\xfe\xba\xbf", 4, 0},
	    {"!<arch>\n", 8, 0},          {"PK\x03\x04", 4, 0},
	    {"PK\x05\x06", 4, 0},         {"\x89PNG\r\n\x1a\n", 8, 0},
	    {"\xff\xd8\xff", 3, 0},       {"GIF87a", 6, 0},
	    {"GIF89a", 6, 0},             {"BM", 2, 0},
	    {"RIFF", 4, 0},               {"\x1f\x8b", 2, 0},
	    {"BZh", 3, 0},                {"\xfd" "7zXZ\0", 6, 0},
	    {"\x28\xb5\x2f\xfd", 4, 0},   {"7z\xbc\xaf\x27\x1c", 6, 0},
	    {"Rar!\x1a\x07", 6, 0},       {"\x04\x22\x4d\x18", 4, 0},
	    {"%PDF-", 5, 0},              {"SQLite format 3\0", 16, 0},
	    {"\0asm", 4, 0},              {"\0\0\1\0", 4, 0},
	    {"II*\0", 4, 0},              {"MM\0*", 4, 0},
	    {"OggS", 4, 0},               {"fLaC", 4, 0},
	    {"ID3", 3, 0},                {"ftyp", 4, 4},
	    {"\0\1\0\0\0", 5, 0},         {"OTTO", 4, 0},
	    {"wOFF", 4, 0},               {"wOF2", 4, 0},
	};
	for (size_t i = 0; i < VX_COUNT(magic); i++)
		if (n >= (size_t)magic[i].at + magic[i].len &&
		    memcmp(head + magic[i].at, magic[i].m, magic[i].len) == 0) {
			/* "BM" and "MZ" are also plausible text starts: insist on more
			 * evidence than two letters (a NUL or control bytes nearby) */
			if (magic[i].len <= 2 && magic[i].at == 0 && head[0] != '\x1f') {
				bool ctrl = false;
				for (size_t k = 2; k < VX_MIN(n, (size_t)64); k++)
					if ((u8)head[k] < 0x09 || ((u8)head[k] > 0x0d && (u8)head[k] < 0x20))
						ctrl = true;
				if (!ctrl)
					continue;
			}
			return true;
		}
	size_t m = VX_MIN(n, (size_t)8192);
	if (memchr(head, 0, m))
		return true;
	size_t ctrl = 0, bad = 0;
	for (size_t i = 0; i < m;) {
		u8 c = (u8)head[i];
		if (c < 0x20 && c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r' && c != 0x1b)
			ctrl++;
		if (c < 0x80) {
			i++;
			continue;
		}
		/* validate one UTF-8 sequence */
		u32 need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
		bool ok = need && c >= 0xC2 && c <= 0xF4;
		size_t k = 1;
		for (; ok && k <= need; k++) {
			if (i + k >= m)
				break; /* cut at the end of the sample: not counted */
			if (((u8)head[i + k] & 0xC0) != 0x80)
				ok = false;
		}
		if (!ok)
			bad += 1;
		i += ok ? need + 1 : 1;
	}
	return ctrl * 10 > m || bad * 10 > m;
}

/* ---- bounded readers ---------------------------------------------------------------- */

typedef struct {
	const u8 *d;
	size_t n;
	bool be;
} Rd;

static bool in_range(const Rd *r, u64 off, u64 len) { return off <= r->n && len <= r->n - off; }

static u64 rd_n(const Rd *r, u64 off, u32 len)
{
	if (!in_range(r, off, len))
		return 0;
	u64 v = 0;
	for (u32 i = 0; i < len; i++) {
		u32 sh = r->be ? (len - 1 - i) * 8 : i * 8;
		v |= (u64)r->d[off + i] << sh;
	}
	return v;
}
static u16 r16(const Rd *r, u64 off) { return (u16)rd_n(r, off, 2); }
static u32 r32(const Rd *r, u64 off) { return (u32)rd_n(r, off, 4); }
static u64 r64(const Rd *r, u64 off) { return rd_n(r, off, 8); }

/* A NUL-terminated name inside [off, off + max) of the buffer, copied. */
static u32 name_at(BinImage *img, const Rd *r, u64 off, u64 max)
{
	if (off >= r->n)
		return pool_add(&img->pool, "", 0);
	u64 lim = VX_MIN(max, (u64)r->n - off);
	const u8 *p = r->d + off;
	u64 k = 0;
	while (k < lim && p[k])
		k++;
	if (k > 1024)
		k = 1024;
	return pool_add(&img->pool, (const char *)p, (u32)k);
}

/* ---- image tables --------------------------------------------------------------------- */

static void add_section(BinImage *img, u32 name, u64 off, u64 size, u64 addr, u32 flags, size_t file_n)
{
	if (off >= file_n || !size)
		return;
	if (size > file_n - off)
		size = file_n - off; /* clip to the file */
	if (img->ns == img->ns_cap) {
		u32 c = img->ns_cap ? img->ns_cap * 2 : 32;
		img->s_name = vx_realloc(img->s_name, c * sizeof(u32));
		img->s_off = vx_realloc(img->s_off, c * sizeof(u64));
		img->s_size = vx_realloc(img->s_size, c * sizeof(u64));
		img->s_addr = vx_realloc(img->s_addr, c * sizeof(u64));
		img->s_flags = vx_realloc(img->s_flags, c * sizeof(u32));
		img->ns_cap = c;
	}
	u32 i = img->ns++;
	img->s_name[i] = name;
	img->s_off[i] = off;
	img->s_size[i] = size;
	img->s_addr[i] = addr;
	img->s_flags[i] = flags | BS_HAS_FILE_DATA;
}

static void add_symbol(BinImage *img, u32 name, u64 addr, u64 size, bool func)
{
	if (img->nsym >= (8u << 20))
		return;
	if (img->nsym == img->sym_cap) {
		u32 c = img->sym_cap ? img->sym_cap * 2 : 256;
		img->y_name = vx_realloc(img->y_name, c * sizeof(u32));
		img->y_addr = vx_realloc(img->y_addr, c * sizeof(u64));
		img->y_size = vx_realloc(img->y_size, c * sizeof(u64));
		img->y_func = vx_realloc(img->y_func, c);
		img->sym_cap = c;
	}
	u32 i = img->nsym++;
	img->y_name[i] = name;
	img->y_addr[i] = addr;
	img->y_size[i] = size;
	img->y_func[i] = func;
}

static const BinImage *g_sort;
static int sym_cmp(const void *a, const void *b)
{
	u32 i = *(const u32 *)a, j = *(const u32 *)b;
	const BinImage *g = g_sort;
	if (g->y_addr[i] != g->y_addr[j])
		return g->y_addr[i] < g->y_addr[j] ? -1 : 1;
	/* functions first, then names */
	if (g->y_func[i] != g->y_func[j])
		return g->y_func[j] - g->y_func[i];
	return strcmp(g->pool.b + g->y_name[i], g->pool.b + g->y_name[j]);
}

static void sort_symbols(BinImage *img)
{
	u32 n = img->nsym;
	if (n < 2)
		return;
	u32 *perm = vx_alloc(n * sizeof(u32));
	for (u32 i = 0; i < n; i++)
		perm[i] = i;
	g_sort = img;
	qsort(perm, n, sizeof(u32), sym_cmp);
	u32 *nn = vx_alloc(n * sizeof(u32));
	u64 *na = vx_alloc(n * sizeof(u64)), *ns = vx_alloc(n * sizeof(u64));
	u8 *nf = vx_alloc(n);
	u32 k = 0;
	for (u32 i = 0; i < n; i++) {
		u32 p = perm[i];
		/* drop exact duplicates (.symtab and .dynsym list the same names) */
		if (k && na[k - 1] == img->y_addr[p] && !strcmp(img->pool.b + nn[k - 1], img->pool.b + img->y_name[p]))
			continue;
		nn[k] = img->y_name[p];
		na[k] = img->y_addr[p];
		ns[k] = img->y_size[p];
		nf[k] = img->y_func[p];
		k++;
	}
	free(img->y_name);
	free(img->y_addr);
	free(img->y_size);
	free(img->y_func);
	img->y_name = nn;
	img->y_addr = na;
	img->y_size = ns;
	img->y_func = nf;
	img->nsym = k;
	img->sym_cap = n;
	free(perm);
}

void bin_free(BinImage *img)
{
	free(img->s_name);
	free(img->s_off);
	free(img->s_size);
	free(img->s_addr);
	free(img->s_flags);
	free(img->y_name);
	free(img->y_addr);
	free(img->y_size);
	free(img->y_func);
	pool_free(&img->pool);
	memset(img, 0, sizeof *img);
}

u32 bin_symbol_at(const BinImage *img, u64 addr, u64 *delta)
{
	u32 lo = 0, hi = img->nsym;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (img->y_addr[mid] <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return UINT32_MAX;
	u32 i = lo - 1;
	/* the first symbol at that address is the preferred one (functions first) */
	while (i > 0 && img->y_addr[i - 1] == img->y_addr[i])
		i--;
	u64 d = addr - img->y_addr[i];
	if (d > (1u << 20))
		return UINT32_MAX;
	if (delta)
		*delta = d;
	return i;
}

static void set_err(BinImage *img, const char *m) { snprintf(img->err, sizeof img->err, "%s", m); }

/* ---- ELF ------------------------------------------------------------------------------- */

static void elf_machine(BinImage *img, u16 m, bool is64)
{
	switch (m) {
	case 3:
		img->arch = BA_X86, img->arch_name = "x86", img->bits = 32;
		break;
	case 62:
		img->arch = BA_X86, img->arch_name = "x86-64", img->bits = 64;
		break;
	case 40:
		img->arch = BA_ARM, img->arch_name = "ARM", img->bits = 32;
		break;
	case 183:
		img->arch = BA_AARCH64, img->arch_name = "AArch64", img->bits = 64;
		break;
	case 243:
		img->arch = BA_RISCV, img->arch_name = is64 ? "RISC-V 64" : "RISC-V 32", img->bits = is64 ? 64 : 32;
		break;
	case 8:
	case 10:
		img->arch = BA_MIPS, img->arch_name = is64 ? "MIPS64" : "MIPS", img->bits = is64 ? 64 : 32;
		break;
	case 20:
		img->arch = BA_PPC, img->arch_name = "PowerPC", img->bits = 32;
		break;
	case 21:
		img->arch = BA_PPC, img->arch_name = "PowerPC64", img->bits = 64;
		break;
	default:
		img->arch = BA_UNKNOWN, img->arch_name = "unknown", img->bits = is64 ? 64 : 32;
	}
}

static void elf_symbols(BinImage *img, const Rd *r, bool is64, u64 shoff, u32 shentsize, u32 shnum, u32 symsec,
                        bool arm)
{
	u64 sh = shoff + (u64)symsec * shentsize;
	u64 off = is64 ? r64(r, sh + 24) : r32(r, sh + 16);
	u64 size = is64 ? r64(r, sh + 32) : r32(r, sh + 20);
	u32 link = r32(r, sh + (is64 ? 40 : 24));
	u64 ent = is64 ? r64(r, sh + 56) : r32(r, sh + 36);
	u64 want = is64 ? 24 : 16;
	if (ent != want || link >= shnum || !in_range(r, off, size))
		return;
	u64 lsh = shoff + (u64)link * shentsize;
	u64 stroff = is64 ? r64(r, lsh + 24) : r32(r, lsh + 16);
	u64 strsize = is64 ? r64(r, lsh + 32) : r32(r, lsh + 20);
	if (!in_range(r, stroff, strsize))
		return;
	u64 count = size / ent;
	for (u64 i = 1; i < count; i++) {
		u64 e = off + i * ent;
		u32 nm = r32(r, e);
		u8 info = (u8)rd_n(r, e + (is64 ? 4 : 12), 1);
		u16 shndx = r16(r, e + (is64 ? 6 : 14));
		u64 val = is64 ? r64(r, e + 8) : r32(r, e + 4);
		u64 sz = is64 ? r64(r, e + 16) : r32(r, e + 8);
		u8 type = info & 0xf;
		if (shndx == 0 || nm == 0 || nm >= strsize || (type != 0 && type != 1 && type != 2 && type != 10))
			continue;
		if (shndx == 0xfff1 && type != 2)
			continue; /* absolute non-functions: not addresses */
		const char *s = (const char *)r->d + stroff + nm;
		/* skip ARM mapping symbols ($a $t $d $x) */
		if (s[0] == '$' && (s[1] == 'a' || s[1] == 't' || s[1] == 'd' || s[1] == 'x') &&
		    (s[2] == 0 || s[2] == '.'))
			continue;
		u32 name = name_at(img, r, stroff + nm, strsize - nm);
		bool func = type == 2 || type == 10;
		if (arm && func)
			val &= ~(u64)1; /* Thumb bit */
		add_symbol(img, name, val, sz, func);
	}
}

static int parse_elf(const u8 *d, size_t n, BinImage *img)
{
	if (n < 52)
		return set_err(img, "truncated ELF header"), -1;
	bool is64 = d[4] == 2;
	if (d[4] != 1 && d[4] != 2)
		return set_err(img, "bad ELF class"), -1;
	Rd r = {d, n, d[5] == 2};
	img->format = BF_ELF;
	img->format_name = is64 ? "ELF64" : "ELF32";
	img->big_endian = r.be;
	u16 type = r16(&r, 16);
	(void)type;
	elf_machine(img, r16(&r, 18), is64);
	bool arm = img->arch == BA_ARM;
	img->entry = is64 ? r64(&r, 24) : r32(&r, 24);
	u64 phoff = is64 ? r64(&r, 32) : r32(&r, 28);
	u64 shoff = is64 ? r64(&r, 40) : r32(&r, 32);
	u32 phentsize = r16(&r, is64 ? 54 : 42), phnum = r16(&r, is64 ? 56 : 44);
	u32 shentsize = r16(&r, is64 ? 58 : 46), shnum = r16(&r, is64 ? 60 : 48);
	u32 shstrndx = r16(&r, is64 ? 62 : 50);
	u32 want_sh = is64 ? 64 : 40, want_ph = is64 ? 56 : 32;
	/* extended numbering lives in section 0 */
	if (shoff && shentsize == want_sh && in_range(&r, shoff, want_sh)) {
		if (shnum == 0)
			shnum = (u32)VX_MIN(is64 ? r64(&r, shoff + 32) : r32(&r, shoff + 20), 1u << 20);
		if (shstrndx == 0xffff)
			shstrndx = r32(&r, shoff + (is64 ? 40 : 24));
	}
	/* lowest PT_LOAD as image base */
	img->image_base = UINT64_MAX;
	if (phoff && phentsize == want_ph)
		for (u32 i = 0; i < phnum && in_range(&r, phoff + (u64)i * phentsize, want_ph); i++) {
			u64 p = phoff + (u64)i * phentsize;
			if (r32(&r, p) == 1) {
				u64 va = is64 ? r64(&r, p + 16) : r32(&r, p + 8);
				img->image_base = VX_MIN(img->image_base, va);
			}
		}
	if (img->image_base == UINT64_MAX)
		img->image_base = 0;
	bool have_sections = false;
	if (shoff && shnum && shentsize == want_sh && in_range(&r, shoff, (u64)shnum * shentsize)) {
		u64 strh = shoff + (u64)shstrndx * shentsize;
		u64 stroff = 0, strsize = 0;
		if (shstrndx < shnum) {
			stroff = is64 ? r64(&r, strh + 24) : r32(&r, strh + 16);
			strsize = is64 ? r64(&r, strh + 32) : r32(&r, strh + 20);
			if (!in_range(&r, stroff, strsize))
				strsize = 0;
		}
		for (u32 i = 1; i < shnum; i++) {
			u64 sh = shoff + (u64)i * shentsize;
			u32 nm = r32(&r, sh);
			u32 st = r32(&r, sh + 4);
			u64 fl = is64 ? r64(&r, sh + 8) : r32(&r, sh + 8);
			u64 addr = is64 ? r64(&r, sh + 16) : r32(&r, sh + 12);
			u64 off = is64 ? r64(&r, sh + 24) : r32(&r, sh + 16);
			u64 size = is64 ? r64(&r, sh + 32) : r32(&r, sh + 20);
			if (st == 0 || st == 8) /* NULL, NOBITS */
				continue;
			u32 name = nm < strsize ? name_at(img, &r, stroff + nm, strsize - nm) : pool_add(&img->pool, "", 0);
			u32 f = (fl & 4) ? BS_CODE : BS_DATA;
			f |= (fl & 1) ? BS_WRITABLE : BS_READONLY;
			if (arm && (f & BS_CODE) && (img->entry & 1))
				f |= BS_THUMB;
			add_section(img, name, off, size, addr, f, n);
			have_sections = true;
			if (st == 2 || st == 11)
				elf_symbols(img, &r, is64, shoff, shentsize, shnum, i, arm);
		}
	}
	if (!have_sections && phoff && phentsize == want_ph) {
		/* stripped of section headers: the loadable segments */
		for (u32 i = 0; i < phnum && in_range(&r, phoff + (u64)i * phentsize, want_ph); i++) {
			u64 p = phoff + (u64)i * phentsize;
			if (r32(&r, p) != 1)
				continue;
			u32 pf = r32(&r, p + (is64 ? 4 : 24));
			u64 off = is64 ? r64(&r, p + 8) : r32(&r, p + 4);
			u64 va = is64 ? r64(&r, p + 16) : r32(&r, p + 8);
			u64 fsz = is64 ? r64(&r, p + 32) : r32(&r, p + 16);
			char nm[64];
			snprintf(nm, sizeof nm, "LOAD %u (%c%c%c)", i, (pf & 4) ? 'r' : '-', (pf & 2) ? 'w' : '-',
			         (pf & 1) ? 'x' : '-');
			u32 f = BS_SEGMENT | ((pf & 1) ? BS_CODE : BS_DATA) | ((pf & 2) ? BS_WRITABLE : BS_READONLY);
			if (arm && (pf & 1) && (img->entry & 1))
				f |= BS_THUMB;
			add_section(img, pool_add(&img->pool, nm, (u32)strlen(nm)), off, fsz, va, f, n);
		}
	}
	if (arm)
		img->entry &= ~(u64)1;
	sort_symbols(img);
	return 0;
}

/* ---- PE / COFF ------------------------------------------------------------------------ */

static u64 pe_rva_to_off(const Rd *r, u64 sect, u32 nsect, u64 rva)
{
	for (u32 i = 0; i < nsect; i++) {
		u64 s = sect + (u64)i * 40;
		u32 vsz = r32(r, s + 8), va = r32(r, s + 12), rawsz = r32(r, s + 16), raw = r32(r, s + 20);
		u32 span = VX_MAX(vsz, rawsz);
		if (rva >= va && rva < (u64)va + span && rva - va < rawsz)
			return raw + (rva - va);
	}
	return UINT64_MAX;
}

static int parse_pe(const u8 *d, size_t n, BinImage *img)
{
	Rd r = {d, n, false};
	u32 pe = r32(&r, 0x3c);
	if (!in_range(&r, pe, 24) || memcmp(d + pe, "PE\0\0", 4) != 0)
		return 1; /* a DOS executable or "MZ" text: not ours */
	u64 coff = pe + 4;
	u16 machine = r16(&r, coff), nsect = r16(&r, coff + 2);
	u32 symptr = r32(&r, coff + 8), nsyms = r32(&r, coff + 12);
	u16 optsz = r16(&r, coff + 16);
	u64 opt = coff + 20;
	u16 magic = r16(&r, opt);
	bool plus = magic == 0x20b;
	img->format = BF_PE;
	img->format_name = plus ? "PE32+" : "PE32";
	switch (machine) {
	case 0x14c:
		img->arch = BA_X86, img->arch_name = "x86", img->bits = 32;
		break;
	case 0x8664:
		img->arch = BA_X86, img->arch_name = "x86-64", img->bits = 64;
		break;
	case 0x1c0:
		img->arch = BA_ARM, img->arch_name = "ARM", img->bits = 32;
		break;
	case 0x1c2:
	case 0x1c4:
		img->arch = BA_ARM, img->arch_name = "ARM Thumb-2", img->bits = 32;
		break;
	case 0xaa64:
		img->arch = BA_AARCH64, img->arch_name = "AArch64", img->bits = 64;
		break;
	case 0x5032:
		img->arch = BA_RISCV, img->arch_name = "RISC-V 32", img->bits = 32;
		break;
	case 0x5064:
		img->arch = BA_RISCV, img->arch_name = "RISC-V 64", img->bits = 64;
		break;
	default:
		img->arch = BA_UNKNOWN, img->arch_name = "unknown", img->bits = plus ? 64 : 32;
	}
	u32 entry_rva = 0;
	u64 dd = 0;
	u32 ndd = 0;
	if (optsz >= 28 && in_range(&r, opt, optsz)) {
		entry_rva = r32(&r, opt + 16);
		img->image_base = plus ? r64(&r, opt + 24) : r32(&r, opt + 28);
		ndd = r32(&r, opt + (plus ? 108 : 92));
		dd = opt + (plus ? 112 : 96);
		if (dd + (u64)ndd * 8 > opt + optsz)
			ndd = (u32)((opt + optsz > dd ? opt + optsz - dd : 0) / 8);
	}
	img->entry = entry_rva ? img->image_base + entry_rva : 0;
	u64 sect = opt + optsz;
	if (!in_range(&r, sect, (u64)nsect * 40))
		nsect = (u16)(in_range(&r, sect, 0) ? (n - sect) / 40 : 0);
	/* COFF string table for "/123" long names */
	u64 strtab = symptr ? (u64)symptr + (u64)nsyms * 18 : 0;
	u32 strsize = strtab ? r32(&r, strtab) : 0;
	for (u32 i = 0; i < nsect; i++) {
		u64 s = sect + (u64)i * 40;
		char nm[9] = {0};
		memcpy(nm, d + s, 8);
		u32 name;
		if (nm[0] == '/' && strtab && strsize) {
			u32 so = (u32)strtoul(nm + 1, NULL, 10);
			name = so < strsize ? name_at(img, &r, strtab + so, strsize - so) : pool_add(&img->pool, nm, (u32)strlen(nm));
		} else {
			name = pool_add(&img->pool, nm, (u32)strlen(nm));
		}
		u32 vsz = r32(&r, s + 8), va = r32(&r, s + 12), rawsz = r32(&r, s + 16), raw = r32(&r, s + 20);
		u32 ch = r32(&r, s + 36);
		u64 size = rawsz;
		if (vsz && vsz < rawsz)
			size = vsz; /* raw size is padded to the file alignment */
		u32 f = (ch & 0x20000020) ? BS_CODE : BS_DATA;
		f |= (ch & 0x80000000u) ? BS_WRITABLE : BS_READONLY;
		if (machine == 0x1c2 || machine == 0x1c4)
			f |= (f & BS_CODE) ? BS_THUMB : 0;
		add_section(img, name, raw, size, img->image_base + va, f, n);
	}
	/* exports */
	if (ndd >= 1) {
		u32 erva = r32(&r, dd), esz = r32(&r, dd + 4);
		u64 eoff = esz ? pe_rva_to_off(&r, sect, nsect, erva) : UINT64_MAX;
		if (eoff != UINT64_MAX && in_range(&r, eoff, 40)) {
			u32 nfun = r32(&r, eoff + 20), nnames = r32(&r, eoff + 24);
			u64 fun = pe_rva_to_off(&r, sect, nsect, r32(&r, eoff + 28));
			u64 names = pe_rva_to_off(&r, sect, nsect, r32(&r, eoff + 32));
			u64 ords = pe_rva_to_off(&r, sect, nsect, r32(&r, eoff + 36));
			if (fun != UINT64_MAX && names != UINT64_MAX && ords != UINT64_MAX)
				for (u32 i = 0; i < nnames && i < (1u << 20); i++) {
					u16 ord = r16(&r, ords + (u64)i * 2);
					if (ord >= nfun || !in_range(&r, names + (u64)i * 4, 4))
						break;
					u32 frva = r32(&r, fun + (u64)ord * 4);
					u64 no = pe_rva_to_off(&r, sect, nsect, r32(&r, names + (u64)i * 4));
					if (no == UINT64_MAX || !frva)
						continue;
					add_symbol(img, name_at(img, &r, no, 512), img->image_base + frva, 0, true);
				}
		}
	}
	if (img->entry)
		add_symbol(img, pool_add(&img->pool, "entry", 5), img->entry, 0, true);
	sort_symbols(img);
	return 0;
}

/* ---- Mach-O ----------------------------------------------------------------------- */

static void macho_cpu(BinImage *img, u32 cpu)
{
	switch (cpu) {
	case 7:
		img->arch = BA_X86, img->arch_name = "x86", img->bits = 32;
		break;
	case 0x01000007:
		img->arch = BA_X86, img->arch_name = "x86-64", img->bits = 64;
		break;
	case 12:
		img->arch = BA_ARM, img->arch_name = "ARM", img->bits = 32;
		break;
	case 0x0100000c:
	case 0x0200000c:
		img->arch = BA_AARCH64, img->arch_name = cpu == 0x0200000c ? "ARM64_32" : "AArch64", img->bits = 64;
		break;
	case 18:
		img->arch = BA_PPC, img->arch_name = "PowerPC", img->bits = 32;
		break;
	case 0x01000012:
		img->arch = BA_PPC, img->arch_name = "PowerPC64", img->bits = 64;
		break;
	default:
		img->arch = BA_UNKNOWN, img->arch_name = "unknown";
	}
}

static int parse_macho_at(const u8 *d, size_t n, u64 base, BinImage *img)
{
	/* the slice is [base, n) of the file; offsets inside are slice-relative */
	if (base + 28 > n)
		return set_err(img, "truncated Mach-O header"), -1;
	u32 m = (u32)d[base] | (u32)d[base + 1] << 8 | (u32)d[base + 2] << 16 | (u32)d[base + 3] << 24;
	bool is64 = m == 0xfeedfacf || m == 0xcffaedfe;
	bool be = m == 0xcefaedfe || m == 0xcffaedfe;
	Rd r = {d, n, be};
	img->format = BF_MACHO;
	img->format_name = is64 ? "Mach-O 64" : "Mach-O";
	img->big_endian = be;
	macho_cpu(img, r32(&r, base + 4));
	if (!img->bits)
		img->bits = is64 ? 64 : 32;
	u32 ncmds = r32(&r, base + 16), sizeofcmds = r32(&r, base + 20);
	u64 lc = base + (is64 ? 32 : 28);
	u64 end = VX_MIN(lc + (u64)sizeofcmds, (u64)n);
	u64 text_vm = 0;
	u64 entryoff = UINT64_MAX;
	u32 symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
	/* section index (1-based, as in nlist n_sect) -> code flag */
	u8 sect_code[256] = {0};
	u32 sect_no = 0;
	for (u32 i = 0; i < ncmds && lc + 8 <= end; i++) {
		u32 cmd = r32(&r, lc), cmdsize = r32(&r, lc + 4);
		if (cmdsize < 8 || lc + cmdsize > end)
			break;
		if (cmd == 1 || cmd == 0x19) { /* LC_SEGMENT(_64) */
			bool s64 = cmd == 0x19;
			char segname[17] = {0};
			memcpy(segname, d + lc + 8, 16);
			u64 vm = s64 ? r64(&r, lc + 24) : r32(&r, lc + 24);
			u32 initprot = r32(&r, lc + (s64 ? 60 : 44));
			u32 nsects = r32(&r, lc + (s64 ? 64 : 48));
			if (!strcmp(segname, "__TEXT"))
				text_vm = vm;
			u64 sp = lc + (s64 ? 72 : 56);
			u32 ssz = s64 ? 80 : 68;
			for (u32 k = 0; k < nsects && sp + ssz <= lc + cmdsize; k++, sp += ssz) {
				char sn[17] = {0}, sg[17] = {0};
				memcpy(sn, d + sp, 16);
				memcpy(sg, d + sp + 16, 16);
				u64 addr = s64 ? r64(&r, sp + 32) : r32(&r, sp + 32);
				u64 size = s64 ? r64(&r, sp + 40) : r32(&r, sp + 36);
				u32 off = r32(&r, sp + (s64 ? 48 : 40));
				u32 fl = r32(&r, sp + (s64 ? 64 : 56));
				u32 type = fl & 0xff;
				bool code = (fl & 0x80000000u) || (fl & 0x400);
				sect_no++;
				if (sect_no < 256)
					sect_code[sect_no] = code;
				if (type == 1 || type == 12 || type == 0x12 || !off)
					continue; /* zerofill: no file data */
				char nm[40];
				snprintf(nm, sizeof nm, "%s,%s", sg, sn);
				u32 f = (code ? BS_CODE : BS_DATA) | ((initprot & 2) ? BS_WRITABLE : BS_READONLY);
				add_section(img, pool_add(&img->pool, nm, (u32)strlen(nm)), base + off, size, addr, f, n);
			}
		} else if (cmd == 2) { /* LC_SYMTAB */
			symoff = r32(&r, lc + 8);
			nsyms = r32(&r, lc + 12);
			stroff = r32(&r, lc + 16);
			strsize = r32(&r, lc + 20);
		} else if (cmd == 0x80000028) { /* LC_MAIN */
			entryoff = r64(&r, lc + 8);
		}
		lc += cmdsize;
	}
	img->image_base = text_vm;
	if (entryoff != UINT64_MAX)
		img->entry = text_vm + entryoff;
	if (nsyms && in_range(&r, base + stroff, strsize)) {
		u32 esz = is64 ? 16 : 12;
		for (u32 i = 0; i < nsyms && i < (4u << 20); i++) {
			u64 e = base + symoff + (u64)i * esz;
			if (!in_range(&r, e, esz))
				break;
			u32 strx = r32(&r, e);
			u8 type = (u8)rd_n(&r, e + 4, 1), sect = (u8)rd_n(&r, e + 5, 1);
			u64 val = is64 ? r64(&r, e + 8) : r32(&r, e + 8);
			if ((type & 0xe0) || (type & 0x0e) != 0x0e || strx >= strsize || !strx)
				continue;
			add_symbol(img, name_at(img, &r, base + stroff + strx, strsize - strx), val, 0, sect_code[sect]);
		}
	}
	sort_symbols(img);
	return 0;
}

static int parse_fat(const u8 *d, size_t n, BinImage *img)
{
	Rd r = {d, n, true};
	bool fat64 = d[3] == 0xbf;
	u32 narch = r32(&r, 4);
	if (narch == 0 || narch > 32)
		return 1; /* a Java class file shares the 0xcafebabe magic */
	u32 esz = fat64 ? 32 : 20;
	u64 pick = UINT64_MAX;
	for (u32 i = 0; i < narch; i++) {
		u64 e = 8 + (u64)i * esz;
		if (!in_range(&r, e, esz))
			break;
		u32 cpu = r32(&r, e);
		u64 off = fat64 ? r64(&r, e + 8) : r32(&r, e + 8);
		if (pick == UINT64_MAX)
			pick = off;
#if defined(__x86_64__)
		if (cpu == 0x01000007)
			pick = off;
#elif defined(__aarch64__)
		if (cpu == 0x0100000c)
			pick = off;
#endif
		(void)cpu;
	}
	if (pick == UINT64_MAX || pick >= n)
		return set_err(img, "fat Mach-O without a usable slice"), -1;
	img->slice_off = pick;
	int rc = parse_macho_at(d, n, pick, img);
	img->format_name = "Mach-O (fat)";
	return rc;
}

int bin_parse(const u8 *d, size_t n, BinImage *img)
{
	memset(img, 0, sizeof *img);
	pool_add(&img->pool, "", 0); /* offset 0 = empty string */
	img->format_name = "data";
	img->arch_name = "";
	if (n >= 4 && !memcmp(d, "\x7f" "ELF", 4))
		return parse_elf(d, n, img);
	if (n >= 0x40 && d[0] == 'M' && d[1] == 'Z')
		return parse_pe(d, n, img);
	if (n >= 4) {
		u32 m = (u32)d[0] | (u32)d[1] << 8 | (u32)d[2] << 16 | (u32)d[3] << 24;
		if (m == 0xfeedface || m == 0xfeedfacf || m == 0xcefaedfe || m == 0xcffaedfe)
			return parse_macho_at(d, n, 0, img);
		if (n >= 8 && d[0] == 0xca && d[1] == 0xfe && d[2] == 0xba && (d[3] == 0xbe || d[3] == 0xbf))
			return parse_fat(d, n, img);
	}
	return 1;
}

/* ---- disassembly -------------------------------------------------------------------------- */

static void row_add(BinDisasm *o, u8 kind, u64 addr, u64 off, u8 len, const char *t1, const char *t2, u64 target)
{
	if (o->n == o->cap) {
		u32 c = o->cap ? o->cap * 2 : 4096;
		o->kind = vx_realloc(o->kind, c);
		o->addr = vx_realloc(o->addr, c * sizeof(u64));
		o->off = vx_realloc(o->off, c * sizeof(u64));
		o->len = vx_realloc(o->len, c);
		o->text = vx_realloc(o->text, c * sizeof(u32));
		o->target = vx_realloc(o->target, c * sizeof(u64));
		o->cap = c;
	}
	u32 i = o->n++;
	o->kind[i] = kind;
	o->addr[i] = addr;
	o->off[i] = off;
	o->len[i] = len;
	o->target[i] = target;
	/* "mnemonic operands" without a temporary buffer */
	u32 l1 = (u32)strlen(t1), l2 = t2 ? (u32)strlen(t2) : 0;
	VX_GROW(o->pool.b, o->pool.cap, o->pool.n + l1 + l2 + 2);
	o->text[i] = o->pool.n;
	memcpy(o->pool.b + o->pool.n, t1, l1);
	o->pool.n += l1;
	if (l2) {
		o->pool.b[o->pool.n++] = ' ';
		memcpy(o->pool.b + o->pool.n, t2, l2);
		o->pool.n += l2;
	}
	o->pool.b[o->pool.n++] = 0;
}

void bin_disasm_free(BinDisasm *d)
{
	free(d->kind);
	free(d->addr);
	free(d->off);
	free(d->len);
	free(d->text);
	free(d->target);
	pool_free(&d->pool);
	memset(d, 0, sizeof *d);
}

/* Direct branch / call target of an instruction, from capstone's detail. */
static u64 branch_target(csh h, int arch, const cs_insn *in)
{
	if (!in->detail || !(cs_insn_group(h, in, CS_GRP_JUMP) || cs_insn_group(h, in, CS_GRP_CALL)))
		return UINT64_MAX;
	switch (arch) {
	case BA_X86: {
		const cs_x86 *x = &in->detail->x86;
		for (int k = 0; k < x->op_count; k++)
			if (x->operands[k].type == X86_OP_IMM)
				return (u64)x->operands[k].imm;
		break;
	}
	case BA_AARCH64: {
		const cs_arm64 *a = &in->detail->arm64;
		for (int k = a->op_count; k-- > 0;)
			if (a->operands[k].type == ARM64_OP_IMM)
				return (u64)a->operands[k].imm;
		break;
	}
	case BA_ARM: {
		const cs_arm *a = &in->detail->arm;
		for (int k = a->op_count; k-- > 0;)
			if (a->operands[k].type == ARM_OP_IMM)
				return (u64)(u32)a->operands[k].imm;
		break;
	}
	case BA_PPC: {
		const cs_ppc *p = &in->detail->ppc;
		for (int k = p->op_count; k-- > 0;)
			if (p->operands[k].type == PPC_OP_IMM)
				return (u64)p->operands[k].imm;
		break;
	}
	default:
		break; /* RISC-V / MIPS: immediates are not always absolute; no targets */
	}
	return UINT64_MAX;
}

/* Can this mnemonic be a direct branch or call? (cheap pre-filter) */
static bool maybe_branch(int arch, const char *m)
{
	switch (arch) {
	case BA_X86:
		return m[0] == 'j' || !strncmp(m, "call", 4) || !strncmp(m, "loop", 4) || !strncmp(m, "bnd j", 5) ||
		       !strncmp(m, "notrack", 7);
	case BA_AARCH64:
	case BA_ARM:
		return m[0] == 'b' || !strncmp(m, "cb", 2) || !strncmp(m, "tb", 2);
	case BA_PPC:
		return m[0] == 'b';
	default:
		return false;
	}
}

/* Capstone arch/mode for an image section; false (with err) if unsupported. */
static bool cs_arch_mode(const BinImage *img, u32 sec, cs_arch *arch, cs_mode *mode, char *err, size_t cap)
{
	*mode = 0;
	switch (img->arch) {
	case BA_X86:
		*arch = CS_ARCH_X86;
		*mode = img->bits == 64 ? CS_MODE_64 : img->bits == 16 ? CS_MODE_16 : CS_MODE_32;
		break;
	case BA_ARM:
		*arch = CS_ARCH_ARM;
		*mode = (img->s_flags[sec] & BS_THUMB) ? CS_MODE_THUMB : CS_MODE_ARM;
		break;
	case BA_AARCH64:
		*arch = CS_ARCH_ARM64;
		*mode = CS_MODE_ARM;
		break;
	case BA_RISCV:
		*arch = CS_ARCH_RISCV;
		*mode = (img->bits == 64 ? CS_MODE_RISCV64 : CS_MODE_RISCV32) | CS_MODE_RISCVC;
		break;
	case BA_MIPS:
		*arch = CS_ARCH_MIPS;
		*mode = img->bits == 64 ? CS_MODE_MIPS64 : CS_MODE_MIPS32;
		break;
	case BA_PPC:
		*arch = CS_ARCH_PPC;
		*mode = img->bits == 64 ? CS_MODE_64 : CS_MODE_32;
		break;
	default:
		snprintf(err, cap, "cannot disassemble %s code", img->arch_name && *img->arch_name ? img->arch_name : "this");
		return false;
	}
	if (img->big_endian && *arch != CS_ARCH_X86)
		*mode = (cs_mode)(*mode | CS_MODE_BIG_ENDIAN);
	return true;
}

/* One contiguous range [addr, addr + size) at file offset off. */
typedef struct {
	const BinImage *img;
	cs_arch arch;
	cs_mode mode;
	const u8 *data;
	u64 off, addr, size;
	u64 sec_end; /* labels only inside the section */
	BinDisasm out;
	int rc;
} Chunk;

static void disasm_chunk(Chunk *c)
{
	const BinImage *img = c->img;
	BinDisasm *o = &c->out;
	memset(o, 0, sizeof *o);
	csh h, hd;
	if (cs_open(c->arch, c->mode, &h) != CS_ERR_OK) {
		snprintf(o->err, sizeof o->err, "capstone does not support %s here", img->arch_name);
		c->rc = -1;
		return;
	}
	if (cs_open(c->arch, c->mode, &hd) != CS_ERR_OK) {
		cs_close(&h);
		snprintf(o->err, sizeof o->err, "capstone does not support %s here", img->arch_name);
		c->rc = -1;
		return;
	}
	/* the main pass runs without detail; instructions whose mnemonic can be
	 * a branch are decoded again with detail on for their target */
	if (c->arch == CS_ARCH_X86) {
		cs_option(h, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
		cs_option(hd, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
	}
	cs_option(hd, CS_OPT_DETAIL, CS_OPT_ON);
	cs_option(h, CS_OPT_SKIPDATA, CS_OPT_ON);
	cs_insn *in = cs_malloc(h), *ind = cs_malloc(hd);
	const u8 *code = c->data + c->off;
	size_t left = (size_t)c->size;
	u64 addr = c->addr, a = addr, end = addr + c->size;
	/* first symbol at or after the chunk start */
	u32 lo = 0, hi = img->nsym;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (img->y_addr[mid] < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	u32 si = lo;
	while (left) {
		u64 here = a;
		if (!cs_disasm_iter(h, &code, &left, &a, in))
			break;
		while (si < img->nsym && img->y_addr[si] <= here && img->y_addr[si] < end) {
			row_add(o, BR_LABEL, img->y_addr[si], c->off + (img->y_addr[si] - addr), 0,
			        img->pool.b + img->y_name[si], NULL, UINT64_MAX);
			si++;
		}
		bool data_row = in->id == 0; /* SKIPDATA ".byte" */
		u64 tgt = UINT64_MAX;
		if (!data_row && maybe_branch(img->arch, in->mnemonic)) {
			const u8 *ip = c->data + c->off + (in->address - addr);
			size_t il = in->size;
			u64 ia = in->address;
			if (cs_disasm_iter(hd, &ip, &il, &ia, ind))
				tgt = branch_target(hd, img->arch, ind);
		}
		row_add(o, data_row ? BR_DATA : BR_INSN, in->address, c->off + (in->address - addr), (u8)in->size,
		        in->mnemonic, in->op_str[0] ? in->op_str : NULL, tgt);
	}
	cs_free(in, 1);
	cs_free(ind, 1);
	cs_close(&h);
	cs_close(&hd);
	c->rc = 0;
}

static void *chunk_main(void *arg)
{
	disasm_chunk(arg);
	return NULL;
}

/* Append chunk output to o (rows in order, text pool rebased). */
static void merge(BinDisasm *o, BinDisasm *c)
{
	u32 need = o->n + c->n;
	if (need > o->cap) {
		u32 cap = o->cap ? o->cap : 4096;
		while (cap < need)
			cap *= 2;
		o->kind = vx_realloc(o->kind, cap);
		o->addr = vx_realloc(o->addr, cap * sizeof(u64));
		o->off = vx_realloc(o->off, cap * sizeof(u64));
		o->len = vx_realloc(o->len, cap);
		o->text = vx_realloc(o->text, cap * sizeof(u32));
		o->target = vx_realloc(o->target, cap * sizeof(u64));
		o->cap = cap;
	}
	u32 base = o->pool.n;
	VX_GROW(o->pool.b, o->pool.cap, o->pool.n + c->pool.n + 1);
	if (c->pool.n)
		memcpy(o->pool.b + o->pool.n, c->pool.b, c->pool.n);
	o->pool.n += c->pool.n;
	if (c->n) {
		memcpy(o->kind + o->n, c->kind, c->n);
		memcpy(o->addr + o->n, c->addr, c->n * sizeof(u64));
		memcpy(o->off + o->n, c->off, c->n * sizeof(u64));
		memcpy(o->len + o->n, c->len, c->n);
		memcpy(o->target + o->n, c->target, c->n * sizeof(u64));
		for (u32 i = 0; i < c->n; i++)
			o->text[o->n + i] = c->text[i] + base;
	}
	o->n += c->n;
}

#define MAX_CHUNKS 64
#define CHUNK_BYTES (2u << 20)

int bin_disasm(const BinImage *img, u32 sec, const u8 *data, size_t n, BinDisasm *o)
{
	memset(o, 0, sizeof *o);
	if (sec >= img->ns) {
		snprintf(o->err, sizeof o->err, "no such section");
		return -1;
	}
	cs_arch arch;
	cs_mode mode;
	if (!cs_arch_mode(img, sec, &arch, &mode, o->err, sizeof o->err))
		return -1;
	u64 off = img->s_off[sec], size = img->s_size[sec];
	if (off > n)
		size = 0;
	else if (size > n - off)
		size = n - off;
	u64 addr = img->s_addr[sec];
	/* Split big sections across threads at safe resync points: function
	 * symbols for variable-length encodings, any 4-byte boundary for
	 * fixed-width ones. Each chunk decodes independently. */
	bool fixed4 = img->arch == BA_AARCH64 || img->arch == BA_PPC || img->arch == BA_MIPS ||
	              (img->arch == BA_ARM && !(img->s_flags[sec] & BS_THUMB));
	u64 cut[MAX_CHUNKS + 1];
	u32 nc = 0;
	cut[nc++] = 0;
	if (size > 2 * CHUNK_BYTES) {
		u64 want = VX_MAX((u64)CHUNK_BYTES, size / (MAX_CHUNKS - 1));
		for (u64 next = want; next < size && nc < MAX_CHUNKS; next += want) {
			u64 at = UINT64_MAX;
			if (fixed4) {
				at = next & ~(u64)3;
			} else {
				/* first function symbol at or after addr + next */
				u32 lo = 0, hi = img->nsym;
				while (lo < hi) {
					u32 mid = (lo + hi) / 2;
					if (img->y_addr[mid] < addr + next)
						lo = mid + 1;
					else
						hi = mid;
				}
				while (lo < img->nsym && !img->y_func[lo])
					lo++;
				if (lo < img->nsym && img->y_addr[lo] < addr + size)
					at = img->y_addr[lo] - addr;
			}
			if (at == UINT64_MAX || at <= cut[nc - 1] || at >= size)
				continue;
			cut[nc++] = at;
			next = at;
		}
	}
	cut[nc] = size;
	Chunk *ch = vx_alloc(nc * sizeof(Chunk));
	for (u32 i = 0; i < nc; i++) {
		ch[i].img = img;
		ch[i].arch = arch;
		ch[i].mode = mode;
		ch[i].data = data;
		ch[i].off = off + cut[i];
		ch[i].addr = addr + cut[i];
		ch[i].size = cut[i + 1] - cut[i];
		ch[i].sec_end = addr + size;
	}
	if (nc == 1) {
		disasm_chunk(&ch[0]);
	} else {
		/* a small pool: at most one thread per CPU, chunks handed out in order */
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		u32 nt = (u32)VX_CLAMP(cpus, 1, 16);
		pthread_t th[64];
		for (u32 base = 0; base < nc; base += nt) {
			u32 k = VX_MIN(nt, nc - base), started = 0;
			for (u32 i = 0; i < k; i++)
				if (pthread_create(&th[i], NULL, chunk_main, &ch[base + i]) == 0)
					started |= 1u << i;
				else
					disasm_chunk(&ch[base + i]);
			for (u32 i = 0; i < k; i++)
				if (started & (1u << i))
					pthread_join(th[i], NULL);
		}
	}
	int rc = 0;
	for (u32 i = 0; i < nc; i++) {
		if (ch[i].rc != 0 && !rc) {
			rc = -1;
			snprintf(o->err, sizeof o->err, "%s", ch[i].out.err);
		}
		if (nc == 1) {
			*o = ch[0].out;
		} else {
			merge(o, &ch[i].out);
			bin_disasm_free(&ch[i].out);
		}
	}
	free(ch);
	return rc;
}

/* ---- data inspector -------------------------------------------------------------------- */

static void put(BinInspect *o, const char *name, const char *fmt, ...)
{
	if (o->n >= BIN_INSPECT_MAX)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(o->value[o->n], sizeof o->value[0], fmt, ap);
	va_end(ap);
	o->name[o->n++] = name;
}

static u64 ld(const u8 *p, u32 len, bool be)
{
	u64 v = 0;
	for (u32 i = 0; i < len; i++)
		v |= (u64)p[i] << (be ? (len - 1 - i) * 8 : i * 8);
	return v;
}

static double half_to_double(u16 h)
{
	u32 s = h >> 15, e = (h >> 10) & 0x1f, m = h & 0x3ff;
	double v;
	if (e == 0)
		v = ldexp((double)m, -24);
	else if (e == 31)
		v = m ? NAN : INFINITY;
	else
		v = ldexp((double)(m | 0x400), (int)e - 25);
	return s ? -v : v;
}

static void time_str(char *b, size_t cap, i64 t)
{
	time_t tt = (time_t)t;
	struct tm tm;
	if ((i64)tt != t || !gmtime_r(&tt, &tm)) {
		snprintf(b, cap, "(out of range)");
		return;
	}
	strftime(b, cap, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

void bin_inspect(const u8 *p, size_t avail, bool be, BinInspect *o)
{
	o->n = 0;
	if (!avail)
		return;
	char bits[9];
	for (int i = 0; i < 8; i++)
		bits[i] = (p[0] >> (7 - i)) & 1 ? '1' : '0';
	bits[8] = 0;
	put(o, "Binary (8 bit)", "%s", bits);
	put(o, "Int8", "%d", (int)(i8)p[0]);
	put(o, "UInt8", "%u", (unsigned)p[0]);
	if (avail >= 2) {
		u64 v = ld(p, 2, be);
		put(o, "Int16", "%d", (int)(i16)v);
		put(o, "UInt16", "%u", (unsigned)v);
	}
	if (avail >= 3) {
		u64 v = ld(p, 3, be);
		i32 s = (i32)(v << 8) >> 8;
		put(o, "Int24", "%d", (int)s);
		put(o, "UInt24", "%u", (unsigned)v);
	}
	if (avail >= 4) {
		u64 v = ld(p, 4, be);
		put(o, "Int32", "%d", (int)(i32)(u32)v);
		put(o, "UInt32", "%u", (unsigned)v);
	}
	if (avail >= 8) {
		u64 v = ld(p, 8, be);
		put(o, "Int64", "%lld", (long long)(i64)v);
		put(o, "UInt64", "%llu", (unsigned long long)v);
	}
	/* LEB128 / SLEB128 (always little-endian groups) */
	{
		u64 v = 0;
		u32 sh = 0, k = 0;
		bool done = false;
		while (k < avail && k < 10) {
			u8 c = p[k++];
			if (sh < 64)
				v |= (u64)(c & 0x7f) << sh;
			sh += 7;
			if (!(c & 0x80)) {
				done = true;
				break;
			}
		}
		if (done) {
			put(o, "LEB128", "%llu (%u byte%s)", (unsigned long long)v, k, k == 1 ? "" : "s");
			i64 sv = (i64)v;
			if (sh < 64 && (p[k - 1] & 0x40))
				sv = (i64)(v | (~0ull << sh));
			put(o, "SLEB128", "%lld (%u byte%s)", (long long)sv, k, k == 1 ? "" : "s");
		}
	}
	if (avail >= 2)
		put(o, "Float16 (half)", "%.6g", half_to_double((u16)ld(p, 2, be)));
	if (avail >= 4) {
		u32 u = (u32)ld(p, 4, be);
		float f;
		memcpy(&f, &u, 4);
		put(o, "Float32 (single)", "%.9g", (double)f);
	}
	if (avail >= 8) {
		u64 u = ld(p, 8, be);
		double f;
		memcpy(&f, &u, 8);
		put(o, "Float64 (double)", "%.17g", f);
	}
	/* characters */
	if (p[0] >= 0x20 && p[0] < 0x7f)
		put(o, "Char (ASCII)", "'%c'", p[0]);
	else if (p[0] >= 0xa0)
		put(o, "Char (Latin-1)", "'%c%c' U+%04X", 0xC0 | (p[0] >> 6), 0x80 | (p[0] & 0x3f), p[0]);
	else
		put(o, "Char (ASCII)", "\\x%02X", p[0]);
	{
		u32 cp;
		u32 k = utf8_decode((const char *)p, avail, &cp);
		if (cp != 0xFFFD || (avail >= 3 && p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD))
			put(o, "UTF-8 code point", "U+%04X (%u byte%s)", cp, k, k == 1 ? "" : "s");
		else
			put(o, "UTF-8 code point", "invalid");
	}
	if (avail >= 2) {
		u32 w = (u32)ld(p, 2, be);
		if (w >= 0xD800 && w < 0xDC00 && avail >= 4) {
			u32 w2 = (u32)ld(p + 2, 2, be);
			if (w2 >= 0xDC00 && w2 < 0xE000)
				put(o, "UTF-16 code point", "U+%04X (surrogate pair)", 0x10000 + ((w - 0xD800) << 10) + (w2 - 0xDC00));
			else
				put(o, "UTF-16 code point", "invalid (lone surrogate)");
		} else if (w >= 0xD800 && w < 0xE000) {
			put(o, "UTF-16 code point", "invalid (lone surrogate)");
		} else {
			put(o, "UTF-16 code point", "U+%04X", w);
		}
	}
	if (avail >= 4) {
		/* MS-DOS date/time (time in the low word, date in the high word) */
		u32 v = (u32)ld(p, 4, be);
		u32 t = v & 0xffff, dt = v >> 16;
		put(o, "DOS date/time", "%04u-%02u-%02u %02u:%02u:%02u", 1980 + (dt >> 9), (dt >> 5) & 15, dt & 31, t >> 11,
		    (t >> 5) & 63, (t & 31) * 2);
		char b[64];
		time_str(b, sizeof b, (i64)(i32)v);
		put(o, "time_t (32 bit)", "%s", b);
	}
	if (avail >= 8) {
		char b[64];
		time_str(b, sizeof b, (i64)ld(p, 8, be));
		put(o, "time_t (64 bit)", "%s", b);
	}
	if (avail >= 16) {
		/* GUID: the first three fields follow the byte order, the rest is raw */
		put(o, "GUID", "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (u32)ld(p, 4, be), (u32)ld(p + 4, 2, be),
		    (u32)ld(p + 6, 2, be), p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}
}
