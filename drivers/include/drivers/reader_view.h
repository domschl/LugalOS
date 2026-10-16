#ifndef LUGALOS_DRIVERS_READER_VIEW_H
#define LUGALOS_DRIVERS_READER_VIEW_H

#include "drivers/canvas1.h"
#include <stdbool.h>
#include <stdint.h>

#define READER_MAX_INSTANCES 4

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
