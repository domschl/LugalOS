/* user/ceres/include/ceres_storage.h — High-Score & Stats Persistence
 *
 * Saves and loads high scores, astronaut rescue counts, and sortie statistics
 * to SD card (/sd0/games/ceres.dat) or Flash (/flash0/ceres.dat) on LugalOS,
 * and local storage (ceres.dat) on host simulator.
 */

#ifndef CERES_STORAGE_H
#define CERES_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#define CERES_SAVE_MAGIC    0x43455253 /* 'CERS' */
#define CERES_SAVE_VERSION  1

typedef struct {
    uint32_t magic;           /* 'CERS' */
    uint32_t version;         /* Version 1 */
    uint32_t hi_score;        /* All-time highest score */
    uint32_t total_rescued;   /* Total survivors rescued */
    uint32_t games_played;    /* Total sorties flown */
    uint32_t highest_sector;  /* Deepest sector reached */
    uint32_t checksum;        /* Simple checksum for data validation */
} ceres_save_data_t;

/* Load saved progress from storage. Returns true on successful read and valid checksum */
bool ceres_storage_load(ceres_save_data_t *out);

/* Save progress to persistent storage. Returns true on success */
bool ceres_storage_save(const ceres_save_data_t *in);

#endif /* CERES_STORAGE_H */
