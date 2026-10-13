/* tests/test_zwrap.c — Unit tests for Z-machine word wrapping */
#include "../user/zmachine/zwrap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct test_sink {
    char   buf[4096];
    size_t len;
    int    width;
};

static void sink_out(void *user, const char *s, size_t len)
{
    struct test_sink *sink = (struct test_sink *)user;
    if (sink->len + len < sizeof(sink->buf)) {
        memcpy(sink->buf + sink->len, s, len);
        sink->len += len;
        sink->buf[sink->len] = '\0';
    }
}

static int sink_width(void *user)
{
    struct test_sink *sink = (struct test_sink *)user;
    return sink->width;
}

static void feed_string(struct z_wrap *w, const char *str)
{
    while (*str) {
        zwrap_putc(w, (uint8_t)*str++);
    }
}

static void test_simple_wrap(void)
{
    struct test_sink sink = { .len = 0, .width = 15 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "You are in an open field.");
    zwrap_flush(&w);

    /* Width 15:
     * "You are in an" = 13 chars (fits in 15)
     * "open field."   = 11 chars
     */
    const char *expected = "You are in an\nopen field.";
    if (strcmp(sink.buf, expected) != 0) {
        fprintf(stderr, "test_simple_wrap FAILED:\nExpected:\n[%s]\nGot:\n[%s]\n",
                expected, sink.buf);
        assert(0);
    }
    printf("PASS: test_simple_wrap\n");
}

static void test_exact_fit(void)
{
    struct test_sink sink = { .len = 0, .width = 10 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "Hello 1234");
    zwrap_flush(&w);

    /* "Hello 1234" = 10 chars exactly */
    assert(strcmp(sink.buf, "Hello 1234") == 0);
    printf("PASS: test_exact_fit\n");
}

static void test_indentation_preserved(void)
{
    struct test_sink sink = { .len = 0, .width = 20 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "  First line.\n  Second line.");
    zwrap_flush(&w);

    const char *expected = "  First line.\n  Second line.";
    assert(strcmp(sink.buf, expected) == 0);
    printf("PASS: test_indentation_preserved\n");
}

static void test_hard_newlines(void)
{
    struct test_sink sink = { .len = 0, .width = 40 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "Line 1\nLine 2\n\nLine 4");
    zwrap_flush(&w);

    const char *expected = "Line 1\nLine 2\n\nLine 4";
    assert(strcmp(sink.buf, expected) == 0);
    printf("PASS: test_hard_newlines\n");
}

static void test_long_word(void)
{
    struct test_sink sink = { .len = 0, .width = 10 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "ABC 1234567890XYZ end");
    zwrap_flush(&w);

    /* "ABC " (4) -> next word is 13 chars, cannot fit on 10-char line.
     * Line 1: "ABC"
     * Line 2: "1234567890" (10 chars)
     * Line 3: "XYZ end"
     */
    const char *expected = "ABC\n1234567890\nXYZ end";
    if (strcmp(sink.buf, expected) != 0) {
        fprintf(stderr, "test_long_word FAILED:\nExpected:\n[%s]\nGot:\n[%s]\n",
                expected, sink.buf);
        assert(0);
    }
    printf("PASS: test_long_word\n");
}

static void test_dynamic_resize(void)
{
    struct test_sink sink = { .len = 0, .width = 48 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    feed_string(&w, "This is line one of the message.\n");
    sink.width = 20; /* User resized screen to narrow split */
    feed_string(&w, "Now the width is much smaller than before.");
    zwrap_flush(&w);

    /* Width 20 for second paragraph */
    const char *expected =
        "This is line one of the message.\n"
        "Now the width is\n"
        "much smaller than\n"
        "before.";
    if (strcmp(sink.buf, expected) != 0) {
        fprintf(stderr, "test_dynamic_resize FAILED:\nExpected:\n[%s]\nGot:\n[%s]\n",
                expected, sink.buf);
        assert(0);
    }
    printf("PASS: test_dynamic_resize\n");
}

static void test_input_echo(void)
{
    struct test_sink sink = { .len = 0, .width = 30 };
    struct z_wrap w;
    zwrap_init(&w, sink_out, sink_width, &sink);

    /* Game outputs prompt: */
    feed_string(&w, "West of House\n>");
    zwrap_flush(&w);

    /* Prompt is visible: */
    assert(strcmp(sink.buf, "West of House\n>") == 0);

    /* Input mode begins: */
    w.in_input = true;
    feed_string(&w, "go no");
    /* User backspaces 'o', types 'orth\n': */
    zwrap_putc(&w, '\b');
    zwrap_putc(&w, ' ');
    zwrap_putc(&w, '\b');
    feed_string(&w, "orth\n");

    /* Echoed without delay: */
    const char *expected = "West of House\n>go no\b \borth\n";
    if (strcmp(sink.buf, expected) != 0) {
        fprintf(stderr, "test_input_echo FAILED:\nExpected:\n[%s]\nGot:\n[%s]\n",
                expected, sink.buf);
        assert(0);
    }
    assert(!w.in_input);

    /* Next game response: */
    feed_string(&w, "Forest path.");
    zwrap_flush(&w);
    const char *expected2 = "West of House\n>go no\b \borth\nForest path.";
    assert(strcmp(sink.buf, expected2) == 0);
    printf("PASS: test_input_echo\n");
}

int main(void)
{
    test_simple_wrap();
    test_exact_fit();
    test_indentation_preserved();
    test_hard_newlines();
    test_long_word();
    test_dynamic_resize();
    test_input_echo();
    printf("All zwrap tests PASSED!\n");
    return 0;
}
