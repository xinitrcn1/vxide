/* Workbench state: documents, editor groups, views, panes, input. */
#ifndef VX_UI_H
#define VX_UI_H

#include "base.h"
#include "text.h"
#include "fstree.h"
#include "search.h"
#include "git.h"
#include "blame.h"
#include "lsp.h"
#include "theme.h"
#include "draw.h"
#include "font.h"
#include "menu.h"

/* ---- input ------------------------------------------------------------ */

enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_SUPER = 8 };
enum { EV_KEY, EV_TEXT, EV_MDOWN, EV_MUP, EV_MMOVE, EV_WHEEL, EV_RESIZE, EV_FOCUS, EV_QUIT };

typedef struct Event {
	int type;
	u32 key; /* SDL keycode */
	int mods;
	char text[32];
	int x, y, button, clicks;
	float wx, wy;
} Event;

/* ---- documents (SoA, fixed capacity) ------------------------------------ */

#define MAX_DOCS 256

typedef struct {
	u32 n;
	Text text[MAX_DOCS];
	char *path[MAX_DOCS]; /* absolute; NULL for untitled */
	char *rel[MAX_DOCS];  /* relative to the root, or display name */
	u8 lang[MAX_DOCS];
	u8 untitled[MAX_DOCS];
	u8 tabs[MAX_DOCS]; /* indentation uses tabs (detected) */
	u8 readonly[MAX_DOCS]; /* virtual documents (diffs, commits) */
	u32 refs[MAX_DOCS]; /* views showing it */

	/* highlighter: lexer state at the start of each line */
	u32 *lstate[MAX_DOCS];
	u32 lstate_cap[MAX_DOCS];
	u32 lstate_ok[MAX_DOCS]; /* states [0, ok] are valid */

	/* LSP sync */
	bool lsp_open[MAX_DOCS];
	i32 lsp_ver[MAX_DOCS];
	u32 lsp_sent[MAX_DOCS]; /* text version last sent */
	u64 lsp_changed_ns[MAX_DOCS];
	i32 sem_req[MAX_DOCS];
	u32 sem_for[MAX_DOCS]; /* text version the request was made for */

	/* diagnostics (SoA per doc) */
	u32 dn[MAX_DOCS], dcap[MAX_DOCS];
	u32 *d_line0[MAX_DOCS], *d_col0[MAX_DOCS], *d_line1[MAX_DOCS], *d_col1[MAX_DOCS];
	u8 *d_sev[MAX_DOCS];
	u32 *d_msg[MAX_DOCS];
	Pool d_pool[MAX_DOCS];

	/* semantic tokens (SoA per doc, sorted by line, col) */
	u32 sn[MAX_DOCS], scap[MAX_DOCS];
	u32 *s_line[MAX_DOCS], *s_col[MAX_DOCS], *s_len[MAX_DOCS];
	u8 *s_cls[MAX_DOCS];

	/* git quick diff marks per line */
	u8 *gm[MAX_DOCS];
	u32 gm_n[MAX_DOCS], gm_cap[MAX_DOCS];
	u32 gm_for[MAX_DOCS]; /* text version */
	u64 gm_due_ns[MAX_DOCS];
} Docs;

/* ---- editor views and groups -------------------------------------------- */

#define MAX_CURSORS 512

typedef struct {
	u32 doc;
	u32 nc;
	u32 head[MAX_CURSORS], anchor[MAX_CURSORS];
	i32 want_x[MAX_CURSORS]; /* preferred x in pixels for up/down, -1 none */
	i64 scroll_y;            /* pixels */
	i32 scroll_x;
	bool preview; /* italic tab, replaced by the next preview open */
	u32 group;    /* undo group currently being extended */
	u8 group_kind;
	u64 group_ns;
} View;

#define MAX_GROUPS 3
#define MAX_TABS 64

typedef struct {
	u32 nt, active;
	View *tab[MAX_TABS];
	u32 mru[MAX_TABS]; /* tab indices, most recent first */
	int x, y, w, h;
} Group;

/* ---- workbench ---------------------------------------------------------- */

