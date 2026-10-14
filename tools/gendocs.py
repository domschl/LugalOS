#!/usr/bin/env python3
"""tools/gendocs.py — LugalOS Markdown to Plain-Text Help Generator.

Translates richly formatted GitHub-flavored Markdown documents from doc/
into clean, 78-column plain-text help files placed in tools/sd_root/help/.
Extracts Markdown hyperlinks into numbered references [1]..[9] so that
the on-device hypertext help browser (man/help) can jump between topics.

Usage:
    python3 tools/gendocs.py --all
    python3 tools/gendocs.py --check
    python3 tools/gendocs.py <input.md> <output.txt>
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import textwrap
from pathlib import Path


DEFAULT_LINE_WIDTH = 78


def strip_markdown_images(text: str) -> str:
    """Replaces ![alt](url) with [Illustration: alt] or empty string if no alt."""
    pattern = re.compile(r'!\[([^\]]*)\]\(([^)]+)\)')

    def replacer(m: re.Match) -> str:
        alt = m.group(1).strip()
        if alt:
            return f"[Illustration: {alt}]"
        return ""

    return pattern.sub(replacer, text)


def clean_markdown_links(text: str, link_registry: list[tuple[str, str]]) -> str:
    """Extracts markdown links [text](url), replaces with 'text [N]' if link is internal topic."""
    text = strip_markdown_images(text)
    pattern = re.compile(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)')

    non_doc_exts = (
        ".png", ".jpg", ".jpeg", ".gif", ".svg", ".bmp", ".webp",
        ".pdf", ".uf2", ".bin", ".hex", ".elf", ".iso", ".img",
    )

    def replacer(m: re.Match) -> str:
        anchor = m.group(1).strip()
        url = m.group(2).strip()

        # Exclude binary media assets and external urls
        if any(url.lower().endswith(ext) for ext in non_doc_exts) or url.startswith("http"):
            return anchor

        # If it's a link to another doc (.md) or external topic (not internal anchor #...)
        if (url.endswith(".md") or ("/" in url and not url.startswith("#"))) and not url.startswith("http"):
            stem = Path(url).stem.lower()
            if stem and stem not in ("readme", "index") and not stem.startswith("#"):
                target_idx = None
                for idx, (t_stem, _) in enumerate(link_registry, start=1):
                    if t_stem == stem:
                        target_idx = idx
                        break
                if target_idx is None and len(link_registry) < 9:
                    link_registry.append((stem, anchor))
                    target_idx = len(link_registry)

                if target_idx is not None:
                    return f"{anchor} [{target_idx}]"

        return anchor

    return pattern.sub(replacer, text)


def strip_markdown_formatting(text: str) -> str:
    """Strips bold, italic, inline code, and HTML tags."""
    text = re.sub(r'<[^>]+>', '', text)
    text = re.sub(r'`([^`]+)`', r'\1', text)
    text = re.sub(r'\*\*\*([^*]+)\*\*\*', r'\1', text)
    text = re.sub(r'\*\*([^*]+)\*\*', r'\1', text)
    text = re.sub(r'\*([^*]+)\*', r'\1', text)
    text = re.sub(r'___([^_]+)___', r'\1', text)
    text = re.sub(r'__([^_]+)__', r'\1', text)
    text = re.sub(r'_([^_]+)_', r'\1', text)
    text = re.sub(r'\$\$([^$]+)\$\$', r'\1', text)
    text = re.sub(r'\$([^$]+)\$', r'\1', text)
    return text


def convert_markdown_to_text(md_content: str, width: int = DEFAULT_LINE_WIDTH) -> str:
    """Converts a Markdown string to formatted terminal-friendly plain text."""
    lines = md_content.splitlines()
    out_lines: list[str] = []
    in_code_block = False
    link_registry: list[tuple[str, str]] = []

    i = 0
    while i < len(lines):
        line = lines[i]

        # Fenced code blocks
        if line.strip().startswith("```"):
            in_code_block = not in_code_block
            i += 1
            continue

        if in_code_block:
            out_lines.append("    " + line)
            i += 1
            continue

        # Blank line
        if not line.strip():
            out_lines.append("")
            i += 1
            continue

        # Horizontal rules
        if re.match(r'^(\*{3,}|-{3,}|_{3,})$', line.strip()):
            out_lines.append("-" * width)
            i += 1
            continue

        # Headers
        header_match = re.match(r'^(#{1,6})\s+(.*)', line)
        if header_match:
            level = len(header_match.group(1))
            htext = header_match.group(2).strip()
            htext = strip_markdown_formatting(clean_markdown_links(htext, link_registry))
            out_lines.append("")
            if level == 1:
                out_lines.append("=" * min(len(htext), width))
                out_lines.append(htext.upper())
                out_lines.append("=" * min(len(htext), width))
            elif level == 2:
                out_lines.append(htext.upper())
                out_lines.append("-" * min(len(htext), width))
            else:
                out_lines.append(f"[{htext}]")
            out_lines.append("")
            i += 1
            continue

        # Blockquote
        if line.startswith(">"):
            quote_text = line.lstrip("> ").strip()
            quote_text = strip_markdown_formatting(clean_markdown_links(quote_text, link_registry))
            wrapped = textwrap.wrap(quote_text, width=width - 4)
            for wl in wrapped:
                out_lines.append("  | " + wl)
            i += 1
            continue

        # Table rows
        if line.strip().startswith("|") and line.strip().endswith("|"):
            cells = [c.strip() for c in line.strip().split("|")[1:-1]]
            # If separator row (---)
            if all(re.match(r'^:?-+:?$', c) for c in cells if c):
                out_lines.append("  " + "-" * (width - 4))
            else:
                cleaned_cells = [strip_markdown_formatting(clean_markdown_links(c, link_registry)) for c in cells]
                row_str = " | ".join(cleaned_cells)
                out_lines.append("  " + row_str)
            i += 1
            continue

        # Bullet list item
        bullet_match = re.match(r'^(\s*)[*+-]\s+(.*)', line)
        if bullet_match:
            indent = len(bullet_match.group(1))
            item_text = bullet_match.group(2).strip()
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry))
            prefix = " " * indent + "  * "
            subsequent_indent = " " * (indent + 4)
            wrapped = textwrap.wrap(item_text, width=width - len(prefix),
                                    initial_indent=prefix,
                                    subsequent_indent=subsequent_indent)
            out_lines.extend(wrapped)
            i += 1
            continue

        # Numbered list item
        num_match = re.match(r'^(\s*)(\d+)\.\s+(.*)', line)
        if num_match:
            indent = len(num_match.group(1))
            num_str = num_match.group(2)
            item_text = num_match.group(3).strip()
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry))
            prefix = " " * indent + f" {num_str}. "
            subsequent_indent = " " * (indent + len(num_str) + 3)
            wrapped = textwrap.wrap(item_text, width=width - len(prefix),
                                    initial_indent=prefix,
                                    subsequent_indent=subsequent_indent)
            out_lines.extend(wrapped)
            i += 1
            continue

        # General paragraph: gather until blank line or special line marker
        para_lines = [line.strip()]
        i += 1
        while i < len(lines):
            curr = lines[i]
            if not curr.strip():
                break
            if curr.strip().startswith(("```", "#", ">", "|")):
                break
            if re.match(r'^(\s*)[*+-]\s+', curr):
                break
            if re.match(r'^\s*\d+\.\s+', curr):
                break
            if re.match(r'^(\*{3,}|-{3,}|_{3,})$', curr.strip()):
                break
            para_lines.append(curr.strip())
            i += 1

        full_para = " ".join(para_lines)
        full_para = strip_markdown_formatting(clean_markdown_links(full_para, link_registry))
        wrapped = textwrap.wrap(full_para, width=width)
        out_lines.extend(wrapped)

    # Append References / SEE ALSO section if any links were registered
    if link_registry:
        out_lines.append("")
        out_lines.append("-" * width)
        out_lines.append("SEE ALSO (Press 1..9 to jump, 'b' to go back):")
        for idx, (t_stem, t_anchor) in enumerate(link_registry, start=1):
            out_lines.append(f"  [{idx}] {t_stem:<16} - {t_anchor}")

    # Normalize excessive blank lines
    res: list[str] = []
    prev_blank = False
    for ol in out_lines:
        if not ol.strip():
            if not prev_blank:
                res.append("")
                prev_blank = True
        else:
            res.append(ol)
            prev_blank = False

    return "\n".join(res).strip() + "\n"


DOC_MAPPINGS: dict[str, str] = {
    "doc/user/shell.md": "tools/sd_root/help/shell.txt",
    "doc/user/editors.md": "tools/sd_root/help/editors.txt",
    "doc/user/ribbon.md": "tools/sd_root/help/ribbon.txt",
    "doc/user/chess.md": "tools/sd_root/help/chess.txt",
    "doc/user/cas.md": "tools/sd_root/help/cas.txt",
    "doc/user/sensors.md": "tools/sd_root/help/sensors.txt",
    "doc/user/zmachine.md": "tools/sd_root/help/zmachine.txt",
    "doc/admin/presets.md": "tools/sd_root/help/presets.txt",
    "doc/admin/flashing.md": "tools/sd_root/help/flashing.txt",
    "doc/admin/networking.md": "tools/sd_root/help/networking.txt",
    "doc/admin/updates.md": "tools/sd_root/help/updates.txt",
    "doc/dev/on_device/c_programming.md": "tools/sd_root/help/c_programming.txt",
    "doc/dev/on_device/lisp_scripting.md": "tools/sd_root/help/lisp.txt",
}


def generate_all(repo_root: Path, check_only: bool = False) -> bool:
    """Generates all configured help files. If check_only=True, verifies parity without writing."""
    all_ok = True
    for src_rel, dst_rel in DOC_MAPPINGS.items():
        src_path = repo_root / src_rel
        dst_path = repo_root / dst_rel

        if not src_path.exists():
            continue

        src_text = src_path.read_text(encoding="utf-8")
        generated = convert_markdown_to_text(src_text)

        if check_only:
            if not dst_path.exists():
                print(f"[DRIFT] Missing generated file: {dst_rel}")
                all_ok = False
            else:
                existing = dst_path.read_text(encoding="utf-8")
                if existing != generated:
                    print(f"[DRIFT] File content mismatch: {dst_rel}")
                    all_ok = False
        else:
            dst_path.parent.mkdir(parents=True, exist_ok=True)
            dst_path.write_text(generated, encoding="utf-8")
            print(f"[GEN] {src_rel} -> {dst_rel} ({len(generated)} bytes)")

    return all_ok


def main() -> int:
    parser = argparse.ArgumentParser(description="LugalOS Markdown to Terminal Help Generator")
    parser.add_argument("--all", action="store_true", help="Generate all configured help topics")
    parser.add_argument("--check", action="store_true", help="Verify that generated help matches markdown")
    parser.add_argument("src", nargs="?", help="Input markdown file")
    parser.add_argument("dst", nargs="?", help="Output text file")

    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parent.parent

    if args.check:
        ok = generate_all(repo_root, check_only=True)
        return 0 if ok else 1

    if args.all:
        generate_all(repo_root, check_only=False)
        return 0

    if args.src and args.dst:
        src_path = Path(args.src)
        dst_path = Path(args.dst)
        content = src_path.read_text(encoding="utf-8")
        out = convert_markdown_to_text(content)
        dst_path.parent.mkdir(parents=True, exist_ok=True)
        dst_path.write_text(out, encoding="utf-8")
        print(f"Generated {dst_path} from {src_path}")
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
