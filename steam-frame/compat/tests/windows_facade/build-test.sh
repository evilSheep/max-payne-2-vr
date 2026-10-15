#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: $0 <output-directory> <built-mp2vrcmp.dll>" >&2
    exit 2
fi
out_dir=$1
facade_dll=$2
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
compiler=${MINGW32_CC:-/opt/homebrew/bin/i686-w64-mingw32-gcc}
mkdir -p "$out_dir/other" "$out_dir/control"
cp "$facade_dll" "$out_dir/mp2vrcmp.dll"
"$compiler" -shared -O2 -Wall -Wextra -Werror -Wl,--kill-at \
    -o "$out_dir/X_ModesMFC.dll" "$project_dir/tests/windows_facade/engine.c"
"$compiler" -shared -O2 -Wall -Wextra -Werror -Wl,--kill-at \
    -o "$out_dir/other/X_ModesMFC.dll" "$project_dir/tests/windows_facade/engine.c"
"$compiler" -shared -O2 -Wall -Wextra -Werror -Wl,--kill-at \
    -o "$out_dir/control/X_ControlMFC.dll" "$project_dir/tests/windows_facade/engine.c"
"$compiler" -O2 -Wall -Wextra -Werror -o "$out_dir/maxpayne2.exe" \
    "$project_dir/tests/windows_facade/harness.c"
printf 'Built Windows facade test files under %s\n' "$out_dir"
printf 'Run from that directory with: maxpayne2.exe same\n'
printf 'Then run: maxpayne2.exe outside "%s/other/X_ModesMFC.dll"\n' "$out_dir"
