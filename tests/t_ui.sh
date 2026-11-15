#!/bin/sh
# Headless UI test: needs a Vulkan device (GPU or llvmpipe).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/vxide-ui-XXXXXX)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src"
printf 'int f(void)\n{\n\treturn 0;\n}\n' > "$tmp/src/a.c"
printf 'int b;\nint value_9;\n' > "$tmp/src/b.c"
printf 'int value_1 = 1;\nint value_2;\n' > "$tmp/src/c.c"
cd "$here"
vx=${VXIDE:-$here/build/vxide}
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/ui.vx "$tmp"
# the file was saved through the piece table + rename
grep -q 'int g(int x)' "$tmp/src/a.c"
# regex replace in files hit c.c only (b.c excluded)
grep -q '^int v1 = 1;$' "$tmp/src/c.c"
grep -q '^int v2;$' "$tmp/src/c.c"
grep -q 'value_9' "$tmp/src/b.c"
# multi-cursor
printf 'aa x\nbb x\ncc x\ndd\n' > "$tmp/src/m.c"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/multicursor.vx "$tmp"
# command palette case transforms
printf 'hello w\303\266rld\nfooBar parseHTTPResponse\n' > "$tmp/src/t.c"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/transform.vx "$tmp"
# overtype mode
printf 'abcdef\nxy\n' > "$tmp/src/o.c"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/overtype.vx "$tmp"
# vim emulation (off by default; turned on through the palette)
printf 'alpha beta gamma\none two three\nx(a, b) y\nsay "hello world" ok\nlast line\n' > "$tmp/src/ops.txt"
printf 'one two three four\nfive six seven\neight nine\nten\n' > "$tmp/src/vis.txt"
printf 'int f(int a)\n{\n\treturn a;\n}\nfoo foo bar\n' > "$tmp/src/misc.c"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/vim.vx "$tmp"
printf 'gamma 1 1 three\nx() y\nsay "ByE" ok\n "ByE" ok\nLAST line\n' | cmp - "$tmp/src/ops.txt"
printf 'four\n!YZfive six seven\neight nine\nten\n' | cmp - "$tmp/src/vis.txt"
printf 'int f(int a)xxx\nadd\nadd\n{\n\treturn a;\n}\nfoo o bar foo\n' | cmp - "$tmp/src/misc.c"
# git actions in a scratch repository
repo="$tmp/repo"
mkdir -p "$repo"
(cd "$repo" && git init -q -b main && git config user.name T && git config user.email t@t &&
	printf 'a\n' > f.txt && git add f.txt && git commit -qm one && printf 'b\n' >> f.txt && echo new > u.txt)
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/gitactions.vx "$repo"
(cd "$repo" && git branch | grep -q topic && git tag | grep -q '^v1$' && test -z "$(git stash list)" &&
	git status --short | grep -q '^ M f.txt' && git status --short | grep -q '^?? u.txt')
# diff editor + range staging
d="$tmp/diffrepo"
mkdir -p "$d"
(cd "$d" && git init -q -b main && git config user.name T && git config user.email t@t &&
	printf 'int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\nint f = 6;\nint g = 7;\n' > m.c &&
	git add m.c && git commit -qm base &&
	printf 'int a = 10;\nint b = 2;\nint c = 3;\nint d = 4;\nint new_line;\nint e = 5;\nint f = 6;\nint g = 70;\n' > m.c)
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/diffstage.vx "$d"
(cd "$d" && test "$(git diff --cached | grep -c '^[-+]int')" = 2 && git diff --cached | grep -q '^+int g = 70;' &&
	git diff | grep -q '^+int a = 10;')
# merge conflict: markers edited by hand, then marked resolved
c="$tmp/conflictrepo"
mkdir -p "$c"
(cd "$c" && git init -q -b main && git config user.name T && git config user.email t@t &&
	printf 'one\ntwo\nthree\n' > f.c && git add f.c && git commit -qm base && git checkout -qb topic &&
	printf 'one\nTOPIC\nthree\n' > f.c && git commit -qam topic && git checkout -q main &&
	printf 'one\nMAIN\nthree\n' > f.c && git commit -qam main && ! git merge topic >/dev/null 2>&1)
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/conflict.vx "$c"
(cd "$c" && test -z "$(git diff --name-only --diff-filter=U)" && ! grep -q '<<<<<<<' f.c)
# profiles: the viewer opens a source annotated; next hot line follows the costs
p="$tmp/prof"
mkdir -p "$p"
cp tests/prof/massif.out.hot tests/prof/hot.c tests/prof/hot.c.gcov "$p/"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/prof.vx "$p"
# find in diffs and binary views
f="$tmp/findrepo"
mkdir -p "$f"
(cd "$f" && git init -q -b main && git config user.name T && git config user.email t@t &&
	seq 1 300 | sed 's/^/int value_/; s/$/;/' > m.c && git add m.c && git commit -qm base &&
	sed 's/^int value_150;/int value_150 = needle;/; s/^int value_7;/int needle_7;/; s/^int value_280;/int value_280; \/\/ needle/' m.c > m.tmp &&
	mv m.tmp m.c && printf 'abc\000\001\002needle\000xyz needle\377' > blob.bin)
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/find.vx "$f"
if command -v cc >/dev/null 2>&1 && printf 'int main(void) { return 0; }\n' > "$f/prog.c" &&
	cc -O1 -o "$f/prog" "$f/prog.c" 2>/dev/null; then
	VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/finddis.vx "$f"
