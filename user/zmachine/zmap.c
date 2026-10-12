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
#include <stdio.h>
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
#include "fs/vfs.h"
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

/* ------------------------------------------------------------------ */
/* Helpers for big-endian serialization & initials                    */
/* ------------------------------------------------------------------ */

static void p16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)(v & 0xFFu);
}

static uint16_t g16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((uint16_t)(*(*p)) << 8);
    v = (uint16_t)(v | (*(*p + 1u)));
    *p += 2u;
    return v;
}

static void get_room_initials(const char *name, char out[3])
{
    out[0] = '?'; out[1] = '?'; out[2] = '\0';
    if (!name || !name[0]) return;

    /* Skip leading articles "The ", "the ", "A ", "An " */
    if (strncmp(name, "The ", 4) == 0 || strncmp(name, "the ", 4) == 0) name += 4;
    else if (strncmp(name, "A ", 2) == 0 || strncmp(name, "a ", 2) == 0) name += 2;
    else if (strncmp(name, "An ", 3) == 0 || strncmp(name, "an ", 3) == 0) name += 3;

    while (*name == ' ' || *name == '\t') name++;
    if (!*name) return;

    char c1 = *name++;
    if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - 'a' + 'A');
    out[0] = c1;

    /* Look for second word (skipping "of ", "the ", "and ") */
    char c2 = '\0';
    const char *p = name;
    while (*p) {
        if (*p == ' ' || *p == '\t') {
            while (*p == ' ' || *p == '\t') p++;
            if (*p && strncmp(p, "of ", 3) != 0 && strncmp(p, "the ", 4) != 0 &&
                strncmp(p, "and ", 4) != 0) {
                c2 = *p;
                if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
                break;
            }
        } else {
            p++;
        }
    }

    if (c2 != '\0') {
        out[1] = c2;
    } else {
        char sc = *name;
        if (sc >= 'a' && sc <= 'z') sc = (char)(sc - 'a' + 'A');
        out[1] = sc ? sc : ' ';
    }
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
    s->zoom_mode = false;

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
    s->zoom_mode = false;
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

void zmap_toggle_zoom(struct zmap_state *s)
{
    if (!s) return;
    s->zoom_mode = !s->zoom_mode;
}

void zmap_set_zoom(struct zmap_state *s, bool zoom)
{
    if (!s) return;
    s->zoom_mode = zoom;
}

bool zmap_get_zoom(const struct zmap_state *s)
{
    return s ? s->zoom_mode : false;
}

/* ------------------------------------------------------------------ */
/* Serialization & Persistence                                        */
/* ------------------------------------------------------------------ */

uint32_t zmap_serialize(const struct zmap_state *s, uint8_t *buf, uint32_t cap)
{
    if (!s || !buf) return 0;
    uint32_t needed = 18u + (uint32_t)s->room_count * 43u + (uint32_t)s->edge_count * 6u;
    if (cap < needed) return 0;

    uint8_t *p = buf;
    *p++ = 'Z'; *p++ = 'M'; *p++ = 'A'; *p++ = 'P';
    p16(&p, 1u); /* version 1 */
    p16(&p, s->zoom_mode ? 1u : 0u);
    p16(&p, s->room_count);
    p16(&p, s->edge_count);
    p16(&p, s->current_room);
    p16(&p, s->last_room);
    *p++ = s->last_dir;
    *p++ = 0; /* reserved */

    for (uint16_t i = 0; i < s->room_count; i++) {
        p16(&p, s->rooms[i].obj_id);
        p16(&p, (uint16_t)s->rooms[i].gx);
        p16(&p, (uint16_t)s->rooms[i].gy);
        *p++ = s->rooms[i].visited;
        memcpy(p, s->rooms[i].name, ZMAP_NAME_MAX);
        p += ZMAP_NAME_MAX;
    }

    for (uint16_t i = 0; i < s->edge_count; i++) {
        p16(&p, s->edges[i].from_obj);
        p16(&p, s->edges[i].to_obj);
        *p++ = s->edges[i].dir;
        *p++ = 0;
    }

    return (uint32_t)(p - buf);
}

