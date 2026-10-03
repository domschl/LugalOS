#!/usr/bin/env python3
"""tools/p9sync.py -- Synchronize local files to a LugalOS node over 9P (USB CDC or UART).

Walks a local staging tree (such as build/rp2350-terminal/sd_root) and synchronizes
directories and files to a remote 9P mount on a connected LugalOS target (typically /sd0),
preserving existing files not present in the local source.
"""

from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
import p9lib


def find_default_serial() -> str | None:
    """Find the 9P serial port (USB CDC interface 02) or fall back to /dev/ttyACM2."""
    links = sorted(glob.glob("/dev/serial/by-id/*LugalOS*-if02"))
    if links:
        return os.path.realpath(links[0])
    for cand in ("/dev/ttyACM2", "/dev/ttyACM1"):
        if os.path.exists(cand):
            return cand
    return None


def find_default_src() -> Path | None:
    """Find the most likely staged sd_root directory."""
    candidates = [
        REPO_ROOT / "build" / "rp2350-terminal" / "sd_root",
        REPO_ROOT / "build" / "rp2350" / "sd_root",
        REPO_ROOT / "build" / "esp32p4" / "sd_root",
        REPO_ROOT / "tools" / "sd_root",
    ]
    for cand in candidates:
        if cand.is_dir():
            return cand
    return None


def sync_tree(
    sess: p9lib.Session,
    src_dir: Path,
    dst_prefix: str,
    force: bool = False,
    dry_run: bool = False,
    verbose: bool = False,
) -> tuple[int, int, int, int]:
    """Recursively syncs `src_dir` to `dst_prefix` on the 9P session.

    Returns (dirs_created, files_synced, files_skipped, bytes_written).
    """
    dst_prefix = dst_prefix.rstrip("/")
    dirs_created = 0
    files_synced = 0
    files_skipped = 0
    bytes_written = 0

    def dir_exists(remote_path: str) -> bool:
        try:
            st = sess.stat(remote_path)
            return st.is_dir
        except Exception:
            return False

    def file_stat(remote_path: str) -> tuple[bool, int]:
        try:
            st = sess.stat(remote_path)
            return (not st.is_dir, st.length)
        except Exception:
            return (False, 0)

    # First, collect all directories and files locally
    local_dirs: list[Path] = []
    local_files: list[Path] = []
    for root, dirs, files in os.walk(src_dir):
        root_path = Path(root)
        for d in sorted(dirs):
            local_dirs.append(root_path / d)
        for f in sorted(files):
            local_files.append(root_path / f)

    # Sort directories by depth so parents are created before children
    local_dirs.sort(key=lambda p: len(p.parts))

    # 1. Ensure directories exist remotely
    for d in local_dirs:
        rel = d.relative_to(src_dir).as_posix()
        remote_dir = f"{dst_prefix}/{rel}"
        if dir_exists(remote_dir):
            if verbose:
                print(f"  [DIR OK]  {remote_dir}")
        else:
            print(f"  [MKDIR]   {remote_dir}")
            if not dry_run:
                sess.mkdir(remote_dir)
            dirs_created += 1

    # 2. Transfer files
    for f in sorted(local_files):
        rel = f.relative_to(src_dir).as_posix()
        remote_path = f"{dst_prefix}/{rel}"
        data = f.read_bytes()
        file_len = len(data)

        exists, r_len = file_stat(remote_path)
        if exists and r_len == file_len and not force:
            if verbose:
                print(f"  [SKIP]    {remote_path} (unchanged, {file_len} bytes)")
            files_skipped += 1
            continue

        action = "UPDATE" if exists else "CREATE"
        print(f"  [{action}]  {rel} -> {remote_path} ({file_len} bytes)")
        if not dry_run:
            sess.write(remote_path, data)
        files_synced += 1
        bytes_written += file_len

    return (dirs_created, files_synced, files_skipped, bytes_written)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Synchronize a local directory to a LugalOS node over 9P."
    )
    parser.add_argument(
        "--serial",
        metavar="PORT",
        default=find_default_serial(),
        help="serial port for 9P (default: auto-detected from /dev/serial/by-id/*-if02)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="serial baud rate (default: 115200)",
    )
    parser.add_argument(
        "--src",
        type=Path,
        default=find_default_src(),
        help="local source directory (default: build/rp2350-terminal/sd_root)",
    )
    parser.add_argument(
        "--dst",
        default="/sd0",
        help="remote destination path (default: /sd0)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite remote files even if size matches",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="preview files to sync without making changes",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="print detailed information on skipped items",
    )

    args = parser.parse_args()

    if not args.serial:
        sys.exit("Error: no 9P serial port specified or auto-detected.")
    if not args.src or not args.src.is_dir():
        sys.exit(f"Error: source directory '{args.src}' does not exist.")

    print(f"Connecting to 9P server on {args.serial}...")
    client = p9lib.connect_serial(args.serial, baudrate=args.baud)
    try:
        with p9lib.Session(client, aname="/") as sess:
            print(f"Syncing local '{args.src}' -> remote '{args.dst}' {'[DRY RUN]' if args.dry_run else ''}...")
            dirs, synced, skipped, total_bytes = sync_tree(
                sess,
                args.src,
                args.dst,
                force=args.force,
                dry_run=args.dry_run,
                verbose=args.verbose,
            )
            print("\nSync Summary:")
            print(f"  Directories created: {dirs}")
            print(f"  Files transferred:   {synced} ({total_bytes} bytes)")
            print(f"  Files unchanged:     {skipped}")
    finally:
        client.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
