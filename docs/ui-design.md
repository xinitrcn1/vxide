# vxide UI/UX design

vxide looks and behaves like VSCodium on purpose. People who use it
already know VSCodium, and anything that works differently without a good
reason costs them effort. This document says what is kept, what is changed
and why. It also lists the rules the code follows, so new UI fits in with
the rest.

## 1. Stance: familiar first, then better

**Jakob's law**: users spend most of their time in other tools, so they
expect this one to work the same way. vxide therefore keeps:

- **Keys.** VSCodium's Linux defaults: Ctrl+P, Ctrl+Shift+P, Ctrl+K
  chords, F12, Alt+Up/Down, and so on.
- **Colours.** Dark Modern / Light Modern workbench colours and Dark+ /
  Light+ token colours, taken from VSCodium's own theme files.
- **Words.** Command titles ("View: Close Editor"), the save prompt,
  placeholders, empty-state hints.
- **Layout.** Explorer on the left, editor groups with tabs and
  breadcrumbs, a bottom panel and a status bar.

vxide departs from VSCodium only when doing so removes friction the user
actually runs into. Each departure is listed in §6, with what it fixes.

## 2. Principles used, and where

| Principle | Meaning here | Examples |
|---|---|---|
| Visibility of system status (Nielsen #1) | The user always knows what happened and what is running | "Disassembling...", "starting" next to a language server, git badge, "1 of 5" in find, status messages after every toggle |
| Match with the real world | Use the user's words, not the program's | "Don't Save" rather than "Discard buffer"; `socket(2)` rather than a file path |
| User control and freedom | Every action can be cancelled or undone | Escape closes every popup and dialog (Cancel); the theme picker previews live and Escape restores the old theme |
| Consistency | Same thing, same look, everywhere | One tab style for file tabs, subtabs, workspace tabs and top tabs; one input box; one list row style |
| Error prevention | Make losing work hard | The Save / Don't Save / Cancel dialog; read-only views (binary, man pages, diffs' left side) refuse edits |
| Recognition over recall | Show options instead of making people remember them | Palette shows keybindings; recently used commands come first; tooltips name icon-only buttons and their keys |
| Flexibility | Shortcuts for experts, visible paths for everyone else | Every clickable status item is also a command; vim mode is opt-in |
| Minimalist design | Show only what is needed now (progressive disclosure) | Explorer actions appear on hover; search details behind "..."; graph columns drop when narrow |
| Fitts's law | Things you hit often are big and near | Whole rows are click targets, not just their text; tab close button sits at the tab's edge; dialog buttons are 28 px tall |
| Hick's law | Fewer, ordered choices decide faster | Recently used group in the palette; current theme / language preselected in pickers |
| Gestalt (proximity, common region) | Things that belong together sit together | Input toggles live inside their box; settings grouped in category tabs; dialog buttons grouped at bottom right |
| Signifiers | What can be clicked looks clickable; what can't, doesn't | Hover highlight only on items that do something (see §4.3) |
| Accessible contrast (WCAG 2.x) | Text that carries meaning must be readable | The Darker theme lifts every meaningful colour to at least 5.5:1 (§5) |

## 3. Structure

```
+-------------------------------------------------------------+
| Project | Git 3 | Search | Manpages | Settings               |  top-level tabs
+---------+---------------------------------------------------+
| side    | tab | tab | tab                                    |  editor group(s)
| bar     | breadcrumbs                                        |
|         | editor                                       |mini |
|         |                                              |map  |
|         +---------------------------------------------------+
|         | panel (Problems / Output)                          |
+---------+---------------------------------------------------+
| branch  problems  status message        Ln, Col  Spaces  LF  |  status bar
+-------------------------------------------------------------+
```

- **Top-level tabs** replace VSCodium's activity bar. They show words as
  well as icons and use about 35 px of height instead of 48 px of width.
  Narrow screens benefit most.
