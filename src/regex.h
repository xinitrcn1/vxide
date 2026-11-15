/* Regular expressions modelled after RE2: guaranteed linear time, no
 * backtracking. Syntax is RE2's: literals, . [classes] \d \w \s (ASCII, as
 * in RE2), \b \B ^ $, * + ? {n,m} with lazy forms, (capturing),
 * (?:non-capturing), (?P<name>...) / (?<name>...), alternation, \x{..}.
 * Backreferences and lookaround are rejected, as RE2 does.
 *
 * Matching is on UTF-8 bytes; '.' and classes consume whole code points
 * (they are compiled to UTF-8 byte-range automata). ^ and $ match at line
 * boundaries. Unless the pattern mentions a newline (\n or a literal one),
 * matches never cross a line end, like VSCodium's search.
 *
 * Execution: a lazily built DFA finds the earliest match end (fast reject
 * for lines and files without matches); a Pike VM then finds the leftmost-
 * first match with its submatches on that line only. */
#ifndef VX_REGEX_H
#define VX_REGEX_H

#include "base.h"

#define RE_MAX_CAPS 32 /* group 0 + 31 */

enum { RE_ICASE = 1 };

typedef struct Regex Regex;

/* NULL on a syntax error; err gets an RE2-style message. */
Regex *re_compile(const char *pat, size_t n, int flags, char *err, size_t errcap);
void re_free(Regex *re);
u32 re_groups(const Regex *re); /* capture groups including group 0 */

/* Leftmost-first match starting at or after `from` in s[0, n). caps gets
 * 2 * re_groups() offsets (UINT32_MAX for groups that did not take part).
 * Text before `from` is visible to ^ and \b. Not thread-safe per Regex
 * (the DFA cache is filled during matching). */
bool re_search(Regex *re, const char *s, u32 n, u32 from, u32 *caps);

/* Expand a replacement: $1..$99, ${1}, $name / ${name}, $&, $$, and
 * \n \t \\ plus the case operators \u \l \U \L \E (VSCodium). */
void re_expand(const Regex *re, const char *repl, size_t nr, const char *s, const u32 *caps,
               char **out, u32 *len, u32 *cap);

#endif
