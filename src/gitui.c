/* Source Control extras: the GRAPH section (scm.graph), git actions from
 * the "..." menu and the command palette (stash, branches, merge, rebase,
 * remotes, tags), read-only diff documents, and the inline-blame hover. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "gitgraph.h"
#include "gitops.h"
#include "syntax.h"

#include <stdio.h>
#include <time.h>
#include <SDL3/SDL.h>

static GitOps *ops;
static GitGraph graph;
static bool graph_ok, graph_open = true;
static u32 graph_gen = UINT32_MAX;
static i32 graph_scroll;
static u32 graph_sel = UINT32_MAX;
static u32 expanded = UINT32_MAX; /* row whose files are listed */
static GitCommitInfo exp_ci;
static bool exp_ok;
static u32 hover_row = UINT32_MAX;
static u64 hover_t0;

/* hover card cache (graph rows and blame) */
static char card_hash[41];
static GitCommitInfo card_ci;
static bool card_ok;

/* which picker / prompt is pending */
enum {
	PK_CHECKOUT = 100,
	PK_BRANCH_FROM,
	PK_DELETE_BRANCH,
	PK_MERGE,
	PK_REBASE,
	PK_STASH_APPLY,
	PK_STASH_POP,
	PK_STASH_DROP,
	PK_DELETE_TAG,
	PK_PUBLISH_REMOTE,
};
enum {
	PR_CREATE_BRANCH = 100,
	PR_BRANCH_FROM_NAME,
	PR_RENAME_BRANCH,
	PR_STASH,
	PR_STASH_UNTRACKED,
	PR_TAG,
	PR_GRAPH_BRANCH,
	PR_COMMIT_ALL,
	PR_CONFIRM_DISCARD_ALL,
	PR_CONFIRM_DISCARD_FILE,
	PR_CONFIRM_FORCE_PUSH,
	PR_CONFIRM_DELETE_BRANCH_FORCE,
};

static char pending_ref[256]; /* branch-from base, force-delete name */
static GitBranchList branches;
static GitStashList stashes;
static GitNameList names;

static bool need_ops(void)
{
	if (!app.git)
		return false;
	if (!ops)
		ops = gitops_start(git_workdir(app.git));
	if (!ops)
		ui_status("Git: the git command was not found");
	return ops != NULL;
}

static void prompt(int kind, const char *title, const char *initial)
{
	app.quick = QUICK_PROMPT;
	app.quick_prompt_kind = kind;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, initial ? initial : "", initial ? strlen(initial) : 0);
	app.focus = FOCUS_QUICK;
}

static void confirm(int kind, const char *title)
{
	app.quick = QUICK_CONFIRM;
	app.quick_prompt_kind = kind;
	snprintf(app.quick_title, sizeof app.quick_title, "%s", title);
	input_set(&app.quick_in, "", 0);
	app.focus = FOCUS_QUICK;
}

/* ---- pickers -------------------------------------------------------------------- */

static void pick_branches(int kind, const char *title, bool locals_only, bool skip_current)
{
	if (!need_ops())
		return;
	gitops_branches_free(&branches);
	if (gitops_branches(ops, &branches) != 0) {
		ui_status("Git: could not list branches");
		return;
	}
	pick_begin(kind, title, NULL);
	for (u32 i = 0; i < branches.n; i++) {
		u8 f = branches.flags[i];
		if (locals_only && !(f & GB_LOCAL))
			continue;
		if (skip_current && (f & GB_CURRENT))
			continue;
		const char *nm = branches.pool.b + branches.name[i];
		char desc[160];
		snprintf(desc, sizeof desc, "%s%s%.8s", (f & GB_CURRENT) ? "current \xC2\xB7 " : "",
		         (f & GB_REMOTE) ? "Remote branch at " : "", branches.pool.b + branches.hash[i]);
		pick_add(nm, desc, i);
	}
	if (!app.pick_n) {
		ui_status("Git: no branches to choose from");
		return;
	}
	pick_show();
}

static void pick_stashes(int kind, const char *title)
{
	if (!need_ops())
		return;
	gitops_stashes_free(&stashes);
	gitops_stashes(ops, &stashes);
	if (!stashes.n) {
		ui_status("Git: there are no stashes in the repository");
		return;
	}
	pick_begin(kind, title, NULL);
	for (u32 i = 0; i < stashes.n; i++)
		pick_add(stashes.pool.b + stashes.message[i], stashes.pool.b + stashes.ref[i], i);
	pick_show();
}

static void pick_names(int kind, const char *title, bool remotes)
{
	if (!need_ops())
		return;
	gitops_names_free(&names);
	if (remotes)
		gitops_remotes(ops, &names);
	else
		gitops_tags(ops, &names);
	if (!names.n) {
		ui_status(remotes ? "Git: this repository has no remotes" : "Git: there are no tags");
		return;
	}
	pick_begin(kind, title, NULL);
	for (u32 i = 0; i < names.n; i++)
		pick_add(names.pool.b + names.name[i], names.pool.b + names.hash[i], i);
	pick_show();
}

static const char *current_branch(void) { return app.git ? git_branch(app.git) : ""; }

