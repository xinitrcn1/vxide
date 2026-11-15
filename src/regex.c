/* RE2-style regex: parse -> AST (SoA) -> byte-level program (SoA) ->
 * lazy DFA (earliest match end) + Pike VM (leftmost-first submatches). */
#include "regex.h"

#include <ctype.h>
#include <stdio.h>

/* ---- program ---------------------------------------------------------------- */

enum {
	OP_BYTES,  /* a = byte-set index: consume one byte in the set */
	OP_SPLIT,  /* prefer a, then b */
	OP_JMP,    /* a */
	OP_SAVE,   /* a = capture slot */
	OP_BOL,    /* empty-width assertions */
	OP_EOL,
	OP_WORDB,
	OP_NWORDB,
	OP_MATCH,
};

#define MAX_INSTR 100000
#define MAX_NODES 100000
#define MAX_REPEAT 1000
#define DFA_MAX_STATES 4096
#define DFA_UNKNOWN 0xFFFFFFFFu
#define DFA_DEAD 0xFFFFFFFEu
#define DFA_MATCH_BIT 0x80000000u

struct Regex {
	/* program (SoA) */
	u8 *op;
	u32 *a, *b;
	u32 n, cap;
	/* byte sets, 32 bytes each, deduplicated by linear search */
	u8 (*sets)[32];
	u32 nsets, sets_cap;
	u32 groups;
	char names[RE_MAX_CAPS][32];
	int flags;
	bool multiline;
	int first_byte; /* literal first byte, or -1 */

	/* lazy DFA: state = sorted kernel pcs + context flags */
	u32 nst, st_cap;
	u32 *st_off, *st_len; /* into kpool */
	u8 *st_flags;         /* bit0 prev is word, bit1 prev is line start */
	u32 *trans;           /* nst * 256: next state | DFA_MATCH_BIT if MATCH before the byte */
	u8 *st_eof;           /* 0 unknown, 1 no match, 2 match at end */
	u32 *kpool;
	u32 kn, kcap;
	u32 *hash; /* open addressing over states */
	u32 hash_cap;

	/* scratch for closures / the Pike VM */
	u32 *sparse, *dense;
	u32 *stack;
	u32 stack_cap;
};

#define PROG_COLS(re) SOA((re)->a), SOA((re)->b), SOA((re)->op)
/* trans holds 256 entries per state: one 1 KiB row element */
#define STATE_COLS(re)                                                                                   \
	SOA((re)->st_off), SOA((re)->st_len), ((SoaCol){(void **)&(re)->trans, 256 * (u32)sizeof(u32)}), \
	    SOA((re)->st_flags), SOA((re)->st_eof)

/* ---- parser / AST --------------------------------------------------------- */

enum {
	N_EMPTY,
	N_CHAR,  /* a = code point */
	N_CLASS, /* ranges [b, b + a) in the parser range pool, neg in flag */
	N_CAT,
	N_ALT,
	N_REPEAT, /* a = min, b = max (UINT32_MAX inf) */
	N_GROUP,  /* a = capture index (0 none) */
	N_BOL,
	N_EOL,
	N_WORDB,
	N_NWORDB,
};

typedef struct {
	u8 *kind, *flag;
	u32 *a, *b, *first, *next;
	u32 n, cap;
	u32 *rlo, *rhi; /* class ranges */
	u32 nr, rcap;
	const char *p;
	size_t i, len;
	Regex *re;
	char *err;
	size_t errcap;
	bool failed;
	int flags;
} Parser;

static void fail(Parser *P, const char *fmt, const char *arg, size_t argn)
{
	if (!P->failed && P->err && P->errcap) {
		char a[64];
		snprintf(a, sizeof a, "%.*s", (int)VX_MIN(argn, sizeof a - 1), arg ? arg : "");
		snprintf(P->err, P->errcap, fmt, a);
	}
	P->failed = true;
}

#define RANGE_COLS(P) SOA((P)->rlo), SOA((P)->rhi)
#define NODE_COLS(P) SOA((P)->a), SOA((P)->b), SOA((P)->first), SOA((P)->next), SOA((P)->kind), SOA((P)->flag)

static u32 node(Parser *P, u8 kind, u32 a, u32 b)
{
	if (P->n >= MAX_NODES) {
		fail(P, "pattern too large - compile failed%s", NULL, 0);
		return 0;
	}
	if (P->n == P->cap) {
		SoaCol c[] = {NODE_COLS(P)};
		soa_grow(c, 6, &P->cap, VX_MAX(P->n + 1, 64u), P->n);
	}
	u32 i = P->n++;
	P->kind[i] = kind;
	P->flag[i] = 0;
	P->a[i] = a;
	P->b[i] = b;
	P->first[i] = P->next[i] = UINT32_MAX;
	return i;
}

static void range(Parser *P, u32 lo, u32 hi)
{
	if (P->nr == P->rcap) {
		SoaCol c[] = {RANGE_COLS(P)};
		soa_grow(c, 2, &P->rcap, VX_MAX(P->nr + 1, 64u), P->nr);
	}
	P->rlo[P->nr] = lo;
	P->rhi[P->nr] = hi;
	P->nr++;
}

static bool at_end(Parser *P) { return P->i >= P->len; }
static char peek(Parser *P) { return at_end(P) ? 0 : P->p[P->i]; }

