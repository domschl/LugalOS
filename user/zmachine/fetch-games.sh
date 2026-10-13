#!/usr/bin/env bash
# fetch-games.sh — download Z-machine story files into additional_games/ and z-data/
#
# Story files are NOT bundled in the LugalOS repository. The ZIL source mirror
# at historicalsource declares an MIT license for the source code; the compiled
# Infocom story binaries are distributed separately for personal, educational,
# and testing use. To keep provenance unambiguous, we record canonical URLs and
# SHA-256 fingerprints here, and users fetch them locally.
#
# Usage:
#   ./fetch-games.sh                  fetch all additional games to additional_games/
#   ./fetch-games.sh all              fetch additional games AND zork1-oracle
#   ./fetch-games.sh <game>           fetch a single game (e.g. planetfall, enchanter)
#   ./fetch-games.sh oracle           fetch zork1 oracle binary to z-data/
#   ./fetch-games.sh --verify         verify compatibility of local story files
#   ./fetch-games.sh --sync-sd        sync additional_games/ to /sd0/games on connected board
#   ./fetch-games.sh --list           list all available games
#
# Files in additional_games/ and z-data/ are git-ignored and never bundled in
# release images. Existing files with matching checksums are preserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
P9LIB_DIR="$REPO_ROOT/host/p9lib"
ADDITIONAL_DIR="$SCRIPT_DIR/additional_games"
ZDATA_DIR="$SCRIPT_DIR/z-data"
PYTHON_BIN=""

# Format: name | relative_path | sha256 | url | description
GAMES=(
  # Differential testing oracle for zork-host (r119)
  "zork1-oracle|z-data/zork1.z3|37084966477dff679282de42974b2077156b1bd68fad92a65d4ea94d8eb64d79|https://raw.githubusercontent.com/historicalsource/zork1/master/COMPILED/zork1.z3|Zork I: The Great Underground Empire (Oracle r119)"

  # Additional Infocom titles from historicalsource
  "planetfall|additional_games/planetfall.z3|94add8ed336846aad0130cd3db08a83f6e38f0a2180412b25432b6cfb8ccf80e|https://raw.githubusercontent.com/historicalsource/planetfall/master/COMPILED/planetfall.z3|Planetfall (1983, Steve Meretzky) - Sci-Fi Comedy [v3]"
  "hitchhiker|additional_games/hitchhiker.z3|579599e82e2c0d1cef73ec8db9d74ded1d7868f8d85d88d65c2fbe6abd979421|https://raw.githubusercontent.com/historicalsource/hitchhikersguide/master/COMPILED/s4.z3|The Hitchhiker's Guide to the Galaxy (1984, Douglas Adams) - Satirical Sci-Fi [v3]"
  "enchanter|additional_games/enchanter.z3|bc9b99be980a1aa67c1aef89d4e5878f1f03edab62fe43ae23b2f7e2c41b1dc8|https://raw.githubusercontent.com/historicalsource/enchanter/master/COMPILED/enchanter.z3|Enchanter (Zork IV) (1983, Marc Blank & Dave Lebling) - Fantasy Magic [v3]"
  "starcross|additional_games/starcross.z3|c7dac463c3a51170f82d68e389202a9ab7cc385ebf2239b83e721986e9522768|https://raw.githubusercontent.com/historicalsource/starcross/master/COMPILED/starcross.z3|Starcross (1982, Dave Lebling) - Hard Sci-Fi [v3]"
  "stationfall|additional_games/stationfall.z3|55983c43da044e4b49206012c0fe536fbcd9aa1f291f60431d02b836e8cc9094|https://raw.githubusercontent.com/historicalsource/stationfall/master/COMPILED/s6.z3%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20%20|Stationfall (1987, Steve Meretzky) - Sci-Fi Adventure [v3]"
)

