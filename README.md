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
  regexes, heredocs). Languages are also detected from shebangs. They were checked against VSCodium's own `c.tmLanguage.json`
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
  Replacements take `$1`, `${1}`, `$name`, `$&`, `$$`, `\n`, `\t` and
  the case operators `\u \l \U \L \E`. On this host the DFA scans
  about 540 MB/s; `(a+)+b` over 100k characters takes 0.3 ms.
- **Git blame** on the cursor line, on by default (VSCodium's
  `git.blame.editorDecoration`): `summary, author (3 days ago)`, or
  "Not Committed Yet" for edited lines. A worker thread runs
  `git blame --incremental --contents <buffer snapshot>`; libgit2 is the
  fallback without a git binary (much slower on long histories). Toggle it
  with "Git: Toggle Git Blame Editor Decoration".
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
| Ctrl+D, Ctrl+Shift+L | Add next occurrence, select all occurrences |
| Shift+Alt+Up/Down, Alt+click | Add cursor above/below, add cursor |
| Alt+Up/Down, Ctrl+Shift+Alt+Up/Down | Move line, copy line |
| Ctrl+Shift+K, Ctrl+Enter, Ctrl+Shift+Enter | Delete line, insert line below/above |
| Ctrl+/, Ctrl+], Ctrl+[ | Toggle comment, indent, outdent |
| Ctrl+L, Ctrl+A | Select line, select all |
| Ctrl+Space, Ctrl+K Ctrl+I | Suggest, hover |
| F12, Ctrl+click, Ctrl+Alt+-, Ctrl+Shift+- | Definition, back, forward |
| F8, Shift+F8 | Next/previous problem |
| Ctrl+K Ctrl+T | Toggle dark/light theme (also the gear in the activity bar) |
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
