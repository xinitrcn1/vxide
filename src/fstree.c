#define _POSIX_C_SOURCE 200809L
#include "fstree.h"

#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>

static const char *excluded[] = {".git", ".svn", ".hg", ".jj", "CVS", ".DS_Store", "Thumbs.db"};

bool fs_excluded(const char *name)
{
	for (size_t i = 0; i < VX_COUNT(excluded); i++)
		if (strcmp(name, excluded[i]) == 0)
			return true;
	return false;
}

static u32 add_node(FsTree *t, const char *name, u32 parent, u8 flags)
{
	if (t->n == t->cap) {
		u32 c = t->cap ? t->cap * 2 : 256;
		t->name = vx_realloc(t->name, c * sizeof(u32));
		t->parent = vx_realloc(t->parent, c * sizeof(u32));
		t->first = vx_realloc(t->first, c * sizeof(u32));
		t->next = vx_realloc(t->next, c * sizeof(u32));
		t->depth = vx_realloc(t->depth, c * sizeof(u16));
		t->flags = vx_realloc(t->flags, c);
		t->cap = c;
	}
	u32 i = t->n++;
	t->name[i] = pool_add(&t->pool, name, (u32)strlen(name));
	t->parent[i] = parent;
	t->first[i] = FS_NONE;
	t->next[i] = FS_NONE;
	t->depth[i] = parent == FS_NONE ? 0 : (u16)(t->depth[parent] + 1);
	t->flags[i] = flags;
	return i;
}

void fs_init(FsTree *t, const char *root)
{
	memset(t, 0, sizeof *t);
	size_t n = strlen(root);
	while (n > 1 && root[n - 1] == '/')
		n--;
	t->root = vx_strndup(root, n);
	const char *base = strrchr(t->root, '/');
	add_node(t, base && base[1] ? base + 1 : t->root, FS_NONE, FS_DIR | FS_OPEN);
	fs_load(t, 0);
}

void fs_free(FsTree *t)
{
	free(t->name);
	free(t->parent);
	free(t->first);
	free(t->next);
	free(t->depth);
	free(t->flags);
	pool_free(&t->pool);
	free(t->root);
	memset(t, 0, sizeof *t);
}

u32 fs_relpath(const FsTree *t, u32 node, char *buf, u32 cap)
{
	/* Walk up collecting names, then write them in order. */
	u32 chain[256];
	u32 k = 0;
	for (u32 i = node; i != 0 && i != FS_NONE && k < 256; i = t->parent[i])
		chain[k++] = i;
	u32 o = 0;
	buf[0] = 0;
	while (k--) {
		const char *nm = fs_name(t, chain[k]);
		u32 l = (u32)strlen(nm);
		if (o + l + 2 >= cap)
			break;
		if (o)
			buf[o++] = '/';
		memcpy(buf + o, nm, l);
		o += l;
		buf[o] = 0;
	}
	return o;
}

typedef struct {
	char *name;
	u8 flags;
} Ent;

static int ent_cmp(const void *a, const void *b)
{
	const Ent *x = a, *y = b;
	int dx = (x->flags & FS_DIR) != 0, dy = (y->flags & FS_DIR) != 0;
	if (dx != dy)
		return dy - dx;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->name, y->name);
}

void fs_load(FsTree *t, u32 node)
{
	char rel[4096], path[8192];
	fs_relpath(t, node, rel, sizeof rel);
	snprintf(path, sizeof path, "%s%s%s", t->root, rel[0] ? "/" : "", rel);
	DIR *d = opendir(path);
	t->flags[node] |= FS_LOADED;
	t->first[node] = FS_NONE;
	if (!d)
		return;
	Ent *e = NULL;
	u32 ne = 0, ce = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		const char *nm = de->d_name;
		if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0 || fs_excluded(nm))
			continue;
		char full[8192];
		snprintf(full, sizeof full, "%s/%s", path, nm);
		struct stat st, lst;
		u8 fl = 0;
		if (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode))
			fl |= FS_LINK;
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
			fl |= FS_DIR;
		VX_GROW(e, ce, ne + 1);
		e[ne].name = vx_strdup(nm);
		e[ne].flags = fl;
		ne++;
	}
	closedir(d);
	qsort(e, ne, sizeof *e, ent_cmp);
	u32 prev = FS_NONE;
	for (u32 i = 0; i < ne; i++) {
		u32 c = add_node(t, e[i].name, node, e[i].flags);
		if (prev == FS_NONE)
			t->first[node] = c;
		else
			t->next[prev] = c;
		prev = c;
		free(e[i].name);
	}
	free(e);
}

