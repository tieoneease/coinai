#pragma once
// Clip files on the LittleFS `storage` partition: /c/NNNNN.ogg, ids kept in NVS.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct {
    uint16_t id;
    uint32_t size;
} clip_info_t;

esp_err_t clipstore_init(void);
uint16_t clipstore_next_id(void);           // allocates and persists the next id (1..65535, wraps)
void clipstore_path(uint16_t id, char *buf, size_t len);
int clipstore_list(clip_info_t *out, int max);  // ascending ids; returns the count
esp_err_t clipstore_remove(uint16_t id);
esp_err_t clipstore_usage(size_t *total, size_t *used);
