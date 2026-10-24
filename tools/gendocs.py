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


def get_help_stem_for_doc(url: str, src_rel: str = "") -> str | None:
    """Resolves a Markdown link URL to its flat on-device help stem."""
    url_clean = url.split("#", 1)[0].strip()
    if not url_clean:
        return None
    p_lower = url_clean.lower().replace("\\", "/")

    if src_rel:
        src_parent = Path(src_rel).parent.as_posix().lower()
        norm = os.path.normpath(f"{src_parent}/{url_clean}").lower().replace("\\", "/")
    else:
        norm = p_lower

    if norm.endswith(("doc/readme.md", "readme.md")):
        return "index"
    if norm.endswith("user/index.md"):
        return "user"
    if norm.endswith("admin/index.md"):
        return "admin"
    if norm.endswith("dev/index.md"):
        return "dev"
    if norm.endswith("boards/index.md"):
        return "boards"
    if norm.endswith("adr/index.md"):
        return "adr"
    if norm.endswith("lisp_scripting.md"):
        return "lisp"
    if norm.endswith(".md"):
        return Path(norm).stem
    return None


def clean_markdown_links(text: str, link_registry: list[tuple[str, str]], src_rel: str = "") -> str:
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
        if any(url.lower().endswith(ext) for ext in non_doc_exts) or url.startswith(("http://", "https://", "spartan://", "mailto:", "file://")):
            return anchor

        # In-document anchor
        if url.startswith("#"):
            return anchor

        stem = get_help_stem_for_doc(url, src_rel)
        if stem:
            target_gmi = f"{stem}.gmi"
            if not any(t[0] == target_gmi for t in link_registry) and len(link_registry) < 12:
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


def convert_markdown_to_gemtext(md_content: str, src_rel: str = "") -> str:
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
            htext = strip_markdown_formatting(clean_markdown_links(htext, link_registry, src_rel))
            out_lines.append(f"{'#' * level} {htext}")
            i += 1
            continue

        # Blockquote
        if line.startswith(">"):
            quote_text = line.lstrip("> ").strip()
            quote_text = strip_markdown_formatting(clean_markdown_links(quote_text, link_registry, src_rel))
            out_lines.append(f"> {quote_text}")
            i += 1
            continue

        # Table rows -> wrap in preformatted block
        if line.strip().startswith("|") and line.strip().endswith("|"):
            table_lines: list[str] = []
            while i < len(lines) and lines[i].strip().startswith("|") and lines[i].strip().endswith("|"):
                row = lines[i].strip()
                row = strip_markdown_formatting(clean_markdown_links(row, link_registry, src_rel))
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
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry, src_rel))
            out_lines.append(f"* {item_text}")
            i += 1
            continue

        # Numbered list item
        num_match = re.match(r'^(\s*)(\d+)\.\s+(.*)', line)
        if num_match:
            num_str = num_match.group(2)
            item_text = num_match.group(3).strip()
            item_text = strip_markdown_formatting(clean_markdown_links(item_text, link_registry, src_rel))
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
        full_para = strip_markdown_formatting(clean_markdown_links(full_para, link_registry, src_rel))
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
    # Portals & Master Index
    "doc/README.md": "tools/sd_root/help/index.gmi",
    "doc/about.md": "tools/sd_root/help/about.gmi",
    "doc/user/index.md": "tools/sd_root/help/user.gmi",
    "doc/admin/index.md": "tools/sd_root/help/admin.gmi",
    "doc/dev/index.md": "tools/sd_root/help/dev.gmi",

    # User Manual Chapters
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
    "doc/user/reader.md": "tools/sd_root/help/reader.gmi",
    "doc/user/spartan.md": "tools/sd_root/help/spartan.gmi",

    # Administrator Manual Chapters
    "doc/admin/presets.md": "tools/sd_root/help/presets.gmi",
    "doc/admin/flashing.md": "tools/sd_root/help/flashing.gmi",
    "doc/admin/consoles.md": "tools/sd_root/help/consoles.gmi",
    "doc/admin/networking.md": "tools/sd_root/help/networking.gmi",
    "doc/admin/identity.md": "tools/sd_root/help/identity.gmi",
    "doc/admin/storage.md": "tools/sd_root/help/storage.gmi",
    "doc/admin/updates.md": "tools/sd_root/help/updates.gmi",

    # Board Reference Guides
    "doc/admin/boards/index.md": "tools/sd_root/help/boards.gmi",
    "doc/admin/boards/esp32c6.md": "tools/sd_root/help/esp32c6.gmi",
    "doc/admin/boards/esp32p4_lcd7b.md": "tools/sd_root/help/esp32p4_lcd7b.gmi",
    "doc/admin/boards/esp32p4_tab5.md": "tools/sd_root/help/esp32p4_tab5.gmi",
    "doc/admin/boards/rp2350.md": "tools/sd_root/help/rp2350.gmi",

    # On-Device System Programming Chapters
    "doc/dev/on_device/c_programming.md": "tools/sd_root/help/c_programming.gmi",
    "doc/dev/on_device/lisp_scripting.md": "tools/sd_root/help/lisp.gmi",
}


def rewrite_markdown_for_help(src_text: str, src_rel: str = "") -> str:
    """Rewrites cross-document links in markdown to flat help namespace."""
    pattern = re.compile(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)')

    def replacer(m: re.Match) -> str:
        anchor = m.group(1).strip()
        url = m.group(2).strip()

        # External URLs or non-doc links
        if url.startswith(("http://", "https://", "spartan://", "mailto:", "file://")):
            return m.group(0)

        # In-document anchors like #section
        if url.startswith("#"):
            return m.group(0)

        # Strip links to non-doc source files on host (e.g. tools/gendocs.py)
        if any(url.lower().endswith(ext) for ext in (".py", ".c", ".h", ".sh", ".json")):
            return anchor

        # Cross-document link
        parts = url.split("#", 1)
        doc_path = parts[0]
        hash_part = f"#{parts[1]}" if len(parts) > 1 else ""

        stem = get_help_stem_for_doc(doc_path, src_rel)
        if stem:
            return f"[{anchor}]({stem}.md{hash_part})"

        return m.group(0)

    return pattern.sub(replacer, src_text)


