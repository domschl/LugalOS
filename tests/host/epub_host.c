#include "fs/epub.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mem_read_cb(void *ctx, uint32_t off, void *dst, uint32_t len) {
    const uint8_t *buf = (const uint8_t *)ctx;
    memcpy(dst, buf + off, len);
    return 0;
}

static void test_xhtml_to_md(void) {
    const char *xhtml =
        "<html><head><title>Skip</title><style>p{}</style></head>"
        "<body><h1>Chapter 1</h1>"
        "<p>Hello &mdash; <em>world</em> &amp; <strong>friends</strong>!</p>"
        "<p>German: &auml;&ouml;&uuml;&szlig; &#x2026;</p>"
        "<img src=\"pic.png\" alt=\"Rabbit\"/>"
        "</body></html>";
    char md[1024];
    uint32_t len = epub_xhtml_to_md((const uint8_t *)xhtml, (uint32_t)strlen(xhtml), md, sizeof(md));
    assert(len > 0);
    assert(strstr(md, "# Chapter 1") != NULL);
    assert(strstr(md, "Skip") == NULL);
    assert(strstr(md, "*world*") != NULL);
    assert(strstr(md, "**friends**") != NULL);
    assert(strstr(md, "äöüß") != NULL);
    assert(strstr(md, "*[Image: Rabbit]*") != NULL);
}

static void test_gutenberg_file(const char *path, const char *expect_substr) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    assert(sz > 0);
    uint8_t *buf = malloc((size_t)sz);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);

    uint8_t *scratch = malloc(512 * 1024);
    char *md = malloc(512 * 1024);
    epub_book_t *book = calloc(1, sizeof(epub_book_t));
    assert(scratch && md && book);

    epub_err_t err = epub_open(book, mem_read_cb, buf, (uint32_t)sz, scratch, 512 * 1024);
    if (err != EPUB_OK) {
        fprintf(stderr, "epub_open(%s) -> err=%d\n", path, (int)err);
    }
    assert(err == EPUB_OK);
    assert(book->spine_count > 0);
    assert(book->title[0] != '\0');

    bool found = false;
    for (int i = 0; i < book->spine_count; i++) {
        uint32_t mlen = 0;
        err = epub_load_chapter_md(book, i, scratch, 512 * 1024, md, 512 * 1024, &mlen);
        assert(err == EPUB_OK);
        if (expect_substr && strstr(md, expect_substr) != NULL) found = true;
    }
    if (expect_substr) assert(found);
    printf("epub_host: '%s' (%s, %d chapters) OK\n", book->title, book->author, book->spine_count);

    free(book);
    free(md);
    free(scratch);
    free(buf);
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_fuzz(int iters, uint32_t seed) {
    uint8_t *scratch = malloc(32768);
    epub_book_t *book = calloc(1, sizeof(epub_book_t));
    assert(scratch && book);
    uint8_t buf[256];

    for (int i = 0; i < iters; i++) {
        for (size_t k = 0; k < sizeof(buf); k++) {
            buf[k] = (uint8_t)(xorshift32(&seed) & 0xFFu);
        }
        (void)epub_open(book, mem_read_cb, buf, sizeof(buf), scratch, 32768);
    }
    free(book);
    free(scratch);
}

int main(int argc, char **argv) {
    int iters = (argc > 1) ? atoi(argv[1]) : 1000;
    uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x40c0ffeeu;
    test_xhtml_to_md();
    test_gutenberg_file("/home/dsc/.gemini/antigravity-cli/brain/ea7d6a4f-a7c1-4653-bb90-6dec76dca55b/scratch/books/alice.epub", "Alice");
    test_gutenberg_file("/home/dsc/.gemini/antigravity-cli/brain/ea7d6a4f-a7c1-4653-bb90-6dec76dca55b/scratch/books/verwandlung.epub", "Gregor");
    test_fuzz(iters, seed);
    printf("epub_host: PASS (%d fuzz iters)\n", iters);
    return 0;
}