void gitui_pick_accept(int kind, u32 item)
{
	if (!ops)
		return;
	u32 v = app.pick_val[item];
	const char *bname = v < branches.n ? branches.pool.b + branches.name[v] : "";
	bool remote = v < branches.n && (branches.flags[v] & GB_REMOTE);
	switch (kind) {
	case PK_CHECKOUT:
		gitops_checkout(ops, bname, remote);
		break;
	case PK_BRANCH_FROM:
		snprintf(pending_ref, sizeof pending_ref, "%s", bname);
		prompt(PR_BRANCH_FROM_NAME, "Branch name (from the selected ref)", "");
		break;
	case PK_DELETE_BRANCH:
		snprintf(pending_ref, sizeof pending_ref, "%s", bname);
		gitops_branch_delete(ops, bname, false);
		break;
	case PK_MERGE:
		gitops_merge(ops, bname);
		break;
	case PK_REBASE:
		gitops_rebase(ops, bname);
		break;
	case PK_STASH_APPLY:
		gitops_stash_apply(ops, v);
		break;
	case PK_STASH_POP:
		gitops_stash_pop(ops, v);
		break;
	case PK_STASH_DROP:
		gitops_stash_drop(ops, v);
		break;
	case PK_DELETE_TAG:
		if (v < names.n)
			gitops_tag_delete(ops, names.pool.b + names.name[v]);
		break;
	case PK_PUBLISH_REMOTE:
		if (v < names.n)
			gitops_publish(ops, names.pool.b + names.name[v], current_branch());
		break;
	}
}

/* Prompts with kind >= 100 come back here (ui.c quick_accept). */
void gitui_prompt_accept(int kind, const char *text)
{
	if (!need_ops())
		return;
	switch (kind) {
	case PR_CREATE_BRANCH:
		if (*text)
			gitops_branch_create(ops, text, NULL, true);
		break;
	case PR_BRANCH_FROM_NAME:
	case PR_GRAPH_BRANCH:
		if (*text)
			gitops_branch_create(ops, text, pending_ref, true);
		break;
	case PR_RENAME_BRANCH:
		if (*text)
			gitops_branch_rename(ops, text);
		break;
	case PR_STASH:
	case PR_STASH_UNTRACKED:
		gitops_stash_push(ops, kind == PR_STASH_UNTRACKED, *text ? text : NULL);
		break;
	case PR_TAG:
		if (*text)
			gitops_tag_create(ops, text, NULL, pending_ref[0] ? pending_ref : NULL);
		break;
	case PR_COMMIT_ALL:
		if (*text)
			gitops_commit(ops, text, false, true, false);
		break;
	case PR_CONFIRM_DISCARD_ALL: {
		const char *argv[] = {"restore", "--worktree", "--", ".", NULL};
		gitops_run(ops, "Discarding changes...", argv, NULL);
		break;
	}
	case PR_CONFIRM_DISCARD_FILE:
		git_discard(app.git, pending_ref);
		app.git_ns = 0;
		break;
	case PR_CONFIRM_FORCE_PUSH:
		gitops_push(ops, true);
		break;
	case PR_CONFIRM_DELETE_BRANCH_FORCE:
		gitops_branch_delete(ops, pending_ref, true);
		break;
	}
}

/* ---- read-only diff documents --------------------------------------------------- */

static void show_text_doc(const char *name, char *text)
{
	if (!text)
		return;
	u32 d = doc_new_virtual(name, text, strlen(text), LANG_DIFF);
	free(text);
	if (d == UINT32_MAX)
		return;
	extern void show_doc_public(u32 d);
	show_doc_public(d);
}


static void open_commit(const char *hash)
{
	if (!app.git)
		return;
	char *t = git_show_commit(git_workdir(app.git), hash); /* libgit2, like git show --stat --patch */
	if (!t) {
		ui_status("Git: cannot show %.8s", hash);
		return;
	}
	char name[64];
	snprintf(name, sizeof name, "%.8s (commit)", hash);
	show_text_doc(name, t);
}

/* ---- commands --------------------------------------------------------------------- */

static const char *graph_ctx_hash(void)
{
	return app.ctx == CTX_GRAPH && app.ctx_a < graph.n ? graph.hash[app.ctx_a] : NULL;
}

