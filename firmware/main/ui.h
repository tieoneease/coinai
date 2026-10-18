#pragma once
// The coin's screens: the eyes bot (eyes.c) in the centre, header or footer with the UI around it.
// Layout, flows and motion follow design/coin-ui/motion/coin-eyes.html (signed off 2026-10).
//
// ui_init() and ui_update() run with the display lock held. ui_event() is safe from any task and
// never blocks. ui_goodbye(), ui_screen_on() and ui_set_drowsy() take the lock themselves and must
// not be called from the LVGL task.

#include <stdbool.h>
#include <stdint.h>
#include "audio.h"
#include "ble.h"
#include "imu.h"
#include "pmic.h"

typedef struct {
    bool pmic_ok;
    pmic_status_t pmic;
    bool imu_ok;
    imu_sample_t imu;
    bool pwr_pressed;
    bool boot_pressed;
    uint32_t pwr_presses;
    uint32_t boot_presses;
    uint32_t uptime_s;
    bool recording;
    int mic_level;           // 0..100
    uint32_t rec_seconds;
    uint16_t last_clip;      // 0 = none yet
    uint32_t last_clip_ms;
    ble_status_t ble;
} ui_state_t;

typedef enum {
    UI_EV_BOOT_DOWN,
    UI_EV_BOOT_UP,
    UI_EV_COMBO_START,      // BOOT held, then PWR: the power-off countdown runs
    UI_EV_COMBO_CANCEL,
    UI_EV_DISCARD_ARMED,    // BOOT went down while recording: holding it discards the clip
    UI_EV_DISCARD_CANCEL,
    UI_EV_DISCARDED,
    UI_EV_CLIP_DONE,        // a recording ended: finalised or discarded (clip result attached)
} ui_ev_t;

typedef enum {
    UI_BYE_STANDBY,     // eyes close, screen fades out
    UI_BYE_SCREEN_OFF,  // same, on USB power (the coin stays awake)
    UI_BYE_POWEROFF,    // "Bye!", hop, the eyes merge into a dot
    UI_BYE_BATTERY,     // "Battery empty", then as power off
} ui_bye_t;

typedef struct {
    void (*standby)(void);    // Screen off tile, face down: as a PWR tap
    void (*power_off)(void);  // Power off confirmed on the screen
} ui_hooks_t;

// First thing in app_main: lets ui_event() queue events before the display exists.
void ui_preinit(void);
void ui_init(const ui_hooks_t *hooks, int wake_kind);  // wake_kind_t from power.h
void ui_update(const ui_state_t *st);
void ui_event(ui_ev_t ev, const audio_clip_result_t *clip);
// Plays the goodbye and returns when it has finished (at most about 2 s).
void ui_goodbye(ui_bye_t kind);
// The screen came back on after a screen-off: the eyes wake.
void ui_screen_on(void);
// The idle timeout is a few seconds away (sleepy eyes), or activity cancelled it.
void ui_set_drowsy(bool drowsy);

// True while the UI needs the coin awake without input: a `ui demo` runs, or a hold-to-confirm is held.
// Lock-free.
bool ui_keep_awake(void);
// Shows a scene from the phone or SamBot (ble_scene_fn); false when it cannot be shown at all.
bool ui_show_scene(uint16_t clip, const char *json, size_t len);
// Seconds of Standby before a running timer card needs the coin again; 0 without one. Call it only
// as the coin goes to Standby: the alarm then waits for the wake.
uint32_t ui_alarm_standby_s(void);
// Uptime in ms of the UI's last tick (every 15 ms while LVGL runs); 0 before the first. Lock-free.
uint32_t ui_heartbeat_ms(void);
// Console: ui perf [reset] | ui demo <flow> | ui show <screen> | ui expr <name>
int ui_console(int argc, char **argv);
