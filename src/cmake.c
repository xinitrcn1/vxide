/* CMake: presets, cache, options, File API targets, command lines. */
#define _POSIX_C_SOURCE 200809L
#include "cmake.h"
#include "json.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE UINT32_MAX
#define PRE_COLS(c) SOA((c)->pre_name), SOA((c)->pre_display), SOA((c)->pre_bindir), SOA((c)->pre_gen)
#define BP_COLS(c) SOA((c)->bp_name), SOA((c)->bp_conf)
#define ENT_COLS(c) SOA((c)->ent_name), SOA((c)->ent_value), SOA((c)->ent_help), SOA((c)->ent_type), SOA((c)->ent_adv)
#define TGT_COLS(c) SOA((c)->tgt_name), SOA((c)->tgt_path), SOA((c)->tgt_type)

static u32 add(CMakeProj *c, const char *s) { return pool_add(&c->pool, s, (u32)strlen(s)); }

void cmake_free(CMakeProj *c)
{
	SoaCol a[] = {PRE_COLS(c)}, b[] = {BP_COLS(c)}, e[] = {ENT_COLS(c)}, t[] = {TGT_COLS(c)};
	soa_free(a, 4);
	soa_free(b, 2);
	soa_free(e, 5);
	soa_free(t, 3);
	pool_free(&c->pool);
	memset(c, 0, sizeof *c);
}

static bool is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---- presets ------------------------------------------------------------------------------ */

/* raw configure presets from every file, before inheritance */
typedef struct {
	char name[128], display[256], bindir[512], gen[64], inherits[8][128];
	u32 ninh;
	bool hidden, has_bindir, has_gen;
} RawPre;

static void str_tok(const Json *j, u32 t, char *out, size_t cap)
{
	out[0] = 0;
	if (json_type(j, t) != JS_STR || j->end[t] - j->start[t] >= cap)
		return;
	out[json_unescape(j->src + j->start[t], j->end[t] - j->start[t], out)] = 0;
}

static void read_presets_file(CMakeProj *c, const char *path, RawPre **pre, u32 *npre, u32 depth)
{
	if (depth > 4)
		return;
	size_t n;
	char *s = vx_read_file(path, &n);
	if (!s)
		return;
	Json j = {0};
	if (json_parse(&j, s, (u32)n) != 0) {
		snprintf(c->err, sizeof c->err, "Cannot parse %s", path);
		json_free(&j);
		free(s);
		return;
	}
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	/* "include": other preset files, relative to this one */
	u32 inc = json_get(&j, 0, "include");
	for (u32 i = 0; inc != JSON_NIL && i < j.size[inc]; i++) {
		char rel[512], p[1600];
		str_tok(&j, json_at(&j, inc, i), rel, sizeof rel);
		if (!rel[0])
			continue;
		if (rel[0] == '/')
			snprintf(p, sizeof p, "%s", rel);
		else
			snprintf(p, sizeof p, "%s/%s", dir, rel);
		read_presets_file(c, p, pre, npre, depth + 1);
	}
	u32 arr = json_get(&j, 0, "configurePresets");
	for (u32 i = 0; arr != JSON_NIL && i < j.size[arr]; i++) {
		u32 o = json_at(&j, arr, i);
		*pre = vx_realloc(*pre, (*npre + 1) * sizeof(RawPre));
		RawPre *r = &(*pre)[(*npre)++];
		memset(r, 0, sizeof *r);
		str_tok(&j, json_get(&j, o, "name"), r->name, sizeof r->name);
		str_tok(&j, json_get(&j, o, "displayName"), r->display, sizeof r->display);
		u32 b = json_get(&j, o, "binaryDir");
		r->has_bindir = b != JSON_NIL;
		str_tok(&j, b, r->bindir, sizeof r->bindir);
		u32 g = json_get(&j, o, "generator");
		r->has_gen = g != JSON_NIL;
		str_tok(&j, g, r->gen, sizeof r->gen);
		r->hidden = json_bool(&j, json_get(&j, o, "hidden"), false);
		u32 in = json_get(&j, o, "inherits");
		if (json_type(&j, in) == JS_STR)
			str_tok(&j, in, r->inherits[r->ninh++], 128);
		else
			for (u32 k = 0; in != JSON_NIL && k < j.size[in] && r->ninh < 8; k++)
				str_tok(&j, json_at(&j, in, k), r->inherits[r->ninh++], 128);
	}
	u32 bps = json_get(&j, 0, "buildPresets");
	for (u32 i = 0; bps != JSON_NIL && i < j.size[bps]; i++) {
		u32 o = json_at(&j, bps, i);
		if (json_bool(&j, json_get(&j, o, "hidden"), false))
			continue;
		char nm[128], cp[128];
		str_tok(&j, json_get(&j, o, "name"), nm, sizeof nm);
		str_tok(&j, json_get(&j, o, "configurePreset"), cp, sizeof cp);
		SoaCol cc[] = {BP_COLS(c)};
		soa_grow(cc, 2, &c->bp_cap, c->nbp + 1, c->nbp);
		c->bp_name[c->nbp] = add(c, nm);
		c->bp_conf[c->nbp] = add(c, cp);
		c->nbp++;
	}
	json_free(&j);
	free(s);
}

