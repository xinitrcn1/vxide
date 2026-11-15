/* Regex engine: JS/PCRE-style semantics as used by VSCodium find/search. */
#include "regex.h"
#include "tap.h"

#include <stdio.h>

/* Match pattern against s from 0; returns "start,end" or "none". */
static const char *m(const char *pat, int flags, const char *s)
{
	static char buf[64];
	char err[128];
	Regex *re = re_compile(pat, strlen(pat), flags, err, sizeof err);
	if (!re) {
		snprintf(buf, sizeof buf, "error: %s", err);
		return buf;
	}
	u32 caps[2 * RE_MAX_CAPS];
	if (re_search(re, s, (u32)strlen(s), 0, caps))
		snprintf(buf, sizeof buf, "%u,%u", caps[0], caps[1]);
	else
		snprintf(buf, sizeof buf, "none");
	re_free(re);
	return buf;
}

static const char *grp(const char *pat, const char *s, u32 g)
{
	static char buf[128];
	Regex *re = re_compile(pat, strlen(pat), 0, NULL, 0);
	u32 caps[2 * RE_MAX_CAPS];
	if (!re || !re_search(re, s, (u32)strlen(s), 0, caps) || caps[2 * g] == UINT32_MAX) {
		re_free(re);
		return "unset";
	}
	snprintf(buf, sizeof buf, "%.*s", (int)(caps[2 * g + 1] - caps[2 * g]), s + caps[2 * g]);
	re_free(re);
	return buf;
}

static const char *rep(const char *pat, const char *s, const char *r)
{
	static char buf[256];
	Regex *re = re_compile(pat, strlen(pat), 0, NULL, 0);
	u32 caps[2 * RE_MAX_CAPS];
	buf[0] = 0;
	if (re && re_search(re, s, (u32)strlen(s), 0, caps)) {
		char *out = NULL;
		u32 len = 0, cap = 0;
		re_expand(re, r, strlen(r), s, caps, &out, &len, &cap);
		snprintf(buf, sizeof buf, "%s", out);
		free(out);
	}
	re_free(re);
	return buf;
}

