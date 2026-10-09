#pragma once
// FreeRTOS API subset for the desktop simulator, on Windows threads (src/freertos.c). Tasks run as
// real threads in parallel, closer to the ESP32-S3's two cores than a single-threaded port would
// be; priorities and core affinity are ignored.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
#define portTICK_PERIOD_MS (1000 / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
#define pdTICKS_TO_MS(t) ((uint32_t)(((uint64_t)(t) * 1000) / configTICK_RATE_HZ))
#define portYIELD_FROM_ISR(...) ((void)0)
#define portYIELD() sim_yield()
#define taskYIELD() sim_yield()
#define tskNO_AFFINITY 0x7FFFFFFF
#define configMAX_PRIORITIES 25
#define tskIDLE_PRIORITY 0

#define taskSCHEDULER_SUSPENDED   0
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING     2

typedef struct sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct sim_queue *QueueHandle_t;
typedef struct sim_queue *SemaphoreHandle_t;

typedef struct {
    int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void sim_critical_enter(void);
void sim_critical_exit(void);
#define portENTER_CRITICAL(mux) sim_critical_enter()
#define portEXIT_CRITICAL(mux) sim_critical_exit()
#define portENTER_CRITICAL_ISR(mux) sim_critical_enter()
#define portEXIT_CRITICAL_ISR(mux) sim_critical_exit()
#define taskENTER_CRITICAL(mux) sim_critical_enter()
#define taskEXIT_CRITICAL(mux) sim_critical_exit()

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps);
#define xTaskCreate(fn, name, stack, arg, prio, out) \
    xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
#define xTaskCreateWithCaps(fn, name, stack, arg, prio, out, caps) \
    xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, tskNO_AFFINITY, caps)
void vTaskDelete(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
char *pcTaskGetName(TaskHandle_t task);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskGetSchedulerState(void);
void sim_yield(void);

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken);
BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t wait);
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *buf, BaseType_t *woken);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
BaseType_t xQueueReset(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);
#define xQueueSendToBack(q, item, wait) xQueueSend(q, item, wait)
#define xQueueOverwrite(q, item) (xQueueReset(q), xQueueSend(q, item, 0))

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