void fs_toggle(FsTree *t, u32 node)
{
	if (!(t->flags[node] & FS_DIR))
		return;
	t->flags[node] ^= FS_OPEN;
	if ((t->flags[node] & FS_OPEN) && !(t->flags[node] & FS_LOADED))
		fs_load(t, node);
}

void fs_collapse_all(FsTree *t)
{
	for (u32 i = 1; i < t->n; i++)
		t->flags[i] &= (u8)~FS_OPEN;
}

u32 fs_rows(FsTree *t, u32 **rows, u32 *cap)
{
	u32 n = 0;
	u32 stack[512];
	u32 sp = 0;
	if (t->first[0] != FS_NONE)
		stack[sp++] = t->first[0];
	while (sp) {
		u32 i = stack[--sp];
		VX_GROW(*rows, *cap, n + 1);
		(*rows)[n++] = i;
		if (t->next[i] != FS_NONE)
			stack[sp++] = t->next[i];
		if ((t->flags[i] & FS_OPEN) && t->first[i] != FS_NONE && sp < 511)
			stack[sp++] = t->first[i];
	}
	return n;
}

static u32 child_named(const FsTree *t, u32 dir, const char *name, size_t len)
{
	for (u32 c = t->first[dir]; c != FS_NONE; c = t->next[c]) {
		const char *nm = fs_name(t, c);
		if (strlen(nm) == len && memcmp(nm, name, len) == 0)
			return c;
	}
	return FS_NONE;
}

static u32 lookup(FsTree *t, const char *rel, bool open)
{
	u32 cur = 0;
	const char *p = rel;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (!(t->flags[cur] & FS_LOADED))
			fs_load(t, cur);
		u32 c = child_named(t, cur, p, len);
		if (c == FS_NONE)
			return FS_NONE;
		if (slash && (t->flags[c] & FS_DIR)) {
			if (open)
				t->flags[c] |= FS_OPEN;
			if (!(t->flags[c] & FS_LOADED))
				fs_load(t, c);
		}
		cur = c;
		if (!slash)
			break;
		p = slash + 1;
	}
	return cur;
}

u32 fs_reveal(FsTree *t, const char *rel) { return lookup(t, rel, true); }

void fs_refresh(FsTree *t)
{
	/* Remember open folders by path, rebuild, reopen. */
	Pool open = {0};
	u32 *offs = NULL, nopen = 0, copen = 0;
	char rel[4096];
	for (u32 i = 1; i < t->n; i++)
		if ((t->flags[i] & (FS_DIR | FS_OPEN)) == (FS_DIR | FS_OPEN)) {
			u32 l = fs_relpath(t, i, rel, sizeof rel);
			VX_GROW(offs, copen, nopen + 1);
			offs[nopen++] = pool_add(&open, rel, l);
		}
	char *root = vx_strdup(t->root);
	fs_free(t);
	fs_init(t, root);
	free(root);
	for (u32 k = 0; k < nopen; k++) {
		u32 n = lookup(t, open.b + offs[k], false);
		if (n != FS_NONE && (t->flags[n] & FS_DIR)) {
			t->flags[n] |= FS_OPEN;
			if (!(t->flags[n] & FS_LOADED))
				fs_load(t, n);
		}
	}
	free(offs);
	pool_free(&open);
}
