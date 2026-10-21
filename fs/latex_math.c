/* fs/latex_math.c — Restricted LaTeX Math Parser & Tokenizer.
 *
 * Implements a freestanding, zero-allocation recursive-descent parser for
 * mathematical formulas in LugalOS. Allocates AST nodes strictly from a stack
 * scratch arena (O(1) SRAM overhead, zero heap).
 */

#include "fs/latex_math.h"
#include "libc/include/string.h"

/* --- Scratch Arena Allocator --- */

void math_arena_init(math_arena_t *arena, void *buffer, size_t capacity) {
    if (!arena) return;
    arena->buf = (uint8_t *)buffer;
    arena->cap = capacity;
    arena->used = 0;
}

void *math_arena_alloc(math_arena_t *arena, size_t size) {
    if (!arena || !arena->buf) return NULL;
    /* 4-byte alignment */
    size_t aligned = (size + 3) & ~3;
    if (arena->used + aligned > arena->cap) return NULL;
    void *ptr = arena->buf + arena->used;
    arena->used += aligned;
    memset(ptr, 0, size);
    return ptr;
}

math_box_t *math_box_new(math_arena_t *arena, math_box_type_t type, math_style_t style) {
    math_box_t *box = (math_box_t *)math_arena_alloc(arena, sizeof(math_box_t));
    if (!box) return NULL;
    box->type = (uint8_t)type;
    box->style = (uint8_t)style;
    return box;
}

/* --- Tokenizer & Symbol Map --- */

typedef enum {
    TOK_EOF = 0,
    TOK_CHAR,
    TOK_CMD,
    TOK_LBRACE,   /* { */
    TOK_RBRACE,   /* } */
    TOK_SUP,      /* ^ */
    TOK_SUB,      /* _ */
    TOK_AMP,      /* & */
    TOK_NEWLINE,  /* \\ */
    TOK_LPAREN,   /* ( */
    TOK_RPAREN,   /* ) */
    TOK_LBRACKET, /* [ */
    TOK_RBRACKET, /* ] */
    TOK_BAR,      /* | */
} math_token_type_t;

typedef struct {
    math_token_type_t type;
    char              cmd[32];
    uint16_t          codepoint;
} math_token_t;

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
} math_lexer_t;

static void lexer_init(math_lexer_t *l, const char *src) {
    l->src = src ? src : "";
    l->pos = 0;
    l->len = strlen(l->src);
}