- There are two kinds of top tab:
  - **Side-bar views** (Project, Git, Search) open a pane next to the
    editors. Clicking the active one hides the side bar, as VSCodium's
    activity bar does.
  - **Workspaces** (Manpages, Settings) fill the whole main area. They
    are separate tasks, not companions to editing. Each has a search bar
    that is always visible and its own tab strip:
    - Manpages has the permanent **Index** plus one closable tab per page.
    - Settings has a fixed set of category tabs; a search jumps to the
      right tab and marks the setting.
  - Opening a file from anywhere (Ctrl+P, a search result, a link) returns
    to the editors.
  - **Workspaces never wait on the disk.** Manpages reads its index
    (apropos) and its pages (man, mandoc) on a worker thread:
    - the tab opens at once, showing "Reading the manual page index…" and
      "Opening x…" while the disk works;
    - the newest search wins and stale results are dropped;
    - resting the pointer on the Manpages tab starts reading the index
      before the click.
- **Square geometry.** No rounded corners anywhere. Shapes are rectangles
  with 1 px borders, so alignment and spacing do the visual grouping. That
  is also cheap to draw: every quad is axis-aligned.

### 3.1 Spacing and type

- Base unit 4 px, scaled by the UI scale. Row height 22 px (lists),
  tabs 35 px, status bar 22 px, inputs at least 26 px tall.
- Fonts: Noto Sans for the UI (13 px), DejaVu Sans Mono for code.
  Small (11 px) is only for view titles, labels and badges, never for
  content the user has to read closely.
- Padding: 20 px left inset for view titles and list content; 6 px inside
  tooltips; 20 px inside dialogs.

## 4. Components and their rules

### 4.1 Inputs

- A single box type everywhere: find, search, palette, commit message,
  Manpages / Settings search.
- Long text wraps onto more lines; the box grows taller rather than
  scrolling text under its icons. The first line leaves room for in-box
  toggles; later lines use the full width.
- When there isn't room for at least about 110 px of text next to the
  toggles (a narrow side bar), the toggles move to their own row under
  the box. Wrapping text one word per line is worse than one extra row.
- Single click places the caret, double click selects a word, triple
  click selects everything.

### 4.2 Tabs

- One visual language for all of them: active = editor background plus a
  1 px accent on top; inactive = darker; borders 1 px.
- Rows wrap instead of scrolling hidden tabs out of view. Subtabs
  (executable sections) wrap the same way.
- **Close.** The close button shows on the active and hovered tabs.
  Middle click closes. A modified tab shows a dot that turns into the
  close button on hover, as in VSCodium. Closing unsaved work asks first
  (§4.6).
- **Tooltip** = the full path, and "unsaved changes" when dirty.

### 4.3 Clickability

> A hover highlight is a promise that clicking does something.

- Status bar items highlight on hover only if clicking them does
  something. Informational items (encoding, line endings) still get a
  tooltip but no highlight.
- Clickable status items:

  | Item | Click |
  |---|---|
  | branch | checkout picker |
  | problems | Problems panel |
  | Ln, Col | Go to Line |
  | Spaces / Tab Size | switch indentation |
  | language | Change Language Mode picker |
  | OVR | leave overtype |
  | server name | its log in Output |

- The empty editor's hints are links. They highlight on hover and run
  their command on click.

### 4.4 Tooltips

- Every icon-only control has one, naming the action and its key, e.g.
  "Match Case (Alt+C)" or "Close (Escape)".
- **Delay 600 ms** after the pointer rests on the control. Moving within
  the same control does not restart the delay; moving to another control
  does.
- Shown under the control, or above it when there is no room (status
  bar). Never shown while a button is held, or while a menu, picker or
  dialog is open.
- **Clicking a control hides its tooltip** until the pointer moves on to
  another control. Once you act on a control, the tip would only cover
  what the click opened (for example the search box under the Manpages
  tab).
- Implemented once: drawing code calls `ui_tip(x, y, w, h, text)` for its
  controls, and the frame draws the one under the pointer.

### 4.5 Hover cards and delays

| What | Delay | Why |
|---|---|---|
| Code hover (LSP) | 150 ms | Reading code is deliberate; a long delay feels broken |
| Inline blame card | 150 ms | Same |
| Git graph commit card | 250 ms | The list is scanned with the mouse; a shorter delay flickers |
| Tooltips | 600 ms | They are for learning, so they must not get in the way of people who already know |

