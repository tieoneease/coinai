#include "audio.h"
#include "esp_attr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ogg.h"
#include "power.h"

static const char *TAG = "audio";

#define SAMPLE_RATE      16000
#define FRAME_SAMPLES    320   // 20 ms
#define FRAME_GRANULE    960   // 20 ms at 48 kHz
#define PREROLL_FRAMES   10    // 200 ms kept from before the press
#define READ_FAIL_MS     2000  // microphone read errors tolerated before the watchdog may fire
#define PAGE_FRAMES      50    // one Ogg page (and flash sync) per second
#define OPUS_PRE_SKIP    312   // encoder lookahead at 16 kHz (104 samples) in 48 kHz units
#define OPUS_BITRATE     24000
#define MIC_GAIN_DB      24.0f // Waveshare's spectrum example value
#define SPEAKER_VOLUME   70
#define PIN_AMP          GPIO_NUM_46
#define CAPTURE_STACK    (32 * 1024)  // Opus encoding peaked at ~23.3 KB
#define PLAYER_STACK     (16 * 1024)
// Speech detector: a frame is voiced when its RMS is above both an absolute level and the
// tracked noise floor plus a margin. The floor falls quickly and rises about 0.5 dB/s.
#define VAD_ABS_DBFS     (-50.0f)
#define VAD_MARGIN_DB    12.0f
#define VAD_FLOOR_INIT   (-65.0f)

static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static void *s_enc;
static audio_clip_cb_t s_cb;
static void *s_cb_ctx;
static SemaphoreHandle_t s_spk_lock;
static TaskHandle_t s_capture_task;

static volatile bool s_start_req, s_stop_req, s_discard_req;
static volatile bool s_recording;
// The clip whose file is open: set before recording starts, cleared only once the file is final
// (EOS page and .met written) or removed. Readers must not treat it as a cut-off clip.
static volatile uint16_t s_open_clip;
static volatile int s_level;
static volatile bool s_shutdown_req;
static SemaphoreHandle_t s_shutdown_done;
static volatile uint64_t s_first_frame_ticks;

// Owned by the capture task.
EXT_RAM_BSS_ATTR static int16_t s_preroll[PREROLL_FRAMES][FRAME_SAMPLES];
static bool s_preroll_voiced[PREROLL_FRAMES];
static float s_floor_db = VAD_FLOOR_INIT;
static uint32_t s_voiced_frames;
static uint64_t s_open_ticks;
static uint64_t s_start_utc_ms;

// Wall-clock ms once the phone has set the clock (HELLO), else 0.
static uint64_t utc_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}
static int s_pre_next, s_pre_count;
EXT_RAM_BSS_ATTR static ogg_opus_writer_t s_ogg;
static FILE *s_file;
static uint16_t s_clip_id;
static uint32_t s_frames;
static int64_t s_enc_us_total, s_enc_us_max;

bool audio_is_recording(void)
{
    return s_recording;
}

uint16_t audio_recording_clip(void)
{
    return s_open_clip;
}

int audio_level(void)
{
    return s_level;
}

uint64_t audio_first_frame_ticks(void)
{
    return s_first_frame_ticks;
}

void audio_rec_start(void)
{
    s_discard_req = false;
    s_stop_req = false;
    s_start_req = true;
}

void audio_rec_stop(bool discard)
{
    s_discard_req = discard;
    s_stop_req = true;
}

static void high_pass(int16_t *x, int n)
{
    // First-order DC blocker with its corner near 80 Hz: y[n] = x[n] - x[n-1] + R * y[n-1].
    static float x1, y1;
    const float r = 0.969f;
    for (int i = 0; i < n; i++) {
        float xi = x[i];
        float yi = xi - x1 + r * y1;
        x1 = xi;
        y1 = yi;
        x[i] = (int16_t)(yi > 32767.f ? 32767 : yi < -32768.f ? -32768 : yi);
    }
}

