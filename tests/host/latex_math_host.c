/* tests/host/latex_math_host.c — Host unit tests for Restricted LaTeX Math Engine.
 * Tests AST parsing, arena allocation, Knuth Appendix G box layout, and Unicode fallback.
 */

#include "fs/latex_math.h"
#include "drivers/latex_layout.h"
#include "drivers/font_math.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) do { \
    g_tests_run++; \
    printf("  [TEST] %s ... ", name); \
    fflush(stdout); \
} while (0)

#define PASS() do { \
    g_tests_passed++; \
    printf("PASS\n"); \
} while (0)

static void test_arena(void) {
    TEST("math_arena allocation & alignment");
    uint8_t buf[256];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    assert(arena.cap == sizeof(buf));
    assert(arena.used == 0);

    void *p1 = math_arena_alloc(&arena, 13);
    assert(p1 != NULL);
    assert(arena.used == 16); /* aligned to 4 */

    math_box_t *box = math_box_new(&arena, MATH_BOX_CHAR, STYLE_TEXT);
    assert(box != NULL);
    assert(box->type == MATH_BOX_CHAR);
    assert(box->style == STYLE_TEXT);

    PASS();
}

static void test_basic_parsing(void) {
    TEST("Basic atoms & simple expressions");
    uint8_t buf[4096];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    math_box_t *box = latex_parse("x + 1", STYLE_TEXT, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_HLIST);

    math_box_t *c = box->u.list.head;
    assert(c != NULL);
    assert(c->type == MATH_BOX_CHAR && c->u.ch.codepoint == 'x');
    c = c->next;
    assert(c != NULL && c->type == MATH_BOX_CHAR && c->u.ch.codepoint == '+');
    c = c->next;
    assert(c != NULL && c->type == MATH_BOX_CHAR && c->u.ch.codepoint == '1');

    PASS();
}

static void test_fractions(void) {
    TEST("Fractions and nested fractions (\\frac)");
    uint8_t buf[4096];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    math_box_t *box = latex_parse("\\frac{a}{b}", STYLE_DISPLAY, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_FRAC);
    assert(box->u.frac.has_bar == true);
    assert(box->u.frac.num != NULL);
    assert(box->u.frac.den != NULL);

    int w = latex_layout(box, 1);
    assert(w > 0);
    assert(box->h > 0);
    assert(box->d > 0);

    PASS();
}

static void test_scripts(void) {
    TEST("Subscripts and superscripts (x_i^2)");
    uint8_t buf[4096];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    math_box_t *box = latex_parse("x_i^2", STYLE_TEXT, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_SCRIPT);
    assert(box->u.script.nucleus != NULL);
    assert(box->u.script.sup != NULL);
    assert(box->u.script.sub != NULL);

    int w = latex_layout(box, 1);
    assert(w > 0);
    assert(box->u.script.sup->x >= box->u.script.nucleus->w);
    assert(box->u.script.sub->x >= box->u.script.nucleus->w);

    PASS();
}

static void test_radicals(void) {
    TEST("Radicals (\\sqrt{x} and \\sqrt[3]{x})");
    uint8_t buf[4096];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    math_box_t *box1 = latex_parse("\\sqrt{x}", STYLE_TEXT, &arena);
    assert(box1 != NULL);
    assert(box1->type == MATH_BOX_RADICAL);
    assert(box1->u.radical.body != NULL);
    assert(box1->u.radical.deg == NULL);

    int w1 = latex_layout(box1, 1);
    assert(w1 > 0);

    math_box_t *box2 = latex_parse("\\sqrt[3]{x}", STYLE_TEXT, &arena);
    assert(box2 != NULL);
    assert(box2->type == MATH_BOX_RADICAL);
    assert(box2->u.radical.deg != NULL);

    PASS();
}

static void test_quadratic_formula(void) {
    TEST("Complex formula: Quadratic formula");
    uint8_t buf[8192];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    const char *quad = "x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}";
    math_box_t *box = latex_parse(quad, STYLE_DISPLAY, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_HLIST);

    int w = latex_layout(box, 1);
    assert(w > 40);
    assert(box->h > 10);
    assert(box->d > 5);

    PASS();
}

static void test_big_operators(void) {
    TEST("Large operators (\\sum, \\int) with limits");
    uint8_t buf[8192];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    const char *gauss = "\\int_0^\\infty e^{-x^2} dx";
    math_box_t *box = latex_parse(gauss, STYLE_DISPLAY, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_HLIST);

    int w = latex_layout(box, 1);
    assert(w > 30);
    assert(box->h > 10);

    PASS();
}

