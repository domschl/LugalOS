#!/usr/bin/env bash
# build.sh — reproducibly build zork2-mit.z3 from the bundled MIT ZIL source.
#
# Toolchain: ZILF (zilf + zapf), see ../../pkgbuild/zilf-hg for the
# PKGBUILD used on Arch; any recent ZILF (>= 1.9) works.
# Reference toolchain: ZILF 1.9 (hg r1694.428d9a205a73).
#
# Output: zork2-mit.z3 — Z-machine v3, Release 1, Serial 112025.
#   Release/serial are pinned via ZAPF asm-options for reproducibility
#   (serial 112025 = Microsoft MIT release date 2025-11-20).  They
#   deliberately do NOT impersonate the retail r63/860811 binary.
#
# Patches: src/ is a byte-exact mirror of the pinned upstream commit.
#   Everything in patches/*.patch is applied to the scratch build tree
#   in sorted order (see README.md for why 0001 is required).
#
# Build determinism: two runs with the same ZILF version produce
# byte-identical output.  Reference sha256 (ZILF 1.9):
#   see README.md

set -euo pipefail
cd "$(dirname "$0")"

ZILF=${ZILF:-zilf}
command -v "$ZILF" >/dev/null || {
    echo "error: '$ZILF' not found." >&2
    echo "Build it from ../../pkgbuild/zilf-hg:  makepkg -si" >&2
    exit 1
}

out="$PWD/zork2-mit.z3"
src="$PWD/src"

# Build in a scratch dir so generated .zap intermediates never touch src/.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$src"/*.zil "$tmp"/
for p in patches/*.patch; do
    [ -e "$p" ] || continue
    patch -s -p1 -d "$tmp" < "$p"
done
(
    cd "$tmp"
    "$ZILF" zork2.zil "$out" --asm-options "-r=1,-s=112025"
)

echo "---"
sha256sum "$(basename "$out")"
file "$(basename "$out")"
