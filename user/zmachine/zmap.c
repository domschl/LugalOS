/* user/zmachine/zmap.c — Auto-mapper for Z-machine Version 3 games
 *
 * Tracks room discovery and movement across all .z3 games and renders
 * a live directed graph on the LugalOS split-screen canvas.
 */
#include "zmap.h"
#include "zvm.h"
#include "ztext.h"

#include <string.h>

#ifdef ZMACHINE_HOST
static inline bool console_canvas(const uint8_t *req, uint32_t n, uint8_t *reply)
{
    (void)req; (void)n; (void)reply;
    return false;
}
#define SCREEN_LAYOUT_TEXT 0
#define SCREEN_LAYOUT_SPLIT_HALF 3
#define SCREEN_REPLY_LEN 16
#else
#include "kernel/console.h"
#include "kernel/printk.h"
#include "drivers/screen.h"
#endif

/* Grid geometry */
#define BOX_W 76
#define BOX_H 26
#define CELL_SPACING_X 108
#define CELL_SPACING_Y 48
#define TOP_BANNER_H 24

/* ------------------------------------------------------------------ */
/* Canvas primitives via console_canvas                               */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *b, int v)
{
    b[0] = (uint8_t)((unsigned)v & 0xffu);
    b[1] = (uint8_t)(((unsigned)v >> 8) & 0xffu);
}

static void canvas_clear(void)
{
    uint8_t req[3] = { 'F', 0, 0 }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, sizeof(req), reply);
}

static void canvas_rect(int x, int y, int w, int h, int color)
{
    uint8_t req[11], reply[SCREEN_REPLY_LEN];
    req[0] = 'r';
    put16(req + 1, x);
    put16(req + 3, y);
    put16(req + 5, w);
    put16(req + 7, h);
    put16(req + 9, color);
    (void)console_canvas(req, sizeof(req), reply);
}

static void canvas_frame(int x, int y, int w, int h, int color)
{
    uint8_t req[11], reply[SCREEN_REPLY_LEN];
    req[0] = 'o';
    put16(req + 1, x);
    put16(req + 3, y);
    put16(req + 5, w);
    put16(req + 7, h);
    put16(req + 9, color);
    (void)console_canvas(req, sizeof(req), reply);
}

static void canvas_line(int x1, int y1, int x2, int y2, int color)
{
    uint8_t req[11], reply[SCREEN_REPLY_LEN];
    req[0] = 'l';
    put16(req + 1, x1);
    put16(req + 3, y1);
    put16(req + 5, x2);
    put16(req + 7, y2);
    put16(req + 9, color);
    (void)console_canvas(req, sizeof(req), reply);
}

static void canvas_text(int x, int y, int color, const char *text)
{
    uint8_t req[64], reply[SCREEN_REPLY_LEN];
    req[0] = 't';
    put16(req + 1, x);
    put16(req + 3, y);
    req[5] = (uint8_t)color;
    size_t len = strlen(text);
    if (len > sizeof(req) - 6) len = sizeof(req) - 6;
    memcpy(req + 6, text, len);
    (void)console_canvas(req, 6u + (uint32_t)len, reply);
}

static void canvas_invert(int x, int y, int w, int h)
{
    uint8_t req[11], reply[SCREEN_REPLY_LEN];
    req[0] = 'i';
    put16(req + 1, x);
    put16(req + 3, y);
    put16(req + 5, w);
    put16(req + 7, h);
    (void)console_canvas(req, sizeof(req), reply);
}

/* ------------------------------------------------------------------ */
/* Direction parsing                                                  */
/* ------------------------------------------------------------------ */

