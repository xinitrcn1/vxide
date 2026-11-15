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
echo "ui test ok"