bool gitui_command(int cmd)
{
	switch (cmd) {
	case CMD_SCM_OPEN_CHANGES:
		if (app.git && app.ctx == CTX_SCM_FILE) {
			if (app.ctx_a)
				diff_open_staged(app.ctx_path);
			else
				diff_open_worktree(app.ctx_path);
		} else if (app.git) {
			/* palette / gutter: changes of the active file */
			View *v = active_view();
			const char *rel = v ? (v->diff_left != UINT32_MAX ? v->diff_rel : doc_repo_rel(v->doc)) : NULL;
			if (rel) {
				char r2[1024];
				snprintf(r2, sizeof r2, "%s", rel);
				diff_open_worktree(r2);
			}
		}
		app.ctx = CTX_NONE;
		return true;
	case CMD_SCM_OPEN_FILE:
		if (app.git) {
			char p[8192];
			snprintf(p, sizeof p, "%s%s", git_workdir(app.git), app.ctx_path);
			open_file(p, false, UINT32_MAX, 0);
		}
		return true;
	case CMD_SCM_STAGE:
		if (app.git) {
			git_stage(app.git, app.ctx_path);
			scm_refresh();
		}
		return true;
	case CMD_SCM_UNSTAGE:
		if (app.git) {
			git_unstage(app.git, app.ctx_path);
			scm_refresh();
		}
		return true;
	case CMD_SCM_DISCARD: {
		snprintf(pending_ref, sizeof pending_ref, "%s", app.ctx_path);
		char t[600];
		snprintf(t, sizeof t, "Are you sure you want to discard changes in '%s'?", app.ctx_path);
		confirm(PR_CONFIRM_DISCARD_FILE, t);
		return true;
	}
	case CMD_SCM_MENU:
		ctx_menu_scm_title(app.side_x + app.side_w - PX(30), PX(30));
		return true;
	case CMD_GIT_SHOW_OUTPUT:
		run_command(CMD_VIEW_OUTPUT);
		return true;
	default:
		break;
	}
	if (cmd < CMD_GIT_UNSTAGE_ALL || cmd > CMD_GRAPH_OPEN_COMMIT)
		return false;
	if (!need_ops())
		return true;
	const char *h = graph_ctx_hash();
	switch (cmd) {
	case CMD_GIT_UNSTAGE_ALL: {
		const char *argv[] = {"reset", "-q", NULL};
		gitops_run(ops, "Unstaging...", argv, NULL);
		break;
	}
	case CMD_GIT_DISCARD_ALL:
		confirm(PR_CONFIRM_DISCARD_ALL, "Are you sure you want to discard ALL changes in tracked files?");
		break;
	case CMD_GIT_COMMIT_ALL:
		if (app.commit_in.n) {
			char *m = vx_strndup(app.commit_in.b, app.commit_in.n);
			gitops_commit(ops, m, false, true, false);
			free(m);
			input_set(&app.commit_in, "", 0);
		} else {
			prompt(PR_COMMIT_ALL, "Commit message (all tracked changes)", "");
		}
		break;
	case CMD_GIT_COMMIT_AMEND:
		if (app.commit_in.n) {
			char *m = vx_strndup(app.commit_in.b, app.commit_in.n);
			gitops_commit(ops, m, true, false, false);
			free(m);
			input_set(&app.commit_in, "", 0);
		} else {
			gitops_commit_amend_no_edit(ops);
		}
		break;
	case CMD_GIT_UNDO_COMMIT:
		gitops_undo_last_commit(ops);
		break;
	case CMD_GIT_FETCH:
		gitops_fetch(ops, false, false);
		break;
	case CMD_GIT_FETCH_PRUNE:
		gitops_fetch(ops, false, true);
		break;
	case CMD_GIT_PULL:
		gitops_pull(ops, false);
		break;
	case CMD_GIT_PULL_REBASE:
		gitops_pull(ops, true);
		break;
	case CMD_GIT_PUSH:
		gitops_push(ops, false);
		break;
	case CMD_GIT_PUSH_FORCE:
		confirm(PR_CONFIRM_FORCE_PUSH, "Are you sure you want to force push (with lease)?");
		break;
	case CMD_GIT_SYNC:
		gitops_pull(ops, false);
		gitops_push(ops, false);
		break;
	case CMD_GIT_PUBLISH:
		gitops_names_free(&names);
		gitops_remotes(ops, &names);
		if (names.n == 1)
			gitops_publish(ops, names.pool.b + names.name[0], current_branch());
		else
			pick_names(PK_PUBLISH_REMOTE, "Pick a remote to publish the branch to", true);
		break;
	case CMD_GIT_CHECKOUT:
		pick_branches(PK_CHECKOUT, "Select a ref to checkout", false, true);
		break;
	case CMD_GIT_CREATE_BRANCH:
		prompt(PR_CREATE_BRANCH, "Please provide a new branch name", "");
		break;
	case CMD_GIT_CREATE_BRANCH_FROM:
		pick_branches(PK_BRANCH_FROM, "Select a ref to create the branch from", false, false);
		break;
	case CMD_GIT_RENAME_BRANCH:
		prompt(PR_RENAME_BRANCH, "Please provide a new branch name", current_branch());
		break;
	case CMD_GIT_DELETE_BRANCH:
		pick_branches(PK_DELETE_BRANCH, "Select a branch to delete", true, true);
		break;
	case CMD_GIT_MERGE:
		pick_branches(PK_MERGE, "Select a branch to merge from", false, true);
		break;
	case CMD_GIT_REBASE:
		pick_branches(PK_REBASE, "Select a branch to rebase onto", false, true);
		break;
	case CMD_GIT_REBASE_CONTINUE: {
		/* continue whatever is in progress */
		u32 st = gitops_state(ops);
		if (st & GST_MERGE)
			gitops_commit(ops, "", false, false, false);
		else
			gitops_rebase_continue(ops);
		break;
	}
	case CMD_GIT_REBASE_ABORT:
		gitops_rebase_abort(ops);
		break;
	case CMD_GIT_MERGE_ABORT:
		gitops_merge_abort(ops);
		break;
	case CMD_GIT_STASH:
		prompt(PR_STASH, "Stash message (optional)", "");
		break;
	case CMD_GIT_STASH_UNTRACKED:
		prompt(PR_STASH_UNTRACKED, "Stash message (optional, includes untracked files)", "");
		break;
	case CMD_GIT_STASH_APPLY_LATEST:
		gitops_stash_apply(ops, 0);
		break;
	case CMD_GIT_STASH_POP_LATEST:
		gitops_stash_pop(ops, 0);
		break;
	case CMD_GIT_STASH_APPLY:
		pick_stashes(PK_STASH_APPLY, "Pick a stash to apply");
		break;
	case CMD_GIT_STASH_POP:
		pick_stashes(PK_STASH_POP, "Pick a stash to pop");
		break;
	case CMD_GIT_STASH_DROP:
		pick_stashes(PK_STASH_DROP, "Pick a stash to drop");
		break;
	case CMD_GIT_CREATE_TAG:
		pending_ref[0] = 0;
		if (h)
			snprintf(pending_ref, sizeof pending_ref, "%s", h);
		prompt(PR_TAG, "Please provide a tag name", "");
		break;
	case CMD_GIT_DELETE_TAG:
		pick_names(PK_DELETE_TAG, "Select a tag to delete", false);
		break;
	case CMD_GRAPH_CHECKOUT:
		if (h) {
			const char *argv[] = {"checkout", "--detach", h, NULL};
			gitops_run(ops, "Checking out...", argv, NULL);
		}
		break;
	case CMD_GRAPH_CREATE_BRANCH:
		if (h) {
			snprintf(pending_ref, sizeof pending_ref, "%s", h);
			prompt(PR_GRAPH_BRANCH, "Please provide a new branch name", "");
		}
		break;
	case CMD_GRAPH_CHERRY_PICK:
		if (h)
			gitops_cherry_pick(ops, h);
		break;
	case CMD_GRAPH_COPY_ID:
		if (h)
			ui_clipboard_set(h, strlen(h));
		break;
	case CMD_GRAPH_COPY_MESSAGE:
		if (h) {
			GitCommitInfo ci;
			if (git_commit_details(git_workdir(app.git), h, &ci) == 0) {
				const char *m = ci.pool.b + ci.message;
				ui_clipboard_set(m, strlen(m));
				git_commit_info_free(&ci);
			}
		}
		break;
	case CMD_GRAPH_OPEN_COMMIT:
		if (h)
			open_commit(h);
		break;
	}
	app.ctx = CTX_NONE;
	return true;
}

