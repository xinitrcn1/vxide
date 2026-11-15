/* ucas's JSON frontend, the tab's side: requests and responses. */
#include "ucasproto.h"
#include "syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void resp_reset(UcasResp *r)
{
	for (u32 i = 0; i < r->nfiles; i++)
		free(r->files[i].data);
	Pool keep = r->str;
	pool_clear(&keep);
	memset(r, 0, sizeof *r);
	r->str = keep;
	r->cmd = r->ucas = r->text = r->kind = UCAS_NONE;
	r->err_code = r->err_msg = r->err_key = r->err_how = UCAS_NONE;
	r->proof_text = r->model = UCAS_NONE;
}

void ucas_resp_free(UcasResp *r)
{
	resp_reset(r);
	pool_free(&r->str);
	memset(r, 0, sizeof *r);
}

static u32 str_of(const Json *j, u32 tok, UcasResp *r)
{
	if (tok == JSON_NIL || j->type[tok] == JS_NULL)
		return UCAS_NONE;
	return json_str_pool(j, tok, &r->str);
}

u32 ucas_b64_decode(const char *s, u32 n, u8 *out)
{
	u32 acc = 0, bits = 0, k = 0;
	for (u32 i = 0; i < n; i++) {
		int c = (u8)s[i], v;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+')
			v = 62;
		else if (c == '/')
			v = 63;
		else if (c == '=')
			break;
		else
			continue; /* "\/" is a JSON escape of '/': the '/' follows */
		acc = (acc << 6) | (u32)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[k++] = (u8)(acc >> bits);
		}
	}
	return k;
}

static int proof_kind(const Json *j, u32 t)
{
	return json_eq(j, t, "symbolic")     ? UCAS_PROOF_SYMBOLIC
	       : json_eq(j, t, "exhaustive") ? UCAS_PROOF_EXHAUSTIVE
	       : json_eq(j, t, "sat")        ? UCAS_PROOF_SAT
	       : json_eq(j, t, "numeric")    ? UCAS_PROOF_NUMERIC
	                                     : UCAS_PROOF_NONE;
}