static int hexv(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Perl classes are ASCII in RE2. */
static void perl_class(Parser *P, char c)
{
	switch (c) {
	case 'd':
		range(P, '0', '9');
		break;
	case 'D':
		range(P, 0, '0' - 1);
		range(P, '9' + 1, 0x10FFFF);
		break;
	case 'w':
		range(P, '0', '9');
		range(P, 'A', 'Z');
		range(P, '_', '_');
		range(P, 'a', 'z');
		break;
	case 'W':
		range(P, 0, '0' - 1);
		range(P, '9' + 1, 'A' - 1);
		range(P, 'Z' + 1, '_' - 1);
		range(P, '_' + 1, 'a' - 1);
		range(P, 'z' + 1, 0x10FFFF);
		break;
	case 's':
		range(P, '\t', '\n');
		range(P, '\f', '\r');
		range(P, ' ', ' ');
		break;
	case 'S':
		range(P, 0, '\t' - 1);
		range(P, '\v', '\v');
		range(P, '\r' + 1, ' ' - 1);
		range(P, ' ' + 1, 0x10FFFF);
		break;
	}
}

/* Escape after '\' producing a code point; UINT32_MAX on error. */
static u32 escape_cp(Parser *P, char c, size_t at)
{
	switch (c) {
	case 't':
		return '\t';
	case 'n':
		P->re->multiline = true;
		return '\n';
	case 'r':
		return '\r';
	case 'f':
		return '\f';
	case 'v':
		return '\v';
	case 'a':
		return 7;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7': {
		/* RE2: \0 and octal \1-\7 only with a leading 0 ... backrefs are errors */
		if (c != '0') {
			fail(P, "invalid escape sequence: %s", P->p + at, 2);
			return UINT32_MAX;
		}
		u32 v = 0;
		for (int k = 0; k < 2 && peek(P) >= '0' && peek(P) <= '7'; k++) {
			v = v * 8 + (u32)(peek(P) - '0');
			P->i++;
		}
		return v;
	}
	case 'x': {
		u32 v = 0;
		if (peek(P) == '{') {
			P->i++;
			int nd = 0;
			while (!at_end(P) && peek(P) != '}') {
				int h = hexv(peek(P));
				if (h < 0 || ++nd > 6) {
					fail(P, "invalid escape sequence: %s", P->p + at, P->i - at + 1);
					return UINT32_MAX;
				}
				v = v * 16 + (u32)h;
				P->i++;
			}
			if (at_end(P) || v > 0x10FFFF) {
				fail(P, "invalid escape sequence: %s", P->p + at, P->i - at);
				return UINT32_MAX;
			}
			P->i++;
			return v;
		}
		for (int k = 0; k < 2; k++) {
			int h = hexv(peek(P));
			if (h < 0) {
				fail(P, "invalid escape sequence: %s", P->p + at, P->i - at + 1);
				return UINT32_MAX;
			}
			v = v * 16 + (u32)h;
			P->i++;
		}
		return v;
	}
	default:
		/* punctuation may be escaped; letters/digits not listed are errors */
		if ((u8)c < 0x80 && !isalnum((u8)c))
			return (u8)c;
		fail(P, "invalid escape sequence: %s", P->p + at, 2);
		return UINT32_MAX;
	}
}

static u32 read_cp(Parser *P)
{
	u32 cp;
	u32 k = utf8_decode(P->p + P->i, P->len - P->i, &cp);
	P->i += k ? k : 1;
	return cp;
}

/* [[:alpha:]] style names (RE2 supports them, ASCII). */
static bool posix_class(Parser *P)
{
	static const struct {
		const char *name;
		const char *spec; /* pairs lo hi */
	} tab[] = {
	    {"alnum", "09AZaz"}, {"alpha", "AZaz"}, {"ascii", "\x01\x7f"}, {"blank", "\t\t  "},
	    {"cntrl", "\x01\x1f\x7f\x7f"}, {"digit", "09"}, {"graph", "!~"}, {"lower", "az"},
	    {"print", " ~"}, {"punct", "!/:@[`{~"}, {"space", "\t\r  "}, {"upper", "AZ"},
	    {"word", "09AZ__az"}, {"xdigit", "09AFaf"},
	};
	if (P->i + 1 >= P->len || P->p[P->i] != '[' || P->p[P->i + 1] != ':')
		return false;
	const char *end = memchr(P->p + P->i + 2, ']', P->len - P->i - 2);
	if (!end || end[-1] != ':')
		return false;
	size_t s = P->i + 2;
	bool neg = s < P->len && P->p[s] == '^';
	if (neg)
		s++;
	size_t nl = (size_t)(end - 1 - (P->p + s));
	for (size_t k = 0; k < VX_COUNT(tab); k++)
		if (strlen(tab[k].name) == nl && !memcmp(tab[k].name, P->p + s, nl)) {
			const char *sp = tab[k].spec;
			size_t sl = strlen(sp);
			if (!strcmp(tab[k].name, "ascii"))
				sl = 2;
			if (!neg) {
				for (size_t q = 0; q + 1 < sl + (sp[0] == 1); q += 2)
					range(P, sp[q] == 1 ? 0 : (u8)sp[q], (u8)sp[q + 1]);
			} else {
				u32 prev = 0;
				for (size_t q = 0; q + 1 < sl; q += 2) {
					u32 lo = sp[q] == 1 ? 0 : (u8)sp[q];
					if (lo > prev)
						range(P, prev, lo - 1);
					prev = (u8)sp[q + 1] + 1u;
				}
				range(P, prev, 0x10FFFF);
			}
			P->i = (size_t)(end - P->p) + 1;
			return true;
		}
	fail(P, "invalid character class range: %s", P->p + P->i, (size_t)(end - (P->p + P->i)) + 1);
	return true;
}

static u32 parse_class(Parser *P, size_t open)
{
	bool neg = false;
	if (peek(P) == '^') {
		neg = true;
		P->i++;
	}
	u32 first = P->nr;
	bool start = true;
	/* ']' right after '[' or '[^' is a literal, as in RE2 */
	while (!at_end(P) && (peek(P) != ']' || start)) {
		start = false;
		if (posix_class(P)) {
			if (P->failed)
				return 0;
			continue;
		}
		u32 lo;
		if (peek(P) == '\\') {
			size_t at = P->i;
			P->i++;
			if (at_end(P)) {
				fail(P, "trailing \\%s", NULL, 0);
				return 0;
			}
			char e = P->p[P->i++];
			if (strchr("dDwWsS", e)) {
				perl_class(P, e);
				continue;
			}
			if (e == 'p' || e == 'P') {
				fail(P, "invalid character class range: %s", P->p + at, 3);
				return 0;
			}
			lo = escape_cp(P, e, at);
			if (P->failed)
				return 0;
		} else {
			lo = read_cp(P);
		}
		u32 hi = lo;
		if (peek(P) == '-' && P->i + 1 < P->len && P->p[P->i + 1] != ']') {
			size_t at = P->i;
			P->i++;
			if (peek(P) == '\\') {
				size_t at2 = P->i;
				P->i++;
				char e = P->p[P->i++];
				hi = escape_cp(P, e, at2);
				if (P->failed)
					return 0;
			} else {
				hi = read_cp(P);
			}
			if (hi < lo) {
				fail(P, "invalid character class range: %s", P->p + at - 1, P->i - at + 1);
				return 0;
			}
		}
		range(P, lo, hi);
	}
	if (at_end(P)) {
		fail(P, "missing closing ]: %s", P->p + open, P->len - open);
		return 0;
	}
	P->i++;
	u32 n = node(P, N_CLASS, P->nr - first, first);
	P->flag[n] = neg;
	return n;
}

static u32 parse_alt(Parser *P);

