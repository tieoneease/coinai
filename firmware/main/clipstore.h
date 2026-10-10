#pragma once
// Clip files on the LittleFS `storage` partition: /c/NNNNN.ogg, ids kept in NVS.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct {
    uint16_t id;
    uint32_t size;
} clip_info_t;

#define CLIP_VOICED_UNKNOWN 0xFFFFFFFFu

// Written next to a clip when it is finalised (/c/NNNNN.met). A clip without one was cut off by a
// reset or power loss; clipstore_get_meta() then computes size, CRC and duration from the file.
typedef struct {
    uint32_t size;
    uint32_t duration_ms;
    uint32_t voiced_ms;     // CLIP_VOICED_UNKNOWN when recomputed
    uint32_t crc32;
    uint64_t start_utc_ms;  // 0 = unknown
} clip_meta_t;

esp_err_t clipstore_init(void);
uint16_t clipstore_next_id(void);           // allocates and persists the next id (1..65535, wraps)
void clipstore_path(uint16_t id, char *buf, size_t len);
int clipstore_list(clip_info_t *out, int max);  // ascending ids; returns the count
int clipstore_count(void);
esp_err_t clipstore_remove(uint16_t id);
// A clip the user discarded. A transfer may still have its file open (LittleFS then refuses to
// delete it); such a clip gets a tombstone file, stays hidden from listings and metadata, and is
// deleted when the transfer stops (ble.c) or at the next boot.
void clipstore_discard(uint16_t id);
bool clipstore_discarded(uint16_t id);
bool clipstore_discard_pending(uint16_t id);  // the latest discard only, from RAM: for hot paths
esp_err_t clipstore_usage(size_t *total, size_t *used);
esp_err_t clipstore_put_meta(uint16_t id, const clip_meta_t *meta);
void clipstore_drop_meta(uint16_t id);  // test hook: makes the clip look cut off
// complete: whether the clip is finalised. A clip without metadata that is not the one being
// recorded (`recording`) was cut off; its metadata is recomputed from the file and saved.
esp_err_t clipstore_get_meta(uint16_t id, uint16_t recording, clip_meta_t *meta, bool *complete);
