/* Qt and Godot integration, the parts of VSCodium's Qt extensions and
 * godot-tools that matter most, kept small:
 *
 *  Qt     - QML language features from qmlls (started like clangd, see
 *           ui.c), .ui files open in Qt Widgets Designer, .ts files in Qt
 *           Linguist, "Preview QML File" runs the qml tool.
 *  Godot  - GDScript language features from the Godot editor's own LSP over
 *           TCP (127.0.0.1:6005 for Godot 4, 6008 for Godot 3; reconnects
 *           while GDScript files are open), open the project in the editor,
 *           run the project (F5) or the current scene (F6) with its output in
 *           the Output panel, stop it (Shift+F5), and an opt-in headless
 *           language server for when the editor is not running (it imports
 *           the project, which is disk-heavy, so it is never started on its
 *           own).
 *
 * Tools are looked up in PATH; VXIDE_GODOT / VXIDE_QMLLS override. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "menu.h"
#include "fstree.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- tools in PATH ------------------------------------------------------------ */

enum { T_QMLLS, T_DESIGNER, T_LINGUIST, T_QML, T_GODOT, T_COUNT };

static const char *const tool_names[T_COUNT][5] = {
    [T_QMLLS] = {"qmlls6", "qmlls", NULL},
    [T_DESIGNER] = {"designer6", "designer", NULL},
    [T_LINGUIST] = {"linguist6", "linguist", NULL},
    [T_QML] = {"qml6", "qml", NULL},
    [T_GODOT] = {"godot", "godot4", "godot-mono", "godot3", NULL},
};
static const char *const tool_env[T_COUNT] = {[T_QMLLS] = "VXIDE_QMLLS", [T_GODOT] = "VXIDE_GODOT"};

static char tool_cache[T_COUNT][1024];
static bool tool_done[T_COUNT];

static bool in_path(const char *name, char *out, size_t cap)
{
	if (strchr(name, '/')) {
		snprintf(out, cap, "%s", name);
		return access(out, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	/* Qt keeps the unsuffixed tools here on FreeBSD */
	char buf[8192];
	snprintf(buf, sizeof buf, "%s:/usr/local/lib/qt6/bin", path);
	for (char *save = NULL, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		snprintf(out, cap, "%s/%s", *d ? d : ".", name);
		if (access(out, X_OK) == 0)
			return true;
	}
	return false;
}

static const char *tool(int t)
{
	if (!tool_done[t]) {
		tool_done[t] = true;
		tool_cache[t][0] = 0;
		const char *env = tool_env[t] ? getenv(tool_env[t]) : NULL;
		if (env && *env)
			snprintf(tool_cache[t], sizeof tool_cache[t], "%s", env);
		else
			for (int i = 0; tool_names[t][i]; i++)
				if (in_path(tool_names[t][i], tool_cache[t], sizeof tool_cache[t]))
					break;
				else
					tool_cache[t][0] = 0;
	}
	return tool_cache[t][0] ? tool_cache[t] : NULL;
}

/* ---- project detection ------------------------------------------------------------- */

static char godot_root[4096]; /* folder holding project.godot, "" when none */
static int godot_ver;         /* 3 or 4 */
static char qt_build[4096];   /* a CMake build folder for qmlls -b, "" when none */

static bool is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int read_godot_version(const char *proj)
{
	FILE *f = fopen(proj, "r");
	if (!f)
		return 4;
	char line[256];
	int v = 4;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "config_version=", 15)) {
			v = atoi(line + 15) >= 5 ? 4 : 3;
			break;
		}
	fclose(f);
	return v;
}

/* project.godot in the root or in a folder right below it (a game kept next
 * to its tools), the way godot-tools looks for it. */