static u32 parse_atom(Parser *P)
{
	size_t at = P->i;
	char c = peek(P);
	switch (c) {
	case '(': {
		P->i++;
		u32 cap = 0;
		bool capture = true;
		if (peek(P) == '?') {
			P->i++;
			char k = peek(P);
			if (k == ':') {
				P->i++;
				capture = false;
			} else if (k == 'P' || k == '<') {
				if (k == 'P')
					P->i++;
				if (peek(P) != '<') {
					fail(P, "invalid named capture group: %s", P->p + at, P->i - at + 1);
					return 0;
				}
				if (P->i + 1 < P->len && (P->p[P->i + 1] == '=' || P->p[P->i + 1] == '!')) {
					fail(P, "invalid perl operator: %s", P->p + at, P->i - at + 2);
					return 0;
				}
				P->i++;
				size_t s = P->i;
				while (!at_end(P) && (isalnum((u8)peek(P)) || peek(P) == '_'))
					P->i++;
				if (peek(P) != '>' || P->i == s) {
					fail(P, "invalid named capture group: %s", P->p + at, P->i - at + 1);
					return 0;
				}
				if (P->re->groups < RE_MAX_CAPS)
					snprintf(P->re->names[P->re->groups], sizeof P->re->names[0], "%.*s", (int)(P->i - s),
					         P->p + s);
				P->i++;
			} else if (k == 'i' || k == 's' || k == 'm' || k == 'U' || k == '-') {
				/* flag groups: accept (?i) / (?i:...) for case-insensitivity */
				bool on = true, icase = false;
				while (!at_end(P) && peek(P) != ')' && peek(P) != ':') {
					char f = peek(P);
					if (f == '-')
						on = false;
					else if (f == 'i')
						icase = on;
					else if (f != 's' && f != 'm' && f != 'U') {
						fail(P, "invalid or unsupported Perl syntax: %s", P->p + at, P->i - at + 1);
						return 0;
					}
					P->i++;
				}
				if (icase)
					P->flags |= RE_ICASE;
				else if (!on)
					P->flags &= ~RE_ICASE;
				if (peek(P) == ')') {
					P->i++;
					return node(P, N_EMPTY, 0, 0);
				}
				P->i++;
				capture = false;
			} else {
				/* lookaround and other Perl extensions: RE2 rejects them */
				fail(P, "invalid perl operator: %s", P->p + at, P->i - at + 1);
				return 0;
			}
		}
		if (capture) {
			if (P->re->groups < RE_MAX_CAPS)
				cap = P->re->groups++;
		}
		u32 inner = parse_alt(P);
		if (P->failed)
			return 0;
		if (peek(P) != ')') {
			fail(P, "missing closing ): %s", P->p + at, P->len - at);
			return 0;
		}
		P->i++;
		u32 n = node(P, N_GROUP, cap, 0);
		P->first[n] = inner;
		return n;
	}
	case '[':
		P->i++;
		return parse_class(P, at);
	case '.': {
		P->i++;
		/* any code point except line ends */
		u32 first = P->nr;
		range(P, '\n', '\n');
		range(P, '\r', '\r');
		u32 n = node(P, N_CLASS, P->nr - first, first);
		P->flag[n] = 1;
		return n;
	}
	case '^':
		P->i++;
		return node(P, N_BOL, 0, 0);
	case '$':
		P->i++;
		return node(P, N_EOL, 0, 0);
	case '\\': {
		P->i++;
		if (at_end(P)) {
			fail(P, "trailing \\%s", NULL, 0);
			return 0;
		}
		char e = P->p[P->i++];
		if (strchr("dDwWsS", e)) {
			u32 first = P->nr;
			perl_class(P, e);
			return node(P, N_CLASS, P->nr - first, first);
		}
		if (e == 'b')
			return node(P, N_WORDB, 0, 0);
		if (e == 'B')
			return node(P, N_NWORDB, 0, 0);
		if (e == 'A')
			return node(P, N_BOL, 1, 0);
		if (e == 'z')
			return node(P, N_EOL, 1, 0);
		if (e == 'p' || e == 'P') {
			fail(P, "invalid character class range: %s", P->p + at, 3);
			return 0;
		}
		if (e == 'Q') {
			/* \Q...\E literal text */
			size_t s = P->i;
			const char *end = NULL;
			for (size_t k = s; k + 1 < P->len; k++)
				if (P->p[k] == '\\' && P->p[k + 1] == 'E') {
					end = P->p + k;
					break;
				}
			size_t stop = end ? (size_t)(end - P->p) : P->len;
			u32 cat = node(P, N_CAT, 0, 0);
			u32 last = UINT32_MAX;
			while (P->i < stop) {
				u32 ch = node(P, N_CHAR, read_cp(P), 0);
				if (last == UINT32_MAX)
					P->first[cat] = ch;
				else
					P->next[last] = ch;
				last = ch;
			}
			P->i = end ? stop + 2 : stop;
			return cat;
		}
		u32 cp = escape_cp(P, e, at);
		if (P->failed)
			return 0;
		return node(P, N_CHAR, cp, 0);
	}
	case '*':
	case '+':
	case '?':
		fail(P, "missing argument to repetition operator: %s", P->p + at, 1);
		return 0;
	default:
		if (c == '\n')
			P->re->multiline = true;
		return node(P, N_CHAR, read_cp(P), 0);
	}
}

static bool parse_braces(Parser *P, u32 *mn, u32 *mx)
{
	size_t save = P->i;
	P->i++;
	u32 a = 0, b;
	bool digits = false;
	while (isdigit((u8)peek(P)) && a <= 100000) {
		a = a * 10 + (u32)(peek(P) - '0');
		P->i++;
		digits = true;
	}
	if (!digits) {
		P->i = save;
		return false;
	}
	b = a;
	if (peek(P) == ',') {
		P->i++;
		b = UINT32_MAX;
		if (isdigit((u8)peek(P))) {
			b = 0;
			while (isdigit((u8)peek(P)) && b <= 100000) {
				b = b * 10 + (u32)(peek(P) - '0');
				P->i++;
			}
		}
	}
	if (peek(P) != '}') {
		P->i = save; /* RE2: not a repetition, '{' is literal */
		return false;
	}
	P->i++;
	if (a > MAX_REPEAT || (b != UINT32_MAX && (b > MAX_REPEAT || b < a))) {
		fail(P, "bad repetition operator: %s", P->p + save, P->i - save);
		return false;
	}
	*mn = a;
	*mx = b;
	return true;
}

static u32 parse_cat(Parser *P)
{
	u32 cat = node(P, N_CAT, 0, 0);
	u32 last = UINT32_MAX;
	while (!at_end(P) && !P->failed && peek(P) != '|' && peek(P) != ')') {
		size_t at = P->i;
		u32 atom;
		if (peek(P) == '{') {
			P->i++;
			atom = node(P, N_CHAR, '{', 0);
		} else {
			atom = parse_atom(P);
		}
		if (P->failed)
			return cat;
		bool repeated = false;
		for (;;) {
			char q = peek(P);
			size_t qat = P->i;
			u32 mn, mx;
			if (q == '*') {
				mn = 0;
				mx = UINT32_MAX;
				P->i++;
			} else if (q == '+') {
				mn = 1;
				mx = UINT32_MAX;
				P->i++;
			} else if (q == '?') {
				mn = 0;
				mx = 1;
				P->i++;
			} else if (q == '{' && parse_braces(P, &mn, &mx)) {
			} else {
				if (P->failed)
					return cat;
				break;
			}
			if (repeated) {
				/* RE2 rejects a** (bad repetition operator) */
				fail(P, "bad repetition operator: %s", P->p + at + (qat - at) - 1, 2);
				return cat;
			}
			repeated = true;
			u32 r = node(P, N_REPEAT, mn, mx);
			P->first[r] = atom;
			P->flag[r] = 1; /* greedy */
			if (peek(P) == '?') {
				P->flag[r] = 0;
				P->i++;
			}
			atom = r;
		}
		if (last == UINT32_MAX)
			P->first[cat] = atom;
		else
			P->next[last] = atom;
		last = atom;
	}
	return cat;
}