enum { SIDE_EXPLORER, SIDE_SEARCH, SIDE_SCM, SIDE_COUNT };
enum { PANEL_PROBLEMS, PANEL_OUTPUT, PANEL_COUNT };
enum {
	FOCUS_EDITOR,
	FOCUS_SIDEBAR,
	FOCUS_PANEL,
	FOCUS_QUICK,
	FOCUS_FIND,
	FOCUS_SEARCH_INPUT,
	FOCUS_SCM_INPUT,
};
enum { QUICK_NONE, QUICK_FILES, QUICK_COMMANDS, QUICK_GOTO_LINE, QUICK_PROMPT, QUICK_CONFIRM, QUICK_PICK };

/* Mouse cursor shapes (main.c maps them to SDL system cursors). */
enum { CURSOR_ARROW, CURSOR_IBEAM, CURSOR_HAND, CURSOR_EW, CURSOR_NS };

/* Single-line text input (find box, quick input, search, commit message
 * is multi-line but uses the same storage). */
typedef struct {
	char *b;
	u32 n, cap;
	u32 cur, anchor;
	int scroll;
} Input;

typedef struct {
	/* config, fixed */
	int scale;
	int theme;
	int w, h;
	char *root;
	bool headless;
	bool running;
	bool dirty; /* needs redraw */
	u64 now;
	u64 blink_t0;

	/* metrics */
	int act_w, side_w, status_h, tabs_h, crumbs_h, panel_h;
	int line_h, char_w, row_h;

	/* layout results */
	int side_x, ed_x, ed_y, ed_w, ed_h, panel_y;

	bool side_visible, panel_visible;
	int side_view, panel_view;
	int focus;
	int hover_x, hover_y;
	bool mouse_down;
	int drag; /* what is being dragged */
	int drag_off;

	Docs docs;
	Group groups[MAX_GROUPS];
	u32 ngroups, active_group;

	/* explorer */
	FsTree tree;
	u32 *rows;
	u32 nrows, rows_cap;
	u32 tree_sel; /* node */
	i32 tree_scroll;
	u64 tree_scan_ns;

	/* files (quick open + search) */
	FileList files;
	bool files_ok;

	/* search view */
	Input search_in, search_repl_in, search_inc_in, search_exc_in;
	int search_box; /* focused box: 0 search, 1 replace, 2 include, 3 exclude */
	bool search_show_replace, search_show_details;
	Matcher search_m;
	int search_flags;
	SearchResults results;
	i32 search_scroll;
	u32 search_sel;
	u8 *search_collapsed; /* per file in files list */

	/* source control */
	Git *git;
	Blame *blame;
	bool blame_on; /* git.blame.editorDecoration.enabled */
	u32 blame_gen;  /* bumps on every git refresh (HEAD may move) */
	u32 blame_doc, blame_key;
	bool blame_asked;
	bool blame_hover; /* mouse rests on the inline blame text */
	char blame_hover_hash[41];
	int blame_hover_x, blame_hover_y;
	u64 blame_hover_t;
	Input commit_in;
	i32 scm_scroll;
	u32 scm_sel;
	u64 git_ns;

	/* LSP */
	Lsp *lsp;
	bool lsp_ready;
	i32 compl_req, hover_req, def_req;
	u32 compl_doc;
	char compl_prefix[128];

	/* completion popup */
	bool compl_open;
	u32 compl_n, compl_sel, compl_top;
	u32 *compl_label, *compl_insert, *compl_detail;
	u8 *compl_kind;
	u32 *compl_filtered;
	u32 compl_nf, compl_cap;
	u32 compl_start; /* byte offset where the word starts */
	Pool compl_pool;

	/* hover popup */
	bool hover_open;
	char *hover_text;
	u32 hover_off; /* doc offset */
	u64 hover_due;
	u32 hover_doc;
	i32 hover_pending_off;

	/* find widget */
	bool find_open, find_replace;
	Input find_in, repl_in;
	int find_flags;
	u32 find_count, find_index;
	u32 find_version; /* text version the count is for */
	u32 find_doc;
	u32 find_hash;    /* query + flags the matches are for */
	Matcher find_m;
	u32 *fm_a, *fm_b; /* all matches in the document (sorted) */
	u32 fm_n, fm_cap;

	/* quick input */
	int quick;
	Input quick_in;
	u32 *quick_items;
	int *quick_score;
	u64 *quick_mask;
	u32 quick_n, quick_cap, quick_sel, quick_top;
	char quick_title[128];
	int quick_prompt_kind;
	u32 quick_arg;

	/* problems / output */
	i32 problems_scroll;
	u32 problems_sel;
	Pool output;
	u32 *out_lines;
	u32 out_n, out_cap;
	i32 output_scroll;

	/* status message */
	char status_msg[256];
	u64 status_until;

	/* navigation history (Alt+Left / Alt+Right) */
	u32 nav_doc[64], nav_off[64];
	u32 nav_n, nav_pos;

	/* chord prefix (Ctrl+K) */
	bool chord;

	/* context menu target */
	int ctx;
	u32 ctx_a, ctx_b, ctx_off;
	char ctx_path[4096];

	/* generic picker (QUICK_PICK): label / description / value per item */
	int pick_kind;
	Pool pick_pool;
	u32 *pick_label, *pick_desc, *pick_val;
	u32 pick_n, pick_cap;
	i32 refs_req, decl_req;

	/* mouse cursor shape wanted by the UI */
	int cursor;

	/* cursor history for Ctrl+U (cursorUndo) */
	u32 ch_doc[32], ch_nc[32];
	u32 *ch_head[32], *ch_anchor[32];
	u32 ch_n;

	/* clipboard fallback when there is no window (headless runs) */
	char *clip;
	bool clip_whole_line;
} App;

