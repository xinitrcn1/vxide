/* The LLM agent's built-in tools, after mcp-fbsd (~/workspace/mcp-fbsd):
 * the same tool names, arguments and terse answers, with every path
 * confined to the project folder. What the server has and this does not:
 * process sessions (proc_*), ptys and images; exec runs a command to its
 * end (or its timeout) and returns its output. */
#define _DEFAULT_SOURCE
#include "llmtools.h"
#include "search.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define READ_LINES 2000
#define LINE_MAX_SHOWN 2000
#define FILE_MAX (16u << 20)

/* ---- the schema ----------------------------------------------------------------------------- */

#define P_STR(n, d) "\"" n "\":{\"type\":\"string\"" d "}"
#define P_INT(n, d) "\"" n "\":{\"type\":\"integer\"" d "}"
#define P_BOOL(n, d) "\"" n "\":{\"type\":\"boolean\"" d "}"
#define D(s) ",\"description\":\"" s "\""
#define OBJ(props, req) "{\"type\":\"object\",\"properties\":{" props "},\"required\":[" req "]}"

enum { F_WRITE = 1, F_EXEC = 2 };

static const struct {
	const char *name;
	unsigned flags;
	const char *desc, *schema;
} tools[] = {
    {"read", 0, "Read a text file. Partial reads end with [lines A-B of N].",
     OBJ(P_STR("path", "") "," P_INT("offset", D("first line, 1-based; negative = from end")) "," P_INT(
             "limit", D("max lines, default 2000")) "," P_BOOL("n", D("prefix line numbers")),
         "\"path\"")},
    {"write", F_WRITE, "Write a file (atomic replace), creating parent dirs.",
     OBJ(P_STR("path", "") "," P_STR("content", "") "," P_BOOL("append", ""), "\"path\",\"content\"")},
    {"edit", F_WRITE,
     "Replace exact text in a file. old must match `count` times (default 1; 0 = all). Use edits[] for several "
     "replacements in one atomic write.",
     OBJ(P_STR("path", "") "," P_STR("old", "") "," P_STR("new", "") "," P_INT("count", "") ","
                                                                                                 "\"edits\":{\"type\":"
                                                                                                 "\"array\",\"items\":" OBJ(
                                                                                                     P_STR("old", "") "," P_STR(
                                                                                                         "new", "") "," P_INT("count", ""),
                                                                                                     "\"old\",\"new\"") "}",
         "\"path\"")},
    {"ls", 0, "List a directory. dir/  link@ -> target  file size.",
     OBJ(P_STR("path", "") "," P_INT("depth", D("default 1")) "," P_BOOL("hidden", D("default true")), "\"path\"")},
    {"stat", 0, "File type, size, mode, owner, mtime.", OBJ(P_STR("path", ""), "\"path\"")},
    {"mv", F_WRITE, "Move or rename.", OBJ(P_STR("from", "") "," P_STR("to", ""), "\"from\",\"to\"")},
    {"mkdir", F_WRITE, "Create a directory and its parents.", OBJ(P_STR("path", ""), "\"path\"")},
    {"search", 0,
     "Find files by name glob, or lines by regex (POSIX ERE / RE2) when re is set. Output: path or path:line:text. Skips "
     "hidden and binary files.",
     OBJ(P_STR("path", "") "," P_STR("glob", D("name filter, e.g. *.c")) "," P_STR("re", D("content pattern")) "," P_BOOL(
             "i", D("ignore case")) "," P_BOOL("fixed", D("re is a literal string")) "," P_BOOL("hidden", "") "," P_INT("max",
                                                                                                                    D("default 200")),
         "\"path\"")},
    {"exec", F_EXEC,
     "Run a shell command in the project. Returns its output (stdout and stderr) and [exit N]; it is killed after "
     "timeout_ms.",
     OBJ(P_STR("cmd", "") "," P_STR("cwd", D("relative to the project")) "," P_INT("timeout_ms", D("default 10000")) "," P_STR(
             "stdin", D("written then closed")),
         "\"cmd\"")},
};

static bool offered(const LlmTools *t, unsigned flags)
{
	if ((flags & (F_WRITE | F_EXEC)) && t->read_only)
		return false;
	if ((flags & F_EXEC) && t->no_exec)
		return false;
	return true;
}