static u32 parse_alt(Parser *P)
{
	u32 first = parse_cat(P);
	if (peek(P) != '|')
		return first;
	u32 alt = node(P, N_ALT, 0, 0);
	P->first[alt] = first;
	u32 last = first;
	while (peek(P) == '|' && !P->failed) {
		P->i++;
		u32 c = parse_cat(P);
		P->next[last] = c;
		last = c;
	}
	return alt;
}

/* ---- code generation ---------------------------------------------------- */

static u32 emit(Parser *P, u8 op, u32 a, u32 b)
{
	Regex *re = P->re;
	if (re->n >= MAX_INSTR) {
		fail(P, "pattern too large - compile failed%s", NULL, 0);
		return 0;
	}
	if (re->n == re->cap) {
		SoaCol c[] = {PROG_COLS(re)};
		soa_grow(c, 3, &re->cap, VX_MAX(re->n + 1, 64u), re->n);
	}
	u32 i = re->n++;
	re->op[i] = op;
	re->a[i] = a;
	re->b[i] = b;
	return i;
}

static u32 set_index(Regex *re, const u8 set[32])
{
	for (u32 i = 0; i < re->nsets; i++)
		if (!memcmp(re->sets[i], set, 32))
			return i;
	if (re->nsets == re->sets_cap) {
		re->sets_cap = re->sets_cap ? re->sets_cap * 2 : 16;
		re->sets = vx_realloc(re->sets, re->sets_cap * 32);
	}
	memcpy(re->sets[re->nsets], set, 32);
	return re->nsets++;
}

static void set_add(u8 set[32], u32 lo, u32 hi)
{
	for (u32 c = lo; c <= hi && c < 256; c++)
		set[c >> 3] |= (u8)(1u << (c & 7));
}

static u32 emit_bytes(Parser *P, u32 lo, u32 hi)
{
	u8 set[32] = {0};
	set_add(set, lo, hi);
	return emit(P, OP_BYTES, set_index(P->re, set), 0);
}

/* Normalise a class to sorted, merged code point ranges (applying case
 * folding, negation, line mode). Writes into lo/hi, returns count. */
static u32 class_ranges(Parser *P, u32 n, u32 **olo, u32 **ohi)
{
	u32 cnt = P->a[n], first = P->b[n];
	u32 cap = cnt * 2 + 8, k = 0;
	u32 *lo = vx_alloc(cap * sizeof(u32)), *hi = vx_alloc(cap * sizeof(u32));
	for (u32 i = 0; i < cnt; i++) {
		lo[k] = P->rlo[first + i];
		hi[k] = P->rhi[first + i];
		k++;
		if (P->flags & RE_ICASE) {
			u32 a = VX_MAX(P->rlo[first + i], (u32)'a'), b = VX_MIN(P->rhi[first + i], (u32)'z');
			if (a <= b) {
				lo[k] = a - 32;
				hi[k] = b - 32;
				k++;
			}
			a = VX_MAX(P->rlo[first + i], (u32)'A');
			b = VX_MIN(P->rhi[first + i], (u32)'Z');
			if (a <= b) {
				lo[k] = a + 32;
				hi[k] = b + 32;
				k++;
			}
		}
	}
	/* sort + merge */
	for (u32 i = 1; i < k; i++) {
		u32 l = lo[i], h = hi[i], j = i;
		while (j > 0 && lo[j - 1] > l) {
			lo[j] = lo[j - 1];
			hi[j] = hi[j - 1];
			j--;
		}
		lo[j] = l;
		hi[j] = h;
	}
	u32 m = 0;
	for (u32 i = 0; i < k; i++) {
		if (m && lo[i] <= hi[m - 1] + 1) {
			if (hi[i] > hi[m - 1])
				hi[m - 1] = hi[i];
		} else {
			lo[m] = lo[i];
			hi[m] = hi[i];
			m++;
		}
	}
	if (P->flag[n]) { /* negate */
		u32 *nlo = vx_alloc((m + 1) * sizeof(u32)), *nhi = vx_alloc((m + 1) * sizeof(u32));
		u32 q = 0, prev = 0;
		for (u32 i = 0; i < m; i++) {
			if (lo[i] > prev) {
				nlo[q] = prev;
				nhi[q] = lo[i] - 1;
				q++;
			}
			prev = hi[i] + 1;
		}
		if (prev <= 0x10FFFF) {
			nlo[q] = prev;
			nhi[q] = 0x10FFFF;
			q++;
		}
		free(lo);
		free(hi);
		lo = nlo;
		hi = nhi;
		m = q;
	}
	/* line mode: classes never match a line end (VSCodium search) */
	if (!P->re->multiline) {
		u32 *nlo = vx_alloc((m * 3 + 1) * sizeof(u32)), *nhi = vx_alloc((m * 3 + 1) * sizeof(u32));
		u32 q = 0;
		for (u32 i = 0; i < m; i++) {
			u32 a = lo[i], b = hi[i];
			/* cut out 0x0A and 0x0D */
			const u32 cuts[2] = {'\n', '\r'};
			u32 s = a;
			for (int c = 0; c < 2; c++)
				if (cuts[c] >= s && cuts[c] <= b) {
					if (cuts[c] > s) {
						nlo[q] = s;
						nhi[q] = cuts[c] - 1;
						q++;
					}
					s = cuts[c] + 1;
				}
			if (s <= b) {
				nlo[q] = s;
				nhi[q] = b;
				q++;
			}
		}
		free(lo);
		free(hi);
		lo = nlo;
		hi = nhi;
		m = q;
	}
	*olo = lo;
	*ohi = hi;
	return m;
}

/* UTF-8 byte-range sequences for a code point range (Russ Cox's split). */
typedef struct {
	u8 lo[4], hi[4];
	u8 n;
} Seq;