static void test_matrices(void) {
    TEST("Matrices (\\begin{pmatrix} ... \\end{pmatrix})");
    uint8_t buf[8192];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    const char *mat_src = "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}";
    math_box_t *box = latex_parse(mat_src, STYLE_TEXT, &arena);
    assert(box != NULL);
    assert(box->type == MATH_BOX_MATRIX);
    assert(box->u.matrix.rows == 2);
    assert(box->u.matrix.cols == 2);
    assert(box->u.matrix.left_delim == DELIM_PAREN_L);
    assert(box->u.matrix.right_delim == DELIM_PAREN_R);

    int w = latex_layout(box, 1);
    assert(w > 20);
    assert(box->h > 10);
    assert(box->d > 10);

    PASS();
}

static void test_unicode_converter(void) {
    TEST("Terminal Unicode math converter (latex_to_unicode)");
    char out[128];

    latex_to_unicode("x^2 + y_i", out, sizeof(out));
    assert(strstr(out, "x²") != NULL);
    assert(strstr(out, "yᵢ") != NULL);

    latex_to_unicode("\\alpha + \\beta = \\gamma", out, sizeof(out));
    assert(strstr(out, "α + β =") != NULL);

    latex_to_unicode("\\int_0^\\infty e^{-x} dx", out, sizeof(out));
    assert(strstr(out, "∫") != NULL);
    assert(strstr(out, "∞") != NULL);

    latex_to_unicode("a \\le b \\pm c \\approx d", out, sizeof(out));
    assert(strstr(out, "≤") != NULL);
    assert(strstr(out, "±") != NULL);
    assert(strstr(out, "≈") != NULL);

    PASS();
}

static void test_rendering(void) {
    TEST("Rendering to Canvas1, Gray8, and RGB565");
    uint8_t buf[16384];
    math_arena_t arena;
    math_arena_init(&arena, buf, sizeof(buf));

    const char *formula = "\\int_0^\\infty e^{-x^2} dx = \\sqrt{\\pi}";
    math_box_t *box = latex_parse(formula, STYLE_DISPLAY, &arena);
    assert(box != NULL);

    /* Test scale 1 (16px base typography) */
    int w1 = latex_layout(box, 1);
    assert(w1 > 50);
    assert(box->scale == 1);

    /* Render to canvas1 */
    uint8_t c_fb[1024];
    memset(c_fb, 0, sizeof(c_fb));
    canvas1_t c;
    canvas1_init(&c, c_fb, 32, 256, 32);
    latex_render_canvas1(box, 10, 20, &c, 1);
    bool c_has_pixels = false;
    for (size_t i = 0; i < sizeof(c_fb); i++) {
        if (c_fb[i] != 0) { c_has_pixels = true; break; }
    }
    assert(c_has_pixels);

    /* Render to Gray8 */
    uint8_t g_fb[256 * 32];
    memset(g_fb, 0xff, sizeof(g_fb));
    latex_render_gray8(box, 10, 20, g_fb, 256, 256, 32, 0x00, 0xff);
    bool g_has_pixels = false;
    for (size_t i = 0; i < sizeof(g_fb); i++) {
        if (g_fb[i] != 0xff) { g_has_pixels = true; break; }
    }
    assert(g_has_pixels);

    /* Render to RGB565 */
    uint16_t rgb_fb[256 * 32];
    memset(rgb_fb, 0xff, sizeof(rgb_fb));
    latex_render_rgb565(box, 10, 20, rgb_fb, 256, 256, 32, 0x0000, 0xffff);
    bool rgb_has_pixels = false;
    for (size_t i = 0; i < 256 * 32; i++) {
        if (rgb_fb[i] != 0xffff) { rgb_has_pixels = true; break; }
    }
    assert(rgb_has_pixels);

    /* Test scale 2 (32px HiDPI) */
    math_arena_init(&arena, buf, sizeof(buf));
    box = latex_parse(formula, STYLE_DISPLAY, &arena);
    assert(box != NULL);
    int w2 = latex_layout(box, 2);
    assert(w2 > w1);
    assert(box->scale == 2);

    PASS();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== LugalOS Host Test: Restricted LaTeX Math Engine ===\n");
    test_arena();
    test_basic_parsing();
    test_fractions();
    test_scripts();
    test_radicals();
    test_quadratic_formula();
    test_big_operators();
    test_matrices();
    test_unicode_converter();
    test_rendering();

    printf("\nAll %d/%d tests passed successfully!\n", g_tests_passed, g_tests_run);
    return 0;
}
