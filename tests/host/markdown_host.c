/* tests/host/markdown_host.c — Host test harness for LugalOS Restricted Markdown parser.
 *
 * Runs under AddressSanitizer, UndefinedBehaviorSanitizer, and Valgrind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "fs/markdown.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) do { \
    g_tests_run++; \
    printf("  [TEST] %s ... ", name); \
} while(0)

#define PASS() do { \
    g_tests_passed++; \
    printf("PASS\n"); \
} while(0)

static void test_blank_lines(void) {
    TEST("Blank lines");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("", &in_code, &in_math, &line) && line.type == MD_LINE_BLANK);
    assert(md_parse_line("   ", &in_code, &in_math, &line) && line.type == MD_LINE_BLANK);
    assert(md_parse_line("\t\t  \r\n", &in_code, &in_math, &line) && line.type == MD_LINE_BLANK);
    PASS();
}

static void test_headings(void) {
    TEST("Headings (H1 to H4, and >H4 fallback)");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("# Heading One", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_H1 && strcmp(line.text, "Heading One") == 0);

    assert(md_parse_line("## Heading Two", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_H2 && strcmp(line.text, "Heading Two") == 0);

    assert(md_parse_line("### Heading Three", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_H3 && strcmp(line.text, "Heading Three") == 0);

    assert(md_parse_line("#### Heading Four", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_H4 && strcmp(line.text, "Heading Four") == 0);

    /* ##### falls back to regular paragraph text */
    assert(md_parse_line("##### Heading Five", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_TEXT && strcmp(line.text, "##### Heading Five") == 0);

    /* #NoSpace is not a heading */
    assert(md_parse_line("#NoSpace", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_TEXT && strcmp(line.text, "#NoSpace") == 0);
    PASS();
}

static void test_code_fences(void) {
    TEST("Fenced code blocks");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("```c", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_CODE_TOGGLE && in_code == true);
    assert(strcmp(line.lang, "c") == 0);

    assert(md_parse_line("int main() { return 0; }", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_CODE_TEXT && in_code == true);
    assert(strcmp(line.text, "int main() { return 0; }") == 0);

    assert(md_parse_line("```", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_CODE_TOGGLE && in_code == false);
    PASS();
}

static void test_math_blocks(void) {
    TEST("Display math blocks ($$ ... $$)");
    bool in_code = false, in_math = false;
    md_line_t line;

    /* Multiline block */
    assert(md_parse_line("$$", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_MATH_TOGGLE && in_math == true);

    assert(md_parse_line("\\int_{-\\infty}^{\\infty} e^{-x^2} dx = \\sqrt{\\pi}", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_MATH_TEXT && in_math == true);

    assert(md_parse_line("$$", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_MATH_TOGGLE && in_math == false);

    /* Single-line block */
    assert(md_parse_line("$$ E = mc^2 $$", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_MATH_TEXT && in_math == false);
    PASS();
}

static void test_quotes_and_alerts(void) {
    TEST("Quotes and GFM Alerts");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("> Standard quote line", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_QUOTE);
    assert(strcmp(line.text, "Standard quote line") == 0);

    assert(md_parse_line("> [!NOTE] This is a note", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_ALERT && line.alert_type == MD_ALERT_NOTE);
    assert(strcmp(line.text, "This is a note") == 0);

    assert(md_parse_line("> [!WARNING] Be careful!", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_ALERT && line.alert_type == MD_ALERT_WARNING);
    assert(strcmp(line.text, "Be careful!") == 0);

    assert(md_parse_line("> [!TIP]", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_ALERT && line.alert_type == MD_ALERT_TIP);
    PASS();
}

static void test_lists(void) {
    TEST("Unordered and Ordered Lists");
    bool in_code = false, in_math = false;
    md_line_t line;

    /* Unordered level 0 */
    assert(md_parse_line("- First item", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_ULIST && line.indent_level == 0);
    assert(strcmp(line.text, "First item") == 0);

    /* Unordered level 1 (2 spaces) */
    assert(md_parse_line("  * Nested item", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_ULIST && line.indent_level == 1);
    assert(strcmp(line.text, "Nested item") == 0);

    /* Ordered level 0 */
    assert(md_parse_line("1. Step one", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_OLIST && line.order_num == 1 && line.indent_level == 0);
    assert(strcmp(line.text, "Step one") == 0);

    /* Ordered level 2 (4 spaces) */
    assert(md_parse_line("    42. Sub-step", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_OLIST && line.order_num == 42 && line.indent_level == 2);
    assert(strcmp(line.text, "Sub-step") == 0);

    PASS();
}

static void test_tables(void) {
    TEST("GFM Pipe Tables");
    bool in_code = false, in_math = false;
    md_line_t line;

    const char *hdr = "| Name | Age | City |";
    assert(md_parse_line(hdr, &in_code, &in_math, &line));
    assert(line.type == MD_LINE_TABLE_ROW);

    char cells[MD_TABLE_MAX_COLS][MD_CELL_MAX];
    int cols = md_parse_table_cells(hdr, cells);
    assert(cols == 3);
    assert(strcmp(cells[0], "Name") == 0);
    assert(strcmp(cells[1], "Age") == 0);
    assert(strcmp(cells[2], "City") == 0);

    const char *sep = "|:---|:---:|---:|";
    assert(md_parse_line(sep, &in_code, &in_math, &line));
    assert(line.type == MD_LINE_TABLE_SEP);
    assert(md_is_table_separator(sep));

    const char *row = "| Alice \\| Bob | 30 | London |";
    cols = md_parse_table_cells(row, cells);
    assert(cols == 3);
    assert(strcmp(cells[0], "Alice | Bob") == 0);
    assert(strcmp(cells[1], "30") == 0);
    assert(strcmp(cells[2], "London") == 0);

    PASS();
}

static void test_footnotes_and_toc(void) {
    TEST("Footnotes and Table of Contents");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("[^ref1]: Footnote details here", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_FOOTNOTE_DEF);
    assert(strcmp(line.footnote_id, "ref1") == 0);
    assert(strcmp(line.text, "Footnote details here") == 0);

    assert(md_parse_line("[TOC]", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_TOC);
    PASS();
}

static void test_gemtext_backwards_compat(void) {
    TEST("Gemtext Link Backward Compatibility");
    bool in_code = false, in_math = false;
    md_line_t line;

    assert(md_parse_line("=> /sd0/help/shell.md Shell Guide", &in_code, &in_math, &line));
    assert(line.type == MD_LINE_GEM_LINK);
    assert(strcmp(line.url, "/sd0/help/shell.md") == 0);
    assert(strcmp(line.label, "Shell Guide") == 0);
    PASS();
}

static void test_inline_spans(void) {
    TEST("Inline spans (bold, italic, code, links, math, images, footnotes)");
    md_span_iter_t iter;
    md_span_t span;

    const char *line = "Normal **bold text** and *italic text* with `code block` and $E=mc^2$.";
    md_span_init(&iter, line);

    /* 1. "Normal " */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, "Normal ", span.len) == 0);

    /* 2. "bold text" */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_BOLD);
    assert(strncmp(span.text, "bold text", span.len) == 0);

    /* 3. " and " */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, " and ", span.len) == 0);

    /* 4. "italic text" */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_ITALIC);
    assert(strncmp(span.text, "italic text", span.len) == 0);

    /* 5. " with " */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, " with ", span.len) == 0);

    /* 6. "code block" */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_CODE);
    assert(strncmp(span.text, "code block", span.len) == 0);

    /* 7. " and " */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, " and ", span.len) == 0);

    /* 8. "E=mc^2" */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_MATH);
    assert(strncmp(span.text, "E=mc^2", span.len) == 0);

    /* 9. "." */
    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, ".", span.len) == 0);

    assert(!md_span_next(&iter, &span));

    /* Test links and images */
    const char *link_line = "See [LugalOS Site](https://lugalos.org) and ![Logo](/logo.png) now[^1].";
    md_span_init(&iter, link_line);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, "See ", span.len) == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_LINK);
    assert(strcmp(span.link_text, "LugalOS Site") == 0);
    assert(strcmp(span.link_url, "https://lugalos.org") == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, " and ", span.len) == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_IMAGE);
    assert(strcmp(span.link_text, "Logo") == 0);
    assert(strcmp(span.link_url, "/logo.png") == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, " now", span.len) == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_FOOTNOTE);
    assert(strcmp(span.footnote_id, "1") == 0);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strncmp(span.text, ".", span.len) == 0);

    assert(!md_span_next(&iter, &span));
    PASS();
}