static void utf8_split(u32 lo, u32 hi, Seq *out, u32 *nout, u32 cap)
{
	if (lo > hi || *nout >= cap)
		return;
	/* skip surrogates */
	if (lo <= 0xDFFF && hi >= 0xD800) {
		if (lo < 0xD800)
			utf8_split(lo, 0xD7FF, out, nout, cap);
		if (hi > 0xDFFF)
			utf8_split(0xE000, hi, out, nout, cap);
		return;
	}
	static const u32 maxes[3] = {0x7F, 0x7FF, 0xFFFF};
	for (int i = 0; i < 3; i++)
		if (lo <= maxes[i] && hi > maxes[i]) {
			utf8_split(lo, maxes[i], out, nout, cap);
			utf8_split(maxes[i] + 1, hi, out, nout, cap);
			return;
		}
	if (hi < 0x80) {
		Seq s = {{(u8)lo}, {(u8)hi}, 1};
		out[(*nout)++] = s;
		return;
	}
	for (int i = 1; i < 4; i++) {
		u32 m = (1u << (6 * i)) - 1;
		if ((lo & ~m) != (hi & ~m)) {
			if ((lo & m) != 0) {
				utf8_split(lo, lo | m, out, nout, cap);
				utf8_split((lo | m) + 1, hi, out, nout, cap);
				return;
			}
			if ((hi & m) != m) {
				utf8_split(lo, (hi & ~m) - 1, out, nout, cap);
				utf8_split(hi & ~m, hi, out, nout, cap);
				return;
			}
		}
	}
	char a[4], b[4];
	u32 na = utf8_encode(lo, a);
	utf8_encode(hi, b);
	Seq s;
	s.n = (u8)na;
	for (u32 k = 0; k < na; k++) {
		s.lo[k] = (u8)a[k];
		s.hi[k] = (u8)b[k];
	}
	out[(*nout)++] = s;
}

static void gen_class(Parser *P, u32 n)
{
	u32 *lo, *hi;
	u32 m = class_ranges(P, n, &lo, &hi);
	/* ASCII part as one byte set */
	u8 ascii[32] = {0};
	bool has_ascii = false;
	Seq seqs[512];
	u32 ns = 0;
	for (u32 i = 0; i < m; i++) {
		if (lo[i] < 0x80) {
			set_add(ascii, lo[i], VX_MIN(hi[i], 0x7Fu));
			has_ascii = true;
		}
		if (hi[i] >= 0x80)
			utf8_split(VX_MAX(lo[i], 0x80u), hi[i], seqs, &ns, VX_COUNT(seqs));
	}
	free(lo);
	free(hi);
	u32 alts = (has_ascii ? 1 : 0) + ns;
	if (alts == 0) {
		/* empty class never matches */
		u8 none[32] = {0};
		emit(P, OP_BYTES, set_index(P->re, none), 0);
		return;
	}
	Regex *re = P->re;
	u32 jumps[513];
	u32 nj = 0;
	u32 k = 0;
	for (u32 alt = 0; alt < alts; alt++) {
		bool last = alt + 1 == alts;
		u32 sp = 0;
		if (!last) {
			sp = emit(P, OP_SPLIT, 0, 0);
			re->a[sp] = re->n;
		}
		if (alt == 0 && has_ascii) {
			emit(P, OP_BYTES, set_index(re, ascii), 0);
		} else {
			Seq *s = &seqs[k++];
			for (u32 b = 0; b < s->n; b++)
				emit_bytes(P, s->lo[b], s->hi[b]);
		}
		if (!last) {
			jumps[nj++] = emit(P, OP_JMP, 0, 0);
			re->b[sp] = re->n;
		}
		if (P->failed)
			return;
	}
	for (u32 j = 0; j < nj; j++)
		re->a[jumps[j]] = re->n;
}

static void gen(Parser *P, u32 n);

static void gen_star(Parser *P, u32 child, bool greedy)
{
	Regex *re = P->re;
	u32 loop = emit(P, OP_SPLIT, 0, 0);
	u32 body = re->n;
	gen(P, child);
	emit(P, OP_JMP, loop, 0);
	u32 after = re->n;
	re->a[loop] = greedy ? body : after;
	re->b[loop] = greedy ? after : body;
}

static void gen(Parser *P, u32 n)
{
	if (P->failed)
		return;
	Regex *re = P->re;
	switch (P->kind[n]) {
	case N_EMPTY:
		break;
	case N_CHAR: {
		u32 cp = P->a[n];
		if (cp < 0x80) {
			u8 set[32] = {0};
			set_add(set, cp, cp);
			if ((P->flags & RE_ICASE) && isalpha((int)cp)) {
				u32 o = islower((int)cp) ? (u32)toupper((int)cp) : (u32)tolower((int)cp);
				set_add(set, o, o);
			}
			if (!re->multiline && (cp == '\n' || cp == '\r'))
				; /* unreachable: \n sets multiline */
			emit(P, OP_BYTES, set_index(re, set), 0);
		} else {
			char buf[4];
			u32 k = utf8_encode(cp, buf);
			for (u32 i = 0; i < k; i++)
				emit_bytes(P, (u8)buf[i], (u8)buf[i]);
		}
		break;
	}
	case N_CLASS:
		gen_class(P, n);
		break;
	case N_CAT:
		for (u32 c = P->first[n]; c != UINT32_MAX; c = P->next[c])
			gen(P, c);
		break;
	case N_ALT: {
		u32 jumps[4096];
		u32 nj = 0;
		for (u32 c = P->first[n]; c != UINT32_MAX; c = P->next[c]) {
			if (P->next[c] == UINT32_MAX) {
				gen(P, c);
				break;
			}
			u32 sp = emit(P, OP_SPLIT, 0, 0);
			re->a[sp] = re->n;
			gen(P, c);
			if (nj < VX_COUNT(jumps))
				jumps[nj++] = emit(P, OP_JMP, 0, 0);
			re->b[sp] = re->n;
		}
		for (u32 k = 0; k < nj; k++)
			re->a[jumps[k]] = re->n;
		break;
	}
	case N_REPEAT: {
		u32 child = P->first[n];
		u32 mn = P->a[n], mx = P->b[n];
		bool greedy = P->flag[n];
		for (u32 k = 0; k < mn && !P->failed; k++)
			gen(P, child);
		if (mx == UINT32_MAX) {
			gen_star(P, child, greedy);
		} else if (mx > mn) {
			u32 *splits = vx_alloc((mx - mn) * sizeof(u32));
			u32 ns = 0;
			for (u32 k = mn; k < mx && !P->failed; k++) {
				u32 sp = emit(P, OP_SPLIT, 0, 0);
				splits[ns++] = sp;
				re->a[sp] = re->n;
				gen(P, child);
			}
			for (u32 k = 0; k < ns; k++) {
				u32 sp = splits[k], body = re->a[sp];
				re->a[sp] = greedy ? body : re->n;
				re->b[sp] = greedy ? re->n : body;
			}
			free(splits);
		}
		break;
	}
	case N_GROUP:
		if (P->a[n]) {
			emit(P, OP_SAVE, 2 * P->a[n], 0);
			gen(P, P->first[n]);
			emit(P, OP_SAVE, 2 * P->a[n] + 1, 0);
		} else {
			gen(P, P->first[n]);
		}
		break;
	case N_BOL:
		emit(P, OP_BOL, P->a[n], 0);
		break;
	case N_EOL:
		emit(P, OP_EOL, P->a[n], 0);
		break;
	case N_WORDB:
		emit(P, OP_WORDB, 0, 0);
		break;
	case N_NWORDB:
		emit(P, OP_NWORDB, 0, 0);
		break;
	}
}

