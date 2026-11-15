# vxide memory and data layout

vxide is written data-oriented: state is tables of plain values, rows are
named by `u32` indices, and a table is one allocation. This document sets
out the rules, the tools to check them, and what applying them bought.

## 1. Rules

1. **Rows are named by `u32` indices, not pointers.**
   - Documents (`app.docs`, `MAX_DOCS`), views (`app.views`, `MAX_VIEWS`),
     tree nodes, search results, symbols, disassembly rows, diagnostics
     and git graph rows are all indices.
   - A pointer is only a short-lived working handle, taken from a table
     that does not move (`View *v = gview(g, t)`). Nothing stores one
     across frames.
   - Indices survive a table growing, and they are half the size of a
     pointer.
2. **One table, one block (coalesced SoA).**
   - All columns of a table live in a single heap block, each column
     8-byte aligned, the first column being the block. See
     `soa_grow` / `soa_fit` / `soa_free` in `base.h`:

     ```c
     #define RESULT_COLS(r) SOA((r)->file), SOA((r)->line), SOA((r)->col), ...
     SoaCol c[] = {RESULT_COLS(r)};
     soa_grow(c, 6, &r->cap, r->n + 1, r->n);   /* grow, keeping n rows */
     soa_free(c, 6);                            /* one free */
     ```

   - A table grows with one allocation and one copy instead of one
     `realloc` per column, frees with one `free`, and its columns sit
     next to each other.
   - Order the columns 8-byte types first, then 4-byte, then 1-byte, so
     there's no padding.
   - Each table's column list is written once, in a `*_COLS` macro.
3. **Strings go in pools.**
   - Many short strings are appended to a `Pool` and named by a `u32`
     offset (`pool_add`), not `strdup`ed one by one.
   - Diff paths of views are interned in `app.strs`.
4. **Built once, then trimmed.**
   - A table that is built once and then only read gets `soa_fit` (or an
     exact `realloc`) at the end, dropping up to half its capacity in
     doubling slack.
   - Examples: disassembly rows, symbols, the line index of a freshly
     loaded file.
5. **Don't keep what you can recompute cheaply.**
   - Disassembly keeps where each instruction is (a `u32` and one byte);
     its text and branch target are decoded again for the rows on screen
     (`bin_row_text`, a few microseconds per screenful).
   - The flat text of an unmodified file is the file's mapping.
6. **Don't copy what already exists.**
   - **Generated documents** (man pages, the Index) adopt the buffer they
     were built in as their original text (`text_adopt`). When their
     columns match, the style-span block is handed over as is.
   - **Text:** an unmodified document is its `mmap`. `text_flat`
     returns the mapping directly, because the page tail past the end of
     the file is zero, so it is already NUL-terminated. Only edited or
     page-aligned files get a copy.
   - **Glyph atlas:** it is the GPU's persistently mapped staging
     buffer; glyphs are rasterized straight into it.
   - **Quads:** they are written into the frame's mapped vertex
     (instance) buffer. There is no CPU-side quad list and no per-frame
     copy.
   - **Quad corners:** they aren't stored anywhere. The vertex shader
     derives them from `gl_VertexIndex` (bit 0 = x, bit 1 = y) and draws
     each instance as a 4-vertex triangle strip. That is a third fewer
     vertex shader runs than two separate triangles (6 vertices), with
     pixel-identical output.
   - **Instance buffer:** the only per-quad data is the 16-byte instance
     (rect, atlas origin, colour). It differs for every quad, so it can't
     be hardcoded. Reading it from a storage buffer by
     `gl_InstanceIndex` instead of through vertex attributes would be the
     same bytes in a different binding, so there is nothing to gain.
7. **A closed thing keeps nothing.**
   - Closing a document frees its diagnostics, semantic tokens, git
     marks, per-line syntax state and message pool.
   - Freeing a view frees its cursor block.
8. **Fixed tables for small bounded sets.**
   - Documents (256), views (512), tabs per group (64) and groups (3)
     are fixed tables in `app` (about 175 KB in total).
   - They need no allocation or free list, and indices into them stay
     stable.
9. **Variable parts grow on demand.**
   - Cursors start with room for 4 and grow (`view_reserve`) up to
     `MAX_CURSORS`. Before this they were 512 inline slots, about 7 KB
     per tab.

## 2. Measuring