**Lifetime rule.** A card opened by the pointer lives only while the
pointer is on its element or on the card itself (including the gap
between them, so the pointer can travel onto the card).

- This is checked every frame against the pointer's position, not only
  when the element receives a move event. A fast move, or leaving the
  window, gives the element no event to close the card with, so it used
  to linger, or in the git graph's case follow the pointer around.
- Leaving the window counts as "nothing is hovered". Dragging is the
  exception: a drag keeps following the pointer outside the window.
- The graph card is anchored to its row, not to the pointer.
- A hover opened from the keyboard (Ctrl+K Ctrl+I) is not tied to the
  pointer; it stays until Escape or a key.

### 4.6 Dialogs (modal)

- Used only for decisions that cannot be undone. Today that is losing
  unsaved changes.
- Text follows VSCodium:
  - Title: "Do you want to save the changes you made to x?"
  - Detail: "Your changes will be lost if you don't save them."
  - With several files, the title says how many and the detail lists them.
- **Buttons** are Save (primary, filled), Don't Save, Cancel, in
  VSCodium's Linux order.
- **Keyboard:** Enter = focused button (Save at first), Escape = Cancel,
  Tab or arrows move the focus. The underlined letters (S, N, C) pick a
  button directly.
- The rest of the window is dimmed and takes no input, so it reads as
  modal.