static void free_parser(Parser *P)
{
	SoaCol c[] = {NODE_COLS(P)};
	soa_free(c, 6);
	SoaCol rc[] = {RANGE_COLS(P)};
	soa_free(rc, 2);
}

Regex *re_compile(const char *pat, size_t len, int flags, char *err, size_t errcap)
{
	Regex *re = vx_alloc(sizeof *re);
	re->groups = 1;
	re->flags = flags;
	re->first_byte = -1;
	/* multiline when the pattern names a newline anywhere */
	for (size_t i = 0; i < len; i++)
		if (pat[i] == '\n' || (pat[i] == '\\' && i + 1 < len && pat[i + 1] == 'n' && (i == 0 || pat[i - 1] != '\\')))
			re->multiline = true;
	Parser P = {0};
	P.p = pat;
	P.len = len;
	P.re = re;
	P.err = err;
	P.errcap = errcap;
	P.flags = flags;
	if (err && errcap)
		err[0] = 0;
	u32 root = parse_alt(&P);
	if (!P.failed && P.i < P.len)
		fail(&P, "unexpected ): %s", pat, len);
	if (!P.failed) {
		emit(&P, OP_SAVE, 0, 0);
		gen(&P, root);
		emit(&P, OP_SAVE, 1, 0);
		emit(&P, OP_MATCH, 0, 0);
	}
	if (!P.failed && re->n > 1 && re->op[1] == OP_BYTES) {
		/* a single-byte set right after SAVE 0 gives a memchr prefilter */
		const u8 *s = re->sets[re->a[1]];
		int cnt = 0, b = -1;
		for (int c = 0; c < 256; c++)
			if (s[c >> 3] & (1 << (c & 7))) {
				cnt++;
				b = c;
			}
		if (cnt == 1)
			re->first_byte = b;
	}
	free_parser(&P);
	if (P.failed) {
		re_free(re);
		return NULL;
	}
	SoaCol pc[] = {PROG_COLS(re)};
	soa_fit(pc, 3, &re->cap, re->n); /* the program is only read from here on */
	re->sparse = vx_alloc(re->n * sizeof(u32));
	re->dense = vx_alloc(re->n * sizeof(u32));
	return re;
}

static void dfa_reset(Regex *re)
{
	re->nst = 0;
	re->kn = 0;
	if (re->hash)
		memset(re->hash, 0xFF, re->hash_cap * sizeof(u32));
}

void re_free(Regex *re)
{
	if (!re)
		return;
	SoaCol pc[] = {PROG_COLS(re)}, sc[] = {STATE_COLS(re)};
	soa_free(pc, 3);
	soa_free(sc, 5);
	free(re->sets);
	free(re->kpool);
	free(re->hash);
	free(re->sparse);
	free(re->dense);
	free(re->stack);
	free(re);
}

u32 re_groups(const Regex *re) { return re->groups; }

/* ---- shared helpers ------------------------------------------------------- */

static bool is_word(int c) { return c >= 0 && (isalnum(c) || c == '_'); }

/* Context for empty-width assertions at a position: prev and next bytes
 * (-1 for the text edges). */
static bool assert_ok(u8 op, u32 arg, int prev, int next)
{
	switch (op) {
	case OP_BOL:
		return arg ? prev < 0 : (prev < 0 || prev == '\n' || (prev == '\r' && next != '\n'));
	case OP_EOL:
		return arg ? next < 0 : (next < 0 || next == '\n' || next == '\r');
	case OP_WORDB:
		return is_word(prev) != is_word(next);
	case OP_NWORDB:
		return is_word(prev) == is_word(next);
	}
	return false;
}

static bool in_set(const Regex *re, u32 set, u8 c) { return re->sets[set][c >> 3] & (1u << (c & 7)); }

static void stack_need(Regex *re, u32 n)
{
	VX_GROW(re->stack, re->stack_cap, n);
}

/* ---- lazy DFA --------------------------------------------------------------- */

/* State flags encode the previous byte class: 0 word / 1 line start / 2 other,
 * and 3 for the text start. Kernel = pcs to expand (sorted, unique). */
static int ctx_prev(u8 f) { return f == 0 ? 'a' : f == 1 ? '\n' : f == 3 ? -1 : ' '; }
static u8 ctx_of(int c) { return c < 0 ? 3 : is_word(c) ? 0 : (c == '\n' || c == '\r') ? 1 : 2; }

static u32 st_hash(const u32 *k, u32 n, u8 f)
{
	u32 h = 2166136261u ^ f;
	for (u32 i = 0; i < n; i++)
		h = (h ^ k[i]) * 16777619u;
	return h;
}

static u32 dfa_state(Regex *re, const u32 *k, u32 n, u8 f)
{
	if (!re->hash) {
		re->hash_cap = DFA_MAX_STATES * 2;
		re->hash = vx_alloc(re->hash_cap * sizeof(u32));
		memset(re->hash, 0xFF, re->hash_cap * sizeof(u32));
	}
	u32 h = st_hash(k, n, f) & (re->hash_cap - 1);
	while (re->hash[h] != UINT32_MAX) {
		u32 s = re->hash[h];
		if (re->st_flags[s] == f && re->st_len[s] == n && !memcmp(re->kpool + re->st_off[s], k, n * sizeof(u32)))
			return s;
		h = (h + 1) & (re->hash_cap - 1);
	}
	if (re->nst >= DFA_MAX_STATES)
		return DFA_UNKNOWN; /* cache full: caller resets */
	if (re->nst == re->st_cap) {
		SoaCol c[] = {STATE_COLS(re)};
		soa_grow(c, 5, &re->st_cap, VX_MAX(re->nst + 1, 64u), re->nst);
	}
	u32 s = re->nst++;
	VX_GROW(re->kpool, re->kcap, re->kn + n + 1);
	memcpy(re->kpool + re->kn, k, n * sizeof(u32));
	re->st_off[s] = re->kn;
	re->st_len[s] = n;
	re->kn += n;
	re->st_flags[s] = f;
	re->st_eof[s] = 0;
	memset(re->trans + (size_t)s * 256, 0xFF, 256 * sizeof(u32));
	re->hash[h] = s;
	return s;
}

/* Expand the kernel of state s with next byte `next` (-1 = end): returns
 * whether MATCH is reachable, and (when next >= 0) the kernel after
 * consuming it in out (sorted unique). The start pc is always added
 * (unanchored search). */
