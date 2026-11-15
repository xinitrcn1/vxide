/* Context menus (right click, the "..." buttons): a stack of up to two
 * levels (menu + one submenu), items are commands with their keybinding
 * shown on the right, separators, and submenu entries. */
#ifndef VX_MENU_H
#define VX_MENU_H

#include "base.h"

struct Event;

/* What the menu was opened on; commands read it (app.ctx). */
enum {
	CTX_NONE,
	CTX_EDITOR,     /* off = byte offset under the mouse */
	CTX_EXPLORER,   /* node */
	CTX_TAB,        /* group, tab */
	CTX_SCM_FILE,   /* path (repo-relative), staged flag in arg */
	CTX_SCM_TITLE,
	CTX_GRAPH,      /* row, hash in path */
};

enum { MI_SEP = 1, MI_DISABLED = 2, MI_SUB = 4 };

void menu_open(int x, int y);
void menu_item(const char *label, int cmd, bool enabled);
void menu_sep(void);
void menu_sub(const char *label, int sub); /* sub: id passed to the filler */
bool menu_is_open(void);
void menu_close(void);
bool menu_event(const struct Event *e); /* true when consumed */
void menu_draw(void);
/* Supplied by the UI: fill submenu `sub` (between menu_open_sub calls). */
void menu_fill_sub(int sub);

#endif
