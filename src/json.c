#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <stdio.h>
#include <math.h>

#define JSON_MAX_DEPTH 512

typedef struct {
	Json *j;
	const char *s;
	u32 n, i;
} JP;

static u32 tok_new(Json *j, u8 type, u32 start)
{
	if (j->n == j->cap) {
		u32 c = j->cap ? j->cap * 2 : 256;
		j->type = vx_realloc(j->type, c);
		j->start = vx_realloc(j->start, c * sizeof(u32));
		j->end = vx_realloc(j->end, c * sizeof(u32));
		j->size = vx_realloc(j->size, c * sizeof(u32));
		j->next = vx_realloc(j->next, c * sizeof(u32));
		j->cap = c;
	}
	u32 t = j->n++;
	j->type[t] = type;
	j->start[t] = start;
	j->end[t] = start;
	j->size[t] = 0;
	j->next[t] = t + 1;
	return t;
}

static void skip_ws(JP *p)
{
	while (p->i < p->n) {
		char c = p->s[p->i];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
		p->i++;
	}
}

static int parse_value(JP *p, u32 depth);

static int parse_string(JP *p)
{
	/* at '"' */
	u32 start = ++p->i;
	while (p->i < p->n) {
		u8 c = (u8)p->s[p->i];
		if (c == '"') {
			u32 t = tok_new(p->j, JS_STR, start);
			p->j->end[t] = p->i;
			p->i++;
			return 0;
		}
		if (c < 0x20)
			return -1;
		if (c == '\\') {
			p->i++;
			if (p->i >= p->n)
				return -1;
			c = (u8)p->s[p->i];
			if (c == 'u') {
				for (int k = 1; k <= 4; k++) {
					if (p->i + k >= p->n)
						return -1;
					char h = p->s[p->i + k];
					bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
					           (h >= 'A' && h <= 'F');
					if (!hex)
						return -1;
				}
				p->i += 4;
			} else if (!strchr("\"\\/bfnrt", c)) {
				return -1;
			}
		}
		p->i++;
	}
	return -1;
}

static int parse_number(JP *p)
{
	u32 start = p->i;
	const char *s = p->s;
	if (p->i < p->n && s[p->i] == '-')
		p->i++;
	if (p->i >= p->n)
		return -1;
	if (s[p->i] == '0') {
		p->i++;
	} else if (s[p->i] >= '1' && s[p->i] <= '9') {
		while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9')
			p->i++;
	} else {
		return -1;
	}
	if (p->i < p->n && s[p->i] == '.') {
		p->i++;
		u32 d = p->i;
		while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9')
			p->i++;
		if (d == p->i)
			return -1;
	}
	if (p->i < p->n && (s[p->i] == 'e' || s[p->i] == 'E')) {
		p->i++;
		if (p->i < p->n && (s[p->i] == '+' || s[p->i] == '-'))
			p->i++;
		u32 d = p->i;
		while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9')
			p->i++;
		if (d == p->i)
			return -1;
	}
	u32 t = tok_new(p->j, JS_NUM, start);
	p->j->end[t] = p->i;
	return 0;
}

static int parse_lit(JP *p, const char *lit, u8 type)
{
	size_t l = strlen(lit);
	if (p->n - p->i < l || memcmp(p->s + p->i, lit, l) != 0)
		return -1;
	u32 t = tok_new(p->j, type, p->i);
	p->i += (u32)l;
	p->j->end[t] = p->i;
	return 0;
}

static int parse_container(JP *p, u32 depth, bool obj)
{
	if (depth >= JSON_MAX_DEPTH)
		return -1;
	u32 t = tok_new(p->j, obj ? JS_OBJ : JS_ARR, p->i);
	p->i++;
	skip_ws(p);
	char close = obj ? '}' : ']';
	if (p->i < p->n && p->s[p->i] == close) {
		p->i++;
		p->j->end[t] = p->i;
		p->j->next[t] = p->j->n;
		return 0;
	}
	for (;;) {
		skip_ws(p);
		if (p->i >= p->n)
			return -1;
		if (obj) {
			if (p->s[p->i] != '"' || parse_string(p))
				return -1;
			skip_ws(p);
			if (p->i >= p->n || p->s[p->i] != ':')
				return -1;
			p->i++;
			skip_ws(p);
		}
		if (parse_value(p, depth + 1))
			return -1;
		p->j->size[t]++;
		skip_ws(p);
		if (p->i >= p->n)
			return -1;
		char c = p->s[p->i++];
		if (c == ',')
			continue;
		if (c == close)
			break;
		return -1;
	}
	p->j->end[t] = p->i;
	p->j->next[t] = p->j->n;
	return 0;
}