/* binaryDir / generator through "inherits" (depth-first, first wins) */
static const RawPre *find_raw(const RawPre *p, u32 n, const char *name)
{
	for (u32 i = 0; i < n; i++)
		if (!strcmp(p[i].name, name))
			return &p[i];
	return NULL;
}

static void inherit(const RawPre *p, u32 n, const RawPre *r, char *bindir, char *gen, int depth)
{
	if (!r || depth > 16)
		return;
	if (!bindir[0] && r->has_bindir)
		snprintf(bindir, 512, "%s", r->bindir);
	if (!gen[0] && r->has_gen)
		snprintf(gen, 64, "%s", r->gen);
	for (u32 i = 0; i < r->ninh; i++)
		inherit(p, n, find_raw(p, n, r->inherits[i]), bindir, gen, depth + 1);
}

/* ${sourceDir}, ${sourceParentDir}, ${sourceDirName}, ${presetName},
 * ${generator}, ${hostSystemName}, ${dollar}, $env{NAME} */
static void expand(const char *in, const char *root, const char *preset, const char *gen, char *out, size_t cap)
{
	char parent[1024], dname[256];
	snprintf(parent, sizeof parent, "%s", root);
	char *sl = strrchr(parent, '/');
	snprintf(dname, sizeof dname, "%s", sl ? sl + 1 : root);
	if (sl)
		*sl = 0;
	size_t o = 0;
	for (const char *s = in; *s && o + 1 < cap;) {
		const char *rep = NULL;
		size_t skip = 0;
		char env[256];
		if (!strncmp(s, "${sourceDir}", 12))
			rep = root, skip = 12;
		else if (!strncmp(s, "${sourceParentDir}", 18))
			rep = parent, skip = 18;
		else if (!strncmp(s, "${sourceDirName}", 16))
			rep = dname, skip = 16;
		else if (!strncmp(s, "${presetName}", 13))
			rep = preset, skip = 13;
		else if (!strncmp(s, "${generator}", 12))
			rep = gen, skip = 12;
		else if (!strncmp(s, "${hostSystemName}", 17))
			rep =
#ifdef __FreeBSD__
			    "FreeBSD",
#elif defined(__linux__)
			    "Linux",
#else
			    "Unknown",
#endif
			skip = 17;
		else if (!strncmp(s, "${dollar}", 9))
			rep = "$", skip = 9;
		else if (!strncmp(s, "$env{", 5) || !strncmp(s, "$penv{", 6)) {
			const char *a = strchr(s, '{') + 1, *e = strchr(a, '}');
			if (e) {
				snprintf(env, sizeof env, "%.*s", (int)(e - a), a);
				rep = getenv(env);
				if (!rep)
					rep = "";
				skip = (size_t)(e + 1 - s);
			}
		}
		if (rep) {
			size_t k = strlen(rep);
			if (o + k >= cap)
				k = cap - o - 1;
			memcpy(out + o, rep, k);
			o += k;
			s += skip;
		} else
			out[o++] = *s++;
	}
	out[o] = 0;
	/* relative binaryDir: relative to the source folder */
	if (out[0] && out[0] != '/') {
		char t[1024];
		snprintf(t, sizeof t, "%s/%s", root, out);
		snprintf(out, cap, "%s", t);
	}
}

