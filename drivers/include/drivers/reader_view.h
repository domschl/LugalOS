#ifndef LUGALOS_DRIVERS_READER_VIEW_H
#define LUGALOS_DRIVERS_READER_VIEW_H

#include "drivers/canvas1.h"
#include <stdbool.h>
#include <stdint.h>

#include "lugalos_config.h"

/* Each instance is ~7.5 KB of static state (links, history, table of
 * contents). The ESP32-C6 has no display, only a RAM screen for Lisp's
 * canvas, and keeps one: four cost it 30 KB of a heap it runs out of. So does
 * an RP2350 without a panel (a Pico 2 W, the clock, the gateway): its heap is
 * 70 pages, and the compiler wants 28 of them at once. */
#if defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO))
#define READER_MAX_INSTANCES 1
#else
#define READER_MAX_INSTANCES 4
#endif

void reader_init(void);
bool reader_open(const char *resource);
int reader_open_instance(uint8_t rid, const char *resource);
void reader_close(void);
void reader_close_instance(uint8_t rid);
void reader_scroll(int delta_y);
void reader_scroll_instance(uint8_t rid, int delta_y);
void reader_touch_fling(int dy, uint32_t ms);
void reader_touch_fling_instance(uint8_t rid, int dy, uint32_t ms);
bool reader_tap(int client_x, int client_y);
bool reader_tap_instance(uint8_t rid, int client_x, int client_y);
void reader_feed_key(int c);
bool reader_follow_link_num(int num);
bool reader_follow_link_num_instance(uint8_t rid, int num);
void reader_draw(uint8_t rid, const canvas1_t *c, int w, int h);
const char *reader_get_title(void);
const char *reader_get_title_instance(uint8_t rid);
const char *reader_get_path(void);
const char *reader_get_path_instance(uint8_t rid);
bool reader_is_active(void);
bool reader_instance_is_active(uint8_t rid);
bool reader_back(void);
bool reader_back_instance(uint8_t rid);
bool reader_set_target(uint8_t rid, int w, int h);
void reader_update(void);
void reader_update_instance(uint8_t rid);
uint8_t reader_get_active_instance(void);
void reader_set_active_instance(uint8_t rid);
int reader_find_by_path(const char *path);
uint8_t *reader_get_aa_backing(uint8_t rid, uint32_t *out_stride, int *out_w, int *out_h);
uint32_t reader_get_seq(uint8_t rid);

#endif /* LUGALOS_DRIVERS_READER_VIEW_H */
