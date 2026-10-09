#include "diag.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "ble.h"
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "lvgl.h"
#include "pmic.h"
#include "power.h"

static const char *TAG = "diag";
static i2c_master_bus_handle_t s_bus;

static int cmd_i2cscan(int argc, char **argv)
{
    int found = 0;
    printf("I2C:");
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            printf(" 0x%02x", addr);
            found++;
        }
    }
    printf("\n%d devices\n", found);
    return 0;
}

static int cmd_pmic(int argc, char **argv)
{
    if (argc == 1) {
        pmic_status_t s;
        if (pmic_read_status(&s) != ESP_OK) {
            printf("PMIC read failed\n");
            return 1;
        }
        printf("battery %s, %d%%, %u mV; charger %s (%s); VBUS %s %u mV; VSYS %u mV; die %.1f C\n",
               s.battery_present ? "present" : "absent", s.battery_pct, s.vbat_mv,
               pmic_charger_state_name(s.charger_state), s.charging ? "charging" : s.discharging ? "discharging" : "standby",
               s.vbus_good ? "good" : "absent", s.vbus_mv, s.vsys_mv, (double)s.die_temp_c);
        return pmic_log_config() == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "reg") == 0) {
        uint8_t reg = (uint8_t)strtoul(argv[2], NULL, 16);
        uint8_t val;
        if (argc == 4) {
            val = (uint8_t)strtoul(argv[3], NULL, 16);
            if (pmic_write_reg(reg, val) != ESP_OK) {
                printf("write failed\n");
                return 1;
            }
        }
        if (pmic_read_reg(reg, &val) != ESP_OK) {
            printf("read failed\n");
            return 1;
        }
        printf("reg 0x%02x = 0x%02x\n", reg, val);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "chg") == 0) {
        esp_err_t err = pmic_set_charge_current_ma(atoi(argv[2]));
        printf("%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: pmic | pmic reg <hex> [<hex value>] | pmic chg <mA>\n");
    return 1;
}

static int cmd_imu(int argc, char **argv)
{
    imu_sample_t s;
    if (imu_read(&s) != ESP_OK) {
        printf("IMU read failed\n");
        return 1;
    }
    printf("accel %.3f %.3f %.3f g (|a| %.3f); gyro %.1f %.1f %.1f dps; %.1f C\n", (double)s.ax, (double)s.ay,
           (double)s.az, sqrt((double)(s.ax * s.ax + s.ay * s.ay + s.az * s.az)), (double)s.gx, (double)s.gy,
           (double)s.gz, (double)s.temp_c);
    return 0;
}

typedef struct {
    const char *step;
    uint32_t ms, free, min, largest;
} mem_mark_t;
static mem_mark_t s_marks[12];
static int s_nmarks;

void diag_mem_mark(const char *step)
{
    if (s_nmarks < (int)(sizeof s_marks / sizeof s_marks[0])) {
        s_marks[s_nmarks++] = (mem_mark_t){
            .step = step,
            .ms = (uint32_t)(esp_timer_get_time() / 1000),
            .free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            .min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            .largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        };
    }
}

// Large internal-RAM allocations (CONFIG_HEAP_USE_HOOKS): who takes the internal heap, and when.
typedef struct {
    uint32_t size, ms;
    char task[12];
    uint32_t pc[6];
} big_alloc_t;
static big_alloc_t s_big[24];
static volatile uint32_t s_nbig;
#define BIG_ALLOC 2048

void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (size < BIG_ALLOC || !esp_ptr_internal(ptr)) {
        return;
    }
    uint32_t i = s_nbig++;
    if (i < sizeof s_big / sizeof s_big[0]) {
        big_alloc_t *b = &s_big[i];
        b->size = size;
        b->ms = (uint32_t)(esp_timer_get_time() / 1000);
        const char *name = xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED ? "-" : pcTaskGetName(NULL);
        strlcpy(b->task, name ? name : "?", sizeof b->task);
        esp_backtrace_frame_t f;
        esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
        for (int d = 0; d < 6; d++) {
            b->pc[d] = 0;
            if (!f.next_pc || !esp_backtrace_get_next_frame(&f)) {
                break;
            }
            b->pc[d] = esp_cpu_process_stack_pc(f.pc);
        }
    }
}

