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
echo "ui test ok"
