#ifndef LUGALOS_FS_LATEX_MATH_H
#define LUGALOS_FS_LATEX_MATH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STYLE_DISPLAY = 0,
    STYLE_TEXT,
    STYLE_SCRIPT,
    STYLE_SCRIPTSCRIPT,
} math_style_t;

typedef enum {
    MATH_BOX_CHAR = 0,
    MATH_BOX_HLIST,
    MATH_BOX_VLIST,
    MATH_BOX_RULE,
    MATH_BOX_DELIM,
    MATH_BOX_FRAC,
    MATH_BOX_RADICAL,
    MATH_BOX_SCRIPT,
    MATH_BOX_MATRIX,
    MATH_BOX_BIGOP,
    MATH_BOX_SPACE,
} math_box_type_t;

typedef enum {
    DELIM_NONE = 0,
    DELIM_PAREN_L,    /* ( */
    DELIM_PAREN_R,    /* ) */
    DELIM_BRACKET_L,  /* [ */
    DELIM_BRACKET_R,  /* ] */
    DELIM_BRACE_L,    /* { */
    DELIM_BRACE_R,    /* } */
    DELIM_BAR,        /* | */
    DELIM_DBAR,       /* || */
    DELIM_ANGLE_L,    /* < */
    DELIM_ANGLE_R,    /* > */
    DELIM_CEIL_L,
    DELIM_CEIL_R,
    DELIM_FLOOR_L,
    DELIM_FLOOR_R,
} math_delim_t;

typedef enum {
    BIGOP_SUM = 0,
    BIGOP_PROD,
    BIGOP_INT,
    BIGOP_IINT,
    BIGOP_OINT,
    BIGOP_UNION,
    BIGOP_INTER,
} math_bigop_t;

#define MATH_MATRIX_MAX_ROWS 8
#define MATH_MATRIX_MAX_COLS 8

typedef struct math_box {
    uint8_t   type;       /* math_box_type_t */
    uint8_t   style;      /* math_style_t */
    int16_t   w, h, d;    /* Advance width, height above baseline, depth below baseline */
    int16_t   x, y;       /* Relative position within parent */
    union {
        struct {
            uint16_t codepoint;   /* Unicode / ASCII codepoint */
            uint8_t  font_family; /* 0: math italic, 1: upright, 2: bbold, 3: cal, 4: symbol */
        } ch;
        struct {
            struct math_box *head;
        } list;
        struct {
            struct math_box *num;
            struct math_box *den;
            int16_t thickness;
            bool    has_bar;      /* true for \frac, false for \binom */
        } frac;
        struct {
            struct math_box *body;
            struct math_box *deg; /* optional degree */
        } radical;
        struct {
            struct math_box *nucleus;
            struct math_box *sub;
            struct math_box *sup;
        } script;
        struct {
            math_delim_t delim;
            int16_t      target_h;
            int16_t      target_d;
        } delim;
        struct {
            math_bigop_t op;
            struct math_box *sub;
            struct math_box *sup;
        } bigop;
        struct {
            struct math_box **cells;
            uint8_t rows;
            uint8_t cols;
            int16_t col_w[MATH_MATRIX_MAX_COLS];
            int16_t row_h[MATH_MATRIX_MAX_ROWS];
            int16_t row_d[MATH_MATRIX_MAX_ROWS];
            math_delim_t left_delim;
            math_delim_t right_delim;
        } matrix;
        struct {
            int16_t width;
        } space;
    } u;
    struct math_box *next;
} math_box_t;

/* Stack scratch arena for zero-heap AST allocation */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
} math_arena_t;

void math_arena_init(math_arena_t *arena, void *buffer, size_t capacity);
void *math_arena_alloc(math_arena_t *arena, size_t size);
math_box_t *math_box_new(math_arena_t *arena, math_box_type_t type, math_style_t style);

/* Parse restricted LaTeX math string into math_box_t tree */
math_box_t *latex_parse(const char *src, math_style_t style, math_arena_t *arena);

/* Pretty-print math expression to terminal buffer using Unicode math symbols */
void latex_to_unicode(const char *src, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* LUGALOS_FS_LATEX_MATH_H */