static bool dfa_step(Regex *re, u32 s, int next, u32 *out, u32 *nout)
{
	u32 *sparse = re->sparse, *dense = re->dense;
	u32 nd = 0;
	int prev = ctx_prev(re->st_flags[s]);
	stack_need(re, re->n * 2 + 2);
	u32 sp = 0;
	const u32 *k = re->kpool + re->st_off[s];
	u32 kn = re->st_len[s];
	re->stack[sp++] = 0;
	for (u32 i = kn; i-- > 0;)
		re->stack[sp++] = k[i];
	bool match = false;
	u32 no = 0;
	while (sp) {
		u32 pc = re->stack[--sp];
		if (sparse[pc] < nd && dense[sparse[pc]] == pc)
			continue;
		sparse[pc] = nd;
		dense[nd++] = pc;
		switch (re->op[pc]) {
		case OP_BYTES:
			if (next >= 0 && in_set(re, re->a[pc], (u8)next))
				out[no++] = pc + 1;
			break;
		case OP_SPLIT:
			re->stack[sp++] = re->b[pc];
			re->stack[sp++] = re->a[pc];
			break;
		case OP_JMP:
			re->stack[sp++] = re->a[pc];
			break;
		case OP_SAVE:
			re->stack[sp++] = pc + 1;
			break;
		case OP_MATCH:
			match = true;
			break;
		default:
			if (assert_ok(re->op[pc], re->a[pc], prev, next))
				re->stack[sp++] = pc + 1;
			break;
		}
	}
	/* sort + unique the new kernel */
	for (u32 i = 1; i < no; i++) {
		u32 v = out[i], j = i;
		while (j > 0 && out[j - 1] > v) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = v;
	}
	u32 m = 0;
	for (u32 i = 0; i < no; i++)
		if (!m || out[m - 1] != out[i])
			out[m++] = out[i];
	*nout = m;
	return match;
}

/* Earliest end of any match starting at or after `from`; UINT32_MAX when
 * none. Returns false if the cache thrashed (caller uses the Pike VM). */
static bool dfa_earliest(Regex *re, const char *s, u32 n, u32 from, u32 *end)
{
	u32 *buf = vx_alloc((re->n + 1) * sizeof(u32));
	int prev = from ? (u8)s[from - 1] : -1;
	u32 st = dfa_state(re, NULL, 0, ctx_of(prev));
	u32 resets = 0;
	if (st == DFA_UNKNOWN) {
		dfa_reset(re);
		st = dfa_state(re, NULL, 0, ctx_of(prev));
	}
	*end = UINT32_MAX;
	for (u32 p = from; p < n; p++) {
		u8 c = (u8)s[p];
		u32 t = re->trans[(size_t)st * 256 + c];
		if (t == DFA_UNKNOWN) {
			u32 nk;
			bool m = dfa_step(re, st, c, buf, &nk);
			u32 ns = dfa_state(re, buf, nk, ctx_of(c));
			if (ns == DFA_UNKNOWN) {
				/* cache full: flush and rebuild from this state's kernel */
				if (++resets > 8) {
					free(buf);
					return false;
				}
				u32 *k = vx_alloc((re->st_len[st] + 1) * sizeof(u32));
				u32 kl = re->st_len[st];
				memcpy(k, re->kpool + re->st_off[st], kl * sizeof(u32));
				u8 f = re->st_flags[st];
				dfa_reset(re);
				st = dfa_state(re, k, kl, f);
				free(k);
				p--;
				continue;
			}
			t = ns | (m ? DFA_MATCH_BIT : 0);
			re->trans[(size_t)st * 256 + c] = t;
		}
		if (t & DFA_MATCH_BIT) {
			*end = p;
			free(buf);
			return true;
		}
		st = t & ~DFA_MATCH_BIT;
	}
	if (!re->st_eof[st]) {
		u32 nk;
		re->st_eof[st] = dfa_step(re, st, -1, buf, &nk) ? 2 : 1;
	}
	if (re->st_eof[st] == 2)
		*end = n;
	free(buf);
	return true;
}

/* ---- Pike VM ----------------------------------------------------------------- */

typedef struct {
	u32 *pc;
	u32 *caps; /* n * ncap */
	u32 n;
	u32 *sparse; /* pc -> index */
} TList;

static void tl_init(TList *l, u32 prog, u32 ncap)
{
	l->pc = vx_alloc(prog * sizeof(u32));
	l->caps = vx_alloc((size_t)prog * ncap * sizeof(u32));
	l->sparse = vx_alloc(prog * sizeof(u32));
	l->n = 0;
}

static void tl_free(TList *l)
{
	free(l->pc);
	free(l->caps);
	free(l->sparse);
}

static bool tl_has(const TList *l, u32 pc) { return l->sparse[pc] < l->n && l->pc[l->sparse[pc]] == pc; }

/* Follow empty transitions from pc0 at position pos in priority order,
 * adding threads to l. cap is scratch (modified and restored). */
static void addthread(Regex *re, TList *l, u32 pc0, u32 pos, u32 *cap, u32 ncap, const char *s, u32 n)
{
	int prev = pos ? (u8)s[pos - 1] : -1;
	int next = pos < n ? (u8)s[pos] : -1;
	/* stack entries: pc, or (slot | 0x80000000, old value) for restores */
	stack_need(re, re->n * 4 + 8);
	u32 sp = 0;
	re->stack[sp++] = pc0;
	while (sp) {
		u32 e = re->stack[--sp];
		if (e & 0x80000000u) {
			u32 val = re->stack[--sp];
			cap[e & 0x7FFFFFFFu] = val;
			continue;
		}
		u32 pc = e;
		if (tl_has(l, pc))
			continue;
		l->sparse[pc] = l->n;
		u32 ti = l->n++;
		l->pc[ti] = pc;
		memcpy(l->caps + (size_t)ti * ncap, cap, ncap * sizeof(u32));
		switch (re->op[pc]) {
		case OP_SPLIT:
			re->stack[sp++] = re->b[pc];
			re->stack[sp++] = re->a[pc];
			break;
		case OP_JMP:
			re->stack[sp++] = re->a[pc];
			break;
		case OP_SAVE: {
			u32 slot = re->a[pc];
			if (slot < ncap) {
				re->stack[sp++] = cap[slot];
				re->stack[sp++] = slot | 0x80000000u;
				cap[slot] = pos;
			}
			re->stack[sp++] = pc + 1;
			break;
		}
		case OP_BOL:
		case OP_EOL:
		case OP_WORDB:
		case OP_NWORDB:
			if (assert_ok(re->op[pc], re->a[pc], prev, next))
				re->stack[sp++] = pc + 1;
			break;
		default:
			break;
		}
	}
}