static int cmd_stats(int argc, char **argv)
{
    uint32_t nbig = s_nbig;
    for (uint32_t i = 0; i < nbig && i < sizeof s_big / sizeof s_big[0]; i++) {
        printf("internal alloc %6lu B at %6lu ms by %s:", (unsigned long)s_big[i].size, (unsigned long)s_big[i].ms,
               s_big[i].task);
        for (int d = 0; d < 6 && s_big[i].pc[d]; d++) {
            printf(" %08lx", (unsigned long)s_big[i].pc[d]);
        }
        printf("\n");
    }
    if (nbig > sizeof s_big / sizeof s_big[0]) {
        printf("(%lu large internal allocations in all)\n", (unsigned long)nbig);
    }
    for (int i = 0; i < s_nmarks; i++) {
        printf("boot %-14s %5lu ms: internal free %6lu min %6lu largest %6lu\n", s_marks[i].step,
               (unsigned long)s_marks[i].ms, (unsigned long)s_marks[i].free, (unsigned long)s_marks[i].min,
               (unsigned long)s_marks[i].largest);
    }
    printf("largest internal block %u, DMA-capable free %u\n",
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    printf("uptime %.1f s; reset reason %d\n", esp_timer_get_time() / 1e6, (int)esp_reset_reason());
    printf("internal free %u (min %u); PSRAM free %u (min %u)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void base64_line(const uint8_t *in, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

// Renders the active screen into an RGB565 buffer and prints it as base64 between markers.
// scripts/fbdump.py turns the output into a PNG.
static int cmd_fbdump(int argc, char **argv)
{
    if (bsp_display_lock(2000) != ESP_OK) {
        printf("display busy\n");
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    int64_t t1 = esp_timer_get_time();
    bsp_display_unlock();
    if (!snap) {
        printf("snapshot failed\n");
        return 1;
    }
    uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    size_t len = (size_t)stride * h;
    // Other tasks' log lines would land inside the base64 stream.
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("FBDUMP-BEGIN %" PRIu32 " %" PRIu32 " %" PRIu32 " RGB565 render_us=%lld\n", w, h, stride, (long long)(t1 - t0));
    char line[80];
    for (size_t off = 0; off < len; off += 57) {
        size_t n = len - off < 57 ? len - off : 57;
        base64_line(snap->data + off, n, line);
        puts(line);
    }
    printf("FBDUMP-END %08" PRIx32 "\n", esp_rom_crc32_le(0, snap->data, len));
    fflush(stdout);
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    lv_draw_buf_destroy(snap);
    return 0;
}

static int cmd_rec(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "stop") == 0)) {
        if (argv[1][2] == 'a') {
            audio_rec_start();  // returns at once; `rec stop` ends it
        } else {
            audio_rec_stop(false);
        }
        return 0;
    }
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
    if (seconds < 1 || seconds > 600) {
        printf("usage: rec <seconds 1-600>\n");
        return 1;
    }
    audio_rec_start();
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    audio_rec_stop(false);
    vTaskDelay(pdMS_TO_TICKS(300));  // let the capture task finalise; its log line has the result
    return 0;
}

// Speaker -> microphones self-test: the clip should contain the tone between 0.5 and 1.5 s.
static int cmd_loopback(int argc, char **argv)
{
    int hz = argc > 1 ? atoi(argv[1]) : 1000;
    audio_rec_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = audio_tone(hz, 1000);
    vTaskDelay(pdMS_TO_TICKS(500));
    audio_rec_stop(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    printf("loopback %d Hz: %s\n", hz, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_clips(int argc, char **argv)
{
    EXT_RAM_BSS_ATTR static clip_info_t list[128];
    int n = clipstore_list(list, 128);
    for (int i = 0; i < n; i++) {
        printf("%5u  %7lu bytes\n", list[i].id, (unsigned long)list[i].size);
    }
    size_t total = 0, used = 0;
    clipstore_usage(&total, &used);
    printf("%d clips; storage %u KB used of %u KB\n", n, (unsigned)(used / 1024), (unsigned)(total / 1024));
    return 0;
}

// clip get <id>: base64 between CLIP-BEGIN/CLIP-END (scripts/clipget.py). clip rm <id|all>.
static int cmd_clip(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "get") == 0) {
        char path[32];
        clipstore_path((uint16_t)atoi(argv[2]), path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("no such clip\n");
            return 1;
        }
        esp_log_level_set("*", ESP_LOG_NONE);
        printf("CLIP-BEGIN %s\n", argv[2]);
        uint8_t chunk[57];
        char line[80];
        uint32_t crc = 0, size = 0;
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
            crc = esp_rom_crc32_le(crc, chunk, n);
            size += n;
            base64_line(chunk, n, line);
            puts(line);
        }
        fclose(f);
        printf("CLIP-END %08" PRIx32 " %" PRIu32 "\n", crc, size);
        fflush(stdout);
        esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "rm") == 0) {
        if (strcmp(argv[2], "all") == 0) {
            EXT_RAM_BSS_ATTR static clip_info_t list[128];
            int n = clipstore_list(list, 128);
            for (int i = 0; i < n; i++) {
                clipstore_remove(list[i].id);
            }
            printf("removed %d clips\n", n);
            return 0;
        }
        esp_err_t err = clipstore_remove((uint16_t)atoi(argv[2]));
        printf("%s\n", err == ESP_OK ? "removed" : "no such clip");
        return err == ESP_OK ? 0 : 1;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "meta") == 0) {
        uint16_t id = (uint16_t)atoi(argv[2]);
        if (argc == 4 && strcmp(argv[3], "redo") == 0) {
            clipstore_drop_meta(id);  // as if power was lost before the clip was finalised
        }
        clip_meta_t m;
        bool complete = false;
        esp_err_t err = clipstore_get_meta(id, audio_recording_clip(), &m, &complete);
        if (err != ESP_OK) {
            printf("no such clip\n");
            return 1;
        }
        printf("clip %u: %s, %lu bytes, %lu ms, voiced %ld, crc %08lx, start %llu\n", id,
               complete ? "complete" : "recording", (unsigned long)m.size, (unsigned long)m.duration_ms,
               m.voiced_ms == CLIP_VOICED_UNKNOWN ? -1L : (long)m.voiced_ms, (unsigned long)m.crc32,
               (unsigned long long)m.start_utc_ms);
        return 0;
    }
    printf("usage: clip get <id> | clip rm <id|all> | clip meta <id> [redo]\n");
    return 1;
}

