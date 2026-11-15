/* CMake: presets with inheritance and macros, option() fallback, the
 * cache after a real configure, File API targets, command lines. */
#define _POSIX_C_SOURCE 200809L
#include "cmake.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void put(const char *dir, const char *name, const char *text)
{
	char p[600];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "w");
	fputs(text, f);
	fclose(f);
}

static u32 ent(const CMakeProj *c, const char *n)
{
	for (u32 i = 0; i < c->nent; i++)
		if (!strcmp(cm_str(c, c->ent_name[i]), n))
			return i;
	return UINT32_MAX;
}

int main(void)
{
	char dir[] = "/tmp/vxide-cmake-XXXXXX";
	if (!mkdtemp(dir))
		return tap_done();
	char cmd[1200];
	snprintf(cmd, sizeof cmd, "mkdir -p %s/src", dir);
	(void)system(cmd);
	put(dir, "CMakeLists.txt",
	    "cmake_minimum_required(VERSION 3.20)\n"
	    "project(demo C)\n"
	    "option(DEMO_FAST \"Build the fast path\" ON)\n"
	    "option(DEMO_TRACE \"Trace every call\" OFF)\n"
	    "set(DEMO_NAME \"world\" CACHE STRING \"Who to greet\")\n"
	    "add_executable(demo src/main.c)\n"
	    "add_executable(tool src/main.c)\n"
	    "add_library(util STATIC src/main.c)\n");
	put(dir, "src/main.c", "int main(void) { return 0; }\n");
	put(dir, "CMakePresets.json",
	    "{\"version\": 6, \"include\": [\"more.json\"],\n"
	    " \"configurePresets\": [\n"
	    "  {\"name\": \"base\", \"hidden\": true, \"binaryDir\": \"${sourceDir}/out/${presetName}\"},\n"
	    "  {\"name\": \"debug\", \"displayName\": \"Debug build\", \"inherits\": \"base\",\n"
	    "   \"cacheVariables\": {\"CMAKE_BUILD_TYPE\": \"Debug\"}},\n"
	    "  {\"name\": \"rel\", \"inherits\": [\"base\"], \"binaryDir\": \"rel-build\"}],\n"
	    " \"buildPresets\": [{\"name\": \"debug\", \"configurePreset\": \"debug\"}]}\n");
	put(dir, "more.json", "{\"version\": 6, \"configurePresets\": [{\"name\": \"extra\", \"binaryDir\": \"$env{HOME}/x\"}]}\n");

	CMakeProj c = {0};
	cmake_load(&c, dir);
	CHECK(c.has_lists);
	CHECK(c.npre == 3); /* debug, rel, extra (base is hidden) */
	char bd[1200], want[1200];
	u32 dbg = UINT32_MAX, rel = UINT32_MAX, ext = UINT32_MAX;
	for (u32 i = 0; i < c.npre; i++) {
		const char *n = cm_str(&c, c.pre_name[i]);
		if (!strcmp(n, "debug"))
			dbg = i;
		if (!strcmp(n, "rel"))
			rel = i;
		if (!strcmp(n, "extra"))
			ext = i;
	}
	CHECK(dbg != UINT32_MAX && !strcmp(cm_str(&c, c.pre_display[dbg]), "Debug build"));
	cmake_build_dir(&c, dbg, bd, sizeof bd);
	snprintf(want, sizeof want, "%s/out/debug", dir);
	CHECK_STR(bd, want); /* inherited binaryDir, macros expanded */
	cmake_build_dir(&c, rel, bd, sizeof bd);
	snprintf(want, sizeof want, "%s/rel-build", dir);
	CHECK_STR(bd, want); /* its own, relative to the source folder */
	cmake_build_dir(&c, ext, bd, sizeof bd);
	snprintf(want, sizeof want, "%s/x", getenv("HOME") ? getenv("HOME") : "");
	CHECK_STR(bd, want); /* from the included file, $env{HOME} */
	CHECK(c.nbp == 1 && !strcmp(cm_str(&c, c.bp_conf[0]), "debug"));

	/* before configuring: the option() and CACHE lines */
	cmake_build_dir(&c, dbg, bd, sizeof bd);
	CHECK(!cmake_read_cache(&c, bd));
	u32 e = ent(&c, "DEMO_FAST");
	CHECK(e != UINT32_MAX && c.ent_type[e] == CV_BOOL && !strcmp(cm_str(&c, c.ent_value[e]), "ON"));
	e = ent(&c, "DEMO_NAME");
	CHECK(e != UINT32_MAX && c.ent_type[e] == CV_STRING && !strcmp(cm_str(&c, c.ent_help[e]), "Who to greet"));

	/* commands */
	char *argv[32], buf[4096];
	u32 n = cmake_configure_argv(&c, dbg, bd, NULL, 0, argv, 32, buf, sizeof buf);
	CHECK(n >= 2 && !strcmp(argv[1], "--preset=debug"));
	const char *defs[] = {"DEMO_TRACE:BOOL=ON"};
	n = cmake_configure_argv(&c, UINT32_MAX, "/b", defs, 1, argv, 32, buf, sizeof buf);
	CHECK(n == 7 && !strcmp(argv[4], "/b") && !strcmp(argv[5], "-DCMAKE_BUILD_TYPE=Debug") &&
	      !strcmp(argv[6], "-DDEMO_TRACE:BOOL=ON"));
	n = cmake_build_argv(&c, "/b", "demo", argv, 32, buf, sizeof buf);
	CHECK(n == 6 && !strcmp(argv[4], "demo"));

	/* a real configure, when cmake is here */
	if (system("cmake --version >/dev/null 2>&1") == 0) {
		cmake_query(bd);
		snprintf(cmd, sizeof cmd, "cd %s && cmake --preset=debug >/dev/null 2>&1", dir);
		CHECK(system(cmd) == 0);
		CHECK(cmake_read_cache(&c, bd));
		e = ent(&c, "DEMO_FAST");
		CHECK(e != UINT32_MAX && !strcmp(cm_str(&c, c.ent_help[e]), "Build the fast path"));
		e = ent(&c, "CMAKE_BUILD_TYPE");
		CHECK(e != UINT32_MAX && !strcmp(cm_str(&c, c.ent_value[e]), "Debug") && !c.ent_adv[e]);
		e = ent(&c, "CMAKE_AR");
		CHECK(e == UINT32_MAX || c.ent_adv[e]); /* advanced */
		CHECK(cmake_read_targets(&c, bd));
		u32 ex[8];
		u32 nx = cmake_executables(&c, ex, 8);
		CHECK(nx == 2);
		bool demo = false;
		for (u32 i = 0; i < nx; i++)
			if (!strcmp(cm_str(&c, c.tgt_name[ex[i]]), "demo")) {
				snprintf(want, sizeof want, "%s/demo", bd);
				demo = !strcmp(cm_str(&c, c.tgt_path[ex[i]]), want);
			}
		CHECK(demo);
		CHECK(c.ntgt == 3);
	} else
		fprintf(stderr, "# no cmake: configure skipped\n");
	cmake_free(&c);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)system(cmd);
	return tap_done();
}
