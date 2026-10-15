#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 <original-rc67.asi> <expected-source-sha256> <output-directory>" >&2
    exit 2
fi
source_asi=$1
expected_sha=$2
out_dir=$3
compiler=${MINGW32_CC:-/opt/homebrew/bin/i686-w64-mingw32-gcc}
patcher_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out_dir"
python3 "$patcher_dir/patch_import.py" patch "$source_asi" "$out_dir/MaxPayne2VR-patched.asi" \
    --expected-sha256 "$expected_sha" --def-file "$out_dir/mp2vrcmp.def"
"$compiler" -shared -static-libgcc -O2 -Wall -Wextra -Werror -Wl,--enable-stdcall-fixup -Wl,--kill-at -Wl,--no-insert-timestamp \
    -o "$out_dir/mp2vrcmp.dll" "$patcher_dir/src/mp2vrcmp.c" "$out_dir/mp2vrcmp.def"
printf 'Built PE32 facade and patched ASI in %s\n' "$out_dir"
