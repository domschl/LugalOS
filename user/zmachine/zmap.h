/* user/zmachine/zmap.h — Auto-mapper for Z-machine Version 3 games
 *
 * Tracks room discovery and movements across all .z3 story files via the
 * Z-machine Version 3 specification contract: Global Variable 0 is always
 * the current room object ID (HERE).
 *
 * Renders a dynamic directed graph on the LugalOS split-screen canvas
 * (console_canvas).
 */
#ifndef USER_ZMACHINE_ZMAP_H
#define USER_ZMACHINE_ZMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct z_vm;

#define ZMAP_MAX_ROOMS 160
#define ZMAP_MAX_EDGES 384
#define ZMAP_NAME_MAX  36

enum {
    ZMAP_DIR_NONE = 0,
    ZMAP_DIR_NORTH,
    ZMAP_DIR_SOUTH,
    ZMAP_DIR_EAST,
    ZMAP_DIR_WEST,
    ZMAP_DIR_NE,
    ZMAP_DIR_NW,
    ZMAP_DIR_SE,
    ZMAP_DIR_SW,
    ZMAP_DIR_UP,
    ZMAP_DIR_DOWN,
    ZMAP_DIR_SPECIAL
};

struct zmap_room {
    uint16_t obj_id;
    int16_t  gx, gy;      /* integer relative grid coordinates */
    uint8_t  visited;
    char     name[ZMAP_NAME_MAX];
};

struct zmap_edge {
    uint16_t from_obj;
    uint16_t to_obj;
    uint8_t  dir;
};

struct zmap_state {
    bool active;
    int canvas_w, canvas_h;

    uint16_t current_room;
    uint16_t last_room;
    uint8_t  last_dir;

    uint16_t room_count;
    struct zmap_room rooms[ZMAP_MAX_ROOMS];

    uint16_t edge_count;
    struct zmap_edge edges[ZMAP_MAX_EDGES];

    bool zoom_mode;
};

/* Lifecycle */
bool zmap_init(struct zmap_state *s);
void zmap_reset(struct zmap_state *s);
void zmap_close(struct zmap_state *s);

/* Turn hooks */
void zmap_on_turn(struct zmap_state *s, struct z_vm *vm);
void zmap_record_command(struct zmap_state *s, const uint8_t *line, int len);

/* Serialization & Persistence */
uint32_t zmap_serialize(const struct zmap_state *s, uint8_t *buf, uint32_t cap);
int zmap_deserialize(struct zmap_state *s, const uint8_t *buf, uint32_t len);
int zmap_save(const struct zmap_state *s, const char *path);
int zmap_load(struct zmap_state *s, const char *path);

/* Zoom control */
void zmap_toggle_zoom(struct zmap_state *s);
void zmap_set_zoom(struct zmap_state *s, bool zoom);
bool zmap_get_zoom(const struct zmap_state *s);

/* Render */
void zmap_render(struct zmap_state *s);

#endif /* USER_ZMACHINE_ZMAP_H */