void llmt_schema(const LlmTools *t, JBuf *b)
{
	jb_raw(b, "[");
	bool first = true;
	for (size_t i = 0; i < VX_COUNT(tools); i++) {
		if (!offered(t, tools[i].flags))
			continue;
		jb_raw(b, first ? "" : ",");
		first = false;
		jb_raw(b, "{\"type\":\"function\",\"function\":{\"name\":");
		jb_str(b, tools[i].name);
		jb_raw(b, ",\"description\":");
		jb_str(b, tools[i].desc);
		jb_raw(b, ",\"parameters\":");
		jb_raw(b, tools[i].schema);
		jb_raw(b, "}}");
	}
	jb_raw(b, "]");
}

/* ---- arguments and answers ---------------------------------------------------------------------- */

typedef struct {
	Json j;
	Pool p; /* unescaped strings */
	bool ok;
} Args;

static void args_parse(Args *a, const char *s, size_t n)
{
	memset(a, 0, sizeof *a);
	/* unescaped strings are never longer than the source, and each argument
	 * is fetched once: with this room the pool never moves, so the strings
	 * handed out stay valid together */
	VX_GROW(a->p.b, a->p.cap, (u32)(2 * n + 64));
	pool_add(&a->p, "", 0);
	a->ok = n == 0 || (json_parse(&a->j, s, (u32)n) == 0 && json_type(&a->j, 0) == JS_OBJ);
	if (n == 0)
		a->ok = json_parse(&a->j, "{}", 2) == 0;
}

static void args_free(Args *a)
{
	json_free(&a->j);
	pool_free(&a->p);
}

/* a string argument, or NULL when absent (*len its length) */
static const char *arg_str_in(Args *a, u32 obj, const char *k, u32 *len)
{
	u32 t = json_get(&a->j, obj, k);
	if (json_type(&a->j, t) != JS_STR)
		return NULL;
	u32 off = json_str_pool(&a->j, t, &a->p);
	if (len)
		*len = (u32)strlen(a->p.b + off); /* a NUL inside is cut there */
	return a->p.b + off;
}
static const char *arg_str(Args *a, const char *k, u32 *len) { return arg_str_in(a, 0, k, len); }

static i64 arg_int(Args *a, const char *k, i64 def)
{
	u32 t = json_get(&a->j, 0, k);
	if (json_type(&a->j, t) == JS_STR) { /* small models quote numbers */
		char tmp[32];
		u32 n = VX_MIN(a->j.end[t] - a->j.start[t], 31u);
		memcpy(tmp, a->j.src + a->j.start[t], n);
		tmp[n] = 0;
		char *e;
		long long v = strtoll(tmp, &e, 10);
		return e != tmp ? v : def;
	}
	return json_int(&a->j, t, def);
}

static bool arg_bool(Args *a, const char *k, bool def)
{
	u32 t = json_get(&a->j, 0, k);
	if (json_type(&a->j, t) == JS_STR)
		return json_eq(&a->j, t, "true") || json_eq(&a->j, t, "1");
	return json_bool(&a->j, t, def);
}

static void outf(JBuf *o, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (k > 0)
		jb_rawn(o, tmp, (size_t)VX_MIN(k, (int)sizeof tmp - 1));
}

static bool fail(JBuf *o, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	jb_reset(o);
	jb_raw(o, tmp);
	return false;
}

/* ---- paths ------------------------------------------------------------------------------------------ */

typedef struct {
	char root[PATH_MAX];
	size_t rootlen;
} Ctx;

static bool under(const Ctx *c, const char *p)
{
	return !strncmp(p, c->root, c->rootlen) && (p[c->rootlen] == '/' || !p[c->rootlen] || c->rootlen == 1);
}

/* the path relative to the project, for answers ("." for the root) */
static const char *rel(const Ctx *c, const char *abs)
{
	if (!under(c, abs))
		return abs;
	const char *r = abs + c->rootlen;
	while (*r == '/')
		r++;
	return *r ? r : ".";
}

/* Resolve `in` (relative to the project, or absolute inside it) to a real
 * path under the root. A path that does not exist yet (must_exist false)
 * resolves its deepest existing folder, and the rest may not climb. */
