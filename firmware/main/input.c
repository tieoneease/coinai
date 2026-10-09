#include "input.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define PIN_PWR     GPIO_NUM_3
#define PIN_BOOT    GPIO_NUM_0
#define DEBOUNCE_MS 15

static QueueHandle_t s_queue;
static input_cb_t s_cb;
static void *s_ctx;
static bool s_assume_pwr;

static bool is_pressed(gpio_num_t pin)
{
    int level = gpio_get_level(pin);
    return pin == PIN_PWR ? level == 1 : level == 0;
}

void input_get(bool *pwr_pressed, bool *boot_pressed)
{
    *pwr_pressed = is_pressed(PIN_PWR);
    *boot_pressed = is_pressed(PIN_BOOT);
}

static void IRAM_ATTR edge_isr(void *arg)
{
    uint32_t pin = (uint32_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &pin, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void input_task(void *arg)
{
    const gpio_num_t pins[2] = {PIN_PWR, PIN_BOOT};
    bool state[2] = {s_assume_pwr, is_pressed(PIN_BOOT)};
    int64_t since[2] = {esp_timer_get_time(), esp_timer_get_time()};
    uint32_t pin = PIN_PWR;
    xQueueSend(s_queue, &pin, 0);  // reconcile the assumed state with the pins once at start
    // The power-off gesture depends on this task, so the task watchdog covers it. It wakes at least
    // once a second, which also re-reads the pins in case an edge was missed.
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_queue, &pin, pdMS_TO_TICKS(1000)) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            while (xQueueReceive(s_queue, &pin, 0) == pdTRUE) {
            }
        }
        for (int i = 0; i < 2; i++) {
            bool now = is_pressed(pins[i]);
            if (now == state[i]) {
                continue;
            }
            int64_t t = esp_timer_get_time();
            uint32_t held_ms = now ? 0 : (uint32_t)((t - since[i]) / 1000);
            state[i] = now;
            since[i] = t;
            input_event_t ev = i == 0 ? (now ? INPUT_PWR_DOWN : INPUT_PWR_UP) : (now ? INPUT_BOOT_DOWN : INPUT_BOOT_UP);
            if (s_cb) {
                s_cb(ev, held_ms, s_ctx);
            }
        }
    }
}

esp_err_t input_init(input_cb_t cb, void *ctx, bool assume_pwr_pressed)
{
    s_cb = cb;
    s_ctx = ctx;
    s_assume_pwr = assume_pwr_pressed;
    s_queue = xQueueCreate(16, sizeof(uint32_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");

    // Both lines are driven externally (SYS_OUT by T1 and R9; GPIO0 by its 10 k pull-up).
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_PWR) | (1ULL << PIN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config");
    // input_init() runs before the touch driver, so the shared GPIO ISR service is installed here.
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_PWR, edge_isr, (void *)PIN_PWR), TAG, "pwr isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_BOOT, edge_isr, (void *)PIN_BOOT), TAG, "boot isr");

    bool pwr, boot;
    input_get(&pwr, &boot);
    ESP_LOGI(TAG, "at start: PWR %s (GPIO3=%d), BOOT %s (GPIO0=%d)", pwr ? "pressed" : "released",
             gpio_get_level(PIN_PWR), boot ? "pressed" : "released", gpio_get_level(PIN_BOOT));

    BaseType_t ok = xTaskCreatePinnedToCore(input_task, "input", 3072, NULL, 10, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