void cmake_load(CMakeProj *c, const char *root)
{
	cmake_free(c);
	pool_add(&c->pool, "", 0);
	snprintf(c->root, sizeof c->root, "%s", root);
	char p[1200];
	snprintf(p, sizeof p, "%s/CMakeLists.txt", root);
	c->has_lists = is_file(p);
	if (!c->has_lists)
		return;
	RawPre *pre = NULL;
	u32 npre = 0;
	snprintf(p, sizeof p, "%s/CMakePresets.json", root);
	if (is_file(p))
		read_presets_file(c, p, &pre, &npre, 0);
	snprintf(p, sizeof p, "%s/CMakeUserPresets.json", root);
	if (is_file(p))
		read_presets_file(c, p, &pre, &npre, 0);
	for (u32 i = 0; i < npre; i++) {
		if (pre[i].hidden || !pre[i].name[0])
			continue;
		char bindir[512] = "", gen[64] = "", bd[1024];
		inherit(pre, npre, &pre[i], bindir, gen, 0);
		if (bindir[0])
			expand(bindir, root, pre[i].name, gen, bd, sizeof bd);
		else
			snprintf(bd, sizeof bd, "%s/build/%s", root, pre[i].name);
		SoaCol cc[] = {PRE_COLS(c)};
		soa_grow(cc, 4, &c->pre_cap, c->npre + 1, c->npre);
		c->pre_name[c->npre] = add(c, pre[i].name);
		c->pre_display[c->npre] = add(c, pre[i].display[0] ? pre[i].display : pre[i].name);
		c->pre_bindir[c->npre] = add(c, bd);
		c->pre_gen[c->npre] = add(c, gen);
		c->npre++;
	}
	free(pre);
}

void cmake_build_dir(const CMakeProj *c, u32 preset, char *out, size_t cap)
{
	if (preset < c->npre)
		snprintf(out, cap, "%s", cm_str(c, c->pre_bindir[preset]));
	else
		snprintf(out, cap, "%s/build", c->root);
}

/* ---- cache / options ---------------------------------------------------------------------- */

static u8 type_of(const char *t)
{
	return !strcmp(t, "BOOL") ? CV_BOOL : !strcmp(t, "STRING") ? CV_STRING : !strcmp(t, "PATH") ? CV_PATH
	       : !strcmp(t, "FILEPATH")                             ? CV_FILEPATH
	                                                            : CV_OTHER;
}

static void ent_add(CMakeProj *c, const char *name, size_t nn, const char *val, size_t vn, const char *help, size_t hn,
                    u8 type)
{
	SoaCol cc[] = {ENT_COLS(c)};
	soa_grow(cc, 5, &c->ent_cap, c->nent + 1, c->nent);
	c->ent_name[c->nent] = pool_add(&c->pool, name, (u32)nn);
	c->ent_value[c->nent] = pool_add(&c->pool, val, (u32)vn);
	c->ent_help[c->nent] = pool_add(&c->pool, help, (u32)hn);
	c->ent_type[c->nent] = type;
	c->ent_adv[c->nent] = 0;
	c->nent++;
}