void projtools_init(void)
{
	godot_root[0] = qt_build[0] = 0;
	for (int t = 0; t < T_COUNT; t++)
		tool_done[t] = false;
	char p[8192];
	snprintf(p, sizeof p, "%s/project.godot", app.root);
	if (is_file(p)) {
		snprintf(godot_root, sizeof godot_root, "%s", app.root);
	} else {
		DIR *d = opendir(app.root);
		struct dirent *de;
		while (d && (de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(p, sizeof p, "%s/%s/project.godot", app.root, de->d_name);
			if (is_file(p)) {
				snprintf(godot_root, sizeof godot_root, "%s/%s", app.root, de->d_name);
				break;
			}
		}
		if (d)
			closedir(d);
	}
	if (godot_root[0]) {
		snprintf(p, sizeof p, "%s/project.godot", godot_root);
		godot_ver = read_godot_version(p);
		ui_log("[godot] project in %s (Godot %d)", godot_root, godot_ver);
	}
	/* qmlls finds QML modules through the build folder (Qt's extension passes
	 * the CMake build directory the same way) */
	static const char *const builds[] = {"build", "out/build", "cmake-build-debug", "cmake-build-release", "_build"};
	for (size_t i = 0; i < VX_COUNT(builds); i++) {
		snprintf(p, sizeof p, "%s/%s/CMakeCache.txt", app.root, builds[i]);
		if (is_file(p)) {
			snprintf(qt_build, sizeof qt_build, "%s/%s", app.root, builds[i]);
			break;
		}
	}
}

const char *godot_dir(void) { return godot_root[0] ? godot_root : NULL; }

int godot_lsp_port(void)
{
	const char *e = getenv("VXIDE_GODOT_PORT");
	if (e && atoi(e) > 0)
		return atoi(e);
	return godot_ver == 3 ? 6008 : 6005;
}

const char *qmlls_path(void) { return tool(T_QMLLS); }
const char *qt_build_dir(void) { return qt_build[0] ? qt_build : NULL; }

/* ---- processes --------------------------------------------------------------- */

/* A GUI tool that outlives us (Designer, the Godot editor): its own session,
 * reparented to init so nothing is left to reap. */
static bool spawn_detached(char *const argv[], const char *cwd)
{
	pid_t pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0) {
		setsid();
		if (fork() != 0)
			_exit(0);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		if (cwd)
			(void)!chdir(cwd);
		execv(argv[0], argv);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
	return true;
}

/* The running game / QML preview: one at a time, output to the Output panel. */
static pid_t run_pid = -1;
static int run_fd = -1;
static char run_line[4096];
static size_t run_n;
static char run_tag[16];

/* Headless Godot language server, owned by us (killed on exit). */
static pid_t lsp_pid = -1;

static pid_t spawn_piped(char *const argv[], const char *cwd, int *out_fd)
{
	int p[2];
	if (pipe(p) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		setpgid(0, 0); /* so a stop reaches the whole tree */
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		if (cwd)
			(void)!chdir(cwd);
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	*out_fd = p[0];
	return pid;
}

static void run_flush_line(void)
{
	run_line[run_n] = 0;
	ui_log("[%s] %s", run_tag, run_line);
	run_n = 0;
}

static void stop_run(void)
{
	if (run_pid > 0) {
		kill(-run_pid, SIGTERM);
		kill(run_pid, SIGTERM);
	}
}

static void start_run(const char *tag, char *const argv[], const char *cwd)
{
	if (run_pid > 0) {
		stop_run();
		waitpid(run_pid, NULL, 0);
		if (run_fd >= 0)
			close(run_fd);
		run_pid = -1;
		run_fd = -1;
	}
	snprintf(run_tag, sizeof run_tag, "%s", tag);
	run_n = 0;
	run_pid = spawn_piped(argv, cwd, &run_fd);
	if (run_pid < 0) {
		ui_status("Could not start %s", argv[0]);
		return;
	}
	ui_log("[%s] started: %s%s%s", tag, argv[0], argv[1] ? " " : "", argv[1] ? argv[1] : "");
	if (app.panel_view != PANEL_OUTPUT || !app.panel_visible)
		run_command(CMD_VIEW_OUTPUT);
}

bool projtools_busy(void) { return run_pid > 0; }

void projtools_tick(void)
{
	if (run_fd >= 0) {
		char buf[4096];
		for (;;) {
			ssize_t k = read(run_fd, buf, sizeof buf);
			if (k > 0) {
				for (ssize_t i = 0; i < k; i++) {
					if (buf[i] == '\n' || run_n == sizeof run_line - 1)
						run_flush_line();
					else if (buf[i] != '\r')
						run_line[run_n++] = buf[i];
				}
				app.dirty = true;
				continue;
			}
			if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
				if (run_n)
					run_flush_line();
				close(run_fd);
				run_fd = -1;
			}
			break;
		}
	}
	if (run_pid > 0 && run_fd < 0) {
		int st;
		pid_t r = waitpid(run_pid, &st, WNOHANG);
		if (r == run_pid || (r < 0 && errno == ECHILD)) {
			if (r == run_pid && WIFEXITED(st))
				ui_log("[%s] exited with code %d", run_tag, WEXITSTATUS(st));
			else
				ui_log("[%s] stopped", run_tag);
			run_pid = -1;
			app.dirty = true;
		}
	}
	if (lsp_pid > 0 && waitpid(lsp_pid, NULL, WNOHANG) == lsp_pid) {
		ui_log("[godot] headless language server exited");
		lsp_pid = -1;
	}
}

void projtools_shutdown(void)
{
	stop_run();
	if (run_pid > 0)
		waitpid(run_pid, NULL, 0);
	run_pid = -1;
	if (lsp_pid > 0) {
		kill(-lsp_pid, SIGTERM);
		kill(lsp_pid, SIGTERM);
		waitpid(lsp_pid, NULL, 0);
		lsp_pid = -1;
	}
}

/* ---- the file a command is about ------------------------------------------------- */

/* The explorer node of a context menu, else the active editor's file. */
static bool target_path(char *out, size_t cap)
{
	out[0] = 0;
	if (app.ctx == CTX_EXPLORER && app.ctx_a != FS_NONE) {
		char rel[4096];
		fs_relpath(&app.tree, app.ctx_a, rel, sizeof rel);
		snprintf(out, cap, "%s/%s", app.root, rel);
		return true;
	}
	View *v = active_view();
	if (v && app.docs.path[v->doc]) {
		snprintf(out, cap, "%s", app.docs.path[v->doc]);
		return true;
	}
	return false;
}

static bool has_ext(const char *p, const char *ext)
{
	size_t n = strlen(p), e = strlen(ext);
	return n > e && !strcmp(p + n - e, ext);
}

/* res:// path of a file inside the Godot project */
static bool res_path(const char *abs, char *out, size_t cap)
{
	size_t n = strlen(godot_root);
	if (!godot_root[0] || strncmp(abs, godot_root, n) != 0 || abs[n] != '/')
		return false;
	snprintf(out, cap, "res://%s", abs + n + 1);
	return true;
}

static bool need_tool(int t, const char *what)
{
	if (tool(t))
		return true;
	ui_status("%s not found in PATH", what);
	return false;
}

static bool need_godot_project(void)
{
	if (godot_root[0])
		return true;
	ui_status("No Godot project (project.godot) in this folder");
	return false;
}

/* ---- commands ------------------------------------------------------------------ */

bool projtools_command(int cmd)
{
	char path[8192];
	switch (cmd) {
	case CMD_QT_DESIGNER:
	case CMD_QT_LINGUIST: {
		bool des = cmd == CMD_QT_DESIGNER;
		if (!need_tool(des ? T_DESIGNER : T_LINGUIST, des ? "Qt Widgets Designer" : "Qt Linguist"))
			return true;
		char *argv[3] = {(char *)tool(des ? T_DESIGNER : T_LINGUIST), NULL, NULL};
		if (target_path(path, sizeof path) && has_ext(path, des ? ".ui" : ".ts"))
			argv[1] = path;
		spawn_detached(argv, app.root);
		ui_status("Opening %s", argv[1] ? strrchr(argv[1], '/') + 1 : (des ? "Qt Widgets Designer" : "Qt Linguist"));
		return true;
	}
	case CMD_QT_PREVIEW_QML: {
		if (!target_path(path, sizeof path) || !has_ext(path, ".qml")) {
			ui_status("Open a .qml file to preview it");
			return true;
		}
		if (!need_tool(T_QML, "The qml tool"))
			return true;
		/* run what is on disk: save first, like the Qt extension does */
		View *v = active_view();
		if (v && app.docs.path[v->doc] && !strcmp(app.docs.path[v->doc], path) &&
		    text_dirty(&app.docs.text[v->doc]))
			doc_save(v->doc);
		char *argv[] = {(char *)tool(T_QML), path, NULL};
		start_run("qml", argv, app.root);
		return true;
	}
	case CMD_GODOT_EDITOR: {
		if (!need_godot_project() || !need_tool(T_GODOT, "Godot"))
			return true;
		char *argv[] = {(char *)tool(T_GODOT), "--editor", "--path", godot_root, NULL};
		spawn_detached(argv, godot_root);
		ui_status("Opening the project in Godot (GDScript features connect when it is up)");
		return true;
	}
	case CMD_GODOT_RUN:
	case CMD_GODOT_RUN_SCENE: {
		if (!need_godot_project() || !need_tool(T_GODOT, "Godot"))
			return true;
		run_command(CMD_SAVE_ALL);
		char res[8192] = "";
		if (cmd == CMD_GODOT_RUN_SCENE) {
			/* a .tscn, or the scene next to a script with the same name */
			if (!target_path(path, sizeof path)) {
				ui_status("Open a scene (.tscn) to run it");
				return true;
			}
			if (has_ext(path, ".gd")) {
				path[strlen(path) - 3] = 0;
				strcat(path, ".tscn");
			}
			if (!has_ext(path, ".tscn") || !is_file(path) || !res_path(path, res, sizeof res)) {
				ui_status("No scene for this file");
				return true;
			}
		}
		char *argv[6] = {(char *)tool(T_GODOT), "--path", godot_root, res[0] ? res : NULL, NULL, NULL};
		start_run("godot", argv, godot_root);
		return true;
	}
	case CMD_GODOT_STOP:
		if (run_pid > 0) {
			stop_run();
			ui_status("Stopping");
		}
		return true;
	case CMD_GODOT_HEADLESS_LSP: {
		if (!need_godot_project() || !need_tool(T_GODOT, "Godot"))
			return true;
		if (lsp_pid > 0) {
			ui_status("The headless language server is running");
			return true;
		}
		char port[16];
		snprintf(port, sizeof port, "%d", godot_lsp_port());
		char *argv[] = {(char *)tool(T_GODOT), "--headless", "--editor", "--lsp-port", port, "--path", godot_root, NULL};
		int fd = -1;
		lsp_pid = spawn_piped(argv, godot_root, &fd);
		if (fd >= 0)
			close(fd); /* its chatter is not interesting; writes fail quietly (SIGPIPE ignored) */
		if (lsp_pid < 0) {
			ui_status("Could not start Godot");
			return true;
		}
		ui_log("[godot] headless language server on port %s", port);
		ui_status("Starting Godot's language server (it imports the project first)");
		extern void lsp_retry_soon(int sv);
		lsp_retry_soon(LSV_GODOT);
		return true;
	}
	default:
		return false;
	}
}

/* Explorer context menu entries for Qt / Godot files. */
void projtools_explorer_menu(u32 node)
{
	if (node == FS_NONE)
		return;
	char rel[4096];
	fs_relpath(&app.tree, node, rel, sizeof rel);
	bool any = false;
#define ITEM(label, cmd)                                                                                          \
	do {                                                                                                          \
		if (!any)                                                                                                 \
			menu_sep();                                                                                           \
		any = true;                                                                                               \
		menu_item(label, cmd, true);                                                                              \
	} while (0)
	if (has_ext(rel, ".ui"))
		ITEM("Open in Qt Widgets Designer", CMD_QT_DESIGNER);
	if (has_ext(rel, ".ts"))
		ITEM("Open in Qt Linguist", CMD_QT_LINGUIST);
	if (has_ext(rel, ".qml"))
		ITEM("Preview QML File", CMD_QT_PREVIEW_QML);
	if (godot_root[0] && (has_ext(rel, ".tscn") || has_ext(rel, ".gd")))
		ITEM("Run Scene in Godot", CMD_GODOT_RUN_SCENE);
	if (godot_root[0] && has_ext(rel, "project.godot"))
		ITEM("Open in Godot Editor", CMD_GODOT_EDITOR);
#undef ITEM
}
