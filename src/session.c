/* Session state, so a restart picks up where the user left (VSCodium's
 * "restore editors" behaviour):
 *
 *   $XDG_STATE_HOME/vxide/sessions/<root, / -> %>   per project: open tabs
 *       (file, cursor, scroll; which one is active), the top-level view,
 *       side bar and panel visibility
 *   $XDG_STATE_HOME/vxide/commands                   recently used palette
 *       commands, newest first
 *
 * Plain text, rewritten with a temp file + rename. Headless runs neither
 * read nor write it unless VXIDE_STATE_DIR is set (tests stay hermetic). */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"

#include <stdio.h>
#include <sys/stat.h>

static bool state_dir(char *out, size_t cap)
{
	const char *t = getenv("VXIDE_STATE_DIR");
	if (t && *t) {
		snprintf(out, cap, "%s", t);
		return true;
	}
	if (app.headless)
		return false;
	const char *x = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (x && *x)
		snprintf(out, cap, "%s/vxide", x);
	else if (home)
		snprintf(out, cap, "%s/.local/state/vxide", home);
	else
		return false;
	return true;
}

static void mkdirs(const char *path)
{
	char d[4096];
	snprintf(d, sizeof d, "%s", path);
	for (char *s = d + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(d, 0755);
			*s = '/';
		}
	mkdir(d, 0755);
}

static bool session_file(char *out, size_t cap)
{
	char dir[4096];
	if (!state_dir(dir, sizeof dir))
		return false;
	char name[2048];
	size_t k = 0;
	for (const char *p = app.root; *p && k + 1 < sizeof name; p++)
		name[k++] = *p == '/' ? '%' : *p;
	name[k] = 0;
	snprintf(out, cap, "%s/sessions/%s", dir, name);
	return true;
}

static FILE *open_tmp(const char *path, char *tmp, size_t cap)
{
	char dir[4096];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		mkdirs(dir);
	}
	snprintf(tmp, cap, "%s.tmp", path);
	return fopen(tmp, "w");
}

/* ---- editors ------------------------------------------------------------------- */

void session_save(void)
{
	char path[4096], tmp[4200];
	if (!session_file(path, sizeof path))
		return;
	FILE *f = open_tmp(path, tmp, sizeof tmp);
	if (!f)
		return;
	fprintf(f, "# vxide session\nview %d\nside %d\npanel %d %d\n", ui_workspace() ? SIDE_EXPLORER : app.side_view,
	        app.side_visible, app.panel_visible, app.panel_view);
	for (u32 gi = 0; gi < app.ngroups; gi++) {
		Group *g = &app.groups[gi];
		for (u32 t = 0; t < g->nt; t++) {
			View *v = gview(g, t);
			const char *p = app.docs.path[v->doc];
			/* real files only: diffs, man pages and untitled buffers are not kept */
			if (!p || v->diff_left != UINT32_MAX || app.docs.man[v->doc])
				continue;
			Text *tx = &app.docs.text[v->doc];
			u32 head = VX_MIN(v->head[0], tx->len);
			u32 l = text_line_of(tx, head);
			u32 c = head - text_line_start(tx, l);
			fprintf(f, "tab %u %d %u %u %lld %s\n", gi, t == g->active, l, c, (long long)v->scroll_y, p);
		}
	}
	if (fclose(f) == 0)
		rename(tmp, path);
}

void session_restore(void)
{
	char path[4096];
	if (!session_file(path, sizeof path))
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[8192];
	char active[4096] = "";
	u32 aline = 0, acol = 0;
	i64 ascroll = 0;
	int view = -1;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		unsigned g, l, c;
		int act, n = 0;
		long long sc;
		if (sscanf(line, "tab %u %d %u %u %lld %n", &g, &act, &l, &c, &sc, &n) == 5 && n > 0) {
			const char *p = line + n;
			struct stat st;
			if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
				continue; /* gone since */
			/* one group: splits are not restored */
			open_file(p, false, l, c);
			View *v = active_view();
			if (v)
				v->scroll_y = sc;
			if (act && g == 0) {
				snprintf(active, sizeof active, "%s", p);
				aline = l;
				acol = c;
				ascroll = sc;
			}
		} else if (sscanf(line, "view %d", &view) == 1) {
		} else if (sscanf(line, "side %d", &act) == 1) {
			app.side_visible = act != 0;
		} else if (sscanf(line, "panel %d %d", &act, &n) == 2) {
			app.panel_visible = act != 0;
			app.panel_view = n == PANEL_OUTPUT ? PANEL_OUTPUT : PANEL_PROBLEMS;
		}
	}
	fclose(f);
	if (active[0]) {
		open_file(active, false, aline, acol);
		View *v = active_view();
		if (v)
			v->scroll_y = ascroll;
	}
	if (view == SIDE_EXPLORER || view == SIDE_SEARCH || view == SIDE_SCM)
		app.side_view = view;
	app.focus = active[0] ? FOCUS_EDITOR : FOCUS_SIDEBAR;
	ui_layout();
}

/* ---- recently used commands ---------------------------------------------------- */

#define MRU_MAX 16
static int mru[MRU_MAX];
static int mru_n;
static bool mru_loaded;

static void mru_load(void)
{
	if (mru_loaded)
		return;
	mru_loaded = true;
	char dir[4096], p[4200];
	if (!state_dir(dir, sizeof dir))
		return;
	snprintf(p, sizeof p, "%s/commands", dir);
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	int c;
	while (mru_n < MRU_MAX && fscanf(f, "%d", &c) == 1)
		if (c > 0 && c < CMD_COUNT)
			mru[mru_n++] = c;
	fclose(f);
}

void command_used(int cmd)
{
	mru_load();
	int k = 0;
	for (int i = 0; i < mru_n; i++)
		if (mru[i] != cmd)
			mru[k++] = mru[i];
	mru_n = VX_MIN(k, MRU_MAX - 1);
	memmove(mru + 1, mru, (size_t)mru_n * sizeof(int));
	mru[0] = cmd;
	mru_n++;
	char dir[4096], p[4200], tmp[4300];
	if (!state_dir(dir, sizeof dir))
		return;
	snprintf(p, sizeof p, "%s/commands", dir);
	FILE *f = open_tmp(p, tmp, sizeof tmp);
	if (!f)
		return;
	for (int i = 0; i < mru_n; i++)
		fprintf(f, "%d\n", mru[i]);
	if (fclose(f) == 0)
		rename(tmp, p);
}

/* Rank of a command among the recently used (0 = newest), -1 if not. */
int command_recent_rank(int cmd)
{
	mru_load();
	for (int i = 0; i < mru_n; i++)
		if (mru[i] == cmd)
			return i;
	return -1;
}
