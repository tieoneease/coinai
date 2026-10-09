// The microphones and speaker (esp_codec_dev through the BSP), paced in real time, and Espressif's
// Opus wrapper over libopus.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#include "bsp/esp-bsp.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "sim_esp.h"
#include "simlink.h"

// ---------- codec devices ----------

#define MIC_RING_FRAMES 16000  // 1 s of stereo frames

typedef struct {
    bool mic;
    bool open;
    int rate, channels;
    int64_t next_us;  // when the next block is due, like an I2S DMA buffer
    float volume;
    bool muted;
} sim_codec_t;

static sim_codec_t s_mic_dev = {.mic = true}, s_spk_dev = {.mic = false, .volume = 1.0f};
static CRITICAL_SECTION s_ring_lock;
static int16_t s_ring[MIC_RING_FRAMES * 2];
static size_t s_ring_head, s_ring_count;  // in stereo frames
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE o, void *p, void **c)
{
    InitializeCriticalSection(&s_ring_lock);
    return TRUE;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    return &s_spk_dev;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    return &s_mic_dev;
}

void sim_mic_input(const int16_t *pcm, size_t samples)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    size_t frames = samples / 2;
    EnterCriticalSection(&s_ring_lock);
    for (size_t i = 0; i < frames; i++) {
        size_t tail = (s_ring_head + s_ring_count) % MIC_RING_FRAMES;
        s_ring[2 * tail] = pcm[2 * i];
        s_ring[2 * tail + 1] = pcm[2 * i + 1];
        if (s_ring_count < MIC_RING_FRAMES) {
            s_ring_count++;
        } else {
            s_ring_head = (s_ring_head + 1) % MIC_RING_FRAMES;  // full: drop the oldest
        }
    }
    LeaveCriticalSection(&s_ring_lock);
}

