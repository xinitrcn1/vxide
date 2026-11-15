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
VXIDE_NO_LSP=1 ./build/vxide --headless 1000x700 --script tests/ui.vx "$tmp"
# the file was saved through the piece table + rename
grep -q 'int g(int x)' "$tmp/src/a.c"
# regex replace in files hit c.c only (b.c excluded)
grep -q '^int v1 = 1;$' "$tmp/src/c.c"
grep -q '^int v2;$' "$tmp/src/c.c"
grep -q 'value_9' "$tmp/src/b.c"
# multi-cursor
printf 'aa x\nbb x\ncc x\ndd\n' > "$tmp/src/m.c"
VXIDE_NO_LSP=1 ./build/vxide --headless 1000x700 --script tests/multicursor.vx "$tmp"
# git actions in a scratch repository
repo="$tmp/repo"
mkdir -p "$repo"
(cd "$repo" && git init -q -b main && git config user.name T && git config user.email t@t &&
	printf 'a\n' > f.txt && git add f.txt && git commit -qm one && printf 'b\n' >> f.txt && echo new > u.txt)
VXIDE_NO_LSP=1 ./build/vxide --headless 1000x700 --script tests/gitactions.vx "$repo"
(cd "$repo" && git branch | grep -q topic && git tag | grep -q '^v1$' && test -z "$(git stash list)" &&
	git status --short | grep -q '^ M f.txt' && git status --short | grep -q '^?? u.txt')
echo "ui test ok"