int zmap_deserialize(struct zmap_state *s, const uint8_t *buf, uint32_t len)
{
    if (!s || !buf || len < 18u) return -1;
    const uint8_t *p = buf;
    if (p[0] != 'Z' || p[1] != 'M' || p[2] != 'A' || p[3] != 'P') return -1;
    p += 4;
    uint16_t ver = g16(&p);
    if (ver != 1u) return -1;
    uint16_t zoom = g16(&p);
    uint16_t rc = g16(&p);
    uint16_t ec = g16(&p);
    uint16_t cur = g16(&p);
    uint16_t last = g16(&p);
    uint8_t ldir = *p++;
    p++; /* reserved */

    if (rc > ZMAP_MAX_ROOMS || ec > ZMAP_MAX_EDGES) return -1;
    uint32_t needed = 18u + (uint32_t)rc * 43u + (uint32_t)ec * 6u;
    if (len < needed) return -1;

    s->zoom_mode = (zoom != 0u);
    s->room_count = rc;
    s->edge_count = ec;
    s->current_room = cur;
    s->last_room = last;
    s->last_dir = ldir;

    for (uint16_t i = 0; i < rc; i++) {
        s->rooms[i].obj_id = g16(&p);
        s->rooms[i].gx = (int16_t)g16(&p);
        s->rooms[i].gy = (int16_t)g16(&p);
        s->rooms[i].visited = *p++;
        memcpy(s->rooms[i].name, p, ZMAP_NAME_MAX);
        s->rooms[i].name[ZMAP_NAME_MAX - 1] = '\0';
        p += ZMAP_NAME_MAX;
    }

    for (uint16_t i = 0; i < ec; i++) {
        s->edges[i].from_obj = g16(&p);
        s->edges[i].to_obj = g16(&p);
        s->edges[i].dir = *p++;
        p++; /* pad */
    }

    return 0;
}

int zmap_save(const struct zmap_state *s, const char *path)
{
    if (!s || !path) return -1;
#ifdef ZMACHINE_HOST
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[18];
    uint8_t *p = hdr;
    *p++ = 'Z'; *p++ = 'M'; *p++ = 'A'; *p++ = 'P';
    p16(&p, 1u);
    p16(&p, s->zoom_mode ? 1u : 0u);
    p16(&p, s->room_count);
    p16(&p, s->edge_count);
    p16(&p, s->current_room);
    p16(&p, s->last_room);
    *p++ = s->last_dir;
    *p++ = 0;
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }
    for (uint16_t i = 0; i < s->room_count; i++) {
        uint8_t rbuf[43];
        p = rbuf;
        p16(&p, s->rooms[i].obj_id);
        p16(&p, (uint16_t)s->rooms[i].gx);
        p16(&p, (uint16_t)s->rooms[i].gy);
        *p++ = s->rooms[i].visited;
        memcpy(p, s->rooms[i].name, ZMAP_NAME_MAX);
        if (fwrite(rbuf, 1, sizeof(rbuf), f) != sizeof(rbuf)) {
            fclose(f);
            return -1;
        }
    }
    for (uint16_t i = 0; i < s->edge_count; i++) {
        uint8_t ebuf[6];
        p = ebuf;
        p16(&p, s->edges[i].from_obj);
        p16(&p, s->edges[i].to_obj);
        *p++ = s->edges[i].dir;
        *p++ = 0;
        if (fwrite(ebuf, 1, sizeof(ebuf), f) != sizeof(ebuf)) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
#else
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return -1;
    uint8_t hdr[18];
    uint8_t *p = hdr;
    *p++ = 'Z'; *p++ = 'M'; *p++ = 'A'; *p++ = 'P';
    p16(&p, 1u);
    p16(&p, s->zoom_mode ? 1u : 0u);
    p16(&p, s->room_count);
    p16(&p, s->edge_count);
    p16(&p, s->current_room);
    p16(&p, s->last_room);
    *p++ = s->last_dir;
    *p++ = 0;
    int off = 0;
    if (vfs_pwrite(fd, hdr, sizeof(hdr), off) != (int)sizeof(hdr)) {
        vfs_close(fd);
        return -1;
    }
    off += (int)sizeof(hdr);
    for (uint16_t i = 0; i < s->room_count; i++) {
        uint8_t rbuf[43];
        p = rbuf;
        p16(&p, s->rooms[i].obj_id);
        p16(&p, (uint16_t)s->rooms[i].gx);
        p16(&p, (uint16_t)s->rooms[i].gy);
        *p++ = s->rooms[i].visited;
        memcpy(p, s->rooms[i].name, ZMAP_NAME_MAX);
        if (vfs_pwrite(fd, rbuf, sizeof(rbuf), off) != (int)sizeof(rbuf)) {
            vfs_close(fd);
            return -1;
        }
        off += (int)sizeof(rbuf);
    }
    for (uint16_t i = 0; i < s->edge_count; i++) {
        uint8_t ebuf[6];
        p = ebuf;
        p16(&p, s->edges[i].from_obj);
        p16(&p, s->edges[i].to_obj);
        *p++ = s->edges[i].dir;
        *p++ = 0;
        if (vfs_pwrite(fd, ebuf, sizeof(ebuf), off) != (int)sizeof(ebuf)) {
            vfs_close(fd);
            return -1;
        }
        off += (int)sizeof(ebuf);
    }
    vfs_close(fd);
    return 0;
#endif
}

