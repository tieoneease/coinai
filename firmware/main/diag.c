#include "diag.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "clipstore.h"
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

static int cmd_stats(int argc, char **argv)
{
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
    static clip_info_t list[128];
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
            static clip_info_t list[128];
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
    printf("usage: clip get <id> | clip rm <id|all>\n");
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
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl), TAG, "repl");

    const esp_console_cmd_t cmds[] = {
        {.command = "i2cscan", .help = "List responding I2C addresses", .func = cmd_i2cscan},
        {.command = "pmic", .help = "PMIC status and registers: pmic | pmic reg <hex> [<hex>] | pmic chg <mA>", .func = cmd_pmic},
        {.command = "imu", .help = "One IMU sample", .func = cmd_imu},
        {.command = "stats", .help = "Uptime, reset reason, heap", .func = cmd_stats},
        {.command = "fbdump", .help = "Dump the active screen as base64 RGB565", .func = cmd_fbdump},
        {.command = "rec", .help = "Record a clip: rec <seconds>", .func = cmd_rec},
        {.command = "clips", .help = "List stored clips and storage use", .func = cmd_clips},
        {.command = "clip", .help = "clip get <id> (base64 dump) | clip rm <id|all>", .func = cmd_clip},
        {.command = "play", .help = "Play a clip on the speaker: play <id>", .func = cmd_play},
        {.command = "tone", .help = "Speaker test tone: tone [hz] [ms]", .func = cmd_tone},
        {.command = "level", .help = "Microphone peak level for 2 s", .func = cmd_level},
        {.command = "loopback", .help = "Record while playing a tone: loopback [hz]", .func = cmd_loopback},
        {.command = "sleep", .help = "Standby: sleep [timer s] [rec]", .func = cmd_sleep},
        {.command = "ptttest", .help = "Automatic standby->record latency trials: ptttest [n]", .func = cmd_ptttest},
        {.command = "pttlog", .help = "Wake-to-record latency records: pttlog [clear]", .func = cmd_pttlog},
        {.command = "wake", .help = "This boot's wake kind and timings", .func = cmd_wake},
        {.command = "poweroff", .help = "Switch the PMIC off", .func = cmd_poweroff},
        {.command = "reboot", .help = "Restart the chip", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}