| Tool | What it tells |
|---|---|
| Script command `heap LABEL` | jemalloc's allocated and resident bytes at that point of a headless script |
| `cmake -B b -DVXIDE_HEAPTRACE=ON` | Every allocation in vxide's own code is tagged with its call site (`malloc`, `calloc`, `realloc`, `strdup`, `vx_*`, `VX_GROW`, `pool_add`, `soa_*`); `heap` then also lists the sites holding the most live bytes |

Libraries (the Vulkan driver, FreeType, libgit2, capstone) keep their own
allocations; the tracer only sees ours. The tracer's own tables (about
40 MB) show up in jemalloc's totals in that build, so compare its "ours"
lines, not the totals.

A session script (`heap.vx`) does the following in turn, measuring after
each step:

1. open the vxide folder;
2. open five sources (300 KB, 10k lines in the largest);
3. show the git graph;
4. search the project;
5. open `build/vxide` (3.8 MB, disassembled);
6. show the Manpages index.

## 3. Results

vxide's own live heap (heap-trace build, after each step):

| Step | Before | After |
|---|---|---|
| Start | 4.06 MB | 0.06 MB |
| 5 files open | 5.45 MB | 0.21 MB |
| Search the project | 5.6 MB | 0.40 MB |
| Binary open (3.8 MB ELF) | 21.06 MB | 1.51 MB |
| Manpages Index loaded (all 18k pages, 2.7 MB of text) | +9.06 MB | +4.22 MB |

Where it went:

| Was | Size | Now |
|---|---|---|
| Disassembly: 45 bytes per row (8-byte address, offset and target columns, a text pool, doubling slack) | 11.7 MB for the binary | 5 bytes per row, trimmed: 1 MB |
| `text_flat` copy of every open file, kept for as long as it was open | 4.8 MB (binary + 5 files) | 0 for unmodified files |
| CPU copy of the 2048x2048 glyph atlas | 4 MB | 0 (the staging buffer is the atlas) |
| CPU quad list, copied to the GPU each frame | 256 KB + a memcpy per frame | 0 |
| A `View` per tab with 512 inline cursors | 7 KB per tab | ~100 B per view plus the cursors in use |
| Diagnostics, semantic tokens, syntax state of closed documents | Kept until the slot was reused | Freed on close |
| Manpages Index: page buffer copied into the document's insert buffer (rounded up to 4 MB), plus a flat copy | 9 MB | The buffer and the span block change owner (`text_adopt`, `doc_new_virtual_owned`): 4.2 MB |

The process total is dominated by the GPU driver: the NVIDIA userspace
driver allocates about 28 MB at `vkCreateDevice`, which vxide can't
change.

## 4. Tables

Every growing row table is one block:

- **Text:** piece table, undo log, diagnostics, semantic tokens.
- **Binaries:** sections, symbols, disassembly rows, hex tags.
- **Search:** results, the file list, replace-in-file edits.
- **Explorer:** tree nodes.
- **Git:**
  - graph rows and edges, commit files and decorations;
  - hunks, word changes, conflicts and markers;
  - op queues, branches, stashes, tags and remotes;
  - blame origins, ranges, commits, edges and jobs.
- **LSP:** documents, pending requests and every event table.
- **JSON:** tokens.
- **Regex:** parser nodes, class ranges, program, DFA states.
- **UI:**
  - picker, locations, completion, find matches, quick-open, problem rows;
  - vim `:s` matches;
  - Manpages spans.

Each table has an `X_COLS` macro listing its columns once.

Converting them also fixed several things:

- **Per-row reallocation:** the blame commit table, the reference
  locations and the replace edits reallocated their columns on every row.
  They now double.
- **A leak:** the buffer blame map leaked its hunk columns when a diff
  failed.
- **Exact sizing:** a file-list copy is now built at its exact size.

Left as they are:

- **Open-addressing hash tables:** the blame path memo and the origin
  hash.
- **Sort-key scratch columns.**
- **Single arrays:** the line index and pools.

## 5. Checking new code

- Grow a table with `soa_grow`, never with one `realloc` per column.
  Free it with `soa_free`, never column by column; freeing a column other
  than the first is an invalid free, and ASan will say so.
- Store indices, not pointers. If something must refer to a view,
  document, row or string later, keep its `u32`.
- Before keeping a copy of something (text, pixels, decoded output), ask
  whether it already exists somewhere (a mapping, a GPU buffer) or is
  cheap to recompute for what is on screen.
- Run `heap.vx` under the trace build before and after a change that
  allocates.