static int parse_value(JP *p, u32 depth)
{
	skip_ws(p);
	if (p->i >= p->n)
		return -1;
	switch (p->s[p->i]) {
	case '{':
		return parse_container(p, depth, true);
	case '[':
		return parse_container(p, depth, false);
	case '"':
		return parse_string(p);
	case 't':
		return parse_lit(p, "true", JS_TRUE);
	case 'f':
		return parse_lit(p, "false", JS_FALSE);
	case 'n':
		return parse_lit(p, "null", JS_NULL);
	default:
		return parse_number(p);
	}
}

int json_parse(Json *j, const char *src, u32 n)
{
	JP p = {j, src, n, 0};
	j->n = 0;
	j->src = src;
	if (parse_value(&p, 0)) {
		j->n = 0;
		return -1;
	}
	skip_ws(&p);
	if (p.i != n) {
		j->n = 0;
		return -1;
	}
	return 0;
}

void json_free(Json *j)
{
	free(j->type);
	free(j->start);
	free(j->end);
	free(j->size);
	free(j->next);
	free(j->stack);
	memset(j, 0, sizeof *j);
}

/* Compare raw string token with key; fast path for escape-free tokens. */
bool json_eq(const Json *j, u32 tok, const char *s)
{
	if (tok == JSON_NIL || j->type[tok] != JS_STR)
		return false;
	const char *r = j->src + j->start[tok];
	u32 n = j->end[tok] - j->start[tok];
	if (!memchr(r, '\\', n)) {
		size_t l = strlen(s);
		return l == n && memcmp(r, s, n) == 0;
	}
	char stackbuf[256];
	char *buf = n < sizeof stackbuf ? stackbuf : vx_alloc(n + 1);
	u32 m = json_unescape(r, n, buf);
	bool eq = strlen(s) == m && memcmp(buf, s, m) == 0;
	if (buf != stackbuf)
		free(buf);
	return eq;
}

u32 json_get(const Json *j, u32 obj, const char *key)
{
	if (obj == JSON_NIL || obj >= j->n || j->type[obj] != JS_OBJ)
		return JSON_NIL;
	u32 k = obj + 1;
	for (u32 i = 0; i < j->size[obj]; i++) {
		u32 v = k + 1;
		if (json_eq(j, k, key))
			return v;
		k = j->next[v];
	}
	return JSON_NIL;
}

u32 json_path(const Json *j, u32 tok, const char *path)
{
	char key[128];
	while (*path && tok != JSON_NIL) {
		const char *dot = strchr(path, '.');
		size_t l = dot ? (size_t)(dot - path) : strlen(path);
		if (l >= sizeof key)
			return JSON_NIL;
		memcpy(key, path, l);
		key[l] = 0;
		tok = json_get(j, tok, key);
		path += l + (dot ? 1 : 0);
	}
	return tok;
}

u32 json_at(const Json *j, u32 arr, u32 i)
{
	if (arr == JSON_NIL || arr >= j->n || i >= j->size[arr])
		return JSON_NIL;
	if (j->type[arr] == JS_ARR) {
		u32 t = arr + 1;
		while (i--)
			t = j->next[t];
		return t;
	}
	if (j->type[arr] == JS_OBJ) {
		u32 k = arr + 1;
		while (i--)
			k = j->next[k + 1];
		return k + 1;
	}
	return JSON_NIL;
}

double json_num(const Json *j, u32 tok, double def)
{
	if (tok == JSON_NIL || j->type[tok] != JS_NUM)
		return def;
	char buf[64];
	u32 n = j->end[tok] - j->start[tok];
	if (n >= sizeof buf)
		return def;
	memcpy(buf, j->src + j->start[tok], n);
	buf[n] = 0;
	return strtod(buf, NULL);
}

i64 json_int(const Json *j, u32 tok, i64 def)
{
	if (tok == JSON_NIL || j->type[tok] != JS_NUM)
		return def;
	const char *s = j->src + j->start[tok], *e = j->src + j->end[tok];
	bool neg = false;
	if (s < e && *s == '-') {
		neg = true;
		s++;
	}
	u64 v = 0;
	const char *p = s;
	while (p < e && *p >= '0' && *p <= '9')
		v = v * 10 + (u64)(*p++ - '0');
	if (p != e) /* fraction or exponent */
		return (i64)json_num(j, tok, (double)def);
	return neg ? -(i64)v : (i64)v;
}