/* option(NAME "help" [ON|OFF]) and set(NAME value CACHE TYPE "help") */
static void read_options(CMakeProj *c)
{
	char p[1200];
	snprintf(p, sizeof p, "%s/CMakeLists.txt", c->root);
	size_t n;
	char *s = vx_read_file(p, &n);
	if (!s)
		return;
	for (size_t i = 0; i < n; i++) {
		bool opt = (i == 0 || s[i - 1] == '\n' || s[i - 1] == ' ' || s[i - 1] == '\t') && !strncmp(s + i, "option(", 7);
		bool set = (i == 0 || s[i - 1] == '\n' || s[i - 1] == ' ' || s[i - 1] == '\t') && !strncmp(s + i, "set(", 4);
		if (!opt && !set)
			continue;
		char *a = s + i + (opt ? 7 : 4), *e = memchr(a, ')', n - (size_t)(a - s));
		if (!e)
			continue;
		/* words, "quoted strings" */
		char w[8][256];
		int nw = 0;
		char *q = a;
		while (q < e && nw < 8) {
			while (q < e && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r'))
				q++;
			if (q >= e)
				break;
			size_t k = 0;
			if (*q == '"') {
				q++;
				while (q < e && *q != '"' && k < 255)
					w[nw][k++] = *q++;
				q++;
			} else
				while (q < e && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && k < 255)
					w[nw][k++] = *q++;
			w[nw++][k] = 0;
		}
		if (opt && nw >= 2) {
			const char *v = nw >= 3 ? w[2] : "OFF";
			ent_add(c, w[0], strlen(w[0]), v, strlen(v), w[1], strlen(w[1]), CV_BOOL);
		} else if (set && nw >= 5 && !strcmp(w[2], "CACHE")) {
			ent_add(c, w[0], strlen(w[0]), w[1], strlen(w[1]), w[4], strlen(w[4]), type_of(w[3]));
		}
		i = (size_t)(e - s);
	}
	free(s);
}

bool cmake_read_cache(CMakeProj *c, const char *builddir)
{
	c->nent = 0;
	c->from_cache = false;
	char p[1200];
	snprintf(p, sizeof p, "%s/CMakeCache.txt", builddir);
	size_t n;
	char *s = vx_read_file(p, &n);
	if (!s) {
		read_options(c);
		return false;
	}
	c->from_cache = true;
	char help[1024];
	size_t hn = 0;
	for (char *l = s, *e = s + n; l < e;) {
		char *nl = memchr(l, '\n', (size_t)(e - l));
		size_t len = (size_t)((nl ? nl : e) - l);
		while (len && l[len - 1] == '\r')
			len--;
		if (len >= 2 && l[0] == '/' && l[1] == '/') {
			/* help comments, possibly several lines */
			size_t k = VX_MIN(len - 2, sizeof help - hn - 2);
			if (hn && k)
				help[hn++] = ' ';
			memcpy(help + hn, l + 2, k);
			hn += k;
		} else if (len && l[0] != '#') {
			char *colon = memchr(l, ':', len), *eq = memchr(l, '=', len);
			if (colon && eq && colon < eq) {
				char type[32];
				snprintf(type, sizeof type, "%.*s", (int)(eq - colon - 1), colon + 1);
				size_t nn = (size_t)(colon - l);
				if (!strcmp(type, "INTERNAL") && nn > 9 && !memcmp(l + nn - 9, "-ADVANCED", 9)) {
					/* NAME-ADVANCED:INTERNAL=1 */
					for (u32 i = 0; i < c->nent; i++)
						if (strlen(cm_str(c, c->ent_name[i])) == nn - 9 &&
						    !memcmp(cm_str(c, c->ent_name[i]), l, nn - 9))
							c->ent_adv[i] = eq[1] == '1';
				} else if (strcmp(type, "INTERNAL") && strcmp(type, "STATIC")) {
					ent_add(c, l, nn, eq + 1, len - (size_t)(eq + 1 - l), help, hn, type_of(type));
				}
			}
			hn = 0;
		} else
			hn = 0;
		l = nl ? nl + 1 : e;
	}
	free(s);
	/* advanced entries come after the cache line they mark: second pass */
	s = vx_read_file(p, &n);
	for (char *l = s, *e = s ? s + n : NULL; l && l < e;) {
		char *nl = memchr(l, '\n', (size_t)(e - l));
		size_t len = (size_t)((nl ? nl : e) - l);
		char *colon = memchr(l, ':', len);
		if (colon && colon - l > 9 && !memcmp(colon - 9, "-ADVANCED", 9) && !strncmp(colon, ":INTERNAL=1", 11))
			for (u32 i = 0; i < c->nent; i++) {
				const char *nm = cm_str(c, c->ent_name[i]);
				if (strlen(nm) == (size_t)(colon - 9 - l) && !memcmp(nm, l, (size_t)(colon - 9 - l)))
					c->ent_adv[i] = 1;
			}
		l = nl ? nl + 1 : e;
	}
	free(s);
	return true;
}

/* ---- File API ------------------------------------------------------------------------------- */

static void mkdirs(const char *p)
{
	char t[1200];
	snprintf(t, sizeof t, "%s", p);
	for (char *s = t + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(t, 0755);
			*s = '/';
		}
	mkdir(t, 0755);
}

void cmake_query(const char *builddir)
{
	char d[1200], f[1300];
	snprintf(d, sizeof d, "%s/.cmake/api/v1/query/client-vxide", builddir);
	mkdirs(d);
	snprintf(f, sizeof f, "%s/codemodel-v2", d);
	if (!is_file(f))
		vx_write_file(f, "", 0);
}

static char *read_json(const char *dir, const char *name, Json *j)
{
	char p[2000];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	size_t n;
	char *s = vx_read_file(p, &n);
	if (!s || json_parse(j, s, (u32)n) != 0) {
		free(s);
		return NULL;
	}
	return s;
}

bool cmake_read_targets(CMakeProj *c, const char *builddir)
{
	c->ntgt = 0;
	char reply[1200];
	snprintf(reply, sizeof reply, "%s/.cmake/api/v1/reply", builddir);
	DIR *dp = opendir(reply);
	if (!dp)
		return false;
	char index[256] = "";
	struct dirent *e;
	while ((e = readdir(dp)))
		if (!strncmp(e->d_name, "index-", 6) && strcmp(e->d_name, index) > 0)
			snprintf(index, sizeof index, "%s", e->d_name);
	closedir(dp);
	if (!index[0])
		return false;
	Json j = {0};
	char *s = read_json(reply, index, &j);
	if (!s)
		return false;
	/* reply -> our query -> codemodel file (fall back to any codemodel-v2) */
	char cmf[256] = "";
	u32 obj = json_get(&j, 0, "objects");
	for (u32 i = 0; obj != JSON_NIL && i < j.size[obj]; i++) {
		u32 o = json_at(&j, obj, i);
		if (json_eq(&j, json_get(&j, o, "kind"), "codemodel"))
			str_tok(&j, json_get(&j, o, "jsonFile"), cmf, sizeof cmf);
	}
	free(s);
	if (!cmf[0]) {
		json_free(&j);
		return false;
	}
	s = read_json(reply, cmf, &j);
	if (!s) {
		json_free(&j);
		return false;
	}
	u32 tg = json_path(&j, json_at(&j, json_get(&j, 0, "configurations"), 0), "targets");
	char (*files)[256] = vx_alloc((tg != JSON_NIL ? j.size[tg] : 0) * 256 + 1);
	u32 nf = 0;
	for (u32 i = 0; tg != JSON_NIL && i < j.size[tg]; i++)
		str_tok(&j, json_get(&j, json_at(&j, tg, i), "jsonFile"), files[nf++], 256);
	free(s);
	for (u32 i = 0; i < nf; i++) {
		Json t = {0};
		char *ts = read_json(reply, files[i], &t);
		if (!ts)
			continue;
		char name[256], type[64], art[1024] = "";
		str_tok(&t, json_get(&t, 0, "name"), name, sizeof name);
		str_tok(&t, json_get(&t, 0, "type"), type, sizeof type);
		str_tok(&t, json_get(&t, json_at(&t, json_get(&t, 0, "artifacts"), 0), "path"), art, sizeof art);
		char full[2200] = "";
		if (art[0])
			snprintf(full, sizeof full, art[0] == '/' ? "%s%s" : "%s/%s", art[0] == '/' ? "" : builddir, art);
		u8 ty = !strcmp(type, "EXECUTABLE")        ? CT_EXECUTABLE
		        : !strcmp(type, "STATIC_LIBRARY") ? CT_STATIC
		        : !strcmp(type, "SHARED_LIBRARY") ? CT_SHARED
		        : !strcmp(type, "MODULE_LIBRARY") ? CT_MODULE
		        : !strcmp(type, "OBJECT_LIBRARY") ? CT_OBJECT
		        : !strcmp(type, "INTERFACE_LIBRARY") ? CT_INTERFACE
		        : !strcmp(type, "UTILITY")           ? CT_UTILITY
		                                             : CT_OTHER;
		SoaCol cc[] = {TGT_COLS(c)};
		soa_grow(cc, 3, &c->tgt_cap, c->ntgt + 1, c->ntgt);
		c->tgt_name[c->ntgt] = add(c, name);
		c->tgt_path[c->ntgt] = add(c, full);
		c->tgt_type[c->ntgt] = ty;
		c->ntgt++;
		json_free(&t);
		free(ts);
	}
	free(files);
	json_free(&j);
	return c->ntgt > 0;
}

u32 cmake_executables(const CMakeProj *c, u32 *out, u32 cap)
{
	u32 k = 0;
	for (u32 i = 0; i < c->ntgt && k < cap; i++)
		if (c->tgt_type[i] == CT_EXECUTABLE)
			out[k++] = i;
	return k;
}

/* ---- commands ------------------------------------------------------------------------------ */

typedef struct {
	char **argv;
	u32 n, max;
	char *buf;
	size_t used, cap;
} Args;

static void arg(Args *a, const char *s)
{
	size_t k = strlen(s) + 1;
	if (a->n + 1 >= a->max || a->used + k > a->cap)
		return;
	memcpy(a->buf + a->used, s, k);
	a->argv[a->n++] = a->buf + a->used;
	a->used += k;
	a->argv[a->n] = NULL;
}

u32 cmake_configure_argv(const CMakeProj *c, u32 preset, const char *builddir, const char *const *defs, u32 ndefs,
                         char **argv, u32 max, char *buf, size_t cap)
{
	Args a = {argv, 0, max, buf, 0, cap};
	arg(&a, "cmake");
	if (preset < c->npre) {
		char p[300];
		snprintf(p, sizeof p, "--preset=%s", cm_str(c, c->pre_name[preset]));
		arg(&a, p);
		/* the preset's folder; -S keeps relative includes right */
		arg(&a, "-S");
		arg(&a, c->root);
	} else {
		arg(&a, "-S");
		arg(&a, c->root);
		arg(&a, "-B");
		arg(&a, builddir);
		/* a debugger wants symbols: Debug unless the cache already says */
		char cache[1200];
		snprintf(cache, sizeof cache, "%s/CMakeCache.txt", builddir);
		bool have_bt = false;
		for (u32 i = 0; i < ndefs; i++)
			have_bt |= !strncmp(defs[i], "CMAKE_BUILD_TYPE", 16);
		if (!is_file(cache) && !have_bt)
			arg(&a, "-DCMAKE_BUILD_TYPE=Debug");
	}
	for (u32 i = 0; i < ndefs; i++) {
		char d[1200];
		snprintf(d, sizeof d, "-D%s", defs[i]);
		arg(&a, d);
	}
	return a.n;
}

u32 cmake_build_argv(const CMakeProj *c, const char *builddir, const char *target, char **argv, u32 max, char *buf,
                     size_t cap)
{
	(void)c;
	Args a = {argv, 0, max, buf, 0, cap};
	arg(&a, "cmake");
	arg(&a, "--build");
	arg(&a, builddir);
	if (target && *target) {
		arg(&a, "--target");
		arg(&a, target);
	}
	arg(&a, "--parallel");
	return a.n;
}