static uint8_t parse_direction(const uint8_t *cmd, int len)
{
    while (len > 0 && (*cmd == ' ' || *cmd == '\t')) {
        cmd++; len--;
    }
    /* Skip prefixes: "go ", "walk ", "climb " */
    if (len >= 3 && cmd[0] == 'g' && cmd[1] == 'o' && cmd[2] == ' ') {
        cmd += 3; len -= 3;
        while (len > 0 && (*cmd == ' ' || *cmd == '\t')) { cmd++; len--; }
    } else if (len >= 5 && memcmp(cmd, "walk ", 5) == 0) {
        cmd += 5; len -= 5;
        while (len > 0 && (*cmd == ' ' || *cmd == '\t')) { cmd++; len--; }
    } else if (len >= 6 && memcmp(cmd, "climb ", 6) == 0) {
        cmd += 6; len -= 6;
        while (len > 0 && (*cmd == ' ' || *cmd == '\t')) { cmd++; len--; }
    }

    if ((len == 1 && cmd[0] == 'n') || (len >= 5 && memcmp(cmd, "north", 5) == 0)) return ZMAP_DIR_NORTH;
    if ((len == 1 && cmd[0] == 's') || (len >= 5 && memcmp(cmd, "south", 5) == 0)) return ZMAP_DIR_SOUTH;
    if ((len == 1 && cmd[0] == 'e') || (len >= 4 && memcmp(cmd, "east", 4) == 0))  return ZMAP_DIR_EAST;
    if ((len == 1 && cmd[0] == 'w') || (len >= 4 && memcmp(cmd, "west", 4) == 0))  return ZMAP_DIR_WEST;
    if ((len == 2 && cmd[0] == 'n' && cmd[1] == 'e') || (len >= 9 && memcmp(cmd, "northeast", 9) == 0)) return ZMAP_DIR_NE;
    if ((len == 2 && cmd[0] == 'n' && cmd[1] == 'w') || (len >= 9 && memcmp(cmd, "northwest", 9) == 0)) return ZMAP_DIR_NW;
    if ((len == 2 && cmd[0] == 's' && cmd[1] == 'e') || (len >= 9 && memcmp(cmd, "southeast", 9) == 0)) return ZMAP_DIR_SE;
    if ((len == 2 && cmd[0] == 's' && cmd[1] == 'w') || (len >= 9 && memcmp(cmd, "southwest", 9) == 0)) return ZMAP_DIR_SW;
    if ((len == 1 && cmd[0] == 'u') || (len >= 2 && memcmp(cmd, "up", 2) == 0))    return ZMAP_DIR_UP;
    if ((len == 1 && cmd[0] == 'd') || (len >= 4 && memcmp(cmd, "down", 4) == 0))  return ZMAP_DIR_DOWN;
    return ZMAP_DIR_SPECIAL;
}

/* ------------------------------------------------------------------ */
/* Graph tracking & collision resolution                              */
/* ------------------------------------------------------------------ */

static struct zmap_room *find_room(struct zmap_state *s, uint16_t obj_id)
{
    for (uint16_t i = 0; i < s->room_count; i++) {
        if (s->rooms[i].obj_id == obj_id) {
            return &s->rooms[i];
        }
    }
    return NULL;
}

static bool has_edge(struct zmap_state *s, uint16_t from_obj, uint16_t to_obj)
{
    for (uint16_t i = 0; i < s->edge_count; i++) {
        if (s->edges[i].from_obj == from_obj && s->edges[i].to_obj == to_obj) {
            return true;
        }
    }
    return false;
}

static void add_edge(struct zmap_state *s, uint16_t from_obj, uint16_t to_obj, uint8_t dir)
{
    if (from_obj == 0 || to_obj == 0 || from_obj == to_obj) return;
    if (has_edge(s, from_obj, to_obj)) return;
    if (s->edge_count < ZMAP_MAX_EDGES) {
        s->edges[s->edge_count].from_obj = from_obj;
        s->edges[s->edge_count].to_obj = to_obj;
        s->edges[s->edge_count].dir = dir;
        s->edge_count++;
    }
}

static bool is_grid_occupied(struct zmap_state *s, int16_t gx, int16_t gy)
{
    for (uint16_t i = 0; i < s->room_count; i++) {
        if (s->rooms[i].gx == gx && s->rooms[i].gy == gy) {
            return true;
        }
    }
    return false;
}