static bool resolve(const Ctx *c, const char *in, char out[PATH_MAX], bool must_exist, JBuf *o)
{
	if (!in || !*in)
		return fail(o, "path required");
	char tmp[PATH_MAX];
	int k = in[0] == '/' ? snprintf(tmp, sizeof tmp, "%s", in) : snprintf(tmp, sizeof tmp, "%s/%s", c->root, in);
	if (k < 0 || (size_t)k >= sizeof tmp)
		return fail(o, "path too long");
	if (realpath(tmp, out))
		goto check;
	if (must_exist || errno != ENOENT)
		return fail(o, "%s: %s", in, strerror(errno));
	{
		size_t l = strlen(tmp);
		while (l > 1 && tmp[l - 1] == '/')
			tmp[--l] = 0;
		char *cut = tmp + l;
		for (;;) {
			char *s = cut;
			while (s > tmp && s[-1] != '/')
				s--;
			if (s == tmp)
				return fail(o, "%s: no such path", in);
			char save = s[-1];
			s[-1] = 0;
			bool ok = realpath(s - 1 == tmp ? "/" : tmp, out) != NULL;
			s[-1] = save;
			if (ok) {
				cut = s;
				break;
			}
			if (errno != ENOENT)
				return fail(o, "%s: %s", in, strerror(errno));
			cut = s - 1;
		}
		for (char *p = cut; *p;) {
			char *e = strchr(p, '/');
			size_t cl = e ? (size_t)(e - p) : strlen(p);
			if (cl == 2 && p[0] == '.' && p[1] == '.')
				return fail(o, "%s: '..' after a missing folder", in);
			p += cl + (e != NULL);
		}
		size_t ol = strlen(out);
		if (ol + 1 + strlen(cut) >= PATH_MAX)
			return fail(o, "path too long");
		if (out[ol - 1] != '/')
			out[ol++] = '/';
		strcpy(out + ol, cut);
	}
check:
	if (!under(c, out))
		return fail(o, "%s: outside the project", in);
	return true;
}

static int mkdir_p(char *p, size_t upto)
{
	for (size_t i = 1; i <= upto; i++) {
		if (i < upto && p[i] != '/')
			continue;
		char ch = p[i];
		p[i] = 0;
		int rc = mkdir(p, 0777);
		p[i] = ch;
		if (rc && errno != EEXIST)
			return -1;
	}
	return 0;
}

static bool parent_dirs(char *path)
{
	char *sl = strrchr(path, '/');
	if (!sl || sl == path)
		return true;
	*sl = 0;
	struct stat st;
	bool miss = stat(path, &st) != 0;
	*sl = '/';
	return !miss || mkdir_p(path, (size_t)(sl - path)) == 0;
}

/* Replace path with data through a temp file and rename, keeping its mode. */
static bool replace_file(char *path, const char *data, size_t n, JBuf *o, const Ctx *c)
{
	struct stat st;
	mode_t mode;
	if (stat(path, &st) == 0)
		mode = st.st_mode & 07777;
	else {
		mode_t um = umask(0);
		umask(um);
		mode = 0666 & ~um;
		if (!parent_dirs(path))
			return fail(o, "mkdir %s: %s", rel(c, path), strerror(errno));
	}
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
	int fd = mkstemp(tmp);
	if (fd < 0)
		return fail(o, "%s: %s", rel(c, path), strerror(errno));
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, data + off, n - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			break;
		off += (size_t)w;
	}
	if (off < n || fchmod(fd, mode) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
		int e = errno;
		unlink(tmp);
		return fail(o, "%s: %s", rel(c, path), strerror(e));
	}
	return true;
}

/* ---- read ------------------------------------------------------------------------------------------- */

