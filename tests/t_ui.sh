#!/bin/sh
# Headless UI test: needs a Vulkan device (GPU or llvmpipe).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/vxide-ui-XXXXXX)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src"
printf 'int f(void)\n{\n\treturn 0;\n}\n' > "$tmp/src/a.c"
printf 'int b;\n' > "$tmp/src/b.c"
cd "$here"
VXIDE_NO_LSP=1 ./build/vxide --headless 1000x700 --script tests/ui.vx "$tmp"
# the file was saved through the piece table + rename
grep -q 'int g(int x)' "$tmp/src/a.c"
echo "ui test ok"