static void resolve_placement(struct zmap_state *s, int16_t base_gx, int16_t base_gy,
                              uint8_t dir, int16_t *out_gx, int16_t *out_gy)
{
    int16_t target_gx = base_gx;
    int16_t target_gy = base_gy;
    switch (dir) {
        case ZMAP_DIR_NORTH: target_gy--; break;
        case ZMAP_DIR_SOUTH: target_gy++; break;
        case ZMAP_DIR_EAST:  target_gx++; break;
        case ZMAP_DIR_WEST:  target_gx--; break;
        case ZMAP_DIR_NE:    target_gx++; target_gy--; break;
        case ZMAP_DIR_NW:    target_gx--; target_gy--; break;
        case ZMAP_DIR_SE:    target_gx++; target_gy++; break;
        case ZMAP_DIR_SW:    target_gx--; target_gy++; break;
        case ZMAP_DIR_UP:    target_gy--; break;
        case ZMAP_DIR_DOWN:  target_gy++; break;
        default:             target_gx++; break;
    }

    if (!is_grid_occupied(s, target_gx, target_gy)) {
        *out_gx = target_gx;
        *out_gy = target_gy;
        return;
    }

    /* Search nearest unoccupied adjacent grid slot */
    static const int8_t offsets[][2] = {
        {1, 0}, {0, 1}, {-1, 0}, {0, -1},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
        {2, 0}, {-2, 0}, {0, 2}, {0, -2}
    };
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        int16_t nx = target_gx + offsets[i][0];
        int16_t ny = target_gy + offsets[i][1];
        if (!is_grid_occupied(s, nx, ny)) {
            *out_gx = nx;
            *out_gy = ny;
            return;
        }
    }
    *out_gx = target_gx;
    *out_gy = target_gy;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

bool zmap_init(struct zmap_state *s)
{
    uint8_t req[8] = { 'L', SCREEN_LAYOUT_SPLIT_HALF }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 2, reply) || reply[0] != 0) {
        s->active = false;
        return false;
    }
    s->active = true;
    s->canvas_w = (int)(reply[2] | (reply[3] << 8));
    s->canvas_h = (int)(reply[4] | (reply[5] << 8));
    if (s->canvas_w <= 0 || s->canvas_h <= 0) {
        s->canvas_w = 400;
        s->canvas_h = 440;
    }
    s->room_count = 0;
    s->edge_count = 0;
    s->current_room = 0;
    s->last_room = 0;
    s->last_dir = ZMAP_DIR_NONE;

    canvas_clear();
    return true;
}

void zmap_reset(struct zmap_state *s)
{
    if (!s) return;
    s->room_count = 0;
    s->edge_count = 0;
    s->current_room = 0;
    s->last_room = 0;
    s->last_dir = ZMAP_DIR_NONE;
    if (s->active) {
        canvas_clear();
    }
}

void zmap_close(struct zmap_state *s)
{
    if (s && s->active) {
        uint8_t req[2] = { 'L', SCREEN_LAYOUT_TEXT }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 2, reply);
        s->active = false;
    }
}

/* ------------------------------------------------------------------ */
/* Turn hooks & rendering                                             */
/* ------------------------------------------------------------------ */

void zmap_record_command(struct zmap_state *s, const uint8_t *line, int len)
{
    if (!s || !s->active || len <= 0) return;
    s->last_dir = parse_direction(line, len);
}

void zmap_on_turn(struct zmap_state *s, struct z_vm *vm)
{
    if (!s || !s->active || !vm) return;

    /* Global 0 is always HERE in v3 */
    uint16_t curr_room_id = z_read_word(&vm->m, vm->m.global_base);
    if (curr_room_id == 0) return;

    struct zmap_room *curr = find_room(s, curr_room_id);
    if (curr == NULL) {
        if (s->room_count < ZMAP_MAX_ROOMS) {
            curr = &s->rooms[s->room_count++];
            curr->obj_id = curr_room_id;
            curr->visited = 1;
            z_object_get_name(vm, curr_room_id, curr->name, sizeof(curr->name));

            if (s->last_room != 0) {
                struct zmap_room *prev = find_room(s, s->last_room);
                int16_t base_x = prev ? prev->gx : 0;
                int16_t base_y = prev ? prev->gy : 0;
                resolve_placement(s, base_x, base_y, s->last_dir, &curr->gx, &curr->gy);
            } else {
                curr->gx = 0;
                curr->gy = 0;
            }
        }
    }

    if (s->last_room != 0 && curr_room_id != s->last_room) {
        add_edge(s, s->last_room, curr_room_id, s->last_dir);
    }

    s->current_room = curr_room_id;
    s->last_room = curr_room_id;
    s->last_dir = ZMAP_DIR_NONE;

    zmap_render(s);
}