int zmap_load(struct zmap_state *s, const char *path)
{
    if (!s || !path) return -1;
#ifdef ZMACHINE_HOST
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[18];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }
    const uint8_t *p = hdr;
    if (p[0] != 'Z' || p[1] != 'M' || p[2] != 'A' || p[3] != 'P') {
        fclose(f);
        return -1;
    }
    p += 4;
    if (g16(&p) != 1u) { fclose(f); return -1; }
    uint16_t zoom = g16(&p);
    uint16_t rc = g16(&p);
    uint16_t ec = g16(&p);
    uint16_t cur = g16(&p);
    uint16_t last = g16(&p);
    uint8_t ldir = *p++;

    if (rc > ZMAP_MAX_ROOMS || ec > ZMAP_MAX_EDGES) {
        fclose(f);
        return -1;
    }
    s->zoom_mode = (zoom != 0u);
    s->room_count = rc;
    s->edge_count = ec;
    s->current_room = cur;
    s->last_room = last;
    s->last_dir = ldir;

    for (uint16_t i = 0; i < rc; i++) {
        uint8_t rbuf[43];
        if (fread(rbuf, 1, sizeof(rbuf), f) != sizeof(rbuf)) {
            fclose(f);
            return -1;
        }
        p = rbuf;
        s->rooms[i].obj_id = g16(&p);
        s->rooms[i].gx = (int16_t)g16(&p);
        s->rooms[i].gy = (int16_t)g16(&p);
        s->rooms[i].visited = *p++;
        memcpy(s->rooms[i].name, p, ZMAP_NAME_MAX);
        s->rooms[i].name[ZMAP_NAME_MAX - 1] = '\0';
    }
    for (uint16_t i = 0; i < ec; i++) {
        uint8_t ebuf[6];
        if (fread(ebuf, 1, sizeof(ebuf), f) != sizeof(ebuf)) {
            fclose(f);
            return -1;
        }
        p = ebuf;
        s->edges[i].from_obj = g16(&p);
        s->edges[i].to_obj = g16(&p);
        s->edges[i].dir = *p++;
    }
    fclose(f);
    return 0;
#else
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;
    uint8_t hdr[18];
    int off = 0;
    if (vfs_pread(fd, hdr, sizeof(hdr), off) != (int)sizeof(hdr)) {
        vfs_close(fd);
        return -1;
    }
    off += (int)sizeof(hdr);
    const uint8_t *p = hdr;
    if (p[0] != 'Z' || p[1] != 'M' || p[2] != 'A' || p[3] != 'P') {
        vfs_close(fd);
        return -1;
    }
    p += 4;
    if (g16(&p) != 1u) { vfs_close(fd); return -1; }
    uint16_t zoom = g16(&p);
    uint16_t rc = g16(&p);
    uint16_t ec = g16(&p);
    uint16_t cur = g16(&p);
    uint16_t last = g16(&p);
    uint8_t ldir = *p++;

    if (rc > ZMAP_MAX_ROOMS || ec > ZMAP_MAX_EDGES) {
        vfs_close(fd);
        return -1;
    }
    s->zoom_mode = (zoom != 0u);
    s->room_count = rc;
    s->edge_count = ec;
    s->current_room = cur;
    s->last_room = last;
    s->last_dir = ldir;

    for (uint16_t i = 0; i < rc; i++) {
        uint8_t rbuf[43];
        if (vfs_pread(fd, rbuf, sizeof(rbuf), off) != (int)sizeof(rbuf)) {
            vfs_close(fd);
            return -1;
        }
        off += (int)sizeof(rbuf);
        p = rbuf;
        s->rooms[i].obj_id = g16(&p);
        s->rooms[i].gx = (int16_t)g16(&p);
        s->rooms[i].gy = (int16_t)g16(&p);
        s->rooms[i].visited = *p++;
        memcpy(s->rooms[i].name, p, ZMAP_NAME_MAX);
        s->rooms[i].name[ZMAP_NAME_MAX - 1] = '\0';
    }
    for (uint16_t i = 0; i < ec; i++) {
        uint8_t ebuf[6];
        if (vfs_pread(fd, ebuf, sizeof(ebuf), off) != (int)sizeof(ebuf)) {
            vfs_close(fd);
            return -1;
        }
        off += (int)sizeof(ebuf);
        p = ebuf;
        s->edges[i].from_obj = g16(&p);
        s->edges[i].to_obj = g16(&p);
        s->edges[i].dir = *p++;
    }
    vfs_close(fd);
    return 0;