/* Leftmost-first match in [from, limit]; captures into out. */
static bool pike(Regex *re, const char *s, u32 n, u32 from, u32 limit, u32 *out)
{
	u32 ncap = 2 * re->groups;
	TList cl, nl;
	tl_init(&cl, re->n, ncap);
	tl_init(&nl, re->n, ncap);
	u32 *cap = vx_alloc(ncap * sizeof(u32));
	bool matched = false;
	for (u32 pos = from;; pos++) {
		if (!matched && pos <= limit && (pos >= n || !utf8_cont((u8)s[pos]))) {
			for (u32 i = 0; i < ncap; i++)
				cap[i] = UINT32_MAX;
			addthread(re, &cl, 0, pos, cap, ncap, s, n);
		}
		if (cl.n == 0)
			break;
		nl.n = 0;
		int c = pos < n ? (u8)s[pos] : -1;
		for (u32 i = 0; i < cl.n; i++) {
			u32 pc = cl.pc[i];
			u32 *tc = cl.caps + (size_t)i * ncap;
			if (re->op[pc] == OP_MATCH) {
				memcpy(out, tc, ncap * sizeof(u32));
				matched = true;
				break; /* lower-priority threads are cut */
			}
			if (re->op[pc] == OP_BYTES && c >= 0 && in_set(re, re->a[pc], (u8)c)) {
				memcpy(cap, tc, ncap * sizeof(u32));
				addthread(re, &nl, pc + 1, pos + 1, cap, ncap, s, n);
			}
		}
		TList t = cl;
		cl = nl;
		nl = t;
		if (pos >= n)
			break;
	}
	tl_free(&cl);
	tl_free(&nl);
	free(cap);
	return matched;
}

bool re_search(Regex *re, const char *s, u32 n, u32 from, u32 *caps)
{
	for (u32 i = 0; i < 2 * RE_MAX_CAPS; i++)
		caps[i] = UINT32_MAX;
	if (from > n)
		return false;
	if (re->first_byte >= 0) {
		const char *p = from < n ? memchr(s + from, re->first_byte, n - from) : NULL;
		if (!p)
			return false;
		from = (u32)(p - s);
	}
	u32 end;
	if (dfa_earliest(re, s, n, from, &end)) {
		if (end == UINT32_MAX)
			return false;
		/* Line mode: the leftmost match lies on the line of the earliest
		 * end, so the Pike VM only runs over that line. */
		u32 start = from;
		if (!re->multiline) {
			u32 ls = end;
			while (ls > from && s[ls - 1] != '\n')
				ls--;
			start = ls;
		}
		u32 le = end;
		if (!re->multiline)
			while (le < n && s[le] != '\n')
				le++;
		else
			le = n;
		u32 tmp[2 * RE_MAX_CAPS];
		if (pike(re, s, le, start, end, tmp)) {
			memcpy(caps, tmp, 2 * re->groups * sizeof(u32));
			return true;
		}
		/* should not happen; fall back to the full VM */
	}
	return pike(re, s, n, from, n, caps);
}

/* ---- replacement ---------------------------------------------------------------- */

static void put(char **out, u32 *len, u32 *cap, const char *s, u32 n, int *mode, int *one)
{
	VX_GROW(*out, *cap, *len + n + 1);
	for (u32 i = 0; i < n; i++) {
		char c = s[i];
		if (*one == 1)
			c = (char)toupper((u8)c);
		else if (*one == 2)
			c = (char)tolower((u8)c);
		else if (*mode == 1)
			c = (char)toupper((u8)c);
		else if (*mode == 2)
			c = (char)tolower((u8)c);
		*one = 0;
		(*out)[(*len)++] = c;
	}
	(*out)[*len] = 0;
}

static u32 group_named(const Regex *re, const char *s, size_t n)
{
	bool digits = n > 0;
	u32 v = 0;
	for (size_t i = 0; i < n; i++) {
		if (!isdigit((u8)s[i]))
			digits = false;
		else
			v = v * 10 + (u32)(s[i] - '0');
	}
	if (digits)
		return v < re->groups ? v : UINT32_MAX;
	for (u32 g = 1; g < re->groups; g++)
		if (strlen(re->names[g]) == n && !memcmp(re->names[g], s, n))
			return g;
	return UINT32_MAX;
}

void re_expand(const Regex *re, const char *r, size_t nr, const char *s, const u32 *caps, char **out,
               u32 *len, u32 *cap)
{
	int mode = 0, one = 0, z = 0;
	VX_GROW(*out, *cap, *len + 1);
	(*out)[*len] = 0;
	for (size_t i = 0; i < nr; i++) {
		char c = r[i];
		if (c == '\\' && i + 1 < nr) {
			char e = r[i + 1];
			if (e == 'n' || e == 't' || e == '\\') {
				const char *lit = e == 'n' ? "\n" : e == 't' ? "\t" : "\\";
				put(out, len, cap, lit, 1, &z, &z);
				i++;
				continue;
			}
			int nm = e == 'U' ? 1 : e == 'L' ? 2 : e == 'E' ? 0 : -1;
			if (nm >= 0) {
				mode = nm;
				i++;
				continue;
			}
			if (e == 'u' || e == 'l') {
				one = e == 'u' ? 1 : 2;
				i++;
				continue;
			}
		}
		if (c == '$' && i + 1 < nr) {
			char e = r[i + 1];
			u32 g = UINT32_MAX;
			size_t adv = 0;
			if (e == '$') {
				put(out, len, cap, "$", 1, &z, &z);
				i++;
				continue;
			}
			if (e == '&') {
				g = 0;
				adv = 1;
			} else if (e == '{') {
				const char *close = memchr(r + i + 2, '}', nr - i - 2);
				if (close) {
					g = group_named(re, r + i + 2, (size_t)(close - (r + i + 2)));
					adv = (size_t)(close - (r + i));
				}
			} else if (isdigit((u8)e)) {
				/* longest group number that exists ($12 -> 12 if there are 12 groups) */
				g = (u32)(e - '0');
				adv = 1;
				if (i + 2 < nr && isdigit((u8)r[i + 2]) && g * 10 + (u32)(r[i + 2] - '0') < re->groups) {
					g = g * 10 + (u32)(r[i + 2] - '0');
					adv = 2;
				}
				if (g >= re->groups)
					g = UINT32_MAX;
			} else if (isalpha((u8)e) || e == '_') {
				size_t k = i + 1;
				while (k < nr && (isalnum((u8)r[k]) || r[k] == '_'))
					k++;
				g = group_named(re, r + i + 1, k - i - 1);
				adv = k - i - 1;
			}
			if (g != UINT32_MAX) {
				u32 a = caps[2 * g], b = caps[2 * g + 1];
				if (a != UINT32_MAX && b != UINT32_MAX && b >= a)
					put(out, len, cap, s + a, b - a, &mode, &one);
				i += adv;
				continue;
			}
		}
		put(out, len, cap, &c, 1, &mode, &one);
	}
}