static esp_err_t encode_frame(const int16_t *mono, bool voiced)
{
    uint8_t pkt[512];
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)mono, .len = FRAME_SAMPLES * sizeof(int16_t)};
    esp_audio_enc_out_frame_t out = {.buffer = pkt, .len = sizeof pkt};
    int64_t t0 = esp_timer_get_time();
    esp_audio_err_t err = esp_opus_enc_process(s_enc, &in, &out);
    int64_t dt = esp_timer_get_time() - t0;
    ESP_RETURN_ON_FALSE(err == ESP_AUDIO_ERR_OK, ESP_FAIL, TAG, "opus encode %d", err);
    s_enc_us_total += dt;
    if (dt > s_enc_us_max) {
        s_enc_us_max = dt;
    }
    ESP_RETURN_ON_ERROR(ogg_opus_packet(&s_ogg, pkt, out.encoded_bytes, FRAME_GRANULE), TAG, "ogg packet");
    s_frames++;
    s_voiced_frames += voiced;
    if (s_frames % PAGE_FRAMES == 0) {
        ESP_RETURN_ON_ERROR(ogg_opus_flush(&s_ogg), TAG, "ogg flush");
    }
    return ESP_OK;
}

static void begin_clip(void)
{
    char path[32];
    s_clip_id = clipstore_next_id();
    s_open_clip = s_clip_id;  // before the file exists
    clipstore_path(s_clip_id, path, sizeof path);
    s_file = fopen(path, "wb");
    if (!s_file || ogg_opus_begin(&s_ogg, s_file, esp_random(), OPUS_PRE_SKIP, SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start clip %u", s_clip_id);
        if (s_file) {
            fclose(s_file);
            s_file = NULL;
            clipstore_remove(s_clip_id);
        }
        s_open_clip = 0;
        return;
    }
    esp_opus_enc_reset(s_enc);
    s_frames = 0;
    s_voiced_frames = 0;
    s_enc_us_total = s_enc_us_max = 0;
    s_open_ticks = power_rtc_ticks();
    s_recording = true;
    // Oldest pre-roll frame first.
    int first = (s_pre_next - s_pre_count + PREROLL_FRAMES) % PREROLL_FRAMES;
    for (int i = 0; i < s_pre_count; i++) {
        int k = (first + i) % PREROLL_FRAMES;
        encode_frame(s_preroll[k], s_preroll_voiced[k]);
    }
    s_pre_count = 0;
    uint64_t now = utc_now_ms();
    s_start_utc_ms = now ? now - (uint64_t)s_frames * 20 : 0;  // s_frames = pre-roll frames already encoded
    ESP_LOGI(TAG, "clip %u recording", s_clip_id);
    if (s_cb) {
        audio_clip_result_t started = {.id = s_clip_id};
        s_cb(&started, true, s_cb_ctx);
    }
}

static void finish_clip(bool discard)
{
    audio_clip_result_t res = {.id = s_clip_id, .discarded = discard, .open_ticks = s_open_ticks};
    s_recording = false;
    if (!s_file) {
        s_open_clip = 0;
        return;
    }
    if (!discard) {
        static const int16_t silence[FRAME_SAMPLES];
        if (ogg_opus_pending_packets(&s_ogg) == 0) {
            encode_frame(silence, false);  // the EOS page must carry a packet
        }
        if (ogg_opus_end(&s_ogg) != ESP_OK) {
            ESP_LOGE(TAG, "clip %u: EOS page failed", s_clip_id);
        }
    }
    fclose(s_file);
    s_file = NULL;
    if (discard) {
        clipstore_discard(s_clip_id);  // a phone streaming it gets ERROR 1 (ble.c)
        ESP_LOGI(TAG, "clip %u discarded", s_clip_id);
    } else {
        res.size = s_ogg.bytes;
        res.duration_ms = s_frames * 20;
        res.voiced_ms = s_voiced_frames * 20;
        res.crc32 = s_ogg.crc32;
        clip_meta_t meta = {.size = res.size, .duration_ms = res.duration_ms, .voiced_ms = res.voiced_ms,
                            .crc32 = res.crc32, .start_utc_ms = s_start_utc_ms};
        if (clipstore_put_meta(s_clip_id, &meta) != ESP_OK) {
            ESP_LOGE(TAG, "clip %u: metadata not saved", s_clip_id);
        }
        ESP_LOGI(TAG, "clip %u: %lu bytes, %lu ms (voiced %lu), crc %08lx; encode avg %lld us max %lld us; stack free %u",
                 s_clip_id, (unsigned long)res.size, (unsigned long)res.duration_ms, (unsigned long)res.voiced_ms,
                 (unsigned long)res.crc32,
                 s_frames ? s_enc_us_total / s_frames : 0, s_enc_us_max,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    s_open_clip = 0;
    if (s_cb) {
        s_cb(&res, false, s_cb_ctx);
    }
}

static void capture_task(void *arg)
{
    int16_t *stereo = malloc(FRAME_SAMPLES * 2 * sizeof(int16_t));
    int16_t mono[FRAME_SAMPLES];
    assert(stereo);
    // Watchdog: a capture that stops delivering frames for 5 s reboots the coin. Read errors are
    // retried for READ_FAIL_MS, then the watchdog is left to fire.
    esp_task_wdt_add(NULL);
    int64_t failing_since = 0;
    for (;;) {
        if (esp_codec_dev_read(s_mic, stereo, FRAME_SAMPLES * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            int64_t now = esp_timer_get_time();
            if (!failing_since) {
                failing_since = now;
                ESP_LOGE(TAG, "microphone read failed");
            }
            if (now - failing_since < READ_FAIL_MS * 1000LL) {
                esp_task_wdt_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        failing_since = 0;
        esp_task_wdt_reset();
        if (!s_first_frame_ticks) {
            s_first_frame_ticks = power_rtc_ticks();
        }
        int peak = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int v = ((int)stereo[2 * i] + stereo[2 * i + 1]) / 2;
            mono[i] = (int16_t)v;
        }
        high_pass(mono, FRAME_SAMPLES);
        float energy = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int a = abs(mono[i]);
            peak = a > peak ? a : peak;
            energy += (float)mono[i] * mono[i];
        }
        s_level = peak * 100 / 32768;
        float db = 10.0f * log10f(energy / FRAME_SAMPLES / (32768.0f * 32768.0f) + 1e-10f);
        s_floor_db += db < s_floor_db ? (db - s_floor_db) * 0.2f : 0.01f;
        s_floor_db = s_floor_db < -90.0f ? -90.0f : s_floor_db > -40.0f ? -40.0f : s_floor_db;
        bool voiced = db > VAD_ABS_DBFS && db > s_floor_db + VAD_MARGIN_DB;

        if (s_shutdown_req) {
            if (s_recording) {
                finish_clip(false);
            }
            esp_task_wdt_delete(NULL);
            break;
        }
        if (s_start_req && !s_recording) {
            s_start_req = false;
            if (s_stop_req) {
                s_stop_req = false;  // released before capture began: nothing to keep
                ESP_LOGI(TAG, "recording cancelled before the first frame");
                if (s_cb) {
                    audio_clip_result_t none = {.discarded = true};  // id 0: no clip was created
                    s_cb(&none, false, s_cb_ctx);
                }
            } else {
                begin_clip();
            }
        }
        if (s_recording) {
            if (encode_frame(mono, voiced) != ESP_OK) {
                ESP_LOGE(TAG, "clip %u: write failed, stopping", s_clip_id);
                s_stop_req = true;
            }
        } else {
            memcpy(s_preroll[s_pre_next], mono, sizeof mono);
            s_preroll_voiced[s_pre_next] = voiced;
            s_pre_next = (s_pre_next + 1) % PREROLL_FRAMES;
            if (s_pre_count < PREROLL_FRAMES) {
                s_pre_count++;
            }
        }
        if (s_stop_req) {
            s_stop_req = false;
            s_start_req = false;
            if (s_recording) {
                finish_clip(s_discard_req);
            }
        }
    }
    free(stereo);
    xSemaphoreGive(s_shutdown_done);
    vTaskDelete(NULL);
}

void audio_shutdown(void)
{
    if (!s_capture_task) {
        return;
    }
    s_shutdown_req = true;
    xSemaphoreTake(s_shutdown_done, portMAX_DELAY);
    s_capture_task = NULL;
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);  // waits out any playback
    gpio_set_level(PIN_AMP, 0);
    esp_codec_dev_close(s_mic);
    esp_codec_dev_close(s_spk);
    ESP_LOGI(TAG, "audio off");
}

static void amp(bool on)
{
    if (on) {
        esp_codec_dev_set_out_mute(s_spk, false);
        gpio_set_level(PIN_AMP, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        gpio_set_level(PIN_AMP, 0);
        esp_codec_dev_set_out_mute(s_spk, true);
    }
}

esp_err_t audio_init(audio_clip_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_spk_lock = xSemaphoreCreateMutex();
    s_shutdown_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_spk_lock && s_shutdown_done, ESP_ERR_NO_MEM, TAG, "semaphores");

    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "codec init");
    // Speaker and mics share one duplex I2S port, so both run the same format.
    esp_codec_dev_sample_info_t fs = {.sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 16};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open mics");
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_spk, SPEAKER_VOLUME);
    amp(false);  // opening the ES8311 switches the amplifier on; keep it off until playback

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = SAMPLE_RATE;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = OPUS_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 2;
    cfg.enable_vbr = true;
    ESP_RETURN_ON_FALSE(esp_opus_enc_open(&cfg, sizeof cfg, &s_enc) == ESP_AUDIO_ERR_OK, ESP_FAIL, TAG, "opus encoder");
    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(s_enc, &in_size, &out_size);
    ESP_RETURN_ON_FALSE(in_size == FRAME_SAMPLES * 2, ESP_FAIL, TAG, "unexpected Opus frame size %d", in_size);

    // Stack in PSRAM (see sdkconfig.defaults): 32 KB of internal RAM is too much to spend here.
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(capture_task, "capture", CAPTURE_STACK, NULL, 18, &s_capture_task,
                                                    0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "capture task");
    ESP_LOGI(TAG, "audio ready: 16 kHz, 2 mics, Opus %d bps VoIP", OPUS_BITRATE);
    return ESP_OK;
}

typedef struct {
    uint16_t clip;  // 0 = tone
    int hz, ms;
    esp_err_t result;
    SemaphoreHandle_t done;
} play_job_t;

static esp_err_t play_ogg(uint16_t id)
{
    char path[32];
    clipstore_path(id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "no clip %u", id);

    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = SAMPLE_RATE;
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    void *dec = NULL;
    uint8_t *pkt = malloc(1500);
    int16_t *pcm = malloc(1920 * sizeof(int16_t));
    int16_t *stereo = malloc(1920 * 2 * sizeof(int16_t));
    esp_err_t ret = ESP_OK;
    if (!pkt || !pcm || !stereo || esp_opus_dec_open(&cfg, sizeof cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    ogg_reader_t r = {.f = f};
    int n, index = 0;
    while ((n = ogg_read_packet(&r, pkt, 1500)) > 0) {
        if (index++ < 2) {
            continue;  // OpusHead, OpusTags
        }
        esp_audio_dec_in_raw_t raw = {.buffer = pkt, .len = (uint32_t)n};
        esp_audio_dec_out_frame_t frame = {.buffer = (uint8_t *)pcm, .len = 1920 * sizeof(int16_t)};
        esp_audio_dec_info_t info;
        if (esp_opus_dec_decode(dec, &raw, &frame, &info) != ESP_AUDIO_ERR_OK) {
            ret = ESP_FAIL;
            break;
        }
        size_t samples = frame.decoded_size / sizeof(int16_t);
        for (size_t i = 0; i < samples; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = pcm[i];
        }
        esp_codec_dev_write(s_spk, stereo, samples * 2 * sizeof(int16_t));
    }
    if (n < 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
out:
    if (dec) {
        esp_opus_dec_close(dec);
    }
    free(pkt);
    free(pcm);
    free(stereo);
    fclose(f);
    return ret;
}

static esp_err_t play_tone(int hz, int ms)
{
    int16_t *buf = malloc(FRAME_SAMPLES * 2 * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "tone buffer");
    int total = SAMPLE_RATE * ms / 1000;
    float phase = 0, step = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    for (int done = 0; done < total; done += FRAME_SAMPLES) {
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int16_t v = (int16_t)(8000 * sinf(phase));
            phase += step;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_spk, buf, FRAME_SAMPLES * 2 * sizeof(int16_t));
    }
    free(buf);
    return ESP_OK;
}

static void player_task(void *arg)
{
    play_job_t *job = arg;
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    amp(true);
    job->result = job->clip ? play_ogg(job->clip) : play_tone(job->hz, job->ms);
    // Let the last DMA buffers drain before the amplifier goes off.
    static const int16_t zeros[FRAME_SAMPLES * 2];
    esp_codec_dev_write(s_spk, (void *)zeros, sizeof zeros);
    amp(false);
    xSemaphoreGive(s_spk_lock);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t run_player(play_job_t *job)
{
    job->done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(job->done, ESP_ERR_NO_MEM, TAG, "semaphore");
    if (xTaskCreatePinnedToCore(player_task, "player", PLAYER_STACK, job, 15, NULL, 0) != pdPASS) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    vSemaphoreDelete(job->done);
    return job->result;
}

esp_err_t audio_play_clip(uint16_t id)
{
    play_job_t job = {.clip = id};
    return run_player(&job);
}

esp_err_t audio_tone(int hz, int ms)
{
    play_job_t job = {.clip = 0, .hz = hz, .ms = ms};
    return run_player(&job);
}
