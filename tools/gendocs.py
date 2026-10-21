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
    "doc/user/calculus.md": "tools/sd_root/help/calculus.gmi",
    "doc/user/algebra.md": "tools/sd_root/help/algebra.gmi",
    "doc/user/plot.md": "tools/sd_root/help/plot.gmi",
    "doc/user/demos.md": "tools/sd_root/help/demos.gmi",
    "doc/user/ea.md": "tools/sd_root/help/ea.gmi",
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


def rewrite_markdown_for_help(src_text: str) -> str:
    """Rewrites cross-document links in markdown to flat help namespace."""
    pattern = re.compile(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)')

    def replacer(m: re.Match) -> str:
        anchor = m.group(1).strip()
        url = m.group(2).strip()

        # External URLs or non-doc links
        if url.startswith(("http://", "https://", "spartan://", "mailto:")):
            return m.group(0)

        # In-document anchors like #section
        if url.startswith("#"):
            return m.group(0)

        # Cross-document link
        parts = url.split("#", 1)
        doc_path = parts[0]
        hash_part = f"#{parts[1]}" if len(parts) > 1 else ""

        if doc_path.endswith(".md"):
            stem = Path(doc_path).stem.lower()
            if stem == "lisp_scripting":
                stem = "lisp"
            return f"[{anchor}]({stem}.md{hash_part})"

        return m.group(0)

    return pattern.sub(replacer, src_text)


def validate_markdown_no_html(src_rel: str, src_text: str) -> bool:
    """Validates that markdown source does not contain raw HTML layout tags."""
    raw_tags = re.findall(r'<([a-zA-Z/][^>]*)>', src_text)
    known_placeholders = {
        'topic', 'target', 'target', 'path', 'story', 'filename', 'host', 'status',
        'meta', 'body', 'name', 'driver', 'who', 'lock', 'protocol', 'why', 'preset',
        'handler', 'ms', 'addr', 'reg', 'n', 'byte-list', 'pin', 'val', 'string',
        'elf-path', 'part', 'channel', 'metric_id', 'min_interval_s', 'max_interval_s',
        'delta', 'file', 'src', 'dst', 'vol', 'pat', 'dir', 'elf', 'lugal.h'
    }
    violations = []
    for tag in raw_tags:
        tag_name = tag.lstrip('/').split()[0].lower()
        if tag_name not in known_placeholders:
            violations.append(tag)
    if violations:
        print(f"[HTML VIOLATION] {src_rel} contains raw HTML tags: {violations}")
        return False
    return True


def generate_all(repo_root: Path, check_only: bool = False) -> bool:
    """Generates all configured help files. If check_only=True, verifies parity without writing."""
    all_ok = True
    for src_rel, gmi_rel in DOC_MAPPINGS.items():
        src_path = repo_root / src_rel
        gmi_path = repo_root / gmi_rel

        stem = Path(gmi_rel).stem
        md_rel = f"tools/sd_root/help/{stem}.md"
        md_path = repo_root / md_rel

        if not src_path.exists():
            continue

        src_text = src_path.read_text(encoding="utf-8")

        # 1. HTML validation
        if not validate_markdown_no_html(src_rel, src_text):
            all_ok = False

        # 2. Markdown output with rewritten cross-doc links
        md_out = rewrite_markdown_for_help(src_text)

        # 3. Gemtext output
        gmi_out = convert_markdown_to_gemtext(src_text)

        if check_only:
            # Verify Markdown file
            if not md_path.exists():
                print(f"[DRIFT] Missing generated file: {md_rel}")
                all_ok = False
            else:
                existing_md = md_path.read_text(encoding="utf-8")
                if existing_md != md_out:
                    print(f"[DRIFT] Markdown file content mismatch: {md_rel}")
                    all_ok = False

            # Verify Gemtext file
            if not gmi_path.exists():
                print(f"[DRIFT] Missing generated file: {gmi_rel}")
                all_ok = False
            else:
                existing_gmi = gmi_path.read_text(encoding="utf-8")
                if existing_gmi != gmi_out:
                    print(f"[DRIFT] Gemtext file content mismatch: {gmi_rel}")
                    all_ok = False
        else:
            # Write help/*.md
            md_path.parent.mkdir(parents=True, exist_ok=True)
            md_path.write_text(md_out, encoding="utf-8")

            # Write help/*.gmi
            gmi_path.parent.mkdir(parents=True, exist_ok=True)
            gmi_path.write_text(gmi_out, encoding="utf-8")

            # Mirror to pub/help for Spartan capsule
            pub_md = repo_root / f"tools/sd_root/pub/help/{stem}.md"
            pub_md.parent.mkdir(parents=True, exist_ok=True)
            pub_md.write_text(md_out, encoding="utf-8")

            pub_gmi = repo_root / f"tools/sd_root/pub/help/{stem}.gmi"
            pub_gmi.parent.mkdir(parents=True, exist_ok=True)
            pub_gmi.write_text(gmi_out, encoding="utf-8")

            print(f"[GEN] {src_rel} -> {stem}.md ({len(md_out)} B) & {stem}.gmi ({len(gmi_out)} B)")

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