/* ---- menus -------------------------------------------------------------------------- */

enum { SUB_COMMIT = 1, SUB_CHANGES, SUB_PULLPUSH, SUB_BRANCH, SUB_STASH, SUB_TAGS };

void ctx_menu_scm_title(int x, int y)
{
	app.ctx = CTX_SCM_TITLE;
	u32 st = need_ops() ? gitops_state(ops) : 0;
	menu_open(x, y);
	menu_item("Pull", CMD_GIT_PULL, true);
	menu_item("Push", CMD_GIT_PUSH, true);
	menu_item("Fetch", CMD_GIT_FETCH, true);
	menu_item("Checkout to...", CMD_GIT_CHECKOUT, true);
	menu_sep();
	menu_sub("Commit", SUB_COMMIT);
	menu_sub("Changes", SUB_CHANGES);
	menu_sub("Pull, Push", SUB_PULLPUSH);
	menu_sub("Branch", SUB_BRANCH);
	menu_sub("Stash", SUB_STASH);
	menu_sub("Tags", SUB_TAGS);
	if (st & GST_REBASE) {
		menu_sep();
		menu_item("Continue Rebase", CMD_GIT_REBASE_CONTINUE, true);
		menu_item("Abort Rebase", CMD_GIT_REBASE_ABORT, true);
	}
	if (st & GST_MERGE) {
		menu_sep();
		menu_item("Abort Merge", CMD_GIT_MERGE_ABORT, true);
	}
	menu_sep();
	menu_item("Show Git Output", CMD_GIT_SHOW_OUTPUT, true);
}

void menu_fill_sub(int sub)
{
	switch (sub) {
	case SUB_COMMIT:
		menu_item("Commit", CMD_GIT_COMMIT, true);
		menu_item("Commit Staged", CMD_GIT_COMMIT, true);
		menu_item("Commit All", CMD_GIT_COMMIT_ALL, true);
		menu_item("Undo Last Commit", CMD_GIT_UNDO_COMMIT, true);
		menu_sep();
		menu_item("Commit Staged (Amend)", CMD_GIT_COMMIT_AMEND, true);
		break;
	case SUB_CHANGES:
		menu_item("Stage All Changes", CMD_GIT_STAGE_ALL, true);
		menu_item("Unstage All Changes", CMD_GIT_UNSTAGE_ALL, true);
		menu_item("Discard All Changes", CMD_GIT_DISCARD_ALL, true);
		break;
	case SUB_PULLPUSH:
		menu_item("Sync", CMD_GIT_SYNC, true);
		menu_sep();
		menu_item("Pull", CMD_GIT_PULL, true);
		menu_item("Pull (Rebase)", CMD_GIT_PULL_REBASE, true);
		menu_sep();
		menu_item("Push", CMD_GIT_PUSH, true);
		menu_item("Push (Force With Lease)", CMD_GIT_PUSH_FORCE, true);
		menu_sep();
		menu_item("Fetch", CMD_GIT_FETCH, true);
		menu_item("Fetch (Prune)", CMD_GIT_FETCH_PRUNE, true);
		break;
	case SUB_BRANCH:
		menu_item("Merge...", CMD_GIT_MERGE, true);
		menu_item("Rebase Branch...", CMD_GIT_REBASE, true);
		menu_sep();
		menu_item("Create Branch...", CMD_GIT_CREATE_BRANCH, true);
		menu_item("Create Branch From...", CMD_GIT_CREATE_BRANCH_FROM, true);
		menu_sep();
		menu_item("Rename Branch...", CMD_GIT_RENAME_BRANCH, true);
		menu_item("Delete Branch...", CMD_GIT_DELETE_BRANCH, true);
		menu_sep();
		menu_item("Publish Branch...", CMD_GIT_PUBLISH, true);
		break;
	case SUB_STASH:
		menu_item("Stash", CMD_GIT_STASH, true);
		menu_item("Stash (Include Untracked)", CMD_GIT_STASH_UNTRACKED, true);
		menu_sep();
		menu_item("Apply Latest Stash", CMD_GIT_STASH_APPLY_LATEST, true);
		menu_item("Apply Stash...", CMD_GIT_STASH_APPLY, true);
		menu_sep();
		menu_item("Pop Latest Stash", CMD_GIT_STASH_POP_LATEST, true);
		menu_item("Pop Stash...", CMD_GIT_STASH_POP, true);
		menu_sep();
		menu_item("Drop Stash...", CMD_GIT_STASH_DROP, true);
		break;
	case SUB_TAGS:
		menu_item("Create Tag", CMD_GIT_CREATE_TAG, true);
		menu_item("Delete Tag...", CMD_GIT_DELETE_TAG, true);
		break;
	}
}

