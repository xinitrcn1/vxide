/* CMake projects: presets (CMakePresets.json, CMakeUserPresets.json,
 * "inherits", "include", ${sourceDir}-style macros), the cache of a build
 * folder (CMakeCache.txt: types, help, advanced), option() lines as a
 * fallback before the first configure, and the targets from the CMake File
 * API (codemodel-v2: executables and their paths). Reads files only; the
 * commands to run are built as argv arrays. */
#ifndef VX_CMAKE_H
#define VX_CMAKE_H

#include "base.h"

enum { CT_EXECUTABLE, CT_STATIC, CT_SHARED, CT_MODULE, CT_OBJECT, CT_INTERFACE, CT_UTILITY, CT_OTHER };
enum { CV_BOOL, CV_STRING, CV_PATH, CV_FILEPATH, CV_OTHER };

typedef struct {
	bool has_lists; /* CMakeLists.txt at the root */
	char root[1024];
	char err[256];
	Pool pool; /* offset 0 = "" */
	/* configure presets (visible ones only) */
	u32 npre, pre_cap;
	u32 *pre_name, *pre_display, *pre_bindir, *pre_gen;
	/* build presets */
	u32 nbp, bp_cap;
	u32 *bp_name, *bp_conf;
	/* cache entries (or option() lines before a configure) */
	u32 nent, ent_cap;
	u32 *ent_name, *ent_value, *ent_help;
	u8 *ent_type, *ent_adv;
	bool from_cache;
	/* targets */
	u32 ntgt, tgt_cap;
	u32 *tgt_name, *tgt_path; /* path: absolute artifact, "" when none */
	u8 *tgt_type;
} CMakeProj;

static inline const char *cm_str(const CMakeProj *c, u32 off) { return c->pool.b ? c->pool.b + off : ""; }

/* Read the presets (and whether there is a CMakeLists.txt). */
void cmake_load(CMakeProj *c, const char *root);
void cmake_free(CMakeProj *c);

/* The build folder: the preset's binaryDir, else root/build. */
void cmake_build_dir(const CMakeProj *c, u32 preset, char *out, size_t cap);
/* The cache of a build folder; false when it was never configured (the
 * option() lines of CMakeLists.txt are listed instead). */
bool cmake_read_cache(CMakeProj *c, const char *builddir);
/* Ask the File API for the codemodel (before configuring). */
void cmake_query(const char *builddir);
/* Targets from the File API reply; false when there is none. */
bool cmake_read_targets(CMakeProj *c, const char *builddir);
u32 cmake_executables(const CMakeProj *c, u32 *out, u32 cap);

/* Commands. defs: "NAME:TYPE=VALUE" overrides (may be NULL). The argv
 * strings live in buf (cap bytes); returns the argument count. */
u32 cmake_configure_argv(const CMakeProj *c, u32 preset, const char *builddir, const char *const *defs, u32 ndefs,
                         char **argv, u32 max, char *buf, size_t cap);
u32 cmake_build_argv(const CMakeProj *c, const char *builddir, const char *target, char **argv, u32 max, char *buf,
                     size_t cap);

#endif
