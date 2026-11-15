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
#include "fswatch.h"

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

enum { LSV_CLANGD, LSV_RUST, LSV_QML, LSV_GODOT, LSV_COUNT };

typedef struct {
	u32 n;
	Text text[MAX_DOCS];
	char *path[MAX_DOCS]; /* absolute; NULL for untitled */
	char *rel[MAX_DOCS];  /* relative to the root, or display name */
	u8 lang[MAX_DOCS];
	u8 untitled[MAX_DOCS];
	u8 tabs[MAX_DOCS]; /* indentation uses tabs (detected) */
	u8 readonly[MAX_DOCS]; /* virtual documents (diffs, commits) */
	u8 bin[MAX_DOCS];
	u8 man[MAX_DOCS];      /* a manual page (man.c): read-only, links */      /* binary: hex editor / disassembly (binview.c) */
	u32 refs[MAX_DOCS]; /* views showing it */

	/* highlighter: lexer state at the start of each line */
	u32 *lstate[MAX_DOCS];
	u32 lstate_cap[MAX_DOCS];
	u32 lstate_ok[MAX_DOCS]; /* states [0, ok] are valid */

	/* LSP sync */
	bool lsp_open[MAX_DOCS];
	i8 lsp_sv[MAX_DOCS]; /* LSV_* serving it, -1 none */
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
#define MAX_VIEWS 512

/* A view (an editor tab's state) lives in app.views and is named by its
 * u32 index everywhere it is stored (groups, workspaces); a View * is only
 * a working handle (the table never moves). Its cursors are a coalesced SoA
 * block that grows on demand (view_reserve), not 512 inline slots. */
typedef struct {
	u32 doc;
	u32 nc, ccap;    /* cursors in use / capacity of the block */
	u32 *head, *anchor;
	i32 *want_x;     /* preferred x in pixels for up/down, -1 none */
	i64 scroll_y;            /* pixels */
	i32 scroll_x;
	bool preview; /* italic tab, replaced by the next preview open */
	/* diff editor: this view shows `doc` on the right and diff_left (a
	 * read-only original) on the left; UINT32_MAX when not a diff */
	u32 diff_left;
	/* multi-part views (an executable's sections): subtabs under the tabs */
	u32 nparts, part;
	u8 diff_kind; /* DIFF_WORKTREE, DIFF_STAGED, DIFF_COMMIT */
	u32 diff_rel_at; /* the diffed path: offset in app.strs (view_rel) */
	u32 group;       /* undo group currently being extended */
	u8 group_kind;
	u64 group_ns;
} View;

#define MAX_GROUPS 3
#define MAX_TABS 64

typedef struct {
	u32 nt, active;
	u32 tab[MAX_TABS]; /* view ids (app.views) */
	u32 mru[MAX_TABS]; /* tab indices, most recent first */
	int x, y, w, h;
} Group;

/* ---- workbench ---------------------------------------------------------- */

enum { DIFF_WORKTREE = 1, DIFF_STAGED, DIFF_COMMIT };
enum { SIDE_EXPLORER, SIDE_SEARCH, SIDE_SCM, SIDE_SETTINGS, SIDE_MAN, SIDE_COUNT };
enum { PANEL_PROBLEMS, PANEL_OUTPUT, PANEL_COUNT };
enum {
	FOCUS_EDITOR,
	FOCUS_SIDEBAR,
	FOCUS_PANEL,
	FOCUS_QUICK,
	FOCUS_FIND,
	FOCUS_SEARCH_INPUT,
	FOCUS_MAN_INPUT,
	FOCUS_WORKSPACE, /* the Manpages page or the Settings page */
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
	int theme_before; /* the theme picker previews live; Escape goes back to this */
	int w, h;
	char *root;
	bool headless;
	bool running;
	bool dirty; /* needs redraw */
	u64 now;
	u64 blink_t0;

	/* metrics */
	int act_w, top_h, side_w, status_h, tabs_h, subtabs_h, crumbs_h, panel_h;
	bool minimap_on; /* settings */
	char bg_path[1024]; /* editor background image, "" = none */
	int bg_opacity;     /* percent */
	int bg_iw, bg_ih;   /* loaded image size, 0 = none */
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
	/* views (u32-indexed table) and their strings */
	View views[MAX_VIEWS];
	u8 view_used[MAX_VIEWS];
	Pool strs;
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

	/* file-system watches (explorer folders, .git, open documents) */
	FsWatch *fsw;
	Pool watch_pool;
	u32 watch_rel[1024]; /* pool offsets of watched explorer folders */
	u32 nwatch;
	u64 watch_sync_ns;
	u64 git_due_ns; /* debounced status refresh request */
	u64 git_req_ns;
	bool search_pending; /* waiting for the file list */
	bool quick_accept_pending; /* Enter pressed in Quick Open while scanning */

	/* source control */
	Git *git;
	Blame *blame;
	bool blame_on; /* git.blame.editorDecoration.enabled */
	bool vim_on;   /* vim keybindings (off by default) */
	u32 blame_gen;  /* bumps on every git refresh (HEAD may move) */
	u32 blame_doc, blame_key;
	bool blame_asked;
	bool blame_hover; /* mouse rests on the inline blame text */
	char blame_hover_hash[41];
	int blame_hover_x, blame_hover_y, blame_hover_x1; /* the blame text's rect: the card lives while the pointer is on it */
	bool hover_mouse; /* the code hover came from the pointer resting (not Ctrl+K Ctrl+I) */
	u64 blame_hover_t;
	Input commit_in;
	i32 scm_scroll;
	u32 scm_sel;
	u64 git_ns;

	/* LSP */
	/* language servers: clangd (C, C++) and rust-analyzer, started when
	 * a document needs them */
	Lsp *lsps[LSV_COUNT];
	bool lsps_ready[LSV_COUNT];
	bool lsps_failed[LSV_COUNT];
	u64 lsps_retry_ns[LSV_COUNT]; /* reconnect time for servers we connect to (Godot) */
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

	/* overtype (insert key) mode, VSCodium's editor.action.toggleOvertypeInsertMode */
	bool overtype;

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

static inline View *gview(const Group *g, u32 t) { return &app.views[g->tab[t]]; }
/* a document's diagnostics / semantic tokens: coalesced SoA blocks */
#define DIAG_COLS(D, d) SOA((D)->d_line0[d]), SOA((D)->d_col0[d]), SOA((D)->d_line1[d]), SOA((D)->d_col1[d]), SOA((D)->d_msg[d]), SOA((D)->d_sev[d])
#define SEM_COLS(D, d) SOA((D)->s_line[d]), SOA((D)->s_col[d]), SOA((D)->s_len[d]), SOA((D)->s_cls[d])
static inline u32 view_id(const View *v) { return (u32)(v - app.views); }
static inline const char *view_rel(const View *v) { return app.strs.b ? app.strs.b + v->diff_rel_at : ""; }
View *view_new(u32 doc);            /* a free slot, one cursor at 0 */
void view_free(View *v);            /* back to the table, cursors freed */
void view_reserve(View *v, u32 n);  /* room for n cursors */
void view_set_rel(View *v, const char *rel);

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
int group_tabs_h(Group *g); /* tab rows (+ subtab row) */
int group_head_h(Group *g); /* tabs + breadcrumbs */
/* binview.c: subtabs of multi-part views */
void view_subtabs_draw(View *v, int x, int y, int w, int h);
void view_subtab_click(View *v, int rel_x, int rel_y, int w);
int view_subtabs_height(View *v, int w);
void binview_init(View *v);
void binview_closed(View *v);
void binview_doc_released(u32 d);
void binview_draw(View *v, int x, int y, int w, int h, bool focused);
bool binview_key(View *v, const Event *e);
void binview_mouse(View *v, const Event *e, int x, int y, int w, int h);
bool binview_command(int cmd);
void binview_context(View *v, int x, int y);
void binview_prompt_accept(int kind, const char *text);
void binview_status(View *v, char *buf, size_t cap);
bool binview_busy(void);
u32 doc_new_untitled(void);
void doc_release(u32 d);
void doc_release_unref(u32 d);
bool doc_save(u32 d);
void doc_replace(u32 d, u32 pos, u32 del, const char *ins, u32 nins, u32 group);
u32 doc_git_color(u32 d, char *letter, bool *has);
u32 path_git_color(const char *rel, char *letter, bool *has);
int file_icon_draw(const char *name, int x, int y, int h);
void open_file(const char *abs_path, bool preview, u32 line, u32 col);
void open_rel(const char *rel, bool preview, u32 line, u32 col);
View *active_view(void);

/* dialog.c: modal Save / Don't Save / Cancel */
enum { DLG_CLOSE_TAB = 1, DLG_CLOSE_ALL, DLG_QUIT };
void dialog_open(int kind, u32 arg, const char *title, const char *detail);
bool dialog_active(void);
void dialog_draw(void);
void dialog_event(const Event *e);
void dialog_result(int kind, u32 arg, int choice); /* ui.c; 0 save, 1 don't save, 2 cancel */
void request_quit(void); /* asks first when files are unsaved */
/* A tooltip for the control at x,y,w,h (shown when the pointer rests there). */
void ui_tip(int x, int y, int w, int h, const char *text);
void doc_set_lang(u32 d, int lang);

/* session.c: reopen the last editors per project; recently used commands */
void session_save(void);
void session_restore(void);
void command_used(int cmd);
int command_recent_rank(int cmd);

/* vim.c: vim emulation, active while app.vim_on. ed_key / ed_text pass
 * events here first; true means vim consumed it. */
bool vim_key(View *v, const Event *e);
bool vim_text(View *v, const char *s, size_t n);
bool vim_claims_key(const Event *e);
const char *vim_mode_label(void); /* "-- INSERT --", "-- VISUAL --", ":wq" being typed..., or NULL */
bool vim_block_cursor(void);      /* normal/visual: draw a block cursor */
void vim_reset(void);             /* back to normal mode (toggle, tab switch) */
bool vim_command(View *v, int cmd); /* ed_command: bound keys vim owns (Ctrl+D/U/W/[) */
u32 vim_cursor_off(View *v, u32 c); /* where cursor c's block is drawn */
bool ed_undo_redo(View *v, bool redo, u32 *pos); /* editor.c; pos = start of the change */
int ed_view_lines(void);                         /* editor.c; whole lines in view */
/* The ready language server of a document that is open on it, else NULL. */
Lsp *doc_lsp(u32 d);
const char *lsv_name(int sv);

/* man.c: manual pages */
bool man_open(const char *name, const char *sec, bool prefer_c);
bool man_follow(u32 d, u32 off);
void man_open_at(View *v, u32 off);
void man_ws_draw(int x, int y, int w, int h);
void man_ws_event(const Event *e, int x, int y, int w, int h);
bool man_ws_key(const Event *e);
bool man_ws_context(int mx, int my, int x, int y, int w);
View *man_active_view(void);
void man_shutdown(void);
void man_prefetch(void); /* start reading the index (pointer on the tab) */
bool man_busy(void);

/* Workspaces: Manpages and Settings take the whole main area instead of
 * the side bar and the editor groups. */
bool ui_workspace(void);
void workspace_area(int *x, int *y, int *w, int *h);
void settings_ws_draw(int x, int y, int w, int h);
void settings_ws_event(const Event *e, int x, int y, int w, int h);
void settings_load(void);
void settings_save(void);
void background_apply(void); /* (re)load app.bg_path into the GPU */
void man_tick(void);
u64 man_wait_hint(void);
/* panes.c helpers shared with other panes */
int view_title(int x, int y, int w, const char *t);
void list_row_bg(int x, int y, int w, int h, bool selected, bool focused, bool hover);

/* projtools.c: Qt and Godot integration */
void projtools_init(void);
void projtools_tick(void);
void projtools_shutdown(void);
bool projtools_busy(void);
bool projtools_command(int cmd);
void projtools_explorer_menu(u32 node);
const char *godot_dir(void);
int godot_lsp_port(void);
const char *qmlls_path(void);
const char *qt_build_dir(void);
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
void ed_draw_line_text(u32 d, u32 l, int tx, int ly, int clip_x0, int clip_x1);
int ed_x_of(Text *t, u32 l, u32 off);
u32 ed_off_at_x(Text *t, u32 l, int px);
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
/* bgwork.c */
void bg_files_request(void);
bool bg_files_poll(void);
bool bg_files_busy(void);
void bg_search_start(const char *query, int flags, const char *inc, const char *exc);
bool bg_search_busy(u32 *done, u32 *total);
bool bg_search_poll(void);
void bg_shutdown(void);
void git_request_refresh(u64 delay_ms);
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
/* Wrapped boxes: reserve = width kept free on the right for icons; long
 * text adds rows instead of running under them. */
int input_height(Input *in, int w, int reserve);
int input_draw_ex(Input *in, int x, int y, int w, int reserve, const char *ph, bool focused);
void input_click_ex(Input *in, int x, int y, int w, int reserve, int mx, int my, int clicks);

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

/* diffview.c */
void diff_open_worktree(const char *repo_rel); /* index (left) vs working file (right, editable) */
void diff_open_staged(const char *repo_rel);   /* HEAD vs index */
void diff_open_commit(const char *hash, const char *repo_rel);
void diff_draw(Group *g, View *v, int x, int y, int w, int h, bool focused);
void diff_mouse(View *v, const Event *e, int x, int y, int w, int h);
bool diff_key(View *v, const Event *e);
bool diff_command(View *v, int cmd);
void diff_view_closed(View *v);
/* conflicts in normal editors */
void conflict_draw(View *v, u32 first, u32 last, int x, int text_x, int text_w, int y0, int lh, int tx);
u32 conflict_regions(u32 d, u32 **start, u32 **sep, u32 **end, u32 **base);

/* commands */
enum {
	CMD_NONE,
	CMD_QUICK_OPEN,
	CMD_COMMANDS,
	CMD_SAVE,
	CMD_SAVE_ALL,
	CMD_NEW_FILE,
	CMD_CLOSE_EDITOR,
	CMD_REVERT_AND_CLOSE,
	CMD_CHANGE_LANGUAGE,
	CMD_TOGGLE_INDENT_STYLE,
	CMD_CLOSE_ALL,
	CMD_NEXT_EDITOR,
	CMD_PREV_EDITOR,
	CMD_TOGGLE_SIDEBAR,
	CMD_TOGGLE_PANEL,
	CMD_VIEW_EXPLORER,
	CMD_VIEW_SEARCH,
	CMD_VIEW_SCM,
	CMD_VIEW_SETTINGS,
	CMD_TOGGLE_VIM,
	CMD_VIEW_MAN,
	CMD_OPEN_MANPAGE,
	CMD_SET_BACKGROUND,
	CMD_CLEAR_BACKGROUND,
	CMD_QT_DESIGNER,
	CMD_QT_LINGUIST,
	CMD_QT_PREVIEW_QML,
	CMD_GODOT_EDITOR,
	CMD_GODOT_RUN,
	CMD_GODOT_RUN_SCENE,
	CMD_GODOT_STOP,
	CMD_GODOT_HEADLESS_LSP,
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
	CMD_THEME_DARKER,
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
	CMD_TRANSFORM_UPPER,
	CMD_TRANSFORM_LOWER,
	CMD_TRANSFORM_TITLE,
	CMD_TRANSFORM_SNAKE,
	CMD_TRANSFORM_KEBAB,
	CMD_TOGGLE_OVERTYPE,
	CMD_GIT_STAGE_RANGES,
	CMD_GIT_UNSTAGE_RANGES,
	CMD_GIT_REVERT_RANGES,
	CMD_GIT_STAGE_CHANGE,   /* gutter: the change under ctx_off line */
	CMD_GIT_REVERT_CHANGE,
	CMD_GIT_MARK_RESOLVED,
	CMD_CONFLICT_NEXT,
	CMD_CONFLICT_PREV,
	CMD_DIFF_NEXT_CHANGE,
	CMD_DIFF_PREV_CHANGE,
	CMD_HEX_ADD_TAG,
	CMD_HEX_REMOVE_TAG,
	CMD_HEX_GOTO,
	CMD_HEX_ENDIAN,
	CMD_COUNT
};

void run_command(int cmd);
const char *command_title(int cmd);
const char *command_keys(int cmd); /* "Ctrl+Shift+P" or NULL */

#endif
