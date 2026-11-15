/* Debug Adapter Protocol client and session model.
 *
 * Speaks DAP to an adapter over its stdio (lldb-dap by default: VXIDE_DAP,
 * then lldb-dap23 / 21 / lldb-dap / lldb-dap19 / lldb-dap-devel, then
 * gdb -i dap). Runs the launch handshake (initialize, launch, initialized,
 * breakpoints, configurationDone) and keeps a model the UI draws: state,
 * threads, the selected thread's frames, the selected frame's scopes,
 * variables (fetched on demand, cached per stop), evaluations, breakpoint
 * results, data breakpoints (hardware watchpoints) and program output.
 * Nothing blocks: dap_pump() reads what is there and advances. */
#ifndef VX_DAP_H
#define VX_DAP_H

#include "base.h"

typedef struct Dap Dap;

enum { DS_STARTING, DS_RUNNING, DS_STOPPED, DS_EXITED };
enum { DW_WRITE, DW_READ, DW_READWRITE };
enum { DV_NONE, DV_PENDING, DV_LOADED, DV_ERROR };

typedef struct {
	const char *adapter; /* argv[0] of the adapter, NULL: auto */
	const char *program;
	const char *const *args;
	u32 nargs;
	const char *cwd;
	bool stop_on_entry;
} DapLaunch;

/* Start an adapter and launch the program. NULL with err set when no
 * adapter could be started. */
Dap *dap_start(const DapLaunch *l, char *err, size_t errcap);
/* Ask the adapter to end the session (the debuggee is terminated); the
 * adapter is killed if it does not go within a second or two. */
void dap_stop(Dap *d);
void dap_free(Dap *d);
/* Read and process whatever arrived. True when the model changed. */
bool dap_pump(Dap *d);
int dap_fd(Dap *d); /* readable fd for poll(), -1 when gone */
bool dap_alive(Dap *d); /* the adapter process still runs */

int dap_state(Dap *d);
const char *dap_adapter(Dap *d);        /* "lldb-dap23" */
const char *dap_stop_reason(Dap *d);     /* "breakpoint", "step", "data breakpoint"... */
const char *dap_stop_text(Dap *d);       /* the adapter's description, may be "" */
int dap_exit_code(Dap *d);               /* after DS_EXITED, -1 unknown */
u32 dap_stop_count(Dap *d);              /* bumps at every stop */
bool dap_supports_data_bp(Dap *d);
bool dap_is_lldb(Dap *d);

/* ---- execution control (no-ops unless stopped / running) ---------------------- */
void dap_continue(Dap *d);
void dap_pause(Dap *d);
void dap_next(Dap *d);     /* step over */
void dap_step_in(Dap *d);
void dap_step_out(Dap *d);

/* ---- breakpoints ----------------------------------------------------------------
 * The UI owns the list; it sends a file's whole set whenever it changes.
 * cond[i] may be NULL. Results: verified and the line the adapter chose. */
void dap_set_breakpoints(Dap *d, const char *path, const u32 *lines, const char *const *cond, u32 n);
bool dap_bp_result(Dap *d, const char *path, u32 line, bool *verified, u32 *actual);

/* ---- threads, frames, scopes ---------------------------------------------------- */
u32 dap_thread_count(Dap *d);
i64 dap_thread_id(Dap *d, u32 i);
const char *dap_thread_name(Dap *d, u32 i);
i64 dap_current_thread(Dap *d);   /* the selected thread */
i64 dap_stopped_thread(Dap *d);   /* the one that stopped */
void dap_select_thread(Dap *d, i64 id); /* fetches its stack */

u32 dap_frame_count(Dap *d);     /* of the selected thread; 0 while loading */
const char *dap_frame_name(Dap *d, u32 i);
const char *dap_frame_path(Dap *d, u32 i); /* "" when no source */
u32 dap_frame_line(Dap *d, u32 i);         /* 1-based, 0 unknown */
u32 dap_frame_col(Dap *d, u32 i);
u64 dap_frame_ip(Dap *d, u32 i);
u32 dap_current_frame(Dap *d);
void dap_select_frame(Dap *d, u32 i); /* fetches its scopes */

u32 dap_scope_count(Dap *d);
const char *dap_scope_name(Dap *d, u32 i);
i64 dap_scope_ref(Dap *d, u32 i);
bool dap_scope_expensive(Dap *d, u32 i);

/* ---- variables: the children of a variablesReference ------------------------------
 * dap_vars() asks for them the first time (DV_PENDING), then returns
 * DV_LOADED with *first / *count into the node table. */
int dap_vars(Dap *d, i64 ref, u32 *first, u32 *count);
const char *dap_var_name(Dap *d, u32 node);
const char *dap_var_value(Dap *d, u32 node);
const char *dap_var_type(Dap *d, u32 node);
const char *dap_var_eval_name(Dap *d, u32 node); /* "" when the adapter gave none */
i64 dap_var_ref(Dap *d, u32 node);               /* > 0: has children */
bool dap_var_changed(Dap *d, u32 node);          /* differs from when it was last seen */

/* ---- evaluate ------------------------------------------------------------------------
 * context: "hover", "watch" or "repl"; in the selected frame. Returns an id
 * to poll; results live until the program runs again. */
i32 dap_evaluate(Dap *d, const char *expr, const char *context);
int dap_eval_result(Dap *d, i32 id, const char **value, const char **type, i64 *ref);

/* ---- data breakpoints (hardware watchpoints) -------------------------------------------
 * Watch an lvalue expression ("counter", "p.x", "*ptr") of the selected
 * frame: lldb gets its address and size (&(expr), sizeof) and an
 * "ADDR/SIZE" data id, gdb resolves the expression itself. Set up
 * asynchronously; dap_watch_*() describe the list. Only while stopped. */
void dap_watch_add(Dap *d, const char *expr, int access);
/* Software watchpoints (gdb only: single-steps, slow, no slot limit). */
void dap_watch_add_ex(Dap *d, const char *expr, int access, bool software);
bool dap_supports_sw_watch(Dap *d);
bool dap_watch_hw(Dap *d, u32 i); /* a hardware watchpoint (vs software) */
void dap_watch_remove(Dap *d, u32 i);
void dap_watch_set_access(Dap *d, u32 i, int access);
u32 dap_watch_count(Dap *d);
const char *dap_watch_name(Dap *d, u32 i);
const char *dap_watch_desc(Dap *d, u32 i); /* "4 bytes at 203A50 counter" or the error */
int dap_watch_access(Dap *d, u32 i);
bool dap_watch_ok(Dap *d, u32 i);          /* accepted by the adapter */
int dap_hw_slots(Dap *d);                  /* hardware watchpoint slots, -1 unknown */

/* ---- output ------------------------------------------------------------------------- */
/* Pop the next chunk of output ("stdout", "stderr", "console"). */
bool dap_output(Dap *d, const char **category, const char **text);

#endif
