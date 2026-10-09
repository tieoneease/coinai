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
    uint32_t voiced_ms;    // frames above the speech threshold (see VAD_* in audio.c)
    uint32_t crc32;
    uint64_t open_ticks;   // RTC slow-clock ticks when the clip file was ready (power_rtc_ticks())
    bool discarded;
} audio_clip_result_t;

// Called on the capture task when a clip starts (started = true; only id is set) and when it is
// finalised or discarded. Must not block.
typedef void (*audio_clip_cb_t)(const audio_clip_result_t *clip, bool started, void *ctx);

esp_err_t audio_init(audio_clip_cb_t cb, void *ctx);
// Finalises any recording, stops capture and powers the codecs down. For standby; not reversible.
void audio_shutdown(void);

// Asynchronous and safe from any task, also before audio_init(): a start requested early is
// served by the first captured frame. A stop that arrives before that frame cancels the clip.
void audio_rec_start(void);
void audio_rec_stop(bool discard);
bool audio_is_recording(void);
uint16_t audio_recording_clip(void);  // id of the clip being recorded, 0 if none
int audio_level(void);                 // peak of the latest 20 ms frame, 0..100
uint64_t audio_first_frame_ticks(void);  // RTC ticks when the first frame arrived, 0 before

// Blocking; each runs on its own task internally.
esp_err_t audio_play_clip(uint16_t id);
esp_err_t audio_tone(int hz, int ms);