/* ---- op results ---------------------------------------------------------------------- */

static u64 busy_since;

void gitui_tick(void)
{
	if (!ops)
		return;
	GitOpResult r;
	bool any = false;
	while (gitops_poll(ops, &r)) {
		any = true;
		ui_log("[git] %s (exit %d, %.0f ms)", r.label ? r.label : "", r.status, (double)r.ns / 1e6);
		if (r.out && *r.out)
			ui_log("%s", r.out);
		if (r.err && *r.err)
			ui_log("%s", r.err);
		if (r.status != 0) {
			const char *e = r.err && *r.err ? r.err : (r.out ? r.out : "");
			char first[300];
			snprintf(first, sizeof first, "%.*s", (int)strcspn(e, "\n"), e);
			/* a branch that is not merged: offer to force delete, like VSCodium */
			if (strstr(e, "not fully merged") && pending_ref[0]) {
				char t[400];
				snprintf(t, sizeof t, "The branch '%s' is not fully merged. Delete anyway?", pending_ref);
				confirm(PR_CONFIRM_DELETE_BRANCH_FORCE, t);
			} else {
				ui_status("Git: %s", first[0] ? first : "command failed (see Output)");
			}
		} else {
			ui_status("Git: %s done", r.label ? r.label : "");
		}
		gitops_result_free(&r);
	}
	char label[128];
	if (gitops_busy(ops, label, sizeof label)) {
		if (!busy_since)
			busy_since = vx_now_ns();
		snprintf(app.status_msg, sizeof app.status_msg, "\xE2\x86\xBB %s", label);
		app.status_until = vx_now_ns() + 1000000000ull;
		app.dirty = true;
	} else {
		busy_since = 0;
	}
	if (any) {
		/* refresh everything that depends on the repository */
		scm_refresh();
		fs_refresh(&app.tree);
		app.files_ok = false;
		for (u32 d = 0; d < app.docs.n; d++) {
			/* reload clean buffers whose files changed (checkout, stash, rebase) */
			if (!app.docs.path[d] || text_dirty(&app.docs.text[d]))
				continue;
			if (text_changed_on_disk(&app.docs.text[d], app.docs.path[d])) {
				size_t n = 0;
				char *s = vx_read_file(app.docs.path[d], &n);
				if (s) {
					Text *t = &app.docs.text[d];
					doc_replace(d, 0, t->len, s, (u32)n, 0);
					t->saved_upos = t->upos;
					free(s);
					/* re-stat so the next check is clean */
					text_restat(t, app.docs.path[d]);
				}
			}
		}
	}
}

/* One line under the Commit button: in-progress operations. */
void gitui_status_line(int x, int y, int w)
{
	if (!ops)
		return;
	static u64 last;
	static u32 st;
	u64 now = vx_now_ns();
	if (now - last > 1000000000ull) {
		st = gitops_state(ops);
		last = now;
	}
	const char *m = (st & GST_REBASE)         ? "Rebase in progress: resolve, stage, then ... > Continue Rebase"
	                : (st & GST_MERGE)        ? "Merge in progress: resolve conflicts, stage, then commit"
	                : (st & GST_CHERRY_PICK) ? "Cherry-pick in progress"
	                                          : NULL;
	if (m)
		draw_text_fit(FONT_SMALL, x + PX(14), y - PX(6), m, strlen(m), w - PX(20), COL(C_WARNING));
}

/* ---- graph section ------------------------------------------------------------------- */

#define LANE_W PX(11)

static void graph_load(void)
{
	if (graph_ok && graph_gen == app.blame_gen)
		return;
	git_graph_free(&graph);
	graph_ok = git_graph_load(&graph, git_workdir(app.git), 600) == 0;
	graph_gen = app.blame_gen;
	expanded = UINT32_MAX;
	if (exp_ok)
		git_commit_info_free(&exp_ci);
	exp_ok = false;
}

int gitui_graph_height(int h)
{
	if (!app.git)
		return 0;
	return graph_open ? VX_MAX(h * 45 / 100, PX(22)) : PX(22);
}

static u32 lane_color(u8 c)
{
	static const int ids[5] = {C_GRAPH_1, C_GRAPH_2, C_GRAPH_3, C_GRAPH_4, C_GRAPH_5};
	return COL(ids[c % 5]);
}

/* Rows of the list: commits, with the expanded commit's files after it. */
static u32 row_count(void)
{
	return graph.n + (exp_ok && expanded < graph.n ? exp_ci.nfiles : 0);
}

/* row -> commit index, or file index (with the high bit) */
static u32 row_item(u32 r)
{
	if (exp_ok && expanded < graph.n && r > expanded) {
		if (r <= expanded + exp_ci.nfiles)
			return 0x80000000u | (r - expanded - 1);
		return r - exp_ci.nfiles;
	}
	return r;
}