extern App app;

#define PX(n) ((n) * app.scale)
#define COL(id) (themes[app.theme].c[id])
#define TOK(id) (themes[app.theme].tok[id])

/* ui.c */
bool ui_init(const char *root, int w, int h, int scale, bool headless);
void ui_shutdown(void);
void ui_event(const Event *e);
void ui_tick(void); /* LSP, timers; call every loop iteration */
void ui_draw(void);
u64 ui_wait_hint_ms(void);
void ui_status(const char *fmt, ...);
void ui_log(const char *fmt, ...);
void ui_layout(void);
char *ui_clipboard_get(void);
void ui_clipboard_set(const char *s, size_t n);

/* documents */
u32 doc_open(const char *abs_path); /* returns doc index or UINT32_MAX */
u32 doc_find(const char *abs_path);  /* open document for a path, or UINT32_MAX */
u32 doc_new_untitled(void);
void doc_release(u32 d);
bool doc_save(u32 d);
void doc_replace(u32 d, u32 pos, u32 del, const char *ins, u32 nins, u32 group);
u32 doc_git_color(u32 d, char *letter, bool *has);
u32 path_git_color(const char *rel, char *letter, bool *has);
int file_icon_draw(const char *name, int x, int y, int h);
void open_file(const char *abs_path, bool preview, u32 line, u32 col);
void open_rel(const char *rel, bool preview, u32 line, u32 col);
View *active_view(void);
void close_tab(u32 g, u32 t);

/* editor.c */
void ed_draw(Group *g, View *v, int x, int y, int w, int h, bool focused);
bool ed_key(View *v, const Event *e);
void ed_text(View *v, const char *s, size_t n);
void ed_mouse(View *v, const Event *e, int x, int y, int w, int h);
void ed_reveal(View *v, u32 off, bool center);
void ed_set_cursor(View *v, u32 off, bool extend);
void ed_find_update(View *v);
void ed_find_next(View *v, bool back);
void ed_replace_one(View *v);
void ed_replace_all(View *v);
u32 ed_new_group(void);
void ed_completion_trigger(View *v, bool explicit_);
void ed_completion_accept(View *v);
void ed_completion_filter(View *v);
void ed_goto_definition(View *v);
void ed_hover_request(View *v);
void ed_draw_popups(View *v);
u32 ed_line_col_to_off(Text *t, u32 line, u32 col);
void ed_lex_until(u32 d, u32 line);
int ed_gutter_w(View *v);
void ed_insert_text(View *v, const char *s, size_t n, u8 kind);
u32 ed_offset_at(View *v, int mx, int my, int x, int y);
int ed_cursor_shape(View *v, int mx, int my, int x, int y, int w, int h, int mods);
u32 blame_key_of(u32 d);
const char *doc_repo_rel(u32 d);
bool ed_command(View *v, int cmd);