static bool t_read(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	if (!resolve(c, arg_str(a, "path", NULL), path, true, o))
		return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return fail(o, "%s: not a regular file", rel(c, path));
	if ((u64)st.st_size > FILE_MAX)
		return fail(o, "%s: %lld bytes, too large to read", rel(c, path), (long long)st.st_size);
	size_t n = 0;
	char *s = vx_read_file(path, &n);
	if (!s)
		return fail(o, "%s: %s", rel(c, path), strerror(errno));
	if (memchr(s, 0, VX_MIN(n, (size_t)4096))) {
		free(s);
		return fail(o, "%s: binary, %zu bytes", rel(c, path), n);
	}
	i64 total = 0;
	for (size_t i = 0; i < n; i++)
		total += s[i] == '\n';
	if (n && s[n - 1] != '\n')
		total++;
	i64 off = arg_int(a, "offset", 1), lim = arg_int(a, "limit", READ_LINES);
	bool num = arg_bool(a, "n", false);
	if (lim <= 0)
		lim = READ_LINES;
	if (off < 0)
		off = VX_MAX(total + off + 1, 1);
	if (off == 0)
		off = 1;
	if (off > total && total) {
		free(s);
		return fail(o, "offset %lld past end (%lld lines)", (long long)off, (long long)total);
	}
	i64 ln = 0, last = 0;
	for (size_t i = 0; i < n;) {
		const char *e = memchr(s + i, '\n', n - i);
		size_t l = e ? (size_t)(e - s) - i : n - i;
		ln++;
		if (ln >= off && ln < off + lim && o->n < LLMT_OUT_MAX) {
			if (num)
				outf(o, "%lld\t", (long long)ln);
			jb_rawn(o, s + i, VX_MIN(l, (size_t)LINE_MAX_SHOWN));
			if (l > LINE_MAX_SHOWN)
				outf(o, "...[+%zu]", l - LINE_MAX_SHOWN);
			jb_raw(o, "\n");
			last = ln;
		}
		i += l + 1;
	}
	free(s);
	if (off > 1 || last < total)
		outf(o, "[lines %lld-%lld of %lld]\n", (long long)off, (long long)last, (long long)total);
	return true;
}

/* ---- write, edit -------------------------------------------------------------------------------------- */

static bool t_write(const Ctx *c, Args *a, JBuf *o)
{
	u32 n;
	const char *data = arg_str(a, "content", &n);
	if (!data)
		return fail(o, "content required");
	char path[PATH_MAX];
	if (!resolve(c, arg_str(a, "path", NULL), path, false, o))
		return false;
	if (arg_bool(a, "append", false)) {
		if (!parent_dirs(path))
			return fail(o, "mkdir: %s", strerror(errno));
		FILE *f = fopen(path, "ab");
		if (!f || fwrite(data, 1, n, f) != n) {
			if (f)
				fclose(f);
			return fail(o, "%s: %s", rel(c, path), strerror(errno));
		}
		if (fclose(f) != 0)
			return fail(o, "%s: %s", rel(c, path), strerror(errno));
		return true;
	}
	return replace_file(path, data, n, o, c);
}

static size_t count_occ(const char *h, size_t hn, const char *nd, size_t nn)
{
	size_t k = 0;
	for (const char *p = h; nn && (size_t)(h + hn - p) >= nn;) {
		const char *q = memmem(p, (size_t)(h + hn - p), nd, nn);
		if (!q)
			break;
		k++;
		p = q + nn;
	}
	return k;
}

/* s[0..*n) with every `old` replaced by `new_` (malloc'd) */
static char *replace_all(const char *s, size_t *n, const char *old, size_t on, const char *nw, size_t nn)
{
	size_t k = count_occ(s, *n, old, on);
	size_t outn = *n + k * nn - k * on;
	char *r = vx_alloc(outn + 1), *w = r;
	const char *p = s, *end = s + *n;
	for (;;) {
		const char *q = (size_t)(end - p) >= on ? memmem(p, (size_t)(end - p), old, on) : NULL;
		if (!q)
			break;
		memcpy(w, p, (size_t)(q - p));
		w += q - p;
		memcpy(w, nw, nn);
		w += nn;
		p = q + on;
	}
	memcpy(w, p, (size_t)(end - p));
	*n = outn;
	return r;
}

static bool t_edit(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	if (!resolve(c, arg_str(a, "path", NULL), path, true, o))
		return false;
	size_t n = 0;
	char *s = vx_read_file(path, &n);
	if (!s)
		return fail(o, "%s: %s", rel(c, path), strerror(errno));
	/* one edit (old, new, count) or edits[] */
	u32 eds = json_get(&a->j, 0, "edits");
	u32 ne = json_type(&a->j, eds) == JS_ARR ? a->j.size[eds] : 0;
	bool single = !ne;
	for (u32 i = 0; i < (single ? 1 : ne); i++) {
		u32 obj = single ? 0 : json_at(&a->j, eds, i);
		u32 on, nn;
		const char *old = arg_str_in(a, obj, "old", &on), *nw = arg_str_in(a, obj, "new", &nn);
		i64 want = json_int(&a->j, json_get(&a->j, obj, "count"), 1);
		if (!old || !nw || !on) {
			free(s);
			return fail(o, "edit %u: old and new required", i + 1);
		}
		size_t k = count_occ(s, n, old, on);
		if (!k || (want > 0 && (i64)k != want)) {
			free(s);
			if (!k)
				return fail(o, "%s: edit %u: old not found", rel(c, path), i + 1);
			return fail(o, "%s: edit %u: old matches %zu times, count is %lld", rel(c, path), i + 1, k, (long long)want);
		}
		char *t = replace_all(s, &n, old, on, nw, nn);
		free(s);
		s = t;
	}
	bool ok = replace_file(path, s, n, o, c);
	free(s);
	return ok;
}

