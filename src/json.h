/* JSON: one-pass parser into a flat token table (SoA, jsmn style) and a
 * growable writer for building messages. Token 0 is the root value.
 * Strings keep their raw (escaped) bytes; start/end exclude the quotes.
 * Object members are stored as key token followed by value token. */
#ifndef VX_JSON_H
#define VX_JSON_H

#include "base.h"

enum { JS_NONE, JS_OBJ, JS_ARR, JS_STR, JS_NUM, JS_TRUE, JS_FALSE, JS_NULL };

#define JSON_NIL 0xFFFFFFFFu /* "no token" */

typedef struct {
	u32 n, cap;
	u8 *type;
	u32 *start, *end; /* byte range in the source */
	u32 *size;        /* direct children: array elements / object members */
	u32 *next;        /* index of the next sibling (one past this subtree) */
	const char *src;  /* source the tokens refer to (not owned) */
	u32 depth_cap;    /* parse stack capacity (internal) */
	u32 *stack;       /* parse stack (internal) */
} Json;

/* Parse src[0..n). Returns 0 on success, -1 on malformed input. The token
 * arrays are reused across calls. Trailing whitespace is allowed. */
int json_parse(Json *j, const char *src, u32 n);
void json_free(Json *j);

/* Member `key` of object token obj, or JSON_NIL. */
u32 json_get(const Json *j, u32 obj, const char *key);
/* Follow a dotted path of member names ("result.capabilities.x"). */
u32 json_path(const Json *j, u32 tok, const char *path);
/* i-th element of an array (or i-th member value of an object). */
u32 json_at(const Json *j, u32 arr, u32 i);
/* First child (for objects: the first key) or JSON_NIL. */
static inline u32 json_first(const Json *j, u32 t)
{
	return (t != JSON_NIL && j->size[t]) ? t + 1 : JSON_NIL;
}
static inline u32 json_type(const Json *j, u32 t) { return t == JSON_NIL ? JS_NONE : j->type[t]; }

/* Compare a string token (unescaped) with s. */
bool json_eq(const Json *j, u32 tok, const char *s);
i64 json_int(const Json *j, u32 tok, i64 def);
double json_num(const Json *j, u32 tok, double def);
bool json_bool(const Json *j, u32 tok, bool def);
/* Unescape a string token into pool; returns the offset. Non-strings are
 * stored as their raw text; JSON_NIL stores "". */
u32 json_str_pool(const Json *j, u32 tok, Pool *p);
/* Unescape s[0..n) into out (needs >= n+1 bytes); returns output length.
 * \uXXXX incl. surrogate pairs become UTF-8; bad escapes become U+FFFD. */
u32 json_unescape(const char *s, u32 n, char *out);

/* Writer. */
typedef struct {
	char *b;
	u32 n, cap;
} JBuf;
void jb_raw(JBuf *b, const char *s);
void jb_rawn(JBuf *b, const char *s, size_t n);
void jb_str(JBuf *b, const char *s);              /* quoted, escaped */
void jb_strn(JBuf *b, const char *s, size_t n);   /* quoted, escaped */
void jb_int(JBuf *b, i64 v);
void jb_reset(JBuf *b);
void jb_free(JBuf *b);

#endif
