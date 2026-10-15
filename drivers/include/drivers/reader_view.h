#ifndef LUGALOS_DRIVERS_READER_VIEW_H
#define LUGALOS_DRIVERS_READER_VIEW_H

#include "drivers/canvas1.h"
#include <stdbool.h>
#include <stdint.h>

void reader_init(void);
bool reader_open(const char *resource);
void reader_close(void);
void reader_scroll(int delta_y);
bool reader_tap(int client_x, int client_y);
void reader_feed_key(int c);
bool reader_follow_link_num(int num);
void reader_draw(const canvas1_t *c, int w, int h);
const char *reader_get_title(void);
const char *reader_get_path(void);
bool reader_is_active(void);
bool reader_back(void);
void reader_set_target(uint8_t cid, int w, int h);
void reader_update(void);

#endif /* LUGALOS_DRIVERS_READER_VIEW_H */