static bool is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static bool is_alpha(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static math_token_t lexer_next(math_lexer_t *l) {
    math_token_t tok;
    memset(&tok, 0, sizeof(tok));

    while (l->pos < l->len && is_space(l->src[l->pos])) {
        l->pos++;
    }

    if (l->pos >= l->len) {
        tok.type = TOK_EOF;
        return tok;
    }

    char c = l->src[l->pos++];

    if (c == '\\') {
        if (l->pos < l->len && l->src[l->pos] == '\\') {
            l->pos++;
            tok.type = TOK_NEWLINE;
            return tok;
        }
        if (l->pos < l->len && (l->src[l->pos] == ',' || l->src[l->pos] == ':' ||
                                l->src[l->pos] == ';' || l->src[l->pos] == '!')) {
            tok.type = TOK_CMD;
            tok.cmd[0] = l->src[l->pos++];
            tok.cmd[1] = '\0';
            return tok;
        }
        if (l->pos < l->len && (l->src[l->pos] == '{' || l->src[l->pos] == '}')) {
            tok.type = TOK_CHAR;
            tok.codepoint = (uint16_t)l->src[l->pos++];
            return tok;
        }

        /* Read command name */
        size_t cpos = 0;
        while (l->pos < l->len && is_alpha(l->src[l->pos]) && cpos < sizeof(tok.cmd) - 1) {
            tok.cmd[cpos++] = l->src[l->pos++];
        }
        tok.cmd[cpos] = '\0';
        tok.type = TOK_CMD;
        return tok;
    }

    if (c == '{') { tok.type = TOK_LBRACE; return tok; }
    if (c == '}') { tok.type = TOK_RBRACE; return tok; }
    if (c == '^') { tok.type = TOK_SUP; return tok; }
    if (c == '_') { tok.type = TOK_SUB; return tok; }
    if (c == '&') { tok.type = TOK_AMP; return tok; }
    if (c == '(') { tok.type = TOK_LPAREN; tok.codepoint = '('; return tok; }
    if (c == ')') { tok.type = TOK_RPAREN; tok.codepoint = ')'; return tok; }
    if (c == '[') { tok.type = TOK_LBRACKET; tok.codepoint = '['; return tok; }
    if (c == ']') { tok.type = TOK_RBRACKET; tok.codepoint = ']'; return tok; }
    if (c == '|') { tok.type = TOK_BAR; tok.codepoint = '|'; return tok; }

    tok.type = TOK_CHAR;
    tok.codepoint = (uint8_t)c;
    return tok;
}

/* Command to symbol codepoint mapper */
typedef struct {
    const char *name;
    uint16_t    codepoint;
} math_sym_entry_t;

static const math_sym_entry_t MATH_SYMBOLS[] = {
    { "alpha",      0x03B1 },
    { "beta",       0x03B2 },
    { "gamma",      0x03B3 },
    { "delta",      0x03B4 },
    { "epsilon",    0x03B5 },
    { "zeta",       0x03B6 },
    { "eta",        0x03B7 },
    { "theta",      0x03B8 },
    { "lambda",     0x03BB },
    { "mu",         0x03BC },
    { "pi",         0x03C0 },
    { "rho",        0x03C1 },
    { "sigma",      0x03C3 },
    { "phi",        0x03C6 },
    { "omega",      0x03C9 },
    { "Gamma",      0x0393 },
    { "Delta",      0x0394 },
    { "Theta",      0x0398 },
    { "Sigma",      0x03A3 },
    { "Omega",      0x03A9 },
    { "pm",         0x00B1 },
    { "mp",         0x2213 },
    { "times",      0x00D7 },
    { "cdot",       0x22C5 },
    { "le",         0x2264 },
    { "leq",        0x2264 },
    { "ge",         0x2265 },
    { "geq",        0x2265 },
    { "neq",        0x2260 },
    { "approx",     0x2248 },
    { "equiv",      0x2261 },
    { "in",         0x2208 },
    { "notin",      0x2209 },
    { "infty",      0x221E },
    { "partial",    0x2202 },
    { "nabla",      0x2207 },
    { "to",         0x2192 },
    { "implies",    0x21D2 },
    { "iff",        0x21D4 },
    { "sqrt",       0x221A },
    { "sum",        0x2211 },
    { "prod",       0x220F },
    { "int",        0x222B },
    { "oint",       0x222E },
};

static uint16_t lookup_cmd_symbol(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    for (size_t i = 0; i < sizeof(MATH_SYMBOLS) / sizeof(MATH_SYMBOLS[0]); i++) {
        if (strcmp(cmd, MATH_SYMBOLS[i].name) == 0) {
            return MATH_SYMBOLS[i].codepoint;
        }
    }
    return 0;
}

/* --- Recursive Descent Parser --- */

typedef struct {
    math_lexer_t lexer;
    math_token_t cur;
    math_arena_t *arena;
    math_style_t style;
} math_parser_t;

static void parser_advance(math_parser_t *p) {
    p->cur = lexer_next(&p->lexer);
}

static math_box_t *parse_expr(math_parser_t *p, math_style_t style, bool stop_at_delim);
static math_box_t *parse_atom(math_parser_t *p, math_style_t style);

static math_box_t *parse_group_or_atom(math_parser_t *p, math_style_t style) {
    if (p->cur.type == TOK_LBRACE) {
        parser_advance(p);
        math_box_t *sub = parse_expr(p, style, false);
        if (p->cur.type == TOK_RBRACE) {
            parser_advance(p);
        }
        return sub;
    }
    return parse_atom(p, style);
}

static math_delim_t token_to_delim(const math_token_t *tok) {
    if (tok->type == TOK_LPAREN) return DELIM_PAREN_L;
    if (tok->type == TOK_RPAREN) return DELIM_PAREN_R;
    if (tok->type == TOK_LBRACKET) return DELIM_BRACKET_L;
    if (tok->type == TOK_RBRACKET) return DELIM_BRACKET_R;
    if (tok->type == TOK_BAR) return DELIM_BAR;
    if (tok->type == TOK_CHAR) {
        if (tok->codepoint == '{') return DELIM_BRACE_L;
        if (tok->codepoint == '}') return DELIM_BRACE_R;
    }
    if (tok->type == TOK_CMD) {
        if (strcmp(tok->cmd, "{") == 0) return DELIM_BRACE_L;
        if (strcmp(tok->cmd, "}") == 0) return DELIM_BRACE_R;
        if (strcmp(tok->cmd, "langle") == 0) return DELIM_ANGLE_L;
        if (strcmp(tok->cmd, "rangle") == 0) return DELIM_ANGLE_R;
        if (strcmp(tok->cmd, "lfloor") == 0) return DELIM_FLOOR_L;
        if (strcmp(tok->cmd, "rfloor") == 0) return DELIM_FLOOR_R;
        if (strcmp(tok->cmd, "lceil") == 0) return DELIM_CEIL_L;
        if (strcmp(tok->cmd, "rceil") == 0) return DELIM_CEIL_R;
        if (strcmp(tok->cmd, "|") == 0) return DELIM_DBAR;
        if (strcmp(tok->cmd, ".") == 0) return DELIM_NONE;
    }
    return DELIM_NONE;
}

static math_box_t *parse_atom(math_parser_t *p, math_style_t style) {
    if (p->cur.type == TOK_CHAR || p->cur.type == TOK_LPAREN || p->cur.type == TOK_RPAREN ||
        p->cur.type == TOK_LBRACKET || p->cur.type == TOK_RBRACKET || p->cur.type == TOK_BAR) {
        math_box_t *b = math_box_new(p->arena, MATH_BOX_CHAR, style);
        if (b) {
            b->u.ch.codepoint = p->cur.codepoint;
            /* Digits are upright, letters are math italic */
            if (p->cur.codepoint >= '0' && p->cur.codepoint <= '9') {
                b->u.ch.font_family = 1;
            } else if (is_alpha((char)p->cur.codepoint)) {
                b->u.ch.font_family = 0;
            } else {
                b->u.ch.font_family = 4; /* symbol / operator */
            }
        }
        parser_advance(p);
        return b;
    }

    if (p->cur.type == TOK_CMD) {
        char cmd[32];
        strncpy(cmd, p->cur.cmd, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        parser_advance(p);

        /* 1. \frac{num}{den} */
        if (strcmp(cmd, "frac") == 0 || strcmp(cmd, "binom") == 0) {
            bool is_binom = (strcmp(cmd, "binom") == 0);
            math_style_t cstyle = (style == STYLE_DISPLAY) ? STYLE_TEXT :
                                  (style == STYLE_TEXT) ? STYLE_SCRIPT : STYLE_SCRIPTSCRIPT;
            math_box_t *num = parse_group_or_atom(p, cstyle);
            math_box_t *den = parse_group_or_atom(p, cstyle);
            math_box_t *frac = math_box_new(p->arena, MATH_BOX_FRAC, style);
            if (frac) {
                frac->u.frac.num = num;
                frac->u.frac.den = den;
                frac->u.frac.has_bar = !is_binom;
                frac->u.frac.thickness = is_binom ? 0 : 1;
            }
            return frac;
        }

        /* 2. \sqrt[deg]{body} */
        if (strcmp(cmd, "sqrt") == 0) {
            math_box_t *deg = NULL;
            if (p->cur.type == TOK_LBRACKET) {
                parser_advance(p);
                deg = parse_expr(p, STYLE_SCRIPTSCRIPT, false);
                if (p->cur.type == TOK_RBRACKET) parser_advance(p);
            }
            math_box_t *body = parse_group_or_atom(p, style);
            math_box_t *rad = math_box_new(p->arena, MATH_BOX_RADICAL, style);
            if (rad) {
                rad->u.radical.body = body;
                rad->u.radical.deg = deg;
            }
            return rad;
        }

        /* 3. \left <delim> ... \right <delim> */
        if (strcmp(cmd, "left") == 0) {
            math_delim_t ldelim = token_to_delim(&p->cur);
            parser_advance(p);
            math_box_t *inner = parse_expr(p, style, false);
            math_delim_t rdelim = DELIM_NONE;
            if (p->cur.type == TOK_CMD && strcmp(p->cur.cmd, "right") == 0) {
                parser_advance(p);
                rdelim = token_to_delim(&p->cur);
                parser_advance(p);
            }

            /* Create delimiter wrapper: left delim + inner + right delim */
            math_box_t *hlist = math_box_new(p->arena, MATH_BOX_HLIST, style);
            math_box_t *ld = math_box_new(p->arena, MATH_BOX_DELIM, style);
            math_box_t *rd = math_box_new(p->arena, MATH_BOX_DELIM, style);
            if (ld) ld->u.delim.delim = ldelim;
            if (rd) rd->u.delim.delim = rdelim;

            if (hlist) {
                hlist->u.list.head = ld;
                if (ld) ld->next = inner;
                /* Append rd to end of inner chain */
                math_box_t *tail = inner;
                while (tail && tail->next) tail = tail->next;
                if (tail) tail->next = rd;
                else if (ld) ld->next = rd;
            }
            return hlist;
        }

        /* 4. Large Operators: \sum, \prod, \int, \oint */
        if (strcmp(cmd, "sum") == 0 || strcmp(cmd, "prod") == 0 ||
            strcmp(cmd, "int") == 0 || strcmp(cmd, "oint") == 0) {
            math_box_t *bop = math_box_new(p->arena, MATH_BOX_BIGOP, style);
            if (bop) {
                if (strcmp(cmd, "sum") == 0) bop->u.bigop.op = BIGOP_SUM;
                else if (strcmp(cmd, "prod") == 0) bop->u.bigop.op = BIGOP_PROD;
                else if (strcmp(cmd, "int") == 0) bop->u.bigop.op = BIGOP_INT;
                else bop->u.bigop.op = BIGOP_OINT;
            }
            return bop;
        }

        /* 5. Spacing commands: \, \: \; \quad */
        if (strcmp(cmd, ",") == 0 || strcmp(cmd, ":") == 0 || strcmp(cmd, ";") == 0 ||
            strcmp(cmd, "quad") == 0 || strcmp(cmd, "qquad") == 0) {
            math_box_t *sp = math_box_new(p->arena, MATH_BOX_SPACE, style);
            if (sp) {
                if (strcmp(cmd, ",") == 0) sp->u.space.width = 3;
                else if (strcmp(cmd, ":") == 0) sp->u.space.width = 4;
                else if (strcmp(cmd, ";") == 0) sp->u.space.width = 5;
                else if (strcmp(cmd, "quad") == 0) sp->u.space.width = 16;
                else sp->u.space.width = 32;
            }
            return sp;
        }

        /* 6. Named Functions: \sin, \cos, \ln, etc. */
        const char *named_fns[] = { "sin", "cos", "tan", "ln", "log", "exp", "det", "lim", "max", "min", NULL };
        for (int i = 0; named_fns[i]; i++) {
            if (strcmp(cmd, named_fns[i]) == 0) {
                math_box_t *hlist = math_box_new(p->arena, MATH_BOX_HLIST, style);
                math_box_t *cur = NULL;
                for (const char *s = cmd; *s; s++) {
                    math_box_t *ch = math_box_new(p->arena, MATH_BOX_CHAR, style);
                    if (ch) {
                        ch->u.ch.codepoint = (uint8_t)*s;
                        ch->u.ch.font_family = 1; /* upright */
                        if (!hlist->u.list.head) hlist->u.list.head = ch;
                        else if (cur) cur->next = ch;
                        cur = ch;
                    }
                }
                return hlist;
            }
        }

        /* 7. Font Modifiers: \mathbf, \mathbb, \mathcal, \mathrm */
        if (strcmp(cmd, "mathbf") == 0 || strcmp(cmd, "mathbb") == 0 ||
            strcmp(cmd, "mathcal") == 0 || strcmp(cmd, "mathrm") == 0) {
            uint8_t fam = (strcmp(cmd, "mathbb") == 0) ? 2 :
                          (strcmp(cmd, "mathcal") == 0) ? 3 :
                          (strcmp(cmd, "mathrm") == 0) ? 1 : 0;
            math_box_t *inner = parse_group_or_atom(p, style);
            /* Tag chars inside with family */
            math_box_t *k = inner;
            while (k) {
                if (k->type == MATH_BOX_CHAR) k->u.ch.font_family = fam;
                k = k->next;
            }
            return inner;
        }

        /* 8. Matrices: \begin{matrix} ... \end{matrix} */
        if (strcmp(cmd, "begin") == 0) {
            char env[32] = {0};
            if (p->cur.type == TOK_LBRACE) {
                parser_advance(p);
                size_t epos = 0;
                while (p->cur.type == TOK_CHAR && is_alpha((char)p->cur.codepoint) && epos < sizeof(env) - 1) {
                    env[epos++] = (char)p->cur.codepoint;
                    parser_advance(p);
                }
                if (p->cur.type == TOK_RBRACE) parser_advance(p);
            }

            math_box_t *mat = math_box_new(p->arena, MATH_BOX_MATRIX, style);
            if (!mat) return NULL;
            mat->u.matrix.cells = (math_box_t **)math_arena_alloc(p->arena,
                MATH_MATRIX_MAX_ROWS * MATH_MATRIX_MAX_COLS * sizeof(math_box_t *));
            if (!mat->u.matrix.cells) return NULL;

            if (strcmp(env, "pmatrix") == 0) {
                mat->u.matrix.left_delim = DELIM_PAREN_L;
                mat->u.matrix.right_delim = DELIM_PAREN_R;
            } else if (strcmp(env, "bmatrix") == 0) {
                mat->u.matrix.left_delim = DELIM_BRACKET_L;
                mat->u.matrix.right_delim = DELIM_BRACKET_R;
            } else if (strcmp(env, "cases") == 0) {
                mat->u.matrix.left_delim = DELIM_BRACE_L;
                mat->u.matrix.right_delim = DELIM_NONE;
            }

            uint8_t r = 0, c = 0;
            while (p->cur.type != TOK_EOF && r < MATH_MATRIX_MAX_ROWS) {
                /* Check for \end{env} */
                if (p->cur.type == TOK_CMD && strcmp(p->cur.cmd, "end") == 0) {
                    parser_advance(p);
                    if (p->cur.type == TOK_LBRACE) {
                        while (p->cur.type != TOK_EOF && p->cur.type != TOK_RBRACE) parser_advance(p);
                        if (p->cur.type == TOK_RBRACE) parser_advance(p);
                    }
                    break;
                }

                math_box_t *cell = parse_expr(p, style, false);
                if (c < MATH_MATRIX_MAX_COLS) {
                    mat->u.matrix.cells[r * MATH_MATRIX_MAX_COLS + c] = cell;
                }

                if (p->cur.type == TOK_AMP) {
                    parser_advance(p);
                    c++;
                } else if (p->cur.type == TOK_NEWLINE) {
                    parser_advance(p);
                    if (c + 1 > mat->u.matrix.cols) mat->u.matrix.cols = c + 1;
                    r++;
                    c = 0;
                } else {
                    break;
                }
            }
            mat->u.matrix.rows = r + 1;
            if (c + 1 > mat->u.matrix.cols) mat->u.matrix.cols = c + 1;
            return mat;
        }

        /* Look up math symbol in dictionary */
        uint16_t cp = lookup_cmd_symbol(cmd);
        if (cp > 0) {
            math_box_t *b = math_box_new(p->arena, MATH_BOX_CHAR, style);
            if (b) {
                b->u.ch.codepoint = cp;
                b->u.ch.font_family = 4; /* symbol */
            }
            return b;
        }

        /* Fallback: treat unrecognized command as upright string */
        math_box_t *hlist = math_box_new(p->arena, MATH_BOX_HLIST, style);
        math_box_t *cur = NULL;
        for (const char *s = cmd; *s; s++) {
            math_box_t *ch = math_box_new(p->arena, MATH_BOX_CHAR, style);
            if (ch) {
                ch->u.ch.codepoint = (uint8_t)*s;
                ch->u.ch.font_family = 1;
                if (!hlist->u.list.head) hlist->u.list.head = ch;
                else if (cur) cur->next = ch;
                cur = ch;
            }
        }
        return hlist;
    }

    return NULL;
}

static math_box_t *parse_expr(math_parser_t *p, math_style_t style, bool stop_at_delim) {
    math_box_t *head = NULL;
    math_box_t *tail = NULL;

    while (p->cur.type != TOK_EOF && p->cur.type != TOK_RBRACE &&
           p->cur.type != TOK_AMP && p->cur.type != TOK_NEWLINE) {
        if (p->cur.type == TOK_CMD && strcmp(p->cur.cmd, "right") == 0) break;
        if (p->cur.type == TOK_CMD && strcmp(p->cur.cmd, "end") == 0) break;

        math_box_t *atom = parse_atom(p, style);
        if (!atom) break;

        /* Check for scripts attached to atom */
        if (p->cur.type == TOK_SUP || p->cur.type == TOK_SUB) {
            math_style_t script_style = (style == STYLE_DISPLAY || style == STYLE_TEXT) ?
                                        STYLE_SCRIPT : STYLE_SCRIPTSCRIPT;
            math_box_t *sup = NULL;
            math_box_t *sub = NULL;

            while (p->cur.type == TOK_SUP || p->cur.type == TOK_SUB) {
                if (p->cur.type == TOK_SUP) {
                    parser_advance(p);
                    sup = parse_group_or_atom(p, script_style);
                } else if (p->cur.type == TOK_SUB) {
                    parser_advance(p);
                    sub = parse_group_or_atom(p, script_style);
                }
            }

            if (atom->type == MATH_BOX_BIGOP) {
                atom->u.bigop.sup = sup;
                atom->u.bigop.sub = sub;
            } else {
                math_box_t *sc = math_box_new(p->arena, MATH_BOX_SCRIPT, style);
                if (sc) {
                    sc->u.script.nucleus = atom;
                    sc->u.script.sup = sup;
                    sc->u.script.sub = sub;
                    atom = sc;
                }
            }
        }

        if (!head) head = atom;
        else tail->next = atom;
        tail = atom;

        if (stop_at_delim) break;
    }

    if (!head) return NULL;
    if (!head->next) return head;

    math_box_t *hlist = math_box_new(p->arena, MATH_BOX_HLIST, style);
    if (hlist) hlist->u.list.head = head;
    return hlist;
}

math_box_t *latex_parse(const char *src, math_style_t style, math_arena_t *arena) {
    if (!src || !arena) return NULL;
    math_parser_t parser;
    lexer_init(&parser.lexer, src);
    parser.arena = arena;
    parser.style = style;
    parser_advance(&parser);

    return parse_expr(&parser, style, false);
}

/* --- Terminal Unicode Converter --- */

void latex_to_unicode(const char *src, char *out, size_t out_sz) {
    if (!src || !out || out_sz == 0) return;
    out[0] = '\0';

    math_lexer_t lex;
    lexer_init(&lex, src);
    math_token_t tok = lexer_next(&lex);
    size_t opos = 0;

    #define EMIT_STR(s) do { \
        const char *_p = (s); \
        while (*_p && opos < out_sz - 1) out[opos++] = *_p++; \
    } while (0)

    #define EMIT_CHAR(c) do { \
        if (opos < out_sz - 1) out[opos++] = (c); \
    } while (0)

    while (tok.type != TOK_EOF && opos < out_sz - 1) {
        if (tok.type == TOK_CMD) {
            if (strcmp(tok.cmd, "alpha") == 0) EMIT_STR("α");
            else if (strcmp(tok.cmd, "beta") == 0) EMIT_STR("β");
            else if (strcmp(tok.cmd, "gamma") == 0) EMIT_STR("γ");
            else if (strcmp(tok.cmd, "delta") == 0) EMIT_STR("δ");
            else if (strcmp(tok.cmd, "epsilon") == 0) EMIT_STR("ε");
            else if (strcmp(tok.cmd, "theta") == 0) EMIT_STR("θ");
            else if (strcmp(tok.cmd, "lambda") == 0) EMIT_STR("λ");
            else if (strcmp(tok.cmd, "mu") == 0) EMIT_STR("μ");
            else if (strcmp(tok.cmd, "pi") == 0) EMIT_STR("π");
            else if (strcmp(tok.cmd, "sigma") == 0) EMIT_STR("σ");
            else if (strcmp(tok.cmd, "phi") == 0) EMIT_STR("φ");
            else if (strcmp(tok.cmd, "omega") == 0) EMIT_STR("ω");
            else if (strcmp(tok.cmd, "Delta") == 0) EMIT_STR("Δ");
            else if (strcmp(tok.cmd, "Sigma") == 0) EMIT_STR("Σ");
            else if (strcmp(tok.cmd, "Omega") == 0) EMIT_STR("Ω");
            else if (strcmp(tok.cmd, "le") == 0 || strcmp(tok.cmd, "leq") == 0) EMIT_STR(" ≤ ");
            else if (strcmp(tok.cmd, "ge") == 0 || strcmp(tok.cmd, "geq") == 0) EMIT_STR(" ≥ ");
            else if (strcmp(tok.cmd, "neq") == 0) EMIT_STR(" ≠ ");
            else if (strcmp(tok.cmd, "approx") == 0) EMIT_STR(" ≈ ");
            else if (strcmp(tok.cmd, "pm") == 0) EMIT_STR(" ± ");
            else if (strcmp(tok.cmd, "mp") == 0) EMIT_STR(" ∓ ");
            else if (strcmp(tok.cmd, "times") == 0) EMIT_STR(" × ");
            else if (strcmp(tok.cmd, "cdot") == 0) EMIT_STR(" · ");
            else if (strcmp(tok.cmd, "infty") == 0) EMIT_STR("∞");
            else if (strcmp(tok.cmd, "partial") == 0) EMIT_STR("∂");
            else if (strcmp(tok.cmd, "nabla") == 0) EMIT_STR("∇");
            else if (strcmp(tok.cmd, "sum") == 0) EMIT_STR("∑");
            else if (strcmp(tok.cmd, "prod") == 0) EMIT_STR("∏");
            else if (strcmp(tok.cmd, "int") == 0) EMIT_STR("∫");
            else if (strcmp(tok.cmd, "sqrt") == 0) EMIT_STR("√");
            else if (strcmp(tok.cmd, "to") == 0) EMIT_STR("→");
            else if (strcmp(tok.cmd, "implies") == 0) EMIT_STR("⇒");
            else if (strcmp(tok.cmd, "iff") == 0) EMIT_STR("⇔");
            else if (strcmp(tok.cmd, "frac") == 0) { /* will process inline */ }
            else if (strcmp(tok.cmd, "left") == 0 || strcmp(tok.cmd, "right") == 0) { }
            else { EMIT_STR(tok.cmd); }
        } else if (tok.type == TOK_SUP) {
            math_token_t next = lexer_next(&lex);
            if (next.type == TOK_CHAR && next.codepoint == '0') EMIT_STR("⁰");
            else if (next.type == TOK_CHAR && next.codepoint == '1') EMIT_STR("¹");
            else if (next.type == TOK_CHAR && next.codepoint == '2') EMIT_STR("²");
            else if (next.type == TOK_CHAR && next.codepoint == '3') EMIT_STR("³");
            else if (next.type == TOK_CHAR && next.codepoint == '4') EMIT_STR("⁴");
            else if (next.type == TOK_CHAR && next.codepoint == '5') EMIT_STR("⁵");
            else if (next.type == TOK_CHAR && next.codepoint == '6') EMIT_STR("⁶");
            else if (next.type == TOK_CHAR && next.codepoint == '7') EMIT_STR("⁷");
            else if (next.type == TOK_CHAR && next.codepoint == '8') EMIT_STR("⁸");
            else if (next.type == TOK_CHAR && next.codepoint == '9') EMIT_STR("⁹");
            else if (next.type == TOK_CHAR && next.codepoint == '+') EMIT_STR("⁺");
            else if (next.type == TOK_CHAR && next.codepoint == '-') EMIT_STR("⁻");
            else if (next.type == TOK_CHAR && next.codepoint == 'n') EMIT_STR("ⁿ");
            else if (next.type == TOK_CHAR && next.codepoint == 'i') EMIT_STR("ⁱ");
            else {
                EMIT_CHAR('^');
                tok = next;
                continue;
            }
        } else if (tok.type == TOK_SUB) {
            math_token_t next = lexer_next(&lex);
            if (next.type == TOK_CHAR && next.codepoint == '0') EMIT_STR("₀");
            else if (next.type == TOK_CHAR && next.codepoint == '1') EMIT_STR("₁");
            else if (next.type == TOK_CHAR && next.codepoint == '2') EMIT_STR("₂");
            else if (next.type == TOK_CHAR && next.codepoint == '3') EMIT_STR("₃");
            else if (next.type == TOK_CHAR && next.codepoint == '4') EMIT_STR("₄");
            else if (next.type == TOK_CHAR && next.codepoint == '5') EMIT_STR("₅");
            else if (next.type == TOK_CHAR && next.codepoint == '6') EMIT_STR("₆");
            else if (next.type == TOK_CHAR && next.codepoint == '7') EMIT_STR("₇");
            else if (next.type == TOK_CHAR && next.codepoint == '8') EMIT_STR("₈");
            else if (next.type == TOK_CHAR && next.codepoint == '9') EMIT_STR("₉");
            else if (next.type == TOK_CHAR && next.codepoint == '+') EMIT_STR("₊");
            else if (next.type == TOK_CHAR && next.codepoint == '-') EMIT_STR("₋");
            else if (next.type == TOK_CHAR && next.codepoint == 'a') EMIT_STR("ₐ");
            else if (next.type == TOK_CHAR && next.codepoint == 'e') EMIT_STR("ₑ");
            else if (next.type == TOK_CHAR && next.codepoint == 'o') EMIT_STR("ₒ");
            else if (next.type == TOK_CHAR && next.codepoint == 'x') EMIT_STR("ₓ");
            else if (next.type == TOK_CHAR && next.codepoint == 'i') EMIT_STR("ᵢ");
            else {
                EMIT_CHAR('_');
                tok = next;
                continue;
            }
        } else if (tok.type == TOK_CHAR || tok.type == TOK_LPAREN || tok.type == TOK_RPAREN ||
                   tok.type == TOK_LBRACKET || tok.type == TOK_RBRACKET || tok.type == TOK_BAR) {
            char ch = (char)tok.codepoint;
            if (ch == '+' || ch == '=' || ch == '-') {
                if (opos > 0 && out[opos - 1] != ' ') EMIT_CHAR(' ');
                EMIT_CHAR(ch);
                EMIT_CHAR(' ');
            } else {
                EMIT_CHAR(ch);
            }
        }

        tok = lexer_next(&lex);
    }

    out[opos] = '\0';
    #undef EMIT_STR
    #undef EMIT_CHAR
}