/* ---- ls, stat, mv, mkdir ------------------------------------------------------------------------------- */

static int cmp_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static void ls_dir(const Ctx *c, const char *dir, int depth, int ind, bool hidden, JBuf *o)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	char **names = NULL;
	u32 nn = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (!hidden && e->d_name[0] == '.')
			continue;
		VX_GROW(names, cap, nn + 1);
		names[nn++] = vx_strdup(e->d_name);
	}
	closedir(d);
	if (nn)
		qsort(names, nn, sizeof *names, cmp_str);
	for (u32 i = 0; i < nn; i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, names[i]);
		struct stat st;
		for (int k = 0; k < ind; k++)
			jb_raw(o, "  ");
		if (lstat(p, &st) != 0)
			outf(o, "%s ?\n", names[i]);
		else if (S_ISLNK(st.st_mode)) {
			char t[PATH_MAX];
			ssize_t k = readlink(p, t, sizeof t - 1);
			t[k > 0 ? k : 0] = 0;
			outf(o, "%s@ -> %s\n", names[i], t);
		} else if (S_ISDIR(st.st_mode)) {
			outf(o, "%s/\n", names[i]);
			if (depth > 1 && o->n < LLMT_OUT_MAX)
				ls_dir(c, p, depth - 1, ind + 1, hidden, o);
		} else
			outf(o, "%s %lld\n", names[i], (long long)st.st_size);
		free(names[i]);
		if (o->n >= LLMT_OUT_MAX) {
			for (u32 k = i + 1; k < nn; k++)
				free(names[k]);
			jb_raw(o, "[cut]\n");
			break;
		}
	}
	free(names);
}

static bool t_ls(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	const char *in = arg_str(a, "path", NULL);
	if (!resolve(c, in && *in ? in : ".", path, true, o))
		return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return fail(o, "%s: not a folder", rel(c, path));
	ls_dir(c, path, (int)VX_CLAMP(arg_int(a, "depth", 1), 1, 8), 0, arg_bool(a, "hidden", true), o);
	if (!o->n)
		jb_raw(o, "(empty)\n");
	return true;
}

static bool t_stat(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	if (!resolve(c, arg_str(a, "path", NULL), path, true, o))
		return false;
	struct stat st;
	if (lstat(path, &st) != 0)
		return fail(o, "%s: %s", rel(c, path), strerror(errno));
	const char *ty = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "dir" : S_ISLNK(st.st_mode) ? "link" : "other";
	struct passwd *pw = getpwuid(st.st_uid);
	struct group *gr = getgrgid(st.st_gid);
	char tm[32];
	struct tm lt;
	time_t mt = st.st_mtime;
	strftime(tm, sizeof tm, "%Y-%m-%d %H:%M:%S", localtime_r(&mt, &lt));
	outf(o, "%s %lld %04o %s:%s %s\n", ty, (long long)st.st_size, (unsigned)(st.st_mode & 07777), pw ? pw->pw_name : "?",
	     gr ? gr->gr_name : "?", tm);
	return true;
}

static bool t_mv(const Ctx *c, Args *a, JBuf *o)
{
	char from[PATH_MAX], to[PATH_MAX];
	if (!resolve(c, arg_str(a, "from", NULL), from, true, o) || !resolve(c, arg_str(a, "to", NULL), to, false, o))
		return false;
	if (!strcmp(from, c->root))
		return fail(o, "cannot move the project folder");
	if (!parent_dirs(to) || rename(from, to) != 0)
		return fail(o, "mv: %s", strerror(errno));
	return true;
}

static bool t_mkdir(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	if (!resolve(c, arg_str(a, "path", NULL), path, false, o))
		return false;
	if (mkdir_p(path, strlen(path)) != 0)
		return fail(o, "mkdir %s: %s", rel(c, path), strerror(errno));
	return true;
}

/* ---- search --------------------------------------------------------------------------------------------- */

typedef struct {
	const Ctx *c;
	const char *glob;
	Matcher m;
	bool has_re, hidden;
	i64 max, found;
	JBuf *o;
} Search;

