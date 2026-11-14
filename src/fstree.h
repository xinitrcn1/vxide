/* Project file tree for the explorer. Directories load lazily on expand.
 * Nodes are SoA; children are kept sorted folders-first, then by name
 * (case-insensitive), like VSCodium's default explorer sort order. */
#ifndef VX_FSTREE_H
#define VX_FSTREE_H

#include "base.h"

#define FS_NONE UINT32_MAX

enum { FS_DIR = 1, FS_OPEN = 2, FS_LOADED = 4, FS_LINK = 8 };

typedef struct {
	u32 n, cap;
	u32 *name;   /* pool offset of the basename */
	u32 *parent;
	u32 *first;  /* first child */
	u32 *next;   /* next sibling */
	u16 *depth;
	u8 *flags;
	Pool pool;
	char *root; /* absolute, no trailing '/' */
	u32 root_name;
} FsTree;

/* Default files.exclude: .git .svn .hg .jj CVS .DS_Store Thumbs.db */
bool fs_excluded(const char *name);

void fs_init(FsTree *t, const char *root);
void fs_free(FsTree *t);
void fs_load(FsTree *t, u32 node);   /* (re)read one directory */
void fs_toggle(FsTree *t, u32 node); /* expand/collapse (loads on demand) */
void fs_collapse_all(FsTree *t);
/* Rebuild from disk, keeping which folders are open. */
void fs_refresh(FsTree *t);
/* Visible rows (DFS over open folders, root excluded). Returns count. */
u32 fs_rows(FsTree *t, u32 **rows, u32 *cap);
/* Path relative to the root ("src/main.c"); returns length. */
u32 fs_relpath(const FsTree *t, u32 node, char *buf, u32 cap);
/* Expand ancestors of relpath and return its node (FS_NONE if absent). */
u32 fs_reveal(FsTree *t, const char *relpath);
static inline const char *fs_name(const FsTree *t, u32 i) { return t->pool.b + t->name[i]; }

#endif