static int cmd_play(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: play <clip id>\n");
        return 1;
    }
    esp_err_t err = audio_play_clip((uint16_t)atoi(argv[1]));
    printf("%s\n", err == ESP_OK ? "played" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_tone(int argc, char **argv)
{
    int hz = argc > 1 ? atoi(argv[1]) : 1000;
    int ms = argc > 2 ? atoi(argv[2]) : 500;
    if (hz < 50 || hz > 7000 || ms < 10 || ms > 5000) {
        printf("usage: tone [hz 50-7000] [ms 10-5000]\n");
        return 1;
    }
    return audio_tone(hz, ms) == ESP_OK ? 0 : 1;
}

static int cmd_level(int argc, char **argv)
{
    printf("mic peak level (0-100), 10 x 200 ms:");
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        printf(" %d", audio_level());
    }
    printf("\n");
    return 0;
}

// sleep [s] [rec]: standby; wake on PWR or after s seconds (rec: the timer wake records like a hold).
static int cmd_sleep(int argc, char **argv)
{
    uint32_t s = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
    bool rec = argc > 2 && strcmp(argv[2], "rec") == 0;
    printf("standby%s\n", s ? "" : " until PWR");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    power_enter_standby(s, rec);
    return 0;
}

static int cmd_ptttest(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10;
    if (n < 1 || n > 16) {
        printf("usage: ptttest [cycles 1-16]\n");
        return 1;
    }
    printf("%d cycles of standby -> timer wake -> record; results in pttlog\n", n);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    power_test_series_begin(n);
    power_enter_standby(1, true);
    return 0;
}

static int cmd_pttlog(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "clear") == 0) {
        power_clear_ptt_log();
        printf("cleared\n");
        return 0;
    }
    power_print_ptt_log();
    return 0;
}

