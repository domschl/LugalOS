#!/usr/bin/env bash
# build.sh — reproducibly build zork1-mit.z3 from the bundled MIT ZIL source.
#
# Toolchain: ZILF (zilf + zapf), see ../../pkgbuild/zilf-hg for the
# PKGBUILD used on Arch; any recent ZILF (>= 1.9) works.
# Reference toolchain: ZILF 1.9 (hg r1694.428d9a205a73).
#
# Output: zork1-mit.z3 — Z-machine v3, Release 1, Serial 112025.
#   Release/serial are pinned via ZAPF asm-options for reproducibility
#   (serial 112025 = Microsoft MIT release date 2025-11-20).  They
#   deliberately do NOT impersonate the retail r119/880429 binary.
#
# Build determinism: two runs with the same ZILF version produce
# byte-identical output.  Reference sha256 (ZILF 1.9):
#   e7e789a67ac87a5c42eee507bca223fdf8ae94c020110942686d5927daa6e6ca  zork1-mit.z3

set -euo pipefail
cd "$(dirname "$0")"

ZILF=${ZILF:-zilf}
command -v "$ZILF" >/dev/null || {
    echo "error: '$ZILF' not found." >&2
    echo "Build it from ../../pkgbuild/zilf-hg:  makepkg -si" >&2
    exit 1
}

out="$PWD/zork1-mit.z3"
src="$PWD/src"

# Build in a scratch dir so generated .zap intermediates never touch src/.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$src"/*.zil "$src/parser.cmp" "$tmp"/
(
    cd "$tmp"
    "$ZILF" zork1.zil "$out" --asm-options "-r=1,-s=112025"
)

echo "---"
sha256sum "$(basename "$out")"
file "$(basename "$out")"