def validate_markdown_no_html(src_rel: str, src_text: str) -> bool:
    """Validates that markdown source does not contain raw HTML layout tags."""
    raw_tags = re.findall(r'<([a-zA-Z/][^>]*)>', src_text)
    known_placeholders = {
        'addr', 'baud', 'board', 'body', 'byte-list', 'c6-zero', 'channel',
        'codepoint', 'delta', 'dir', 'driver', 'dst', 'elf', 'elf-path', 'file',
        'filename', 'handler', 'hex', 'host', 'lock', 'lugal.h', 'max_interval_s',
        'meta', 'metric_id', 'min_interval_s', 'ms', 'n', 'name', 'part', 'pat',
        'path', 'pin', 'port', 'preset', 'protocol', 'reg', 'src', 'status',
        'story', 'string', 'target', 'topic', 'val', 'vol', 'who', 'why'
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


def check_relative_markdown_links(repo_root: Path) -> bool:
    """Verifies that all relative markdown links in doc/ resolve to existing files."""
    md_files = list(repo_root.glob("doc/**/*.md")) + [repo_root / "README.md", repo_root / "AGENTS.md"]
    broken = []
    for md_file in md_files:
        content = md_file.read_text(encoding="utf-8")
        matches = re.findall(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)', content)
        for anchor, url in matches:
            url_clean = url.strip()
            if url_clean.startswith(('http://', 'https://', 'spartan://', 'mailto:', 'file:', '#')):
                continue
            path_part = url_clean.split("#", 1)[0].strip()
            if not path_part:
                continue
            target = (md_file.parent / path_part).resolve()
            if not target.exists():
                broken.append((str(md_file.relative_to(repo_root)), url_clean))
    if broken:
        print(f"[BROKEN LINKS] Found {len(broken)} broken link(s) in markdown files:")
        for src, link in broken:
            print(f"  {src} -> {link}")
        return False
    return True


def check_embedded_font_characters(repo_root: Path) -> bool:
    """Verifies that all markdown source documents use characters supported by embedded fonts (ASCII 0x20-0x7E)."""
    md_files = sorted(list(repo_root.glob("doc/**/*.md")) + [repo_root / "README.md"])
    violations = []
    for md_file in md_files:
        rel_path = str(md_file.relative_to(repo_root))
        with open(md_file, encoding="utf-8") as f:
            for line_no, line in enumerate(f, 1):
                for ch in line:
                    if ch not in "\n\r\t" and not (0x20 <= ord(ch) <= 0x7E):
                        violations.append((rel_path, line_no, ch, ord(ch)))
    if violations:
        print(f"[FONT CHARSET VIOLATION] Found {len(violations)} character(s) outside embedded font range (ASCII 0x20-0x7E):")
        for src, line_no, ch, cp in violations[:20]:
            print(f"  {src}:{line_no}: character '{ch}' (U+{cp:04X})")
        if len(violations) > 20:
            print(f"  ... and {len(violations) - 20} more.")
        return False
    return True


def generate_all(repo_root: Path, check_only: bool = False) -> bool:
    """Generates all configured help files. If check_only=True, verifies parity without writing."""
    all_ok = True

    # 1. Link integrity check
    if not check_relative_markdown_links(repo_root):
        all_ok = False

    # 2. Embedded font character set validation
    if not check_embedded_font_characters(repo_root):
        all_ok = False

    for src_rel, gmi_rel in DOC_MAPPINGS.items():
        src_path = repo_root / src_rel
        gmi_path = repo_root / gmi_rel

        stem = Path(gmi_rel).stem
        md_rel = f"tools/sd_root/help/{stem}.md"
        md_path = repo_root / md_rel

        if not src_path.exists():
            print(f"[MISSING SOURCE] Configured document {src_rel} does not exist!")
            all_ok = False
            continue

        src_text = src_path.read_text(encoding="utf-8")

        # 2. HTML validation
        if not validate_markdown_no_html(src_rel, src_text):
            all_ok = False

        # 3. Markdown output with rewritten cross-doc links
        md_out = rewrite_markdown_for_help(src_text, src_rel)

        # 4. Gemtext output
        gmi_out = convert_markdown_to_gemtext(src_text, src_rel)

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

            # Mirror public topics to pub/help for Spartan capsule (Gemtext protocol)
            spartan_topics = {
                "index", "toc", "about", "shell", "reader", "spartan",
                "editors", "c_programming", "lisp", "networking", "cas"
            }
            if stem in spartan_topics:
                pub_gmi = repo_root / f"tools/sd_root/pub/help/{stem}.gmi"
                pub_gmi.parent.mkdir(parents=True, exist_ok=True)
                pub_gmi.write_text(gmi_out, encoding="utf-8")

            # If this was index.gmi, also mirror as toc.md / toc.gmi
            if stem == "index":
                toc_md = repo_root / "tools/sd_root/help/toc.md"
                toc_gmi = repo_root / "tools/sd_root/help/toc.gmi"
                toc_md.write_text(md_out, encoding="utf-8")
                toc_gmi.write_text(gmi_out, encoding="utf-8")
                (repo_root / "tools/sd_root/pub/help/toc.gmi").write_text(gmi_out, encoding="utf-8")

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
        ok = generate_all(repo_root, check_only=False)
        return 0 if ok else 1

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