resolve_alias() {
  local target="$1"
  case "$target" in
    hitchhikers|hitchhikersguide|hhgg) echo "hitchhiker" ;;
    zork4|ench)                       echo "enchanter" ;;
    oracle)                           echo "zork1-oracle" ;;
    *)                                echo "$target" ;;
  esac
}

list_games() {
  echo "Available Z-Machine Games from historicalsource:"
  echo "------------------------------------------------"
  printf "%-14s %-24s %s\n" "Identifier" "File" "Description"
  echo "------------------------------------------------"
  for entry in "${GAMES[@]}"; do
    local name file sha url desc
    IFS='|' read -r name file sha url desc <<<"$entry"
    printf "%-14s %-24s %s\n" "$name" "$file" "$desc"
  done
  echo "------------------------------------------------"
}

fetch_one() {
  local entry=$1
  local name file sha url desc
  IFS='|' read -r name file sha url desc <<<"$entry"
  local out="$SCRIPT_DIR/$file"
  local dest_dir
  dest_dir="$(dirname "$out")"
  mkdir -p "$dest_dir"

  if [[ -f "$out" ]] && \
     [[ "$(sha256sum "$out" | cut -d' ' -f1)" == "$sha" ]]; then
    echo "  [OK]   $name: already present ($file)"
    return 0
  fi

  echo "  [GET]  $name: downloading from historicalsource..."
  local tmp
  tmp=$(mktemp)
  trap 'rm -f "$tmp"' RETURN
  if ! curl -fsSL --retry 3 -o "$tmp" "$url"; then
    echo "ERROR: Failed to download $name from $url" >&2
    rm -f "$tmp"
    return 1
  fi

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
  echo "  [DONE] $name: verified -> $file"
}

ensure_python_env() {
  local mode="${1:-}"
  if [[ -n "$PYTHON_BIN" ]]; then
    return 0
  fi

  local p9_venv="$P9LIB_DIR/.venv/bin/python"
  local root_venv="$REPO_ROOT/.venv/bin/python"

  # 1. Check existing virtual environment in host/p9lib
  if [[ -x "$p9_venv" ]] && "$p9_venv" -c "import serial" 2>/dev/null; then
    PYTHON_BIN="$p9_venv"
    return 0
  fi

  # 2. Check existing virtual environment in repository root
  if [[ -x "$root_venv" ]] && "$root_venv" -c "import serial" 2>/dev/null; then
    PYTHON_BIN="$root_venv"
    return 0
  fi

  # 3. If uv is available, initialize/sync host/p9lib virtual environment
  if command -v uv >/dev/null 2>&1; then
    if [[ "$mode" != "quiet" ]]; then
      echo "Initializing Python environment for 9P utilities via uv (host/p9lib)..." >&2
    fi
    if uv sync --project "$P9LIB_DIR" >&2; then
      if [[ -x "$p9_venv" ]] && "$p9_venv" -c "import serial" 2>/dev/null; then
        PYTHON_BIN="$p9_venv"
        return 0
      fi
    fi
  fi

  # 4. Fall back to system python3 if pyserial is installed
  if command -v python3 >/dev/null 2>&1 && python3 -c "import serial" 2>/dev/null; then
    PYTHON_BIN="python3"
    return 0
  fi

  # 5. Missing environment: silent for background probes, actionable error otherwise
  if [[ "$mode" == "quiet" ]]; then
    return 1
  fi

  echo "ERROR: A Python environment with 'pyserial' is required for 9P file transmission." >&2
  if ! command -v uv >/dev/null 2>&1; then
    echo "  'uv' is not installed or not in PATH." >&2
    echo "  To install uv, see https://docs.astral.sh/uv/ or run:" >&2
    echo "      curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
    echo "  Then re-run this script to automatically initialize the environment." >&2
    echo "  Alternatively, create a virtual environment in host/p9lib or install 'pyserial':" >&2
    echo "      python3 -m venv host/p9lib/.venv && host/p9lib/.venv/bin/pip install pyserial" >&2
  else
    echo "  Failed to initialize host/p9lib virtual environment with 'uv sync --project host/p9lib'." >&2
  fi
  return 1
}