- **When it appears:** closing the last view of a modified file (Ctrl+W,
  middle click, tab close button), Close All, quitting (Ctrl+Q, or the
  window's close button).
- **When it doesn't:**
  - "Close Others" and "Close to the Right" skip modified files instead.
  - vim's `:q!` uses "View: Revert and Close Editor".
- Untitled files: Save opens Save As. When quitting, an untitled file
  that is still unsaved keeps the window open with a message, instead of
  being lost silently.

### 4.7 Pickers and the palette

- The palette lists recently used commands first, labelled "recently
  used", then "other commands", as VSCodium does. Typing still ranks by
  match quality, and among equal matches recent commands come first.
- **Pickers** open with the current value selected (theme, language).
- The **theme picker** applies the selected theme as you move. Escape
  restores the old one, Enter keeps the new one.

### 4.8 Narrow side bar

The side bar is narrow by default (150 px at 1x, the user's choice), so
every view must still work at that width:

- view titles are truncated before their action icons, never overlapped
  by them;
- search toggles get their own row (§4.1);
- search results drop leading whitespace and, when the text before the
  match would take more than a quarter of the row, show only its end
  after "…", so the match is always visible;
- the git graph shows only the commit subject when narrow (author and
  refs are in the hover card);
- the explorer root name is truncated with "…" while the header actions
  are shown.

## 5. Colour themes

| Theme | Use |
|---|---|
| Dark Modern | VSCodium's default dark |
| Dark Modern Darker | Near-black surfaces, higher contrast |
| Light Modern | VSCodium's default light |

**Dark Modern Darker** is derived from Dark Modern at start-up
(`theme_init`), not drawn up separately, so it can't drift from it:

1. Neutral dark surfaces drop to half their brightness: editor #1F1F1F
   becomes #0F0F0F, tabs and side bar #181818 become #0C0C0C.
2. Borders, hover and selection colours stay as they are, so they stand
   out more against the darker surfaces.
3. Colours that carry meaning are raised until they reach a WCAG
   contrast ratio:
   - body text: at least 7:1 (AAA);
   - git decorations, errors, warnings, secondary text, line numbers and
     all syntax colours: at least 5.5:1.

   Raising keeps the hue: channels are scaled up first and only then
   mixed toward white.

Measured (`theme_contrast`, on the tab bar background):

| Colour | Dark Modern | Darker |
|---|---|---|
| git deleted (tab / explorer) | #C74E39, 3.87:1 (fails AA) | #EE5D45, 5.86:1 |
| line numbers | #6E7681, 3.87:1 | #838E99, 5.86:1 |
| error | #F14C4C, 4.97:1 | #F84E4E, 5.77:1 |
| inactive tab text | #9D9D9D, 6.55:1 | 7.21:1 |
| comments (on the editor) | #6A9955, 4.95:1 | 5.71:1 |

Light Modern is unchanged. Two of its colours sit under 4.5:1 (inactive
tab text, error); they match VSCodium exactly and are left alone.

## 6. Annoyances removed

| Annoyance | Principle | Fix |
|---|---|---|
| Closing a modified tab, Close All or quitting threw the changes away without a word | Error prevention | Save / Don't Save / Cancel dialog (§4.6) |
| Every restart began with an empty editor | Recognition over recall | Per-project session: open tabs, cursor, scroll, active tab, side bar and panel state come back (`$XDG_STATE_HOME/vxide/sessions/`) |
| Settings were forgotten between runs | Consistency with VSCodium | `$XDG_CONFIG_HOME/vxide/settings.conf` |
| Icon buttons (Aa, ab, .*, git actions, explorer actions) gave no hint what they do | Recognition over recall | Tooltips with the key (§4.4) |
| Status bar items looked static, though in VSCodium they're the quickest way to change indentation or language | Discoverability | Clickable items with hover highlight and tooltips (§4.3) |
| In a narrow side bar the Search box wrapped after four letters, and titles ran under icons | Aesthetic integrity | Narrow layout rules (§4.8) |
| Search results cut off the match itself | Visibility | Match-anchored snippets (§4.8) |
| The Git top tab kept empty room for a badge | Visual noise | Badge space only when there is a count |
| The welcome hints looked like links but weren't | Signifiers | They are links now (§4.3) |
| A workspace's tab didn't look active when the side bar was hidden | Visibility of status | Workspaces count as active whatever the side bar does |
| Ctrl+K Ctrl+T toggled blindly between two themes | User control | VSCodium's picker with live preview |
| The red of a deleted file was hard to read | Contrast | Dark Modern Darker (§5) |
| Hover cards (blame, code hover, graph) stayed open, or followed the pointer, after a fast move away from their element | Consistency, user control | Per-frame lifetime rule (§4.5) |
| A tooltip popped up over what a click had just opened | Minimalism | Clicking a control hides its tooltip (§4.4) |
| Clicking Manpages froze the UI while apropos read the disk (seconds on a cold HDD) | Visibility of status, responsiveness | Worker thread, a loading state, and prefetch when the pointer rests on the tab (§3) |
| Earlier rounds: the explorer could not scroll to the end of big trees | Visibility | It now reveals the selection only when the selection changes, and has a draggable scrollbar |
| Earlier rounds: long lines in search/input bars ran under the icons | Consistency | Inputs wrap and grow (§4.1) |

## 7. Persistence

| What | Where | When |
|---|---|---|
| Settings | `$XDG_CONFIG_HOME/vxide/settings.conf` | On every change |
| Session (per folder) | `$XDG_STATE_HOME/vxide/sessions/<folder>` | On quit |
| Recently used commands | `$XDG_STATE_HOME/vxide/commands` | On use |

- Files are written to a temporary file and then renamed, so a crash
  can't leave half a file.
- Headless runs read and write none of them unless `VXIDE_SETTINGS` /
  `VXIDE_STATE_DIR` is set, so tests stay hermetic.
- The session reopens real files only. Diffs, man pages, untitled buffers
  and editor splits are not restored.

## 8. Testing the UI

- Every rule above is checked with headless scripts (`--headless WxH
  --script`) and screenshots: the dialog, session restore, tooltips (they
  appear once the scripted pointer rests 600 ms), narrow side bar
  layouts, theme contrast.
- Headless `wait` now draws frames like the windowed loop, so timed UI
  behaves the same in scripts.
- `ctest` runs the unit suites and the scripted UI tests.

## 9. Not done yet / next candidates

- **Restore editor splits** in the session (only one group comes back
  today).
- **Keyboard focus ring** for top tabs and status items. Today keyboard
  users reach them through the palette.
- **Drag and drop tab reordering**, and dragging a tab into another group.
- **Live preview for the language picker**, as the theme picker has.
- **Change line endings** from the status bar (VSCodium's "Select End of
  Line Sequence").
- **Preview of the selected file in Quick Open** (VSCodium previews as you
  move).