static void draw_refs(const char *refs, int *tx, int ty, int rh, int maxx)
{
	/* "HEAD -> main, origin/main, tag: v1" -> square badges */
	char buf[512];
	snprintf(buf, sizeof buf, "%s", refs);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ')
			tok++;
		if (!strncmp(tok, "HEAD -> ", 8))
			tok += 8;
		if (!strcmp(tok, "HEAD") || !*tok)
			continue;
		bool tag = !strncmp(tok, "tag: ", 5);
		if (tag)
			tok += 5;
		bool remote = !tag && strchr(tok, '/') && !strncmp(tok, "origin/", 7);
		u32 bg = tag ? COL(C_GRAPH_BASE_REF) : remote ? COL(C_GRAPH_REMOTE_REF) : COL(C_GRAPH_REF);
		int tw = font_width(FONT_SMALL, tok, strlen(tok));
		int bw = tw + PX(10);
		if (*tx + bw > maxx)
			break;
		int by = ty + (rh - PX(16)) / 2;
		draw_rect(*tx, by, bw, PX(16), bg);
		draw_text(FONT_SMALL, *tx + PX(5), by + (PX(16) - font_m[FONT_SMALL].line) / 2, tok, strlen(tok), 0xFFFFFFFFu);
		*tx += bw + PX(4);
	}
}

void gitui_graph_draw(int x, int y, int w, int h)
{
	int hh = PX(22);
	draw_clip(x, y, w, h);
	draw_rect(x, y, w, PX(1), COL(C_SECTION_HEADER_BORDER));
	draw_icon(graph_open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + PX(2), y + (hh - PX(16)) / 2,
	          COL(C_SECTION_HEADER_FG));
	draw_text(FONT_SMALL_BOLD, x + PX(20), y + (hh - font_m[FONT_SMALL_BOLD].line) / 2, "GRAPH", 5,
	          COL(C_SECTION_HEADER_FG));
	draw_icon(ICON_REFRESH, x + w - PX(26), y + (hh - PX(16)) / 2, COL(C_SECTION_HEADER_FG));
	if (!graph_open) {
		draw_noclip();
		return;
	}
	graph_load();
	int cy = y + hh, list_h = h - hh;
	int rh = app.row_h;
	u32 nrows = row_count();
	int maxs = VX_MAX((int)nrows * rh - list_h, 0);
	graph_scroll = VX_CLAMP(graph_scroll, 0, maxs);
	draw_clip(x, cy, w, list_h);
	int gx = x + PX(8);
	int graph_w = (int)VX_MIN(graph.lanes, 12) * LANE_W + PX(6);
	bool focused = app.focus == FOCUS_SIDEBAR;
	i64 now = (i64)time(NULL);
	for (u32 r = (u32)(graph_scroll / rh); r < nrows; r++) {
		int ry = cy + (int)r * rh - graph_scroll;
		if (ry > y + h)
			break;
		u32 it = row_item(r);
		bool hover = app.hover_x >= x && app.hover_x < x + w && app.hover_y >= ry && app.hover_y < ry + rh &&
		             app.hover_y >= cy;
		if ((r == graph_sel && focused) || hover)
			draw_rect(x, ry, w, rh, (r == graph_sel && focused) ? COL(C_LIST_ACTIVE_BG) : COL(C_LIST_HOVER_BG));
		int ty = ry + (rh - font_m[FONT_UI].line) / 2;
		if (it & 0x80000000u) {
			/* a file of the expanded commit; the lane of that commit keeps going */
			u32 f = it & 0x7FFFFFFFu;
			u32 c = expanded;
			for (u32 e = 0; e < graph.edge_count[c]; e++) {
				u32 k = graph.edge_first[c] + e;
				if (graph.e_half[k] == 1) /* lower half: lanes continuing below */
					draw_rect(gx + graph.e_x1[k] * LANE_W + LANE_W / 2 - PX(1), ry, PX(2), rh,
					          lane_color(graph.e_color[k]));
			}
			const char *p = exp_ci.pool.b + exp_ci.path[f];
			const char *sl = strrchr(p, '/');
			const char *base = sl ? sl + 1 : p;
			int tx = file_icon_draw(base, gx + graph_w + PX(8), ry, rh);
			char st = exp_ci.status[f];
			u32 sc = st == 'A' ? COL(C_GIT_ADDED) : st == 'D' ? COL(C_GIT_DELETED) : COL(C_GIT_MODIFIED);
			tx = draw_text(FONT_UI, tx, ty, base, strlen(base), sc);
			if (sl)
				tx = draw_text_fit(FONT_UI, tx + PX(6), ty, p, (size_t)(sl - p), x + w - PX(40) - tx, COL(C_DESCRIPTION_FG));
			char lb[2] = {st, 0};
			draw_text(FONT_UI, x + w - PX(20), ty, lb, 1, sc);
			continue;
		}
		u32 c = it;
		/* lanes: upper half (top -> dot), lower half (dot -> bottom), square lines */
		int mid = ry + rh / 2;
		for (u32 e = 0; e < graph.edge_count[c]; e++) {
			u32 k = graph.edge_first[c] + e;
			int x0 = gx + graph.e_x0[k] * LANE_W + LANE_W / 2, x1 = gx + graph.e_x1[k] * LANE_W + LANE_W / 2;
			u32 col = lane_color(graph.e_color[k]);
			if (graph.e_half[k] == 0) {
				draw_rect(x0 - PX(1), ry, PX(2), mid - ry, col);
				if (x0 != x1)
					draw_rect(VX_MIN(x0, x1) - PX(1), mid - PX(1), abs(x1 - x0) + PX(2), PX(2), col);
			} else {
				if (x0 != x1)
					draw_rect(VX_MIN(x0, x1) - PX(1), mid - PX(1), abs(x1 - x0) + PX(2), PX(2), col);
				draw_rect(x1 - PX(1), mid, PX(2), ry + rh - mid, col);
			}
		}
		/* the commit marker: a square (filled, ringed for merges, hollow for HEAD) */
		int dx = gx + graph.lane[c] * LANE_W + LANE_W / 2;
		u32 dc = lane_color(graph.color[c]);
		int s = PX(8);
		if (graph.flags[c] & GG_HEAD) {
			draw_rect(dx - s / 2 - PX(1), mid - s / 2 - PX(1), s + PX(2), s + PX(2), dc);
			draw_rect(dx - s / 2 + PX(1), mid - s / 2 + PX(1), s - PX(2), s - PX(2), COL(C_SIDEBAR_BG));
		} else if (graph.flags[c] & GG_MERGE) {
			draw_rect(dx - s / 2 - PX(1), mid - s / 2 - PX(1), s + PX(2), s + PX(2), dc);
			draw_rect(dx - s / 2 + PX(1), mid - s / 2 + PX(1), s - PX(2), s - PX(2), COL(C_SIDEBAR_BG));
			draw_rect(dx - PX(2), mid - PX(2), PX(4), PX(4), dc);
		} else {
			draw_rect(dx - s / 2, mid - s / 2, s, s, dc);
		}
		int tx = gx + VX_MAX(graph_w, (int)(graph.lane[c] + 1) * LANE_W + PX(6));
		const char *subj = graph.pool.b + graph.subject[c];
		const char *auth = graph.pool.b + graph.author[c];
		int right = x + w - PX(8);
		tx = draw_text_fit(FONT_UI, tx, ty, subj, strlen(subj), VX_MAX(right - tx - PX(60), PX(40)), COL(C_SIDEBAR_FG));
		tx += PX(8);
		const char *refs = graph.pool.b + graph.refs[c];
		if (*refs)
			draw_refs(refs, &tx, ry, rh, right - PX(40));
		if (tx < right - PX(30))
			draw_text_fit(FONT_UI, tx, ty, auth, strlen(auth), right - tx, COL(C_DESCRIPTION_FG));
		(void)now;
	}
	draw_noclip();
}