static void test_entities_and_no_html(void) {
    TEST("Entity decoding and No-HTML literal preservation");
    md_span_iter_t iter;
    md_span_t span;

    /* Entities &amp;, &lt;, &gt; are decoded, but <div class="test"> is literal text */
    const char *line = "Tom &amp; Jerry &lt;3 <div class=\"box\">Hello</div>";
    md_span_init(&iter, line);

    assert(md_span_next(&iter, &span) && span.style == MD_STYLE_NONE);
    assert(strcmp(span.text, "Tom & Jerry <3 <div class=\"box\">Hello</div>") == 0);
    assert(!md_span_next(&iter, &span));
    PASS();
}

static void test_slug_and_resolve(void) {
    TEST("Heading slug and URL/Anchor resolution");
    char slug[64];
    md_heading_slug("1. Quick Examples by Domain!", slug, sizeof(slug));
    assert(strcmp(slug, "1-quick-examples-by-domain") == 0);

    char out[128];
    assert(md_resolve_url("/sd0/help/shell.md", "#builtins", out, sizeof(out)));
    assert(strcmp(out, "/sd0/help/shell.md#builtins") == 0);

    assert(md_resolve_url("spartan://mozz.us/capsule/page.md", "#sec2", out, sizeof(out)));
    assert(strcmp(out, "spartan://mozz.us/capsule/page.md#sec2") == 0);

    assert(md_resolve_url("/sd0/help/shell.md", "editors.md", out, sizeof(out)));
    assert(strcmp(out, "/sd0/help/editors.md") == 0);

    assert(md_resolve_url("/sd0/help/shell.md", "/sd0/pub/readme.md", out, sizeof(out)));
    assert(strcmp(out, "/sd0/pub/readme.md") == 0);

    PASS();
}

int main(void) {
    printf("=== Running LugalOS Markdown Host Tests (ASan/UBSan) ===\n");
    test_blank_lines();
    test_headings();
    test_code_fences();
    test_math_blocks();
    test_quotes_and_alerts();
    test_lists();
    test_tables();
    test_footnotes_and_toc();
    test_gemtext_backwards_compat();
    test_inline_spans();
    test_entities_and_no_html();
    test_slug_and_resolve();

    printf("\nMarkdown Host Tests Passed: %d / %d\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
