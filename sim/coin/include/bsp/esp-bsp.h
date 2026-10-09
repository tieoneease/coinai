#pragma once
// The Waveshare BSP functions the firmware uses, for the desktop simulator.
#include "esp_codec_dev.h"
#include "sim_esp.h"

#define BSP_LCD_H_RES 466
#define BSP_LCD_V_RES 466

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