#endif
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
    char banner[64];
    banner[0] = 'M'; banner[1] = 'A'; banner[2] = 'P'; banner[3] = ':'; banner[4] = ' ';
    size_t blen = 5;
    if (cur && cur->name[0]) {
        size_t nlen = strlen(cur->name);
        if (nlen > 24) nlen = 24;
        memcpy(banner + blen, cur->name, nlen);
        blen += nlen;
    }
    const char *tag = s->zoom_mode ? " [Tab:Detail]" : " [Tab:Zoom]";
    size_t tlen = strlen(tag);
    if (blen + tlen < sizeof(banner) - 1) {
        memcpy(banner + blen, tag, tlen);
        blen += tlen;
    }
    banner[blen] = '\0';
    canvas_text(8, 4, 0, banner);
    canvas_line(0, TOP_BANNER_H - 2, s->canvas_w - 1, TOP_BANNER_H - 2, 1);

    /* 2. Viewport coordinates */
    int cx = s->canvas_w / 2;
    int cy = TOP_BANNER_H + (s->canvas_h - TOP_BANNER_H) / 2;

    int box_w = s->zoom_mode ? 26 : BOX_W;
    int box_h = s->zoom_mode ? 16 : BOX_H;
    int sp_x  = s->zoom_mode ? 42 : CELL_SPACING_X;
    int sp_y  = s->zoom_mode ? 26 : CELL_SPACING_Y;

    /* 3. Draw Edges first */
    for (uint16_t i = 0; i < s->edge_count; i++) {
        struct zmap_room *r1 = find_room(s, s->edges[i].from_obj);
        struct zmap_room *r2 = find_room(s, s->edges[i].to_obj);
        if (!r1 || !r2) continue;

        int x1 = cx + (r1->gx - cam_gx) * sp_x;
        int y1 = cy + (r1->gy - cam_gy) * sp_y;
        int x2 = cx + (r2->gx - cam_gx) * sp_x;
        int y2 = cy + (r2->gy - cam_gy) * sp_y;

        /* Skip if completely outside canvas */
        if ((x1 < -box_w && x2 < -box_w) || (x1 > s->canvas_w + box_w && x2 > s->canvas_w + box_w) ||
            (y1 < 0 && y2 < 0) || (y1 > s->canvas_h + box_h && y2 > s->canvas_h + box_h)) {
            continue;
        }

        canvas_line(x1, y1, x2, y2, 1);

        /* Draw elevation badge if vertical exit and in normal mode */
        if (!s->zoom_mode) {
            if (s->edges[i].dir == ZMAP_DIR_UP) {
                canvas_text((x1 + x2) / 2 - 4, (y1 + y2) / 2 - 8, 0, "^");
            } else if (s->edges[i].dir == ZMAP_DIR_DOWN) {
                canvas_text((x1 + x2) / 2 - 4, (y1 + y2) / 2 - 8, 0, "v");
            }
        }
    }

    /* 4. Draw Room Boxes */
    for (uint16_t i = 0; i < s->room_count; i++) {
        struct zmap_room *r = &s->rooms[i];
        int rx = cx + (r->gx - cam_gx) * sp_x;
        int ry = cy + (r->gy - cam_gy) * sp_y;
        int bx = rx - box_w / 2;
        int by = ry - box_h / 2;

        /* Clip to canvas bounds */
        if (bx + box_w < 0 || bx >= s->canvas_w || by + box_h < TOP_BANNER_H || by >= s->canvas_h) {
            continue;
        }

        if (s->zoom_mode) {
            char inits[3];
            get_room_initials(r->name, inits);

            if (r->obj_id == s->current_room) {
                /* Current room in overview: inverted box with outer frame */
                canvas_rect(bx, by, box_w, box_h, 0);
                canvas_text(bx + 5, by, 0, inits);
                canvas_invert(bx, by, box_w, box_h);
                canvas_frame(bx - 2, by - 2, box_w + 4, box_h + 4, 1);
            } else {
                /* Visited room in overview: white rect with black frame */
                canvas_rect(bx, by, box_w, box_h, 0);
                canvas_frame(bx, by, box_w, box_h, 1);
                canvas_text(bx + 5, by, 0, inits);
            }
        } else {
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
                canvas_rect(bx, by, box_w, box_h, 0);
                canvas_text(bx + 6, by + 5, 0, label);
                canvas_invert(bx, by, box_w, box_h);
                canvas_frame(bx - 2, by - 2, box_w + 4, box_h + 4, 1);
            } else {
                /* Visited room: White fill with black outline */
                canvas_rect(bx, by, box_w, box_h, 0);
                canvas_frame(bx, by, box_w, box_h, 1);
                canvas_text(bx + 6, by + 5, 0, label);
            }
        }
    }
}