fi
# Markdown preview, including input that is easy to get wrong
m="$tmp/md"
mkdir -p "$m"
printf '# Title\n\nSome *text* with `code` and a [link](#title).\n\n- a\n  - b\n    - c\n1. one\n\n> quote\n\n| a | b |\n|---|--:|\n| 1 | 2 |\n' > "$m/README.md"
printf -- '-\n- \n-   \n1.\n* * *\n- - -\n>\n> > >\n' > "$m/odd.md"
printf '%s\n' '- [ ]' '[a](' '![b](' '**x' '`y' '~~z' '<a' '&' '|a|' '|-|' '    code' '#' '####### no' 'Title' '=====' >> "$m/odd.md"
printf '```\nunclosed fence\n' >> "$m/odd.md"
VXIDE_NO_LSP=1 "$vx" --headless 1000x700 --script tests/md.vx "$m"
grep -q '^- new item$' "$m/README.md" && ! grep -q ignored "$m/README.md"
# links in the preview: Markdown opens previewed, other files as usual
l="$tmp/mdlink"
mkdir -p "$l"
printf '# Main\n\nSee [the other doc](other.md#second) and [code](a.c).\n' > "$l/README.md"
printf '# Other\n\ntext\n\n## Second\n\nbody\n' > "$l/other.md"
printf 'int x;\n' > "$l/a.c"
VXIDE_NO_LSP=1 "$vx" --headless 1000x600 --script tests/mdlink.vx "$l"
# Profiling workspace (FreeBSD): a stand-in pmcstat lists events and runs
# the program; it must run in the project folder with its arguments
if [ "$(uname)" = FreeBSD ]; then
	pm="$tmp/pmc"
	mkdir -p "$pm/sub"
	cat > "$tmp/fake-pmcstat" <<'FAKE'
#!/bin/sh
if [ "$1" = -L ]; then printf '\tev.one\n\tev.two\nSOFT\n\tclock.hard\n'; exit 0; fi
while [ $# -gt 0 ]; do case "$1" in -d) shift ;; -p|-P|-n|-O) shift 2 ;; *) break ;; esac; done
"$@"; rc=$?
printf '# p/ev.one\n            42\n' >&2
exit $rc
FAKE
	printf '#!/bin/sh\nprintf "%%s|" "$@" > ran\n' > "$pm/mark.sh"
	chmod +x "$tmp/fake-pmcstat" "$pm/mark.sh"
	VXIDE_PMCSTAT="$tmp/fake-pmcstat" VXIDE_NO_LSP=1 "$vx" --headless 1100x700 --script tests/pmc.vx "$pm"
	test "$(cat "$pm/ran")" = 'a|b c|'
fi
# Workflow: CMake configure + build, a debug session (cmake and lldb-dap / gdb)
if command -v cmake >/dev/null 2>&1 && { command -v lldb-dap >/dev/null 2>&1 || command -v lldb-dap23 >/dev/null 2>&1 ||
	command -v gdb >/dev/null 2>&1; }; then
	wf="$tmp/wf"
	mkdir -p "$wf/src"
	printf 'cmake_minimum_required(VERSION 3.16)\nproject(wf C)\nadd_executable(demo src/main.c)\n' > "$wf/CMakeLists.txt"
	printf '#include <stdio.h>\nstatic int counter;\nstatic int twice(int v)\n{\n\treturn v * 2;\n}\nint main(void)\n{\n\tint a = 1;\n\ta += 2;\n\tcounter = twice(a);\n\tcounter += 1;\n\tprintf("%%d\\n", counter);\n\treturn 0;\n}\n' > "$wf/src/main.c"
	VXIDE_NO_LSP=1 "$vx" --headless 1200x800 --script tests/workflow.vx "$wf"
	test -x "$wf/build/demo"
fi
echo "ui test ok"
