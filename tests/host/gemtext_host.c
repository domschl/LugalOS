/* tests/host/gemtext_host.c — Host test harness for LugalOS Gemtext parser.
 *
 * Runs under AddressSanitizer, UndefinedBehaviorSanitizer, and Valgrind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "fs/gemtext.h"

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
    bool in_pre = false;
    gem_line_t line;

    assert(gemtext_parse_line("", &in_pre, &line) && line.type == GEM_LINE_BLANK);
    assert(gemtext_parse_line("   ", &in_pre, &line) && line.type == GEM_LINE_BLANK);
    assert(gemtext_parse_line("\t\t  \r\n", &in_pre, &line) && line.type == GEM_LINE_BLANK);
    PASS();
}

static void test_headings(void) {
    TEST("Headings (H1, H2, H3, overflow)");
    bool in_pre = false;
    gem_line_t line;

    assert(gemtext_parse_line("# Heading One", &in_pre, &line));
    assert(line.type == GEM_LINE_H1 && strcmp(line.text, "Heading One") == 0);

    assert(gemtext_parse_line("## Heading Two", &in_pre, &line));
    assert(line.type == GEM_LINE_H2 && strcmp(line.text, "Heading Two") == 0);

    assert(gemtext_parse_line("### Heading Three", &in_pre, &line));
    assert(line.type == GEM_LINE_H3 && strcmp(line.text, "Heading Three") == 0);

    /* #### is not a Gemtext heading, becomes regular text */
    assert(gemtext_parse_line("#### Four Hashes", &in_pre, &line));
    assert(line.type == GEM_LINE_TEXT && strcmp(line.text, "#### Four Hashes") == 0);
    PASS();
}

static void test_links(void) {
    TEST("Hyperlinks (=> target [label])");
    bool in_pre = false;
    gem_line_t line;

    /* Link with explicit label */
    assert(gemtext_parse_line("=> c_programming.gmi C Programming Guide", &in_pre, &line));
    assert(line.type == GEM_LINE_LINK);
    assert(strcmp(line.url, "c_programming.gmi") == 0);
    assert(strcmp(line.label, "C Programming Guide") == 0);

    /* Link without label (label defaults to URL) */
    assert(gemtext_parse_line("=> spartan://mozz.us/capsule", &in_pre, &line));
    assert(line.type == GEM_LINE_LINK);
    assert(strcmp(line.url, "spartan://mozz.us/capsule") == 0);
    assert(strcmp(line.label, "spartan://mozz.us/capsule") == 0);

    /* Link with extra whitespace */
    assert(gemtext_parse_line("=>   /sd0/help/shell.gmi    Shell Manual  \r\n", &in_pre, &line));
    assert(line.type == GEM_LINE_LINK);
    assert(strcmp(line.url, "/sd0/help/shell.gmi") == 0);
    assert(strcmp(line.label, "Shell Manual") == 0);

    PASS();
}

static void test_lists_and_quotes(void) {
    TEST("Lists and Blockquotes");
    bool in_pre = false;
    gem_line_t line;

    /* Bullet list item */
    assert(gemtext_parse_line("* An item in a list", &in_pre, &line));
    assert(line.type == GEM_LINE_LIST);
    assert(strcmp(line.text, "An item in a list") == 0);

    /* Asterisk without space is regular text */
    assert(gemtext_parse_line("*notalist", &in_pre, &line));
    assert(line.type == GEM_LINE_TEXT);

    /* Blockquote */
    assert(gemtext_parse_line("> A famous quotation", &in_pre, &line));
    assert(line.type == GEM_LINE_QUOTE);
    assert(strcmp(line.text, "A famous quotation") == 0);

    PASS();
}

static void test_preformatted_blocks(void) {
    TEST("Preformatted code blocks");
    bool in_pre = false;
    gem_line_t line;

    /* Enter preformatted block with alt */
    assert(gemtext_parse_line("```c", &in_pre, &line));
    assert(line.type == GEM_LINE_PRE_TOGGLE);
    assert(in_pre == true);
    assert(strcmp(line.alt, "c") == 0);

    /* Verbatim lines inside preformatted mode */
    assert(gemtext_parse_line("# This is NOT a heading", &in_pre, &line));
    assert(line.type == GEM_LINE_PRE_TEXT);
    assert(strcmp(line.text, "# This is NOT a heading") == 0);

    assert(gemtext_parse_line("=> not_a_link.gmi", &in_pre, &line));
    assert(line.type == GEM_LINE_PRE_TEXT);

    /* Exit preformatted block */
    assert(gemtext_parse_line("```", &in_pre, &line));
    assert(line.type == GEM_LINE_PRE_TOGGLE);
    assert(in_pre == false);

    /* Following line is back to normal text */
    assert(gemtext_parse_line("# Heading After Pre", &in_pre, &line));
    assert(line.type == GEM_LINE_H1);
    assert(strcmp(line.text, "Heading After Pre") == 0);

    PASS();
}

static void test_url_resolution(void) {
    TEST("gemtext_resolve_url()");
    char out[256];

    /* Relative file path in local directory */
    assert(gemtext_resolve_url("/sd0/help/editors.gmi", "c_programming.gmi", out, sizeof(out)));
    assert(strcmp(out, "/sd0/help/c_programming.gmi") == 0);

    /* Absolute local path replaces base path */
    assert(gemtext_resolve_url("/sd0/help/editors.gmi", "/sd0/books/iliad.gmi", out, sizeof(out)));
    assert(strcmp(out, "/sd0/books/iliad.gmi") == 0);

    /* External fully qualified URL */
    assert(gemtext_resolve_url("/sd0/help/editors.gmi", "spartan://mozz.us/capsule", out, sizeof(out)));
    assert(strcmp(out, "spartan://mozz.us/capsule") == 0);

    /* Relative path on remote Spartan server */
    assert(gemtext_resolve_url("spartan://mozz.us:300/docs/index.gmi", "spec.gmi", out, sizeof(out)));
    assert(strcmp(out, "spartan://mozz.us:300/docs/spec.gmi") == 0);

    /* Absolute root path on remote Spartan server */
    assert(gemtext_resolve_url("spartan://mozz.us:300/docs/index.gmi", "/about.gmi", out, sizeof(out)));
    assert(strcmp(out, "spartan://mozz.us:300/about.gmi") == 0);

    PASS();
}

static void test_fuzz_robustness(void) {
    TEST("Fuzz and boundary robustness");
    bool in_pre = false;
    gem_line_t line;

    /* Null pointer safety */
    assert(!gemtext_parse_line(NULL, &in_pre, &line));
    assert(!gemtext_parse_line("test", NULL, &line));
    assert(!gemtext_parse_line("test", &in_pre, NULL));

    /* Very long line */
    char long_line[1024];
    memset(long_line, 'A', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    assert(gemtext_parse_line(long_line, &in_pre, &line));
    assert(line.type == GEM_LINE_TEXT);

    /* Malformed links */
    assert(gemtext_parse_line("=>", &in_pre, &line));
    assert(line.type == GEM_LINE_LINK);
    assert(strcmp(line.url, "") == 0);

    assert(gemtext_parse_line("=>    ", &in_pre, &line));
    assert(line.type == GEM_LINE_LINK);

    PASS();
}

int main(void) {
    printf("=== LugalOS Host Test: Gemtext Parser & URL Resolver ===\n");
    test_blank_lines();
    test_headings();
    test_links();
    test_lists_and_quotes();
    test_preformatted_blocks();
    test_url_resolution();
    test_fuzz_robustness();

    printf("\nAll %d/%d tests passed successfully!\n", g_tests_passed, g_tests_run);
    return 0;
}