static bool skip_dir(const char *name)
{
	return !strcmp(name, ".git") || !strcmp(name, ".svn") || !strcmp(name, ".hg") || !strcmp(name, "node_modules");
}

static bool line_has(Search *s, const char *l, size_t n)
{
	u32 a, b;
	return matcher_next(&s->m, l, (u32)n, 0, &a, &b);
}

static void search_file(Search *s, const char *path)
{
	if (!s->has_re) {
		outf(s->o, "%s\n", rel(s->c, path));
		s->found++;
		return;
	}
	struct stat st;
	if (stat(path, &st) != 0 || (u64)st.st_size > FILE_MAX)
		return;
	size_t n = 0;
	char *b = vx_read_file(path, &n);
	if (!b)
		return;
	if (!memchr(b, 0, VX_MIN(n, (size_t)4096))) {
		u32 ln = 0;
		for (size_t i = 0; i < n && s->found < s->max;) {
			const char *e = memchr(b + i, '\n', n - i);
			size_t l = e ? (size_t)(e - b) - i : n - i;
			ln++;
			if (line_has(s, b + i, l)) {
				size_t shown = VX_MIN(l, (size_t)300);
				outf(s->o, "%s:%u:", rel(s->c, path), ln);
				jb_rawn(s->o, b + i, shown);
				jb_raw(s->o, shown < l ? "...\n" : "\n");
				s->found++;
			}
			i += l + 1;
		}
	}
	free(b);
}

static void search_walk(Search *s, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	char **names = NULL;
	u32 nn = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (!s->hidden && e->d_name[0] == '.'))
			continue;
		VX_GROW(names, cap, nn + 1);
		names[nn++] = vx_strdup(e->d_name);
	}
	closedir(d);
	if (nn)
		qsort(names, nn, sizeof *names, cmp_str);
	for (u32 i = 0; i < nn; i++) {
		if (s->found < s->max && s->o->n < LLMT_OUT_MAX) {
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s", dir, names[i]);
			struct stat st;
			if (lstat(p, &st) == 0) {
				if (S_ISDIR(st.st_mode) && !skip_dir(names[i]))
					search_walk(s, p);
				else if (S_ISREG(st.st_mode) && (!s->glob || fnmatch(s->glob, names[i], 0) == 0))
					search_file(s, p);
			}
		}
		free(names[i]);
	}
	free(names);
}

static bool t_search(const Ctx *c, Args *a, JBuf *o)
{
	char path[PATH_MAX];
	const char *in = arg_str(a, "path", NULL);
	if (!resolve(c, in && *in ? in : ".", path, true, o))
		return false;
	Search s = {0};
	s.c = c;
	s.o = o;
	s.glob = arg_str(a, "glob", NULL);
	if (s.glob && !*s.glob)
		s.glob = NULL;
	s.hidden = arg_bool(a, "hidden", false);
	s.max = VX_CLAMP(arg_int(a, "max", 200), 1, 5000);
	u32 rn;
	const char *re = arg_str(a, "re", &rn);
	if (re && rn) {
		int fl = (arg_bool(a, "i", false) ? 0 : SEARCH_CASE) | (arg_bool(a, "fixed", false) ? 0 : SEARCH_REGEX);
		if (!matcher_init(&s.m, re, rn, fl)) {
			bool ok = fail(o, "re: %s", s.m.err[0] ? s.m.err : "bad pattern");
			matcher_free(&s.m);
			return ok;
		}
		s.has_re = true;
	}
	struct stat st;
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		search_file(&s, path);
	else
		search_walk(&s, path);
	if (s.has_re)
		matcher_free(&s.m);
	if (!s.found)
		jb_raw(o, "no matches\n");
	else if (s.found >= s.max)
		outf(o, "[first %lld matches]\n", (long long)s.max);
	return true;
}

/* ---- exec ----------------------------------------------------------------------------------------------- */

/* Output as a model wants it: ANSI escapes removed, a \r-rewritten line
 * kept as its last state, and the middle of a long output cut. */