/* panes.c */
void explorer_draw(int x, int y, int w, int h);
bool explorer_event(const Event *e, int x, int y, int w, int h);
void explorer_reveal(u32 d);
void search_draw(int x, int y, int w, int h);
bool search_event(const Event *e, int x, int y, int w, int h);
void search_run(void);
void search_tick(void);
void search_replace(u32 file, u32 result); /* UINT32_MAX = all */
void search_step_result(int dir);
void ui_ensure_files(void);
void scm_draw(int x, int y, int w, int h);
bool scm_event(const Event *e, int x, int y, int w, int h);
void scm_refresh(void);
void problems_draw(int x, int y, int w, int h);
bool problems_event(const Event *e, int x, int y, int w, int h);
void output_draw(int x, int y, int w, int h);
bool output_event(const Event *e, int x, int y, int w, int h);
u32 problems_count(int sev);

/* input.c-like helpers (in panes.c) */
void input_set(Input *in, const char *s, size_t n);
bool input_key(Input *in, const Event *e); /* edit keys; false if unhandled */
void input_text(Input *in, const char *s, size_t n);
void input_draw(Input *in, int x, int y, int w, int h, const char *placeholder, bool focused);
void input_click(Input *in, int font, int x0, int mx);

/* actions.c: context menus, picker results, the commands below CMD_QUIT */
void ctx_menu_editor(View *v, int x, int y, u32 off);
void ctx_menu_explorer(int x, int y, u32 node);
void ctx_menu_tab(int x, int y, u32 group, u32 tab);
void ctx_menu_scm_file(int x, int y, const char *repo_rel, bool staged);
void ctx_menu_scm_title(int x, int y);
bool actions_command(int cmd); /* true when handled */
void pick_begin(int kind, const char *title, const char *placeholder);
void pick_add(const char *label, const char *desc, u32 val);
void pick_show(void);
void pick_accept(int kind, u32 item);
void actions_take_locations(const LspEvent *ev);
void cursor_history_push(View *v);
u32 doc_new_virtual(const char *name, const char *text, size_t n, int lang);