int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs)
{
    sim_codec_t *d = h;
    d->rate = fs->sample_rate;
    d->channels = fs->channel;
    d->open = true;
    d->next_us = esp_timer_get_time();
    if (d->mic) {
        EnterCriticalSection(&s_ring_lock);
        s_ring_head = s_ring_count = 0;
        LeaveCriticalSection(&s_ring_lock);
        uint8_t on = 1;
        sim_send(IPC_MIC_STATE, &on, 1);
    }
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_close(esp_codec_dev_handle_t h)
{
    sim_codec_t *d = h;
    if (d->open && d->mic) {
        uint8_t off = 0;
        sim_send(IPC_MIC_STATE, &off, 1);
    }
    d->open = false;
    return ESP_CODEC_DEV_OK;
}

// Waits until this block's audio time has come; a long stall resynchronises instead of bursting.
static void pace(sim_codec_t *d, int frames)
{
    int64_t now = esp_timer_get_time();
    if (d->next_us < now - 100000) {
        d->next_us = now;
    }
    d->next_us += (int64_t)frames * 1000000 / d->rate;
    int64_t wait = d->next_us - now;
    if (wait > 1000) {
        Sleep((DWORD)(wait / 1000));
    }
}

int esp_codec_dev_read(esp_codec_dev_handle_t h, void *data, int len)
{
    sim_codec_t *d = h;
    if (!d->open) {
        return -1;
    }
    int frames = len / (2 * d->channels);
    pace(d, frames);
    int16_t *out = data;
    EnterCriticalSection(&s_ring_lock);
    for (int i = 0; i < frames; i++) {
        int16_t l = 0, r = 0;
        if (s_ring_count) {
            l = s_ring[2 * s_ring_head];
            r = s_ring[2 * s_ring_head + 1];
            s_ring_head = (s_ring_head + 1) % MIC_RING_FRAMES;
            s_ring_count--;
        }
        if (d->channels == 2) {
            out[2 * i] = l;
            out[2 * i + 1] = r;
        } else {
            out[i] = (int16_t)((l + r) / 2);
        }
    }
    LeaveCriticalSection(&s_ring_lock);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_write(esp_codec_dev_handle_t h, void *data, int len)
{
    sim_codec_t *d = h;
    if (!d->open) {
        return -1;
    }
    int frames = len / (2 * d->channels);
    int16_t *in = data;
    int16_t *mono = malloc(frames * sizeof(int16_t));
    for (int i = 0; i < frames; i++) {
        float v = d->channels == 2 ? (in[2 * i] + in[2 * i + 1]) / 2.0f : in[i];
        v = d->muted ? 0 : v * d->volume;
        mono[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    sim_send(IPC_SPEAKER, mono, frames * sizeof(int16_t));
    free(mono);
    pace(d, frames);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int volume)
{
    ((sim_codec_t *)h)->volume = volume / 100.0f;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t h, bool mute)
{
    ((sim_codec_t *)h)->muted = mute;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db)
{
    return ESP_CODEC_DEV_OK;  // the host delivers microphone audio at the coin's gain
}

// ---------- Opus ----------

static const int s_duration_us[] = {2500, 5000, 10000, 20000, 40000, 60000, 80000, 100000, 120000};

typedef struct {
    OpusEncoder *enc;
    int rate, channels, frame_samples;
} sim_enc_t;

typedef struct {
    OpusDecoder *dec;
    int rate, channels, max_samples;
} sim_dec_t;

esp_audio_err_t esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd)
{
    esp_opus_enc_config_t *c = cfg;
    if (cfg_sz != sizeof *c || c->frame_duration < 0 || c->frame_duration > 8) {
        return ESP_AUDIO_ERR_INVALID_PARAMETER;
    }
    int app = c->application_mode == ESP_OPUS_ENC_APPLICATION_VOIP    ? OPUS_APPLICATION_VOIP
              : c->application_mode == ESP_OPUS_ENC_APPLICATION_AUDIO ? OPUS_APPLICATION_AUDIO
                                                                      : OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    int err;
    OpusEncoder *enc = opus_encoder_create(c->sample_rate, c->channel, app, &err);
    if (err != OPUS_OK) {
        return ESP_AUDIO_ERR_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(c->bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c->complexity));
    opus_encoder_ctl(enc, OPUS_SET_VBR(c->enable_vbr));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(c->enable_fec));
    opus_encoder_ctl(enc, OPUS_SET_DTX(c->enable_dtx));
    sim_enc_t *e = calloc(1, sizeof *e);
    e->enc = enc;
    e->rate = c->sample_rate;
    e->channels = c->channel;
    e->frame_samples = (int)((int64_t)c->sample_rate * s_duration_us[c->frame_duration] / 1000000);
    *enc_hd = e;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size)
{
    sim_enc_t *e = enc_hd;
    *in_size = e->frame_samples * e->channels * 2;
    *out_size = 1276 * 3;  // the largest Opus packet for up to 60 ms
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in, esp_audio_enc_out_frame_t *out)
{
    sim_enc_t *e = enc_hd;
    if (in->len < (uint32_t)(e->frame_samples * e->channels * 2)) {
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    int n = opus_encode(e->enc, (const opus_int16 *)in->buffer, e->frame_samples, out->buffer, (opus_int32)out->len);
    if (n < 0) {
        return ESP_AUDIO_ERR_FAIL;
    }
    out->encoded_bytes = n;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_enc_reset(void *enc_hd)
{
    opus_encoder_ctl(((sim_enc_t *)enc_hd)->enc, OPUS_RESET_STATE);
    return ESP_AUDIO_ERR_OK;
}

void esp_opus_enc_close(void *enc_hd)
{
    sim_enc_t *e = enc_hd;
    if (e) {
        opus_encoder_destroy(e->enc);
        free(e);
    }
}

esp_audio_err_t esp_opus_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle)
{
    esp_opus_dec_cfg_t *c = cfg;
    int err;
    OpusDecoder *dec = opus_decoder_create(c->sample_rate, c->channel, &err);
    if (err != OPUS_OK) {
        return ESP_AUDIO_ERR_FAIL;
    }
    sim_dec_t *d = calloc(1, sizeof *d);
    d->dec = dec;
    d->rate = c->sample_rate;
    d->channels = c->channel;
    int us = c->frame_duration >= 0 && c->frame_duration <= 8 ? s_duration_us[c->frame_duration] : 60000;
    d->max_samples = (int)((int64_t)c->sample_rate * us / 1000000);
    *dec_handle = d;
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                    esp_audio_dec_info_t *info)
{
    sim_dec_t *d = dec_handle;
    int capacity = frame->len / (2 * d->channels);
    int n = opus_decode(d->dec, raw->buffer, (opus_int32)raw->len, (opus_int16 *)frame->buffer, capacity, 0);
    if (n == OPUS_BUFFER_TOO_SMALL) {
        frame->needed_size = d->max_samples * 2 * d->channels;
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    if (n < 0) {
        return ESP_AUDIO_ERR_FAIL;
    }
    raw->consumed = raw->len;
    frame->decoded_size = n * 2 * d->channels;
    if (info) {
        info->sample_rate = d->rate;
        info->channel = d->channels;
        info->bits_per_sample = 16;
        info->bitrate = 0;
        info->frame_size = frame->decoded_size;
    }
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t esp_opus_dec_close(void *dec_handle)
{
    sim_dec_t *d = dec_handle;
    if (d) {
        opus_decoder_destroy(d->dec);
        free(d);
    }
    return ESP_AUDIO_ERR_OK;
}
