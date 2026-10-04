#!/usr/bin/env bash
# fetch-games.sh — download Z-machine story files into z-data/
#
# Story files are NOT bundled in the LugalOS repository.  The ZIL
# source mirror at historicalsource/zork1 declares an MIT license for
# the source code; the compiled Infocom story binary is distributed
# separately in the IF-community tradition of free redistribution of
# the classic Infocom titles.  To keep provenance unambiguous we do
# not re-host the binary: we record its canonical URL and SHA-256
# fingerprint here, and each user fetches it locally.
#
# Usage:
#   ./fetch-games.sh                  fetch all oracle binaries
#   ./fetch-games.sh zork1-oracle     fetch a single one
#
# Files are written to z-data/ (git-ignored).  Existing files with a
# matching checksum are left untouched; a checksum mismatch is fatal
# and the bad file is removed.

set -euo pipefail

DEST_DIR="$(cd "$(dirname "$0")" && pwd)/z-data"

# name | output file | sha256 | url
#
# NOTE: these are retail binaries used ONLY as differential-testing
# oracles for zork-host.  The shippable MIT game builds live in
# games/<slug>/, built from checked-in MIT sources — see games/README.md.
GAMES=(
  "zork1-oracle|zork1.z3|37084966477dff679282de42974b2077156b1bd68fad92a65d4ea94d8eb64d79|https://raw.githubusercontent.com/historicalsource/zork1/master/COMPILED/zork1.z3"
)

fetch_one() {
  local entry=$1
  local name file sha url
  IFS='|' read -r name file sha url <<<"$entry"
  local out="$DEST_DIR/$file"
  mkdir -p "$DEST_DIR"

  if [[ -f "$out" ]] && \
     [[ "$(sha256sum "$out" | cut -d' ' -f1)" == "$sha" ]]; then
    echo "$name: already present and verified ($file)"
    return 0
  fi

  echo "$name: downloading $url"
  local tmp
  tmp=$(mktemp)
  trap 'rm -f "$tmp"' RETURN
  curl -fsSL --retry 3 -o "$tmp" "$url"

  local got
  got=$(sha256sum "$tmp" | cut -d' ' -f1)
  if [[ "$got" != "$sha" ]]; then
    echo "ERROR: $name checksum mismatch!" >&2
    echo "  expected: $sha" >&2
    echo "  got:      $got" >&2
    rm -f "$tmp"
    return 1
  fi

  mv "$tmp" "$out"
  trap - RETURN
  chmod 644 "$out"
  echo "$name: verified -> $out"
}

if [[ $# -gt 0 ]]; then
  found=0
  for entry in "${GAMES[@]}"; do
    [[ "${entry%%|*}" == "$1" ]] && { fetch_one "$entry"; found=1; }
  done
  [[ $found -eq 1 ]] || { echo "unknown game: $1" >&2; exit 2; }
else
  for entry in "${GAMES[@]}"; do
    fetch_one "$entry"
  done
fi
