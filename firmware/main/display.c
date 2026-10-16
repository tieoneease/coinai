#include "display.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// Two draw buffers of 24 lines (22 KB each) in internal DMA-capable RAM: LVGL renders into SRAM and the
// SPI DMA reads them directly. Measured alternatives: the BSP's 50-line PSRAM buffers made the SPI
// driver malloc a 46 KB internal bounce buffer per flush (ESP_ERR_NO_MEM once RAM got tight), and
// DMA straight from PSRAM (esp_lcd psram_dma_direct) underflowed at 40 MHz QSPI.
#define BUF_LINES 24

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

// One task at a time on the panel. ESP-IDF's SPI master is not thread-safe for one device used from
// several tasks: the bus lock keeps one bit per device, so a second task's acquire merges into the
// first's and is never woken when the first releases. That froze the UI task in a flush for good
// (2026-10-16 core dump) while the control loop set the brightness. Every panel operation goes
// through this lock: the LVGL flush (draw_bitmap, wrapped below) and the commands sent here.
static SemaphoreHandle_t s_io_lock;
static esp_err_t (*s_draw_bitmap)(esp_lcd_panel_t *, int, int, int, int, const void *);

static esp_err_t draw_bitmap_locked(esp_lcd_panel_t *panel, int x1, int y1, int x2, int y2, const void *data)
{
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    esp_err_t err = s_draw_bitmap(panel, x1, y1, x2, y2, data);
    xSemaphoreGive(s_io_lock);
    return err;
}

// Waveshare BSP 3.0.0 lcd_init_cmds, unchanged.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

// The CO5300 takes windows that start on an even and end on an odd coordinate.
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static esp_err_t tx_param(uint8_t cmd, const void *param, size_t len)
{
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), param, len);  // QSPI write opcode
    xSemaphoreGive(s_io_lock);
    return err;
}

static int s_brightness;

esp_err_t display_set_brightness(int percent)
{
    ESP_RETURN_ON_FALSE(s_io_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG, "0-100");
    uint8_t v = (uint8_t)(percent * 255 / 100);
    s_brightness = percent;
    return tx_param(0x51, &v, 1);
}

int display_get_brightness(void)
{
    return s_brightness;
}

lv_display_t *display_start(void)
{
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
                                                              BSP_LCD_DATA3, BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_cfg.trans_queue_depth = CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_cfg, &s_io));

    co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof s_init_cmds / sizeof s_init_cmds[0],
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel));
    esp_lcd_panel_set_gap(s_panel, 0x06, 0);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_io_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_io_lock ? ESP_OK : ESP_ERR_NO_MEM);
    s_draw_bitmap = s_panel->draw_bitmap;  // LVGL's flushes take the lock too
    s_panel->draw_bitmap = draw_bitmap_locked;

    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    lv_cfg.task_core_id = 1;        // core 0 has audio (and later the radio)
    ESP_ERROR_CHECK(esp_lv_adapter_init(&lv_cfg));

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = BUF_LINES,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "register display failed");
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    bsp_display_cfg_t touch_cfg = {.touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1}};  // as bsp_display_start()
    if (bsp_touch_new(&touch_cfg, &s_touch) == ESP_OK) {
        const esp_lv_adapter_touch_config_t t = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch);
        // input.c installed the GPIO ISR service first. The touch interrupt setup tolerates that,
        // but the GPIO driver still logs an error for the second install.
        esp_log_level_set("gpio", ESP_LOG_NONE);
        lv_indev_t *indev = esp_lv_adapter_register_touch(&t);
        esp_log_level_set("gpio", CONFIG_LOG_DEFAULT_LEVEL);
        if (!indev) {
            ESP_LOGE(TAG, "register touch failed");
        }
    } else {
        ESP_LOGE(TAG, "touch init failed");
    }
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    return disp;
}
