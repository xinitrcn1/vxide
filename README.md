# vxide

A project-based code editor in C11, built with BSD make, whose whole UI is
drawn with Vulkan. Its look and its keyboard shortcuts follow VSCodium
(Dark Modern / Light Modern themes, Dark+ / Light+ syntax colours). It
talks to clangd over LSP and to git through libgit2.

```
make            # build/vxide
make test       # unit tests + a headless scripted UI test
./build/vxide [--scale N] [--light] [DIR]
```

There is no project file: the folder you open is the project. The
explorer hides `.git`, `.svn`, `.hg`, `.jj`, `CVS`, `.DS_Store` and
`Thumbs.db` (VSCodium's default `files.exclude`). Quick Open and Search
also skip `node_modules`, `bower_components` and git-ignored paths.

## Requirements

FreeBSD 15 (or Linux) with: a Vulkan 1.1 driver, `vulkan-loader`/headers,
`sdl3` (window and input only), `freetype2` (glyph rasters), `libgit2`,
`glslangValidator` (shaders are compiled at build time), and clangd in
`PATH` for language features. Fonts: Noto Sans for the UI and
DejaVu Sans Mono for code, with DejaVu Sans as the fallback.

## What it does

- **Workbench**: activity bar, a side bar (Explorer, Search, Source
  Control), up to three editor groups with tabs and breadcrumbs, a panel
  (Problems, Output) and a status bar. The side bar and panel can be
  resized by dragging their edges. Everything is square: solid rects, 1 px
  borders, glyphs, and icons drawn from distance fields at start-up.
- **Editor**: multiple cursors, auto-closing brackets and quotes, smart
  Enter/indent, toggle comment, move/copy/delete lines, find/replace,
  bracket matching, indent guides, current-line box, selection-occurrence
  highlight, minimap, a scrollbar with an overview ruler (git changes,
  errors, cursor), and preview tabs.
- **Highlighting**: hand-written lexers for C23, x86 assembly (AT&T/GAS and
  Intel/NASM, detected from the file head), Lisp (CL, Scheme, Elisp,
  Clojure), shell (sh/bash/zsh, heredocs), Markdown (with fenced code
  highlighted in its own language), troff/man/mdoc and Perl (POD, `s{}{}`,
  regexes, heredocs), CMake, Make (BSD and GNU; recipe lines highlighted as
  shell), JSON / JSON with Comments, ignore files (.gitignore and friends),
  diff/patch, git commit messages, git-rebase-todo, Python, YAML and
  CODEOWNERS. Languages are also
  detected from shebangs and file heads. They were checked against VSCodium's own `c.tmLanguage.json`
  scopes and the Dark+/Light+ rules. When clangd is running, its semantic
  tokens are overlaid the way the clangd extension does it (readonly
  variables as constants, macros, parameters, ...).
- **LSP (clangd)**: diagnostics (squiggles, Problems, explorer and tab
  colours, F8), completion, hover, go to definition (F12 / Ctrl+click,
  with back/forward), semantic tokens. Positions use UTF-8 (UTF-16 is the
  fallback). Set `VXIDE_CLANGD=clangd23` to use another server.
- **Search**: the editor find widget and the Search view share one
  matcher: literal, case, whole word, and regex (Alt+C / Alt+W / Alt+R). The
  Search view has replace with a struck-out/inserted preview, Replace All
  (Ctrl+Alt+Enter), per-file and per-match replace and dismiss,
  files to include/exclude globs (Ctrl+Shift+J), and F4/Shift+F4 to step
  through results. Files that are open are edited in their buffers (one
  undo step); other files are rewritten atomically.
- **Regex engine** (`src/regex.c`): built in, modelled after RE2 and
  linear time. The pattern compiles to byte-level instructions, with
  UTF-8 classes as byte-range sequences. A lazy DFA finds the earliest
  match end, and a Pike VM then finds the leftmost-first match and its
  groups on that line. RE2 syntax: classes, `[[:alpha:]]`, `\d \w \s`
  (ASCII), `\b`, lazy quantifiers, `{n,m}` up to 1000, named groups,
  `(?i)`, `\Q..\E`, `\x{..}`. Lookaround and backreferences are
  rejected with RE2's messages. `^`/`$` match per line, and matches cross
  line ends only when the pattern contains `\n` (as in VSCodium).
  Replacements take `$0`/`$&` (whole match), `$1`, `${1}`, `$name`, `$$`, `\n`, `\t` and
  the case operators `\u \l \U \L \E`. On this host the DFA scans
  about 540 MB/s; `(a+)+b` over 100k characters takes 0.3 ms.
- **Search** skips git-ignored files (search.useIgnoreFiles), also when
  the project is a subfolder of the repository; files in results, Quick
  Open, tabs and the explorer carry their git colour (yellow modified,
  green untracked/added, red deleted, grey ignored).
- **Slow disks** (written for a 5400 RPM drive): nothing on the UI thread
  waits on the disk. Git status runs on a worker thread (with stat data
  written back to the index, like `git status`) when something happens -
  a save, a git operation, a change under .git, window focus - plus a 60 s
  fallback, instead of every 3 s. The explorer reacts to kqueue / inotify
  events on its open folders instead of rescanning every 5 s, and reads
  entry types from readdir (no stat per file). Quick Open's file list and
  Search run on background threads; Search reads files in inode order with
  sequential read-ahead and reports progress. Opened files get read-ahead
  (posix_madvise WILLNEED); clangd's background index runs at low priority.
  Idle file-system lookups on a 3800-file repository went from about 36,400
  to 7,900 per 12 s (the one initial status), and none after that.
- **Git blame** on the cursor line, on by default (VSCodium's
  `git.blame.editorDecoration`): `summary, author (3 days ago)`, or
  "Not Committed Yet" for edited lines. It runs on libgit2 with its own
  engine (`src/lgblame.c`): libgit2's git_blame_file diffs whole trees at
  every step (12 s on a 22k-line file with 1915 commits); this one looks
  up only the tree entries along the file's path (memoised by tree id),
  diffs blobs on a thread pool, and stops once every line has an owner -
  0.28 s for that file (git: 0.25 s), matching `git blame` on 64,945 lines
  but two ambiguous diff placements. The live buffer is mapped onto the
  HEAD blob with one diff per edit. Toggle it
  with "Git: Toggle Git Blame Editor Decoration". Hovering the blame text
  shows the commit: author, relative and absolute date, the full message,
  hash and change stats.
- **Source Control graph** (VSCodium's scm.graph, "auto": the current
  branch, its upstream and its base): a GRAPH section with
  square lanes and commit markers (hollow for HEAD, ringed for merges),
  branch/tag badges, a hover card per commit, click to list a commit's
  files, click a file to open its diff, double click to open the whole
  commit. Right click: checkout (detached), create branch/tag, cherry pick,
  copy id/message.
- **Git actions** from the view's "..." menu and the palette, run
  asynchronously on libgit2 (the git CLI only as a fallback when an https
  remote needs a credential helper), with output in Output > Git: pull/push/fetch/sync
  (prune, rebase, force with lease), publish, checkout, create/rename/delete
  branch, merge, rebase (continue/abort), abort merge, stash (include
  untracked, apply/pop/drop latest or picked), tags, commit all/amend, undo
  last commit, unstage/discard all. Rebase/merge in progress shows in the
  view. "Git: Open Changes" shows a file's diff read-only.
- **Diff editor** (VSCodium style, side by side): unchanged lines face each
  other, changed lines are paired with word-level highlights, filler rows
  pad the shorter side. Working-tree diffs keep the real file editable on
  the right. Each change has Stage and Revert buttons (Unstage in staged
  diffs); Git: Stage / Unstage / Revert Selected Ranges (Ctrl+K Ctrl+Alt+S,
  Ctrl+K Ctrl+N, Ctrl+K Ctrl+R) apply every change touching the selection,
  in normal editors as well. Clicking a quick-diff mark in the gutter offers
  Stage Change / Revert Change / Open Changes; Alt+F5 / Shift+Alt+F5 go to
  the next / previous difference. Source Control opens diffs on click.
- **Merge conflicts**: the markers stay as plain, editable text (no Accept
  buttons); current / base / incoming regions are shaded with VSCodium's
  merge colours and labelled. Conflicted files are listed under "Merge
  Changes"; "+" or Git: Mark as Resolved stages the file, and refuses while
  markers remain. Merge Conflict: Next / Previous Conflict navigate.
- **Overtype**: Insert toggles insert/overtype (block cursor, OVR in the
  status bar); pasting overtypes too.
- **Context menus** (right click): editor (go to definition, declaration,
  type definition, references, find all references, change all occurrences,
  cut/copy/paste, toggle comment, command palette), explorer, tabs, Source
  Control files and graph rows.
- **Mouse cursors**: I-beam over text and inputs, a hand on Ctrl+hover
  (go to definition), resize arrows on the sashes.
- **Git**: branch and ahead/behind in the status bar, explorer
  decorations (M/U/A/D colours, folder dots, ignored files greyed out),
  quick-diff gutter against the index, and a Source Control view with
  stage, unstage, discard and commit (Ctrl+Enter, with a "stage all and
  commit?" prompt when nothing is staged).
- **Files**: documents are piece tables over the file `mmap`ed read-only
  (`MAP_PRIVATE`). Lines that sit in one piece are drawn straight from the
  mapping. A save writes a temp file, `fsync`s it, `rename`s it over the
  original, then remaps it. CRLF files keep CRLF, and tabs vs spaces are
  detected per file.

## Keys (VSCodium Linux defaults)

| Keys | Action |
|---|---|
| Ctrl+P | Go to file (`:N` goes to a line, `>` runs commands) |
| Ctrl+Shift+P, F1 | Command palette |
| Ctrl+G | Go to line |
| Ctrl+S, Ctrl+K S | Save, save all |
| Ctrl+N | New untitled file (saving asks for a path) |
| Ctrl+W, Ctrl+K W | Close editor, close all |
| Ctrl+Tab, Ctrl+PgUp/PgDn | Switch editors |
| Ctrl+\\, Ctrl+1/2/3 | Split editor, focus group |
| Ctrl+B, Ctrl+J | Toggle side bar, toggle panel |
| Ctrl+Shift+E / F / G | Explorer / Search / Source Control |
| Ctrl+Shift+M, Ctrl+Shift+U | Problems, Output |
| Ctrl+F, Ctrl+H, F3, Shift+F3 | Find, replace, next, previous (Alt+C case, Alt+W word, Alt+R regex) |
| Enter / Ctrl+Alt+Enter in replace | Replace one / replace all |
| Ctrl+Shift+H, Ctrl+Shift+J, F4/Shift+F4 | Replace in files, include/exclude globs, next/previous result |
| Ctrl+D, Ctrl+K Ctrl+D, Ctrl+Shift+L, Ctrl+F2 | Add next occurrence, move last selection to next, select / change all occurrences |
| Shift+Alt+Up/Down, Ctrl+Shift+Up/Down, Alt+click | Add cursor above/below, add cursor |
| Shift+Alt+I, Ctrl+U | Cursors at the ends of the selected lines, cursor undo |
| Shift+Alt+drag, middle drag, Ctrl+Shift+Alt+arrows/PgUp/PgDn | Column (box) selection |
| Shift+F12, Shift+Alt+F12 | Go to references, find all references |
| Ctrl+Alt+C, Ctrl+Shift+Alt+C | Copy path, copy relative path |
| Alt+Up/Down, Ctrl+Shift+Alt+Up/Down | Move line, copy line |
| Ctrl+Shift+K, Ctrl+Enter, Ctrl+Shift+Enter | Delete line, insert line below/above |
| Ctrl+/, Ctrl+], Ctrl+[ | Toggle comment, indent, outdent |
| Ctrl+L, Ctrl+A | Select line, select all |
| Ctrl+Space, Ctrl+K Ctrl+I | Suggest, hover |
| F12, Ctrl+click, Ctrl+Alt+-, Ctrl+Shift+- | Definition, back, forward |
| F8, Shift+F8 | Next/previous problem |
| Ctrl+K Ctrl+T | Toggle dark/light theme (also the gear in the activity bar) |
| Ctrl+Shift+P | Command palette (delete the `>` to search files); includes Transform to Upper/Lower/Title/Snake/Kebab Case |
| Insert | Toggle overtype |
| Ctrl+K Ctrl+Alt+S, Ctrl+K Ctrl+N, Ctrl+K Ctrl+R | Stage / unstage / revert selected ranges |
| Alt+F5, Shift+Alt+F5 | Next / previous difference in a diff |
| Explorer: arrows, Enter, Space, F2, Delete | Navigate, open, preview, rename, delete |

## Layout of the code

Data-oriented: state lives in flat arrays (struct-of-arrays tables indexed
by ids) with string pools, and per-frame work is a linear pass that fills
one quad buffer.

| File | Role |
|---|---|
| `src/gpu.c` | Vulkan: one render pass, one pipeline, one instanced draw per frame; swapchain or offscreen target |
| `shaders/ui.*` | A quad is a solid rect, an atlas coverage rect (`texelFetch`) or a squiggle |
| `src/font.c`, `src/icons.c` | FreeType glyph cache in an R8 atlas (shelf packer, open hashing); icons from SDF primitives |
| `src/draw.c` | Quad list with CPU clipping (atlas origins move with the cut) |
| `src/text.c` | mmap piece table, line-start index, undo log (SoA) |
| `src/syntax.c` | Line lexers with a per-line state, cached per document |
| `src/regex.c` | RE2-style regex: byte program, lazy DFA, Pike VM, replacement expansion |
| `src/blame.c` | Inline blame worker thread (git CLI, libgit2 fallback) |
| `src/gitgraph.c`, `src/gitops.c` | Commit graph lanes, commit details; async git runner and queries |
| `src/gitui.c` | Graph section, git actions, hover cards |
| `src/lgblame.c` | Blame on libgit2 primitives |
| `src/githunk.c`, `src/diffview.c` | Hunks, word diffs, range staging, conflicts; the diff editor |
| `src/fswatch.c`, `src/bgwork.c` | kqueue/inotify watches; background file scan and search |
| `src/menu.c`, `src/actions.c` | Context menus, picker, references, multi-cursor and path actions |
| `src/lsp.c`, `src/json.c` | LSP client over pipes (non-blocking), jsmn-style token table |
| `src/git.c` | libgit2: status table, index ops, commit, quick diff |
| `src/fstree.c`, `src/search.c` | Explorer tree, file walk, fuzzy match, text search |
| `src/ui.c` | Layout, input routing, keybindings, commands, tabs, status bar, quick input |
| `src/editor.c` | Editor drawing and editing, find, completion, hover |
| `src/panes.c` | Explorer, Search, Source Control, Problems, Output, input box |

## Testing without a display

`vxide --headless WxH --script FILE DIR` renders into an offscreen Vulkan
image and runs a script of `open`, `key ctrl+shift+p`, `type text\n`,
`click X Y`, `move`, `wheel`, `wait MS`, `waitlsp`, `expect text|line|file|problems ...`,
`print` and `shot out.png`. `tests/t_ui.sh` uses it. To run the real
windowed path without a display, set `SDL_VIDEO_DRIVER=offscreen` (NVIDIA
supplies `VK_EXT_headless_surface`).

Environment variables: `VXIDE_NO_LSP`, `VXIDE_CLANGD`, `VXIDE_THEME=light`,
`VXIDE_GPU=<name substring>`, `VXIDE_VALIDATE` (Khronos validation layer),
`VXIDE_LOG` (copy the Output log to stderr), `VXIDE_NO_BLAME`,
`VXIDE_BLAME_LIBGIT2` (force the libgit2 blame).

## Not done yet

No integrated terminal, folding, word wrap, rename or code actions, diff
editor, or settings UI. There is no hunk staging, push/pull or stash. The
UI scale is a whole number (1x or 2x), and IME composition is untested.