static void clean_output(const char *s, size_t n, JBuf *o)
{
	JBuf t = {0};
	size_t line0 = 0; /* where the current line starts in t */
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == 0x1B) {
			if (i + 1 < n && s[i + 1] == '[') {
				i += 2;
				while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E))
					i++;
			} else if (i + 1 < n && s[i + 1] == ']') {
				while (i < n && s[i] != 0x07 && !(s[i] == 0x1B && i + 1 < n && s[i + 1] == '\\'))
					i++;
			} else
				i++;
			continue;
		}
		if (ch == '\r') {
			if (i + 1 < n && s[i + 1] == '\n')
				continue;
			t.n = (u32)line0; /* the line is rewritten */
			continue;
		}
		jb_rawn(&t, s + i, 1);
		if (ch == '\n')
			line0 = t.n;
	}
	if (t.n > LLMT_OUT_MAX) {
		u32 keep = LLMT_OUT_MAX / 2;
		jb_rawn(o, t.b, keep);
		outf(o, "\n[... %u bytes cut ...]\n", t.n - 2 * keep);
		jb_rawn(o, t.b + t.n - keep, keep);
	} else if (t.n)
		jb_rawn(o, t.b, t.n);
	jb_free(&t);
}

static bool t_exec(const Ctx *c, const LlmTools *lt, Args *a, JBuf *o)
{
	const char *cmd = arg_str(a, "cmd", NULL);
	if (!cmd || !*cmd)
		return fail(o, "cmd required");
	char cwd[PATH_MAX];
	const char *cw = arg_str(a, "cwd", NULL);
	if (!resolve(c, cw && *cw ? cw : ".", cwd, true, o))
		return false;
	i64 tmo = arg_int(a, "timeout_ms", lt->exec_timeout_ms > 0 ? lt->exec_timeout_ms : 10000);
	tmo = VX_CLAMP(tmo, 100, 600000);
	u32 inn = 0;
	const char *in = arg_str(a, "stdin", &inn);
	int pi[2], po[2];
	if (pipe(pi) != 0)
		return fail(o, "pipe: %s", strerror(errno));
	if (pipe(po) != 0) {
		close(pi[0]);
		close(pi[1]);
		return fail(o, "pipe: %s", strerror(errno));
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pi[0], 0);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_adddup2(&fa, po[1], 2);
	posix_spawn_file_actions_addclose(&fa, pi[0]);
	posix_spawn_file_actions_addclose(&fa, pi[1]);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	posix_spawn_file_actions_addclose(&fa, po[1]);
	posix_spawn_file_actions_addchdir_np(&fa, cwd);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	sigset_t none, def;
	sigemptyset(&none);
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &def);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	posix_spawnattr_setpgroup(&at, 0);
	/* the environment, with nothing that waits on a terminal */
	u32 ne = 0;
	while (environ[ne])
		ne++;
	char **env = vx_alloc((ne + 4) * sizeof *env);
	u32 k = 0;
	for (u32 i = 0; i < ne; i++)
		if (strncmp(environ[i], "TERM=", 5) && strncmp(environ[i], "NO_COLOR=", 9) && strncmp(environ[i], "PAGER=", 6))
			env[k++] = environ[i];
	env[k++] = "TERM=dumb";
	env[k++] = "NO_COLOR=1";
	env[k++] = "PAGER=cat";
	env[k] = NULL;
	char *argv[] = {"sh", "-c", (char *)cmd, NULL};
	pid_t pid;
	int rc = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	free(env);
	close(pi[0]);
	close(po[1]);
	if (rc != 0) {
		close(pi[1]);
		close(po[0]);
		return fail(o, "exec: %s", strerror(rc));
	}
	/* stdin: written as the child reads it, then closed */
	fcntl(pi[1], F_SETFL, fcntl(pi[1], F_GETFL) | O_NONBLOCK);
	u32 in_off = 0;
	if (!in || !inn) {
		close(pi[1]);
		pi[1] = -1;
	}
	JBuf raw = {0};
	u64 end = vx_now_ns() + (u64)tmo * 1000000ull;
	bool timed_out = false, cancelled = false;
	for (;;) {
		struct pollfd pf[2] = {{po[0], POLLIN, 0}, {pi[1], POLLOUT, 0}};
		int np = pi[1] >= 0 ? 2 : 1;
		int r = poll(pf, (nfds_t)np, 50);
		if (r < 0 && errno != EINTR)
			break;
		if (np == 2 && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
			ssize_t w = write(pi[1], in + in_off, inn - in_off);
			if (w > 0)
				in_off += (u32)w;
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				in_off = inn;
			if (in_off >= inn) {
				close(pi[1]);
				pi[1] = -1;
			}
		}
		if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			char b[8192];
			ssize_t n = read(po[0], b, sizeof b);
			if (n > 0) {
				jb_rawn(&raw, b, (size_t)n);
				if (raw.n > 8u * LLMT_OUT_MAX) {
					/* a chatty command: keep the head and the latest tail */
					u32 keep = 2u * LLMT_OUT_MAX;
					memmove(raw.b + keep, raw.b + raw.n - keep, keep);
					raw.n = 2 * keep;
				}
				continue;
			}
			if (n == 0 || (errno != EINTR && errno != EAGAIN))
				break; /* every writer is gone */
		}
		if (lt->cancel && *lt->cancel) {
			cancelled = true;
			break;
		}
		if (vx_now_ns() > end) {
			timed_out = true;
			break;
		}
	}
	if (timed_out || cancelled)
		kill(-pid, SIGKILL);
	if (pi[1] >= 0)
		close(pi[1]);
	close(po[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	clean_output(raw.b, raw.n, o);
	jb_free(&raw);
	if (o->n && o->b[o->n - 1] != '\n')
		jb_raw(o, "\n");
	if (cancelled)
		outf(o, "[stopped by the user]\n");
	else if (timed_out)
		outf(o, "[killed after %lld ms]\n", (long long)tmo);
	else if (WIFSIGNALED(status))
		outf(o, "[signal %d]\n", WTERMSIG(status));
	else
		outf(o, "[exit %d]\n", WEXITSTATUS(status));
	return !timed_out && !cancelled && WIFEXITED(status);
}

/* ---- dispatch ------------------------------------------------------------------------------------------- */

bool llmt_call(const LlmTools *t, const char *name, const char *args, size_t n, JBuf *out)
{
	jb_reset(out);
	Ctx c;
	if (!t->root || !realpath(t->root, c.root))
		return fail(out, "no project folder");
	c.rootlen = strlen(c.root);
	size_t ti = VX_COUNT(tools);
	for (size_t i = 0; i < VX_COUNT(tools); i++)
		if (!strcmp(tools[i].name, name))
			ti = i;
	bool ok;
	Args a;
	args_parse(&a, args, n);
	if (ti == VX_COUNT(tools) || !offered(t, tools[ti].flags))
		ok = fail(out, "unknown tool: %s", name);
	else if (!a.ok)
		ok = fail(out, "%s: the arguments are not a JSON object", name);
	else {
		switch (ti) {
		case 0: ok = t_read(&c, &a, out); break;
		case 1: ok = t_write(&c, &a, out); break;
		case 2: ok = t_edit(&c, &a, out); break;
		case 3: ok = t_ls(&c, &a, out); break;
		case 4: ok = t_stat(&c, &a, out); break;
		case 5: ok = t_mv(&c, &a, out); break;
		case 6: ok = t_mkdir(&c, &a, out); break;
		case 7: ok = t_search(&c, &a, out); break;
		default: ok = t_exec(&c, t, &a, out); break;
		}
		if (ok && !out->n)
			jb_raw(out, "ok");
	}
	args_free(&a);
	jb_rawn(out, "", 0); /* NUL-terminated even when empty */
	return ok;
}

void llmt_title(const char *name, const char *args, size_t n, char *out, size_t cap)
{
	Args a;
	args_parse(&a, args, n);
	const char *key = !strcmp(name, "exec") ? "cmd" : !strcmp(name, "mv") ? "from" : "path";
	const char *v = a.ok ? arg_str(&a, key, NULL) : NULL;
	char extra[200] = "";
	if (a.ok && !strcmp(name, "search")) {
		const char *re = arg_str(&a, "re", NULL), *gl = arg_str(&a, "glob", NULL);
		snprintf(extra, sizeof extra, "%s%s%s%s", re ? " /" : "", re ? re : "", re ? "/" : "", gl ? gl : "");
		if (gl && re)
			snprintf(extra, sizeof extra, " /%s/ in %s", re, gl);
	} else if (a.ok && !strcmp(name, "mv")) {
		const char *to = arg_str(&a, "to", NULL);
		snprintf(extra, sizeof extra, " -> %s", to ? to : "?");
	}
	if (v) {
		/* one line of it */
		size_t l = strcspn(v, "\n");
		snprintf(out, cap, "%s %.*s%s%s", name, (int)VX_MIN(l, (size_t)160), v, l < strlen(v) ? " ..." : "", extra);
	} else
		snprintf(out, cap, "%s%s", name, extra);
	args_free(&a);
}