static int cmd_wake(int argc, char **argv)
{
    uint64_t first = audio_first_frame_ticks();
    int64_t ref = power_ref_to_boot_us();
    printf("wake %s; ref->boot %s%.1f ms; boot->first frame %.1f ms\n", power_wake_name(power_wake_kind()),
           ref < 0 ? "unknown " : "", ref < 0 ? 0.0 : ref / 1000.0,
           first ? power_ticks_to_us(power_boot_ticks(), first) / 1000.0 : -1.0);
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    printf("powering off; PWR turns the board back on\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    power_off();
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        ble_set_bench(argc < 3 || strcmp(argv[2], "off") != 0);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        esp_err_t err = ble_forget_bonds();
        printf("bonds %s\n", err == ESP_OK ? "deleted" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "inject") == 0) {
        if (strcmp(argv[2], "off") == 0) {
            ble_console_link(false);
            return 0;
        }
        static uint8_t tlv[256];
        size_t hex = strlen(argv[2]);
        if (hex % 2 || hex / 2 > sizeof tlv) {
            printf("inject: even number of hex digits, at most %u bytes\n", (unsigned)sizeof tlv);
            return 1;
        }
        for (size_t i = 0; i < hex / 2; i++) {
            char byte[3] = {argv[2][2 * i], argv[2][2 * i + 1], 0};
            tlv[i] = (uint8_t)strtoul(byte, NULL, 16);
        }
        ble_inject(tlv, hex / 2);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "txpower") == 0) {
        return ble_set_tx_power(atoi(argv[2])) == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "adv") == 0) {
        ble_set_adv_mode(strcmp(argv[2], "off") == 0 ? BLE_ADV_OFF : strcmp(argv[2], "idle") == 0 ? BLE_ADV_IDLE : BLE_ADV_DELIVER);
        return 0;
    }
    if (argc >= 2) {
        printf("usage: ble [bench [on|off] | forget | adv off|idle|deliver | txpower <dBm>]\n");
        return 1;
    }
    ble_status_t b;
    ble_get_status(&b);
    printf("connected %d, authenticated %d, bench %d, pairing %d, pending %u, busy %d\n", b.connected,
           b.authenticated, b.bench, b.passkey != 0, b.pending, ble_busy());
    if (b.transferring) {
        printf("sending clip %u: %lu of %lu bytes\n", b.tx_clip, (unsigned long)b.tx_offset, (unsigned long)b.tx_size);
    }
    if (b.reply_state >= 0) {
        printf("reply for clip %u, state %d: %s\n", b.reply_clip, b.reply_state, b.reply_text);
    }
    printf("reply window %s; RSSI %d dBm\n", b.reply_window ? "open" : "closed", ble_conn_rssi());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

esp_err_t diag_start(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "coin> ";
    repl_cfg.max_cmdline_length = 640;  // `ble inject` carries up to 244 bytes as hex
    repl_cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl), TAG, "repl");

    const esp_console_cmd_t cmds[] = {
        {.command = "i2cscan", .help = "List responding I2C addresses", .func = cmd_i2cscan},
        {.command = "pmic", .help = "PMIC status and registers: pmic | pmic reg <hex> [<hex>] | pmic chg <mA>", .func = cmd_pmic},
        {.command = "imu", .help = "One IMU sample", .func = cmd_imu},
        {.command = "stats", .help = "Uptime, reset reason, heap", .func = cmd_stats},
        {.command = "fbdump", .help = "Dump the active screen as base64 RGB565", .func = cmd_fbdump},
        {.command = "rec", .help = "Record a clip: rec <seconds> | rec start | rec stop", .func = cmd_rec},
        {.command = "clips", .help = "List stored clips and storage use", .func = cmd_clips},
        {.command = "clip", .help = "clip get <id> (base64 dump) | clip rm <id|all> | clip meta <id> [redo]", .func = cmd_clip},
        {.command = "play", .help = "Play a clip on the speaker: play <id>", .func = cmd_play},
        {.command = "tone", .help = "Speaker test tone: tone [hz] [ms]", .func = cmd_tone},
        {.command = "level", .help = "Microphone peak level for 2 s", .func = cmd_level},
        {.command = "loopback", .help = "Record while playing a tone: loopback [hz]", .func = cmd_loopback},
        {.command = "sleep", .help = "Standby: sleep [timer s] [rec]", .func = cmd_sleep},
        {.command = "ptttest", .help = "Automatic standby->record latency trials: ptttest [n]", .func = cmd_ptttest},
        {.command = "pttlog", .help = "Wake-to-record latency records: pttlog [clear]", .func = cmd_pttlog},
        {.command = "wake", .help = "This boot's wake kind and timings", .func = cmd_wake},
        {.command = "poweroff", .help = "Switch the PMIC off", .func = cmd_poweroff},
        {.command = "ble", .help = "BLE status | ble bench [on|off] | ble forget | ble adv off|idle|deliver", .func = cmd_ble},
        {.command = "reboot", .help = "Restart the chip", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}
