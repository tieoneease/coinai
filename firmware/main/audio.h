#pragma once
// Capture: ES7210 MIC1+MIC2, 16 kHz -> mono + high-pass -> Opus 24 kbps VoIP, 20 ms -> Ogg on flash.
// Playback: Ogg Opus clip or test tone -> ES8311 -> amplifier (GPIO46) -> speaker.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t id;
    uint32_t size;
    uint32_t duration_ms;
    uint32_t crc32;
    bool discarded;
} audio_clip_result_t;

// Called on the capture task when a clip is finalised or discarded.
typedef void (*audio_clip_cb_t)(const audio_clip_result_t *clip, void *ctx);

esp_err_t audio_init(audio_clip_cb_t cb, void *ctx);

// Both are asynchronous and safe from any task. Recording includes a short pre-roll.
void audio_rec_start(void);
void audio_rec_stop(bool discard);
bool audio_is_recording(void);
int audio_level(void);  // peak of the latest 20 ms frame, 0..100

// Blocking; each runs on its own task internally.
esp_err_t audio_play_clip(uint16_t id);
esp_err_t audio_tone(int hz, int ms);