check_connected_board() {
  local mode="${1:-}"
  if ! ensure_python_env "$mode"; then
    return 1
  fi
  "$PYTHON_BIN" -c "import sys; sys.path.insert(0, '$REPO_ROOT/tools'); import p9sync; p = p9sync.find_default_serial(); sys.exit(0 if p else 1)" 2>/dev/null
}

sync_to_sd() {
  echo "Synchronizing additional games and README to connected LugalOS board..."
  if ! ensure_python_env; then
    return 1
  fi
  if ! check_connected_board; then
    echo "ERROR: No connected LugalOS 9P interface detected." >&2
    echo "Ensure board is attached via USB (CDC-ACM interface 02)." >&2
    return 1
  fi
  "$PYTHON_BIN" "$REPO_ROOT/tools/p9sync.py" --src "$ADDITIONAL_DIR" --dst /sd0/games
}

verify_compat() {
  echo "Verifying game compatibility with LugalOS Z-Machine & Auto-Mapper..."
  ensure_python_env quiet || true
  "${PYTHON_BIN:-python3}" "$REPO_ROOT/tests/test_zmachine_compat.py"
}

# --- Argument dispatch ---

fetch_additional() {
  for entry in "${GAMES[@]}"; do
    local name="${entry%%|*}"
    [[ "$name" == "zork1-oracle" ]] && continue
    fetch_one "$entry"
  done
}

if [[ $# -eq 0 ]]; then
  echo "Fetching additional games into user/zmachine/additional_games/..."
  fetch_additional
  echo ""
  echo "Download complete."

  if check_connected_board quiet; then
    echo ""
    read -r -p "Connected LugalOS board detected. Sync games to SD card (/sd0/games)? [y/N]: " ans || ans="n"
    if [[ "$ans" =~ ^[Yy]$ ]]; then
      sync_to_sd
    fi
  fi
  exit 0
fi

case "$1" in
  --help|-h)
    echo "Usage: ./fetch-games.sh [command | game_name] [options]"
    echo ""
    echo "Commands:"
    echo "  (none)        Download all additional games to additional_games/"
    echo "  all           Download all additional games AND zork1-oracle"
    echo "  oracle        Download zork1-oracle to z-data/"
    echo "  <game_name>   Download a specific game (e.g. planetfall, hitchhiker, enchanter)"
    echo "  --sync-sd     Sync additional_games/ to /sd0/games on connected board over 9P"
    echo "  --verify      Verify compatibility with LugalOS Z-Machine & Auto-Mapper"
    echo "  --list, -l    List all available games and descriptions"
    exit 0
    ;;
  --list|-l)
    list_games
    exit 0
    ;;
  --verify)
    verify_compat
    exit 0
    ;;
  --sync-sd)
    sync_to_sd
    exit 0
    ;;
  all)
    echo "Fetching all games (oracle + additional games)..."
    for entry in "${GAMES[@]}"; do
      fetch_one "$entry"
    done
    exit 0
    ;;
  oracle|zork1-oracle)
    for entry in "${GAMES[@]}"; do
      [[ "${entry%%|*}" == "zork1-oracle" ]] && { fetch_one "$entry"; exit 0; }
    done
    ;;
  additional|games)
    echo "Fetching all additional games..."
    fetch_additional
    exit 0
    ;;
  *)
    target=$(resolve_alias "$1")
    found=0
    for entry in "${GAMES[@]}"; do
      if [[ "${entry%%|*}" == "$target" ]]; then
        fetch_one "$entry"
        found=1
        break
      fi
    done
    if [[ $found -eq 0 ]]; then
      echo "ERROR: Unknown game '$1'" >&2
      echo "Run './fetch-games.sh --list' to see available titles." >&2
      exit 2
    fi
    exit 0
    ;;
esac