/* commands */
enum {
	CMD_NONE,
	CMD_QUICK_OPEN,
	CMD_COMMANDS,
	CMD_SAVE,
	CMD_SAVE_ALL,
	CMD_NEW_FILE,
	CMD_CLOSE_EDITOR,
	CMD_CLOSE_ALL,
	CMD_NEXT_EDITOR,
	CMD_PREV_EDITOR,
	CMD_TOGGLE_SIDEBAR,
	CMD_TOGGLE_PANEL,
	CMD_VIEW_EXPLORER,
	CMD_VIEW_SEARCH,
	CMD_VIEW_SCM,
	CMD_VIEW_PROBLEMS,
	CMD_VIEW_OUTPUT,
	CMD_SPLIT,
	CMD_FOCUS_GROUP1,
	CMD_FOCUS_GROUP2,
	CMD_FOCUS_GROUP3,
	CMD_GOTO_LINE,
	CMD_FIND,
	CMD_REPLACE,
	CMD_FIND_NEXT,
	CMD_FIND_PREV,
	CMD_REPLACE_IN_FILES,
	CMD_SEARCH_REPLACE_ALL,
	CMD_SEARCH_NEXT_RESULT,
	CMD_SEARCH_PREV_RESULT,
	CMD_SEARCH_TOGGLE_DETAILS,
	CMD_UNDO,
	CMD_REDO,
	CMD_CUT,
	CMD_COPY,
	CMD_PASTE,
	CMD_SELECT_ALL,
	CMD_SELECT_LINE,
	CMD_ADD_NEXT_MATCH,
	CMD_SELECT_ALL_MATCHES,
	CMD_TOGGLE_COMMENT,
	CMD_INDENT,
	CMD_OUTDENT,
	CMD_MOVE_LINE_UP,
	CMD_MOVE_LINE_DOWN,
	CMD_COPY_LINE_UP,
	CMD_COPY_LINE_DOWN,
	CMD_DELETE_LINE,
	CMD_INSERT_LINE_BELOW,
	CMD_INSERT_LINE_ABOVE,
	CMD_CURSOR_ABOVE,
	CMD_CURSOR_BELOW,
	CMD_GOTO_DEFINITION,
	CMD_NAV_BACK,
	CMD_NAV_FORWARD,
	CMD_NEXT_PROBLEM,
	CMD_PREV_PROBLEM,
	CMD_TRIGGER_SUGGEST,
	CMD_SHOW_HOVER,
	CMD_TOGGLE_THEME,
	CMD_THEME_DARK,
	CMD_THEME_LIGHT,
	CMD_GIT_REFRESH,
	CMD_GIT_COMMIT,
	CMD_GIT_STAGE_ALL,
	CMD_GIT_STAGE_FILE,
	CMD_GIT_UNSTAGE_FILE,
	CMD_GIT_DISCARD_FILE,
	CMD_GIT_TOGGLE_BLAME,
	CMD_REFRESH_EXPLORER,
	CMD_COLLAPSE_FOLDERS,
	CMD_REVEAL_IN_EXPLORER,
	CMD_RESTART_LSP,
	CMD_QUIT,
	CMD_GOTO_DECLARATION,
	CMD_GOTO_TYPE_DEFINITION,
	CMD_GOTO_REFERENCES,
	CMD_FIND_ALL_REFERENCES,
	CMD_CHANGE_ALL_OCCURRENCES,
	CMD_CURSOR_UNDO,
	CMD_CURSORS_LINE_ENDS,
	CMD_MOVE_SEL_NEXT_MATCH,
	CMD_COLUMN_SELECT_LEFT,
	CMD_COLUMN_SELECT_RIGHT,
	CMD_COLUMN_SELECT_UP,
	CMD_COLUMN_SELECT_DOWN,
	CMD_COPY_PATH,
	CMD_COPY_REL_PATH,
	CMD_EXPLORER_NEW_FILE,
	CMD_EXPLORER_NEW_FOLDER,
	CMD_EXPLORER_RENAME,
	CMD_EXPLORER_DELETE,
	CMD_CLOSE_OTHERS,
	CMD_CLOSE_RIGHT,
	CMD_SCM_OPEN_CHANGES,
	CMD_SCM_OPEN_FILE,
	CMD_SCM_STAGE,
	CMD_SCM_UNSTAGE,
	CMD_SCM_DISCARD,
	CMD_SCM_MENU,
	CMD_GIT_UNSTAGE_ALL,
	CMD_GIT_DISCARD_ALL,
	CMD_GIT_COMMIT_ALL,
	CMD_GIT_COMMIT_AMEND,
	CMD_GIT_UNDO_COMMIT,
	CMD_GIT_FETCH,
	CMD_GIT_FETCH_PRUNE,
	CMD_GIT_PULL,
	CMD_GIT_PULL_REBASE,
	CMD_GIT_PUSH,
	CMD_GIT_PUSH_FORCE,
	CMD_GIT_SYNC,
	CMD_GIT_PUBLISH,
	CMD_GIT_CHECKOUT,
	CMD_GIT_CREATE_BRANCH,
	CMD_GIT_CREATE_BRANCH_FROM,
	CMD_GIT_RENAME_BRANCH,
	CMD_GIT_DELETE_BRANCH,
	CMD_GIT_MERGE,
	CMD_GIT_REBASE,
	CMD_GIT_REBASE_CONTINUE,
	CMD_GIT_REBASE_ABORT,
	CMD_GIT_MERGE_ABORT,
	CMD_GIT_STASH,
	CMD_GIT_STASH_UNTRACKED,
	CMD_GIT_STASH_APPLY_LATEST,
	CMD_GIT_STASH_APPLY,
	CMD_GIT_STASH_POP_LATEST,
	CMD_GIT_STASH_POP,
	CMD_GIT_STASH_DROP,
	CMD_GIT_CREATE_TAG,
	CMD_GIT_DELETE_TAG,
	CMD_GRAPH_CHECKOUT,
	CMD_GRAPH_CREATE_BRANCH,
	CMD_GRAPH_CHERRY_PICK,
	CMD_GRAPH_COPY_ID,
	CMD_GRAPH_COPY_MESSAGE,
	CMD_GRAPH_OPEN_COMMIT,
	CMD_GIT_SHOW_OUTPUT,
	CMD_COUNT
};

void run_command(int cmd);
const char *command_title(int cmd);
const char *command_keys(int cmd); /* "Ctrl+Shift+P" or NULL */

#endif