static void expand(u32 c)
{
	if (exp_ok)
		git_commit_info_free(&exp_ci);
	exp_ok = false;
	if (expanded == c) {
		expanded = UINT32_MAX;
		return;
	}
	expanded = c;
	exp_ok = git_commit_details(git_workdir(app.git), graph.hash[c], &exp_ci) == 0;
}

static u32 row_at(int y, int my)
{
	int cy = y + PX(22);
	if (my < cy)
		return UINT32_MAX;
	u32 r = (u32)((my - cy + graph_scroll) / app.row_h);
	return r < row_count() ? r : UINT32_MAX;
}

bool gitui_graph_event(const Event *e, int x, int y, int w, int h)
{
	if (!app.git)
		return false;
	if (e->type == EV_KEY)
		return false;
	if (graph_open)
		graph_load(); /* events can arrive before the first draw */
	if (e->type == EV_WHEEL) {
		graph_scroll -= (int)(e->wy * app.row_h * 3);
		app.dirty = true;
		return true;
	}
	if (e->type == EV_MMOVE) {
		u32 r = row_at(y, e->y);
		u32 c = r == UINT32_MAX ? UINT32_MAX : row_item(r);
		if (c & 0x80000000u)
			c = UINT32_MAX;
		if (c != hover_row) {
			hover_row = c;
			hover_t0 = vx_now_ns();
		}
		app.dirty = true;
		return true;
	}
	if (e->type != EV_MDOWN || e->button != 1)
		return true;
	if (e->y < y + PX(22)) {
		if (e->x >= x + w - PX(30)) {
			graph_ok = false;
			graph_load();
		} else {
			graph_open = !graph_open;
		}
		app.dirty = true;
		return true;
	}
	u32 r = row_at(y, e->y);
	if (r == UINT32_MAX)
		return true;
	graph_sel = r;
	app.focus = FOCUS_SIDEBAR;
	u32 it = row_item(r);
	hover_row = UINT32_MAX;
	if (it & 0x80000000u) {
		u32 f = it & 0x7FFFFFFFu;
		const char *p = exp_ci.pool.b + exp_ci.path[f];
		diff_open_commit(graph.hash[expanded], p); /* side by side, like VSCodium */
		return true;
	}
	if (e->clicks >= 2) {
		app.ctx = CTX_GRAPH;
		app.ctx_a = it;
		gitui_command(CMD_GRAPH_OPEN_COMMIT);
		return true;
	}
	expand(it);
	(void)h;
	return true;
}

bool gitui_graph_context(int x, int y, int w, int h, int mx, int my)
{
	(void)x;
	(void)w;
	(void)h;
	if (graph_open)
		graph_load();
	u32 r = row_at(y, my);
	if (r == UINT32_MAX)
		return false;
	u32 c = row_item(r);
	if (c & 0x80000000u)
		return false;
	graph_sel = r;
	app.ctx = CTX_GRAPH;
	app.ctx_a = c;
	menu_open(mx, my);
	menu_item("Open Changes", CMD_GRAPH_OPEN_COMMIT, true);
	menu_sep();
	menu_item("Checkout (Detached)", CMD_GRAPH_CHECKOUT, true);
	menu_item("Create Branch...", CMD_GRAPH_CREATE_BRANCH, true);
	menu_item("Create Tag...", CMD_GIT_CREATE_TAG, true);
	menu_sep();
	menu_item("Cherry Pick", CMD_GRAPH_CHERRY_PICK, true);
	menu_sep();
	menu_item("Copy Commit ID", CMD_GRAPH_COPY_ID, true);
	menu_item("Copy Commit Message", CMD_GRAPH_COPY_MESSAGE, true);
	return true;
}

/* ---- hover card (graph rows and inline blame) ---------------------------------------- */