bool ucas_parse(UcasResp *r, const char *line, u32 n)
{
	resp_reset(r);
	Json j = {0};
	if (json_parse(&j, line, n) != 0 || j.type[0] != JS_OBJ) {
		json_free(&j);
		return false;
	}
	r->parsed = true;
	r->id = json_int(&j, json_get(&j, 0, "id"), -1);
	r->ok = json_bool(&j, json_get(&j, 0, "ok"), false);
	r->cmd = str_of(&j, json_get(&j, 0, "cmd"), r);
	r->ucas = str_of(&j, json_get(&j, 0, "ucas"), r);
	r->text = str_of(&j, json_get(&j, 0, "text"), r);
	u32 res = json_get(&j, 0, "result");
	r->kind = str_of(&j, json_get(&j, res, "kind"), r);

	u32 e = json_get(&j, 0, "error");
	if (json_type(&j, e) == JS_OBJ) {
		r->err_code = str_of(&j, json_get(&j, e, "code"), r);
		r->err_msg = str_of(&j, json_get(&j, e, "message"), r);
		r->err_key = str_of(&j, json_get(&j, e, "key"), r);
		r->err_how = str_of(&j, json_get(&j, e, "how"), r);
		u32 pos = json_get(&j, e, "pos");
		r->err_line = (int)json_int(&j, json_get(&j, pos, "line"), 0);
		r->err_col = (int)json_int(&j, json_get(&j, pos, "col"), 0);
		u32 rd = json_get(&j, e, "readings");
		for (u32 i = 0; json_type(&j, rd) == JS_ARR && i < j.size[rd] && r->nread < UCAS_MAX_READINGS; i++) {
			u32 t = json_at(&j, rd, i);
			r->read_text[r->nread] = str_of(&j, json_get(&j, t, "reading"), r);
			r->read_index[r->nread] = (int)json_int(&j, json_get(&j, t, "index"), (i64)i + 1);
			r->read_default[r->nread] = json_bool(&j, json_get(&j, t, "default"), false);
			r->nread++;
		}
	}
	u32 pf = json_get(&j, 0, "proof");
	if (json_type(&j, pf) == JS_OBJ) {
		r->proof = proof_kind(&j, json_get(&j, pf, "kind"));
		r->proof_text = str_of(&j, json_get(&j, pf, "text"), r);
	}
	u32 ck = json_get(&j, 0, "checks");
	for (u32 i = 0; json_type(&j, ck) == JS_ARR && i < j.size[ck] && r->nchecks < UCAS_MAX_CHECKS; i++) {
		u32 t = json_at(&j, ck, i);
		r->check_kind[r->nchecks] = str_of(&j, json_get(&j, t, "kind"), r);
		r->check_text[r->nchecks] = str_of(&j, json_get(&j, t, "text"), r);
		r->check_passed[r->nchecks] = json_bool(&j, json_get(&j, t, "passed"), false);
		r->nchecks++;
	}
	u32 nt = json_get(&j, 0, "notes");
	for (u32 i = 0; json_type(&j, nt) == JS_ARR && i < j.size[nt] && r->nnotes < UCAS_MAX_NOTES; i++) {
		u32 t = json_at(&j, nt, i);
		r->note_kind[r->nnotes] = str_of(&j, json_get(&j, t, "kind"), r);
		r->note_text[r->nnotes] = str_of(&j, json_get(&j, t, "text"), r);
		r->nnotes++;
	}
	u32 cd = json_get(&j, 0, "conditions");
	for (u32 i = 0; json_type(&j, cd) == JS_ARR && i < j.size[cd] && r->nconds < UCAS_MAX_CONDS; i++)
		r->cond[r->nconds++] = str_of(&j, json_at(&j, cd, i), r);

	/* english: result.statements, result.understood */
	u32 st = json_get(&j, res, "statements");
	for (u32 i = 0; json_type(&j, st) == JS_ARR && i < j.size[st] && r->nstmts < UCAS_MAX_STMTS; i++)
		r->stmt[r->nstmts++] = str_of(&j, json_at(&j, st, i), r);
	r->understood = json_bool(&j, json_get(&j, res, "understood"), false);

	/* state: result.functions[].signature / body, result.active_model */
	u32 fn = json_get(&j, res, "functions");
	for (u32 i = 0; json_type(&j, fn) == JS_ARR && i < j.size[fn] && r->nfuncs < UCAS_MAX_FUNCS; i++) {
		u32 t = json_at(&j, fn, i);
		r->func_sig[r->nfuncs] = str_of(&j, json_get(&j, t, "signature"), r);
		r->func_body[r->nfuncs] = str_of(&j, json_get(&j, t, "body"), r);
		r->nfuncs++;
	}
	r->model = str_of(&j, json_get(&j, res, "active_model"), r);

	u32 fl = json_get(&j, 0, "files");
	for (u32 i = 0; json_type(&j, fl) == JS_ARR && i < j.size[fl] && r->nfiles < UCAS_MAX_FILES; i++) {
		u32 t = json_at(&j, fl, i);
		u32 b = json_get(&j, t, "base64");
		if (json_type(&j, b) != JS_STR)
			continue; /* written to disk, or not sent */
		UcasFile *f = &r->files[r->nfiles];
		u32 bn = j.end[b] - j.start[b];
		f->data = malloc((size_t)bn * 3 / 4 + 4);
		if (!f->data)
			continue;
		f->n = ucas_b64_decode(line + j.start[b], bn, f->data);
		f->name = str_of(&j, json_get(&j, t, "name"), r);
		f->mime = str_of(&j, json_get(&j, t, "mime"), r);
		r->nfiles++;
	}
	json_free(&j);
	return true;
}

void ucas_request(JBuf *b, i64 id, const char *cmd, const char *key, const char *text, size_t n, const char *code,
                  size_t code_n, const char *lang, const char *assume_key, int assume_index, double timeout_s)
{
	jb_raw(b, "{\"id\": ");
	jb_int(b, id);
	jb_raw(b, ", \"cmd\": ");
	jb_str(b, cmd);
	jb_raw(b, ", \"args\": {");
	bool first = true;
	if (key) {
		jb_str(b, key);
		jb_raw(b, ": ");
		jb_strn(b, text, n);
		first = false;
	}
	if (code) {
		jb_raw(b, first ? "\"code\": " : ", \"code\": ");
		jb_strn(b, code, code_n);
		first = false;
		if (lang && *lang) {
			jb_raw(b, ", \"lang\": ");
			jb_str(b, lang);
		}
	}
	if (timeout_s > 0) {
		char t[48];
		snprintf(t, sizeof t, "%s\"timeout_s\": %.0f", first ? "" : ", ", timeout_s);
		jb_raw(b, t);
		first = false;
	}
	if (assume_key) {
		jb_raw(b, first ? "\"assume\": {" : ", \"assume\": {");
		jb_str(b, assume_key);
		jb_raw(b, ": ");
		jb_int(b, assume_index);
		jb_raw(b, "}");
	}
	jb_raw(b, "}}\n");
}

bool ucas_looks_english(const char *s, size_t n)
{
	u32 words = 0, run = 0;
	for (size_t i = 0; i <= n; i++) {
		char c = i < n ? s[i] : ' ';
		if (isalpha((unsigned char)c) || c == '\'') {
			run++;
		} else {
			if (run >= 2)
				words++;
			run = 0;
			if (c == ';' || c == '{' || c == '}' || c == '&' || c == '|')
				return false; /* code: ucas's own syntax */
		}
	}
	return words >= 3;
}

