#!/usr/bin/env python3
"""tools/gendocs.py — LugalOS Markdown to Gemtext Help Generator.

Translates richly formatted GitHub-flavored Markdown documents from doc/
into clean Gemtext (text/gemini, .gmi) files placed in tools/sd_root/help/.
Extracts Markdown hyperlinks into explicit Gemtext link lines (=> target [label])
so that on-device hypertext browsers (gem, man, Ribbon Reader) can navigate
unambiguously between topics.

Usage:
    python3 tools/gendocs.py --all
    python3 tools/gendocs.py --check
    python3 tools/gendocs.py <input.md> <output.gmi>
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


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
    """Extracts markdown links [text](url), strips to anchor text in prose and registers target for Gemtext => links."""
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
                target_gmi = f"{stem}.gmi"
                if not any(t[0] == target_gmi for t in link_registry) and len(link_registry) < 9:
                    link_registry.append((target_gmi, anchor))

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


def convert_markdown_to_gemtext(md_content: str) -> str:
    """Converts a Markdown string to clean Gemtext (text/gemini) format."""
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
            out_lines.append(line.strip())
            i += 1
            continue

        if in_code_block:
            out_lines.append(line)
            i += 1
            continue

        # Blank line
        if not line.strip():
            out_lines.append("")
            i += 1
            continue

        # Horizontal rules -> empty separator
        if re.match(r'^(\*{3,}|-{3,}|_{3,})$', line.strip()):
            out_lines.append("")
            i += 1
            continue

        # Headers (#, ##, ###)
        header_match = re.match(r'^(#{1,6})\s+(.*)', line)
        if header_match:
            level = min(len(header_match.group(1)), 3)
            htext = header_match.group(2).strip()
            htext = strip_markdown_formatting(clean_markdown_links(htext, link_registry))
            out_lines.append(f"{'#' * level} {htext}")
            i += 1
            continue

        # Blockquote
        if line.startswith(">"):
            quote_text = line.lstrip("> ").strip()
            quote_text = strip_markdown_formatting(clean_markdown_links(quote_text, link_registry))
            out_lines.append(f"> {quote_text}")
            i += 1
            continue

        # Table rows -> wrap in preformatted block
        if line.strip().startswith("|") and line.strip().endswith("|"):
            table_lines: list[str] = []
            while i < len(lines) and lines[i].strip().startswith("|") and lines[i].strip().endswith("|"):
                row = lines[i].strip()
                row = strip_markdown_formatting(clean_markdown_links(row, link_registry))
                table_lines.append(row)
                i += 1
            out_lines.append("```")
            out_lines.extend(table_lines)
            out_lines.append("```")
            continue

        # Bullet list item
        bullet_match = re.match(r'^(\s*)[*+-]\s+(.*)', line)
        if bullet_match:
            item_text = bullet_match.group(2).strip()
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry))
            out_lines.append(f"* {item_text}")
            i += 1
            continue

        # Numbered list item
        num_match = re.match(r'^(\s*)(\d+)\.\s+(.*)', line)
        if num_match:
            num_str = num_match.group(2)
            item_text = num_match.group(3).strip()
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry))
            out_lines.append(f"{num_str}. {item_text}")
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
        out_lines.append(full_para)

    # Append Related Topics section if any links were registered
    if link_registry:
        out_lines.append("")
        out_lines.append("### Related Topics")
        for t_target, t_anchor in link_registry:
            out_lines.append(f"=> {t_target} {t_anchor}")

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
    "doc/user/shell.md": "tools/sd_root/help/shell.gmi",
    "doc/user/editors.md": "tools/sd_root/help/editors.gmi",
    "doc/user/ribbon.md": "tools/sd_root/help/ribbon.gmi",
    "doc/user/chess.md": "tools/sd_root/help/chess.gmi",
    "doc/user/cas.md": "tools/sd_root/help/cas.gmi",
    "doc/user/sensors.md": "tools/sd_root/help/sensors.gmi",
    "doc/user/zmachine.md": "tools/sd_root/help/zmachine.gmi",
    "doc/admin/presets.md": "tools/sd_root/help/presets.gmi",
    "doc/admin/flashing.md": "tools/sd_root/help/flashing.gmi",
    "doc/admin/networking.md": "tools/sd_root/help/networking.gmi",
    "doc/admin/updates.md": "tools/sd_root/help/updates.gmi",
    "doc/dev/on_device/c_programming.md": "tools/sd_root/help/c_programming.gmi",
    "doc/dev/on_device/lisp_scripting.md": "tools/sd_root/help/lisp.gmi",
    "doc/user/reader.md": "tools/sd_root/help/reader.gmi",
    "doc/user/spartan.md": "tools/sd_root/help/spartan.gmi",
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
        generated = convert_markdown_to_gemtext(src_text)

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
            
            # Mirror to pub/help for Spartan capsule
            pub_dst = repo_root / "tools/sd_root/pub/help" / dst_path.name
            pub_dst.parent.mkdir(parents=True, exist_ok=True)
            pub_dst.write_text(generated, encoding="utf-8")

    return all_ok


def main() -> int:
    parser = argparse.ArgumentParser(description="LugalOS Markdown to Gemtext Help Generator")
    parser.add_argument("--all", action="store_true", help="Generate all configured help topics")
    parser.add_argument("--check", action="store_true", help="Verify that generated help matches markdown")
    parser.add_argument("src", nargs="?", help="Input markdown file")
    parser.add_argument("dst", nargs="?", help="Output text/gmi file")

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
        out = convert_markdown_to_gemtext(content)
        dst_path.parent.mkdir(parents=True, exist_ok=True)
        dst_path.write_text(out, encoding="utf-8")
        print(f"Generated {dst_path} from {src_path}")
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