void zmap_render(struct zmap_state *s)
{
    if (!s || !s->active) return;

    canvas_clear();

    /* Find current room for camera centering and title */
    struct zmap_room *cur = find_room(s, s->current_room);
    int16_t cam_gx = cur ? cur->gx : 0;
    int16_t cam_gy = cur ? cur->gy : 0;

    /* 1. Header Banner */
    char banner[48];
    banner[0] = 'M'; banner[1] = 'A'; banner[2] = 'P'; banner[3] = ':'; banner[4] = ' ';
    size_t blen = 5;
    if (cur && cur->name[0]) {
        size_t nlen = strlen(cur->name);
        if (nlen > sizeof(banner) - 6) nlen = sizeof(banner) - 6;
        memcpy(banner + blen, cur->name, nlen);
        blen += nlen;
    }
    banner[blen] = '\0';
    canvas_text(8, 4, 0, banner);
    canvas_line(0, TOP_BANNER_H - 2, s->canvas_w - 1, TOP_BANNER_H - 2, 1);

    /* 2. Viewport coordinates */
    int cx = s->canvas_w / 2;
    int cy = TOP_BANNER_H + (s->canvas_h - TOP_BANNER_H) / 2;

    /* 3. Draw Edges first */
    for (uint16_t i = 0; i < s->edge_count; i++) {
        struct zmap_room *r1 = find_room(s, s->edges[i].from_obj);
        struct zmap_room *r2 = find_room(s, s->edges[i].to_obj);
        if (!r1 || !r2) continue;

        int x1 = cx + (r1->gx - cam_gx) * CELL_SPACING_X;
        int y1 = cy + (r1->gy - cam_gy) * CELL_SPACING_Y;
        int x2 = cx + (r2->gx - cam_gx) * CELL_SPACING_X;
        int y2 = cy + (r2->gy - cam_gy) * CELL_SPACING_Y;

        /* Skip if completely outside canvas */
        if ((x1 < -BOX_W && x2 < -BOX_W) || (x1 > s->canvas_w + BOX_W && x2 > s->canvas_w + BOX_W) ||
            (y1 < 0 && y2 < 0) || (y1 > s->canvas_h + BOX_H && y2 > s->canvas_h + BOX_H)) {
            continue;
        }

        canvas_line(x1, y1, x2, y2, 1);

        /* Draw elevation badge if vertical exit */
        if (s->edges[i].dir == ZMAP_DIR_UP) {
            canvas_text((x1 + x2) / 2 - 4, (y1 + y2) / 2 - 8, 0, "^");
        } else if (s->edges[i].dir == ZMAP_DIR_DOWN) {
            canvas_text((x1 + x2) / 2 - 4, (y1 + y2) / 2 - 8, 0, "v");
        }
    }

    /* 4. Draw Room Boxes */
    for (uint16_t i = 0; i < s->room_count; i++) {
        struct zmap_room *r = &s->rooms[i];
        int rx = cx + (r->gx - cam_gx) * CELL_SPACING_X;
        int ry = cy + (r->gy - cam_gy) * CELL_SPACING_Y;
        int bx = rx - BOX_W / 2;
        int by = ry - BOX_H / 2;

        /* Clip to canvas bounds */
        if (bx + BOX_W < 0 || bx >= s->canvas_w || by + BOX_H < TOP_BANNER_H || by >= s->canvas_h) {
            continue;
        }

        /* 8-character label, skipping leading articles */
        const char *display_name = r->name;
        if (strncmp(display_name, "The ", 4) == 0) display_name += 4;
        else if (strncmp(display_name, "the ", 4) == 0) display_name += 4;
        else if (strncmp(display_name, "A ", 2) == 0) display_name += 2;
        else if (strncmp(display_name, "An ", 3) == 0) display_name += 3;

        char label[10];
        size_t nlen = strlen(display_name);
        if (nlen > 8) nlen = 8;
        memcpy(label, display_name, nlen);
        label[nlen] = '\0';

        if (r->obj_id == s->current_room) {
            /* Current room: White fill, draw text, then invert interior for crisp white-on-black */
            canvas_rect(bx, by, BOX_W, BOX_H, 0);
            canvas_text(bx + 6, by + 5, 0, label);
            canvas_invert(bx, by, BOX_W, BOX_H);
            canvas_frame(bx - 2, by - 2, BOX_W + 4, BOX_H + 4, 1);
        } else {
            /* Visited room: White fill with black outline */
            canvas_rect(bx, by, BOX_W, BOX_H, 0);
            canvas_frame(bx, by, BOX_W, BOX_H, 1);
            canvas_text(bx + 6, by + 5, 0, label);
        }
    }
}