int ucas_open_depth(const char *s, size_t n)
{
	int d = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '(' || s[i] == '[' || s[i] == '{')
			d++;
		else if (s[i] == ')' || s[i] == ']' || s[i] == '}')
			d--;
	}
	return d;
}

/* ---- code blocks -------------------------------------------------------------------------------- */

static u32 ws(const char *l, u32 n, u32 i)
{
	while (i < n && (l[i] == ' ' || l[i] == '\t'))
		i++;
	return i;
}

bool ucas_fence_open(const char *l, u32 n, const char **tag, u32 *tag_n)
{
	u32 i = ws(l, n, 0), k = i;
	while (k < n && l[k] == '`')
		k++;
	if (k - i < 3)
		return false;
	u32 a = ws(l, n, k), b = a;
	while (b < n && l[b] != ' ' && l[b] != '\t' && l[b] != '`' && l[b] != '\r')
		b++;
	if (ws(l, n, b) < n && l[ws(l, n, b)] != '\r')
		return false; /* ```c foo: not a fence of ours */
	*tag = l + a;
	*tag_n = b - a;
	return true;
}

bool ucas_fence_close(const char *l, u32 n)
{
	u32 i = ws(l, n, 0), k = i;
	while (k < n && l[k] == '`')
		k++;
	while (k < n && (l[k] == ' ' || l[k] == '\t' || l[k] == '\r'))
		k++;
	return k - i >= 3 && k == n && l[i] == '`';
}

static bool blank(const char *s, u32 n)
{
	for (u32 i = 0; i < n; i++)
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	return true;
}

static const char *ucas_lang_of(int lang)
{
	switch (lang) {
	case LANG_GLSL: return "glsl";
	case LANG_SPIRV: return "spv";
	case LANG_C:
	case LANG_CPP: return "c";
	}
	return NULL;
}

u32 ucas_split(const char *s, u32 n, UcasSeg *out, u32 max)
{
	u32 k = 0, pend = 0; /* pend: the start of text not yet in a piece */
	u32 last_line = UINT32_MAX; /* the start of the last non-blank line since pend */
	for (u32 i = 0; i < n && k < max;) {
		u32 e = i;
		while (e < n && s[e] != '\n')
			e++;
		const char *tag;
		u32 tag_n;
		if (ucas_fence_open(s + i, e - i, &tag, &tag_n)) {
			/* the statement it belongs to: the last line before it */
			u32 st = last_line, se = i;
			if (st != UINT32_MAX && k < max) {
				if (!blank(s + pend, st - pend)) {
					out[k] = (UcasSeg){s + pend, st - pend, NULL, 0, NULL, 0, LANG_NONE, NULL, true};
					k++;
				}
			}
			if (k == max)
				break;
			UcasSeg *g = &out[k++];
			memset(g, 0, sizeof *g);
			if (st != UINT32_MAX) {
				g->stmt = s + st;
				g->stmt_n = se - st;
				while (g->stmt_n && (g->stmt[g->stmt_n - 1] == '\n' || g->stmt[g->stmt_n - 1] == '\r' ||
				                     g->stmt[g->stmt_n - 1] == ' '))
					g->stmt_n--;
			} else {
				g->stmt = "simplify";
				g->stmt_n = 8;
			}
			g->tag = tag;
			g->tag_n = tag_n;
			g->lang = tag_n ? syn_block_lang(tag, tag_n) : LANG_NONE;
			g->ucas_lang = ucas_lang_of(g->lang);
			/* the code: up to the closing fence, or the end */
			u32 c0 = e < n ? e + 1 : n, j = c0;
			g->code = s + c0;
			for (;;) {
				if (j >= n) {
					g->code_n = n - c0;
					i = n;
					break;
				}
				u32 je = j;
				while (je < n && s[je] != '\n')
					je++;
				if (ucas_fence_close(s + j, je - j)) {
					g->code_n = j > c0 ? j - 1 - c0 : 0;
					g->closed = true;
					i = je < n ? je + 1 : n;
					break;
				}
				j = je < n ? je + 1 : n;
			}
			pend = i;
			last_line = UINT32_MAX;
			continue;
		}
		if (!blank(s + i, e - i))
			last_line = i;
		i = e < n ? e + 1 : n;
	}
	if (k < max && pend < n && !blank(s + pend, n - pend))
		out[k++] = (UcasSeg){s + pend, n - pend, NULL, 0, NULL, 0, LANG_NONE, NULL, true};
	return k;
}