static bool card_load(const char *hash)
{
	if (card_ok && !strncmp(card_hash, hash, strlen(hash)))
		return true;
	if (card_ok)
		git_commit_info_free(&card_ci);
	card_ok = git_commit_details(git_workdir(app.git), hash, &card_ci) == 0;
	snprintf(card_hash, sizeof card_hash, "%s", hash);
	return card_ok;
}

static int wrap_lines(const char *s, size_t n, int w, int font, int x, int *y, bool draw_it, u32 col)
{
	int lines = 0;
	size_t i = 0;
	while (i < n) {
		size_t e = i;
		int pen = 0;
		size_t last_sp = (size_t)-1;
		while (e < n && s[e] != '\n') {
			u32 cp;
			u32 k = utf8_decode(s + e, n - e, &cp);
			int a = font_glyphs.adv[font_glyph(font, cp)];
			if (pen + a > w && e > i)
				break;
			if (s[e] == ' ')
				last_sp = e;
			pen += a;
			e += k;
		}
		if (e < n && s[e] != '\n' && last_sp != (size_t)-1 && last_sp > i)
			e = last_sp + 1;
		if (draw_it)
			draw_text(font, x, *y, s + i, e - i, col);
		*y += font_m[font].line;
		lines++;
		i = e < n && s[e] == '\n' ? e + 1 : e;
	}
	return lines;
}

/* VSCodium's blame / graph hover: author, when, message, hash and stats. */
static void draw_card(const char *hash, int ax, int ay)
{
	if (!card_load(hash))
		return;
	draw_noclip(); /* popups draw over everything */
	GitCommitInfo *ci = &card_ci;
	int pw = PX(460), pad = PX(10);
	char head[400], when[64], date[64], foot[200];
	blame_ago(ci->author_time, (i64)time(NULL), when, sizeof when);
	time_t tt = (time_t)ci->author_time;
	struct tm tmv;
	localtime_r(&tt, &tmv);
	strftime(date, sizeof date, "%B %e, %Y at %H:%M", &tmv);
	snprintf(head, sizeof head, "%s, %s (%s)", ci->pool.b + ci->author, when, date);
	snprintf(foot, sizeof foot, "%.8s  \xC2\xB7  %u file%s changed, %u insertion%s(+), %u deletion%s(-)", ci->hash,
	         ci->nfiles, ci->nfiles == 1 ? "" : "s", ci->total_ins, ci->total_ins == 1 ? "" : "s", ci->total_del,
	         ci->total_del == 1 ? "" : "s");
	const char *msg = ci->pool.b + ci->message;
	size_t mn = strlen(msg);
	while (mn && (msg[mn - 1] == '\n' || msg[mn - 1] == ' '))
		mn--;
	/* measure */
	int y = 0;
	wrap_lines(head, strlen(head), pw - 2 * pad, FONT_UI_BOLD, 0, &y, false, 0);
	y += PX(8);
	wrap_lines(msg, VX_MIN(mn, (size_t)2000), pw - 2 * pad, FONT_UI, 0, &y, false, 0);
	y += PX(9);
	y += font_m[FONT_UI].line;
	int ph = y + 2 * pad;
	int px = VX_MIN(ax, app.w - pw - PX(4));
	int py = ay - ph - PX(4);
	if (py < 0)
		py = ay + app.line_h + PX(4);
	draw_rect(px - PX(1), py, pw + PX(2), ph + PX(3), COL(C_WIDGET_SHADOW));
	draw_rect(px, py, pw, ph, COL(C_WIDGET_BG));
	draw_frame(px, py, pw, ph, COL(C_WIDGET_BORDER));
	draw_clip(px, py, pw, ph);
	y = py + pad;
	wrap_lines(head, strlen(head), pw - 2 * pad, FONT_UI_BOLD, px + pad, &y, true, COL(C_FG));
	y += PX(8);
	wrap_lines(msg, VX_MIN(mn, (size_t)2000), pw - 2 * pad, FONT_UI, px + pad, &y, true, COL(C_FG));
	y += PX(4);
	draw_rect(px + pad, y, pw - 2 * pad, PX(1), COL(C_WIDGET_BORDER));
	y += PX(5);
	draw_text_fit(FONT_UI, px + pad, y, foot, strlen(foot), pw - 2 * pad, COL(C_LINK));
	draw_noclip();
}

void gitui_draw_popups(void)
{
	if (!app.git)
		return;
	u64 now = vx_now_ns();
	if (app.side_visible && app.side_view == SIDE_SCM && hover_row < graph.n && now - hover_t0 > 500000000ull &&
	    !menu_is_open() && !app.quick)
		draw_card(graph.hash[hover_row], app.hover_x + PX(12), app.hover_y);
	if (app.blame_hover && now - app.blame_hover_t > 300000000ull && !menu_is_open() && !app.quick)
		draw_card(app.blame_hover_hash, app.blame_hover_x, app.blame_hover_y);
}

bool gitui_wants_redraw(void)
{
	u64 now = vx_now_ns();
	return (hover_row != UINT32_MAX && now - hover_t0 < 700000000ull) ||
	       (app.blame_hover && now - app.blame_hover_t < 500000000ull) || (ops && gitops_busy(ops, NULL, 0));
}

void gitui_shutdown(void)
{
	if (ops)
		gitops_stop(ops);
	ops = NULL;
	git_graph_free(&graph);
	if (exp_ok)
		git_commit_info_free(&exp_ci);
	if (card_ok)
		git_commit_info_free(&card_ci);
	gitops_branches_free(&branches);
	gitops_stashes_free(&stashes);
	gitops_names_free(&names);
}