bool json_bool(const Json *j, u32 tok, bool def)
{
	if (tok == JSON_NIL)
		return def;
	if (j->type[tok] == JS_TRUE)
		return true;
	if (j->type[tok] == JS_FALSE)
		return false;
	return def;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static i32 hex4(const char *s, u32 n, u32 i)
{
	if (i + 4 > n)
		return -1;
	i32 v = 0;
	for (u32 k = 0; k < 4; k++) {
		int h = hexval(s[i + k]);
		if (h < 0)
			return -1;
		v = v * 16 + h;
	}
	return v;
}

u32 json_unescape(const char *s, u32 n, char *out)
{
	u32 o = 0;
	for (u32 i = 0; i < n; i++) {
		char c = s[i];
		if (c != '\\' || i + 1 >= n) {
			out[o++] = c;
			continue;
		}
		c = s[++i];
		switch (c) {
		case 'b': out[o++] = '\b'; break;
		case 'f': out[o++] = '\f'; break;
		case 'n': out[o++] = '\n'; break;
		case 'r': out[o++] = '\r'; break;
		case 't': out[o++] = '\t'; break;
		case 'u': {
			i32 v = hex4(s, n, i + 1);
			u32 cp = 0xFFFD;
			if (v >= 0) {
				i += 4;
				cp = (u32)v;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					i32 lo = (i + 2 < n && s[i + 1] == '\\' && s[i + 2] == 'u')
					             ? hex4(s, n, i + 3)
					             : -1;
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + ((u32)lo - 0xDC00);
						i += 6;
					} else {
						cp = 0xFFFD;
					}
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					cp = 0xFFFD;
				}
			}
			/* worst case: "\uXXXX" (6 bytes) -> 3 bytes; pair 12 -> 4 */
			o += utf8_encode(cp, out + o);
			break;
		}
		default: out[o++] = c; break; /* \" \\ \/ and unknown */
		}
	}
	out[o] = 0;
	return o;
}

u32 json_str_pool(const Json *j, u32 tok, Pool *p)
{
	if (tok == JSON_NIL)
		return pool_add(p, "", 0);
	const char *r = j->src + j->start[tok];
	u32 n = j->end[tok] - j->start[tok];
	if (j->type[tok] != JS_STR)
		return pool_add(p, r, n);
	VX_GROW(p->b, p->cap, p->n + n + 1);
	u32 off = p->n;
	u32 m = json_unescape(r, n, p->b + off);
	p->n += m + 1;
	return off;
}

/* ---- writer ---- */

void jb_rawn(JBuf *b, const char *s, size_t n)
{
	VX_GROW(b->b, b->cap, b->n + (u32)n + 1);
	memcpy(b->b + b->n, s, n);
	b->n += (u32)n;
	b->b[b->n] = 0;
}

void jb_raw(JBuf *b, const char *s) { jb_rawn(b, s, strlen(s)); }

void jb_strn(JBuf *b, const char *s, size_t n)
{
	VX_GROW(b->b, b->cap, b->n + (u32)n + 3);
	b->b[b->n++] = '"';
	size_t run = 0;
	for (size_t i = 0; i < n; i++) {
		u8 c = (u8)s[i];
		const char *esc = NULL;
		char ubuf[8];
		switch (c) {
		case '"': esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '\n': esc = "\\n"; break;
		case '\r': esc = "\\r"; break;
		case '\t': esc = "\\t"; break;
		case '\b': esc = "\\b"; break;
		case '\f': esc = "\\f"; break;
		default:
			if (c < 0x20) {
				snprintf(ubuf, sizeof ubuf, "\\u%04x", c);
				esc = ubuf;
			}
		}
		if (esc) {
			if (run)
				jb_rawn(b, s + i - run, run);
			run = 0;
			jb_raw(b, esc);
		} else {
			run++;
		}
	}
	if (run)
		jb_rawn(b, s + n - run, run);
	jb_rawn(b, "\"", 1);
}

void jb_str(JBuf *b, const char *s) { jb_strn(b, s, strlen(s)); }

void jb_int(JBuf *b, i64 v)
{
	char buf[32];
	int l = snprintf(buf, sizeof buf, "%lld", (long long)v);
	jb_rawn(b, buf, (size_t)l);
}

void jb_reset(JBuf *b)
{
	b->n = 0;
	if (b->b)
		b->b[0] = 0;
}

void jb_free(JBuf *b)
{
	free(b->b);
	b->b = NULL;
	b->n = b->cap = 0;
}