int main(void)
{
	/* literals, classes, escapes */
	CHECK_STR(m("abc", 0, "xxabcxx"), "2,5");
	CHECK_STR(m("a.c", 0, "a\nc abc"), "4,7");
	CHECK_STR(m("[a-c]+", 0, "zzbcaq"), "2,5");
	CHECK_STR(m("[^a-c]+", 0, "abxyc"), "2,4");
	CHECK_STR(m("\\d+", 0, "ab 123 4"), "3,6");
	CHECK_STR(m("\\w+", 0, "  foo_1 "), "2,7");
	CHECK_STR(m("\\s\\S", 0, "ab c"), "2,4");
	CHECK_STR(m("[\\d.]+", 0, "v1.25x"), "1,5");
	CHECK_STR(m("\\x41\\x{42}", 0, "zAB"), "1,3");
	CHECK_STR(m("\\u0042", 0, "B"), "error: invalid escape sequence: \\u");
	CHECK_STR(m("\\.\\*", 0, "a.*b"), "1,3");
	CHECK_STR(m("[]a]+", 0, "x]a]"), "1,4"); /* RE2: leading ] is literal */
	CHECK_STR(m("[[:digit:]x]+", 0, "a1x2"), "1,4");
	CHECK_STR(m("\\Qa.b\\E", 0, "axb a.b"), "4,7");
	/* quantifiers */
	CHECK_STR(m("a{2,3}", 0, "aaaa"), "0,3");
	CHECK_STR(m("a{2}", 0, "a aa"), "2,4");
	CHECK_STR(m("a{2,}", 0, "aaaaa"), "0,5");
	CHECK_STR(m("<.+>", 0, "<a><b>"), "0,6");
	CHECK_STR(m("<.+?>", 0, "<a><b>"), "0,3");
	CHECK_STR(m("x{", 0, "ax{"), "1,3"); /* literal brace */
	CHECK_STR(m("(a*)*b", 0, "aaab"), "0,4"); /* empty-loop guard */
	CHECK_STR(m("(a|)*c", 0, "aac"), "0,3");
	CHECK_STR(m("a**", 0, "a"), "error: bad repetition operator: **");
	CHECK_STR(m("a{1001}", 0, "a"), "error: bad repetition operator: {1001}");
	/* alternation, groups */
	CHECK_STR(m("cat|dog", 0, "hotdog"), "3,6");
	CHECK_STR(grp("(\\w+)@(\\w+)", "mail: bob@host", 2), "host");
	CHECK_STR(grp("(?:ab)+(c)", "ababc", 1), "c");
	CHECK_STR(grp("(?<year>\\d{4})-(?<m>\\d\\d)", "on 2026-11", 1), "2026");
	CHECK_STR(grp("(a)|(b)", "b", 1), "unset");
	CHECK_STR(m("(", 0, "x"), "error: missing closing ): (");
	CHECK_STR(m("a)", 0, "x"), "error: unexpected ): a)");
	CHECK_STR(m("[a", 0, "x"), "error: missing closing ]: [a");
	/* anchors, line mode */
	CHECK_STR(m("^b", 0, "ab\nb"), "3,4");
	CHECK_STR(m("a$", 0, "ab\na\n"), "3,4");
	CHECK_STR(m("^$", 0, "x\n\ny"), "2,2");
	CHECK_STR(m("a\\sb", 0, "a\nb a b"), "4,7"); /* \s does not cross lines */
	CHECK_STR(m("a\\nb", 0, "a\nb"), "0,3");     /* \n opts into multiline */
	CHECK_STR(m("a[^x]b", 0, "a\nb"), "none");
	CHECK_STR(m("\\bfoo\\b", 0, "afoo foo"), "5,8");
	CHECK_STR(m("\\Boo", 0, "oo foo"), "4,6");
	/* RE2 rejects lookaround and backreferences */
	CHECK_STR(m("foo(?=bar)", 0, "x"), "error: invalid perl operator: (?=");
	CHECK_STR(m("(?<=a)b", 0, "x"), "error: invalid perl operator: (?<=");
	CHECK_STR(m("(\\w)\\1", 0, "x"), "error: invalid escape sequence: \\1");
	CHECK_STR(m("(?i)HeLLo", 0, "say hello"), "4,9");
	CHECK_STR(m("\\x{e9}", 0, "\xc3\xa9"), "0,2");
	/* case-insensitive */
	CHECK_STR(m("hello", RE_ICASE, "Say HeLLo"), "4,9");
	CHECK_STR(m("[a-z]+", RE_ICASE, "123ABC"), "3,6");
	/* UTF-8 */
	CHECK_STR(m("caf.", 0, "un café"), "3,8");
	CHECK_STR(m("[é-ë]", 0, "xë"), "1,3");
	CHECK_STR(m("[^a]", 0, "\xc3\xa9"), "0,2"); /* whole code point */
	CHECK_STR(m("\\D", 0, "1\xc3\xa9"), "1,3");
	/* replacement */
	CHECK_STR(rep("(\\w+)-(\\w+)", "foo-bar", "$2_$1"), "bar_foo");
	CHECK_STR(rep("b+", "abbbc", "[$&]$$"), "[bbb]$");
	CHECK_STR(rep("(\\w+)", "word", "\\u$1 \\U$1\\E!"), "Word WORD!");
	CHECK_STR(rep("(?P<x>a)", "a", "<${x}$x>\\n"), "<aa>\n");
	CHECK_STR(rep("a", "a", "$9"), "$9");
	/* linear time: the classic exponential case finishes at once */
	{
		static char s[100001];
		memset(s, 'a', 100000);
		Regex *re = re_compile("(a+)+b", 6, 0, NULL, 0);
		u32 caps[2 * RE_MAX_CAPS];
		u64 t0 = vx_now_ns();
		CHECK(!re_search(re, s, 100000, 0, caps));
		u64 t1 = vx_now_ns();
		CHECK(t1 - t0 < 2000000000ull);
		fprintf(stderr, "(a+)+b over 100k a's: %.1f ms\n", (double)(t1 - t0) / 1e6);
		re_free(re);
		re = re_compile("(x+x+)+y", 8, 0, NULL, 0);
		memset(s, 'x', 5000);
		CHECK(!re_search(re, s, 5000, 0, caps));
		re_free(re);
	}
	/* leftmost-first like Perl/RE2, not leftmost-longest */
	CHECK_STR(m("a|ab", 0, "ab"), "0,1");
	CHECK_STR(m("(a|ab)(c|bcd)", 0, "abcd"), "0,4");
	/* from offset sees context for ^ and \b */
	{
		Regex *re = re_compile("^c|\\bb", 7, 0, NULL, 0);
		u32 caps[2 * RE_MAX_CAPS];
		CHECK(re_search(re, "ab c\nc", 6, 1, caps) && caps[0] == 5);
		re_free(re);
	}
	/* DFA throughput on a big buffer */
	{
		u32 n = 16 << 20;
		char *big = vx_alloc(n + 1);
		for (u32 i = 0; i < n; i++)
			big[i] = (i % 61 == 60) ? '\n' : (char)('a' + i % 23);
		memcpy(big + n - 20, "foo_bar(123)", 12);
		Regex *re = re_compile("\\w+\\(\\d+\\)", 10, 0, NULL, 0);
		u32 caps[2 * RE_MAX_CAPS];
		u64 t0 = vx_now_ns();
		bool ok = re_search(re, big, n, 0, caps);
		u64 t1 = vx_now_ns();
		CHECK(ok && caps[0] == n - 20 && caps[1] == n - 8);
		fprintf(stderr, "16 MiB scan: %.1f ms (%.0f MB/s)\n", (double)(t1 - t0) / 1e6, n / ((double)(t1 - t0) / 1e3));
		re_free(re);
		free(big);
	}
	return tap_done();
}
