/* Minimal test helpers. */
#ifndef VX_TAP_H
#define VX_TAP_H

#include <stdio.h>
#include <string.h>

static int tap_fail, tap_n;

#define CHECK(cond)                                                          \
	do {                                                                     \
		tap_n++;                                                             \
		if (!(cond)) {                                                       \
			tap_fail++;                                                      \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			        #cond);                                                  \
		}                                                                    \
	} while (0)

#define CHECK_STR(a, b)                                                       \
	do {                                                                      \
		const char *a_ = (a), *b_ = (b);                                      \
		tap_n++;                                                              \
		if (!a_ || !b_ || strcmp(a_, b_) != 0) {                              \
			tap_fail++;                                                       \
			fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
			        a_ ? a_ : "(null)", b_ ? b_ : "(null)");                  \
		}                                                                     \
	} while (0)

static inline int tap_done(void)
{
	fprintf(stderr, "%d checks, %d failed\n", tap_n, tap_fail);
	return tap_fail ? 1 : 0;
}

#endif
