/* Binary files: detection, executable images (ELF, PE/COFF, Mach-O),
 * disassembly through capstone, and the value decoders behind the hex
 * editor's data inspector. Read-only; every offset and size read from a
 * file is bounds-checked against the buffer (hostile files are expected). */
#ifndef VX_BINFILE_H
#define VX_BINFILE_H

#include "base.h"

/* Hex view or text view? true when the head of a file looks binary:
 *  - a known binary magic number (ELF, MZ/PE, Mach-O and fat Mach-O, ar
 *    archives, zip/jar, png, jpeg, gif, bmp, webp/riff, gzip, bzip2, xz,
 *    zstd, 7z, rar, lz4, pdf, sqlite, wasm, Java class, ico, tiff, ogg,
 *    flac, mp3/id3, mp4/mov "ftyp", ttf/otf/woff), or
 *  - a NUL byte in the first 8 KiB, or
 *  - more than 10% of the bytes are control characters other than
 *    \t \n \v \f \r and ESC, or
 *  - more than 10% of the bytes belong to invalid UTF-8 sequences.
 * An empty file is text. */
bool bin_is_binary(const char *head, size_t n);

/* ---- executable images ------------------------------------------------------ */

enum { BF_NONE, BF_ELF, BF_PE, BF_MACHO };
enum { BA_UNKNOWN, BA_X86, BA_ARM, BA_AARCH64, BA_RISCV, BA_MIPS, BA_PPC };

/* Section flags. Only sections with bytes in the file are listed
 * (ELF SHT_NOBITS, Mach-O zerofill and empty sections are left out). */
enum {
	BS_CODE = 1,          /* executable instructions */
	BS_DATA = 2,          /* everything else */
	BS_READONLY = 4,
	BS_WRITABLE = 8,
	BS_HAS_FILE_DATA = 16, /* always set for listed sections */
	BS_SEGMENT = 32,       /* ELF without section headers: a PT_LOAD segment */
	BS_THUMB = 64,         /* 32-bit ARM code in Thumb mode */
};

typedef struct {
	int format;              /* BF_* */
	const char *format_name; /* "ELF64", "PE32+", "Mach-O 64", ... */
	int arch;                /* BA_* */
	const char *arch_name;   /* "x86-64", "AArch64", ... */
	u32 bits;                /* 16, 32 or 64 (instruction set width) */
	bool big_endian;
	u64 entry;      /* virtual address, 0 when unknown */
	u64 image_base; /* PE ImageBase, Mach-O __TEXT vmaddr, ELF lowest PT_LOAD vaddr */
	u64 slice_off;  /* fat Mach-O: offset of the chosen slice (offsets below are absolute) */

	/* sections (SoA) */
	u32 ns, ns_cap;
	u32 *s_name; /* pool offset */
	u64 *s_off, *s_size, *s_addr;
	u32 *s_flags;

	/* symbols (SoA), sorted by address, duplicates removed */
	u32 nsym, sym_cap;
	u32 *y_name; /* pool offset */
	u64 *y_addr, *y_size;
	u8 *y_func;

	Pool pool;
	char err[128];
} BinImage;

/* 0: an executable/object image was recognised and parsed; 1: not an
 * executable format (show the whole file in the hex editor); -1: a known
 * format but malformed (err says why; what could be read is kept). */
int bin_parse(const u8 *data, size_t n, BinImage *out);
void bin_free(BinImage *img);
static inline const char *bin_str(const BinImage *img, u32 off) { return img->pool.b + off; }
/* Symbol containing addr (or the nearest one before it, within 1 MiB);
 * UINT32_MAX when none. *delta = addr - symbol address. */
u32 bin_symbol_at(const BinImage *img, u64 addr, u64 *delta);

/* ---- disassembly --------------------------------------------------------------- */

enum { BR_INSN, BR_LABEL, BR_DATA };

/* Rows: instructions, symbol labels before the address they name, and
 * ".byte" rows for undecodable bytes. Five bytes a row (coalesced SoA):
 * where the row is and what it is; the text and the branch target are
 * decoded again when a row is shown (bin_row_text), which costs a few
 * microseconds for a screenful and saves ~40 bytes a row (a 2 MB .text
 * has ~500k rows). */
typedef struct {
	u32 n, cap;
	u32 sec;      /* the section disassembled */
	u64 base;     /* its virtual address */
	u64 base_off; /* its file offset */
	u32 *rel;     /* instruction / data: address - base; label: symbol index */
	u8 *kl;       /* kind << 4 | length in bytes (0 for labels; x86 tops out at 15) */
	size_t cs, csd; /* capstone handles for bin_row_text (plain / with detail), opened on first use */
	char err[128];
} BinDisasm;

/* Disassemble section `section` of img (data/n is the whole file).
 * 0 on success; -1 with err for unsupported architectures. */
int bin_disasm(const BinImage *img, u32 section, const u8 *data, size_t n, BinDisasm *out);
void bin_disasm_free(BinDisasm *d);

static inline u32 bin_row_kind(const BinDisasm *d, u32 r) { return d->kl[r] >> 4; }
static inline u32 bin_row_len(const BinDisasm *d, u32 r) { return d->kl[r] & 15; }
static inline u64 bin_row_addr(const BinImage *img, const BinDisasm *d, u32 r)
{
	return bin_row_kind(d, r) == BR_LABEL ? img->y_addr[d->rel[r]] : d->base + d->rel[r];
}
/* file offset of an instruction / data row */
static inline u64 bin_row_off(const BinDisasm *d, u32 r) { return d->base_off + d->rel[r]; }
/* "mov rax, qword ptr [rsp + 8]", ".byte 0x0f" or a label's symbol name,
 * and the row's direct branch / call target (UINT64_MAX when none; target
 * may be NULL). */
void bin_row_text(const BinImage *img, BinDisasm *d, const u8 *data, size_t n, u32 r, char *buf, size_t cap,
                  u64 *target);

/* ---- data inspector -------------------------------------------------------------- */

#define BIN_INSPECT_MAX 32

/* Values of the bytes at p (avail bytes available), like HxD's data
 * inspector; fields that need more bytes than available are omitted. */
typedef struct {
	u32 n;
	const char *name[BIN_INSPECT_MAX];
	char value[BIN_INSPECT_MAX][96];
} BinInspect;

void bin_inspect(const u8 *p, size_t avail, bool big_endian, BinInspect *out);

#endif
