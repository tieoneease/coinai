// FreeRTOS tasks, queues and semaphores on Windows threads, and the task watchdog.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_esp.h"
#include "simlink.h"

struct sim_task {
    HANDLE thread;
    char name[16];
    TaskFunction_t fn;
    void *arg;
    uint32_t stack;
    // task watchdog
    bool wdt;
    int64_t wdt_last_us;
    struct sim_task *next;
};

struct sim_queue {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE can_send, can_recv;
    uint8_t *buf;
    size_t item, cap, head, count;
    // mutexes
    bool mutex, recursive;
    struct sim_task *owner;
    int depth;
};

static __thread struct sim_task *t_self;
static CRITICAL_SECTION s_tasks_lock;
static struct sim_task *s_tasks;
static CRITICAL_SECTION s_critical;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE o, void *p, void **c)
{
    InitializeCriticalSection(&s_tasks_lock);
    InitializeCriticalSectionAndSpinCount(&s_critical, 1000);
    return TRUE;
}

static void init(void)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
}

static DWORD wait_ms(TickType_t ticks)
{
    return ticks == portMAX_DELAY ? INFINITE : pdTICKS_TO_MS(ticks);
}

// ---------- tasks ----------

static struct sim_task *self(void)
{
    if (!t_self) {
        // A thread the simulator started itself (link reader, timers): give it a name for logs.
        init();
        struct sim_task *t = calloc(1, sizeof *t);
        snprintf(t->name, sizeof t->name, "sim%lu", (unsigned long)GetCurrentThreadId());
        t_self = t;
        EnterCriticalSection(&s_tasks_lock);
        t->next = s_tasks;
        s_tasks = t;
        LeaveCriticalSection(&s_tasks_lock);
    }
    return t_self;
}

static DWORD WINAPI task_main(void *p)
{
    struct sim_task *t = p;
    t_self = t;
    t->fn(t->arg);
    // A FreeRTOS task must not return; on the chip this aborts. Mirror vTaskDelete(NULL) instead.
    vTaskDelete(NULL);
    return 0;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    init();
    struct sim_task *t = calloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name ? name : "task");
    t->fn = fn;
    t->arg = arg;
    t->stack = stack_bytes;
    EnterCriticalSection(&s_tasks_lock);
    t->next = s_tasks;
    s_tasks = t;
    LeaveCriticalSection(&s_tasks_lock);
    // x86-64 code needs more stack than Xtensa code for the same C; reserve generously.
    SIZE_T reserve = stack_bytes * 8 < 1024 * 1024 ? 1024 * 1024 : stack_bytes * 8;
    t->thread = CreateThread(NULL, reserve, task_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->thread) {
        return pdFAIL;
    }
    if (out) {
        *out = t;
    }
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps)
{
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, out, core);
}

void vTaskDelete(TaskHandle_t task)
{
    if (!task || task == t_self) {
        if (t_self) {
            t_self->wdt = false;
        }
        ExitThread(0);
    }
    task->wdt = false;
    TerminateThread(task->thread, 0);
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    vTaskDelete(task);
}

void vTaskDelay(TickType_t ticks)
{
    Sleep(ticks ? pdTICKS_TO_MS(ticks) : 0);
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(esp_timer_get_time() * configTICK_RATE_HZ / 1000000);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return self();
}

char *pcTaskGetName(TaskHandle_t task)
{
    return (task ? task : self())->name;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    // Stack use on x86-64 says nothing about the coin; report half the requested size.
    struct sim_task *t = task ? task : self();
    return t->stack / 2;
}

BaseType_t xTaskGetSchedulerState(void)
{
    return taskSCHEDULER_RUNNING;
}

void sim_yield(void)
{
    SwitchToThread();
}

void sim_critical_enter(void)
{
    init();
    EnterCriticalSection(&s_critical);
}

void sim_critical_exit(void)
{
    LeaveCriticalSection(&s_critical);
}

// ---------- queues and semaphores ----------

static struct sim_queue *queue_new(size_t cap, size_t item)
{
    struct sim_queue *q = calloc(1, sizeof *q);
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->can_send);
    InitializeConditionVariable(&q->can_recv);
    q->cap = cap;
    q->item = item;
    q->buf = item ? calloc(cap, item) : NULL;
    return q;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    return queue_new(length, item_size);
}

static BaseType_t queue_put(struct sim_queue *q, const void *item, TickType_t wait, bool front)
{
    DWORD ms = wait_ms(wait);
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    EnterCriticalSection(&q->cs);
    while (q->count >= q->cap) {
        if (ms == 0) {
            LeaveCriticalSection(&q->cs);
            return pdFALSE;
        }
        DWORD left = INFINITE;
        if (ms != INFINITE) {
            int64_t l = (deadline - esp_timer_get_time()) / 1000;
            if (l <= 0) {
                LeaveCriticalSection(&q->cs);
                return pdFALSE;
            }
            left = (DWORD)l;
        }
        SleepConditionVariableCS(&q->can_send, &q->cs, left);
    }
    if (q->item) {
        size_t slot;
        if (front) {
            q->head = (q->head + q->cap - 1) % q->cap;
            slot = q->head;
        } else {
            slot = (q->head + q->count) % q->cap;
        }
        memcpy(q->buf + slot * q->item, item, q->item);
    }
    q->count++;
    LeaveCriticalSection(&q->cs);
    WakeConditionVariable(&q->can_recv);
    return pdTRUE;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    return queue_put(q, item, wait, false);
}

BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t wait)
{
    return queue_put(q, item, wait, true);
}

BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken)
{
    if (woken) {
        *woken = pdFALSE;
    }
    return queue_put(q, item, 0, false);
}

static BaseType_t queue_get(struct sim_queue *q, void *buf, TickType_t wait)
{
    DWORD ms = wait_ms(wait);
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    EnterCriticalSection(&q->cs);
    while (q->count == 0) {
        if (ms == 0) {
            LeaveCriticalSection(&q->cs);
            return pdFALSE;
        }
        DWORD left = INFINITE;
        if (ms != INFINITE) {
            int64_t l = (deadline - esp_timer_get_time()) / 1000;
            if (l <= 0) {
                LeaveCriticalSection(&q->cs);
                return pdFALSE;
            }
            left = (DWORD)l;
        }
        SleepConditionVariableCS(&q->can_recv, &q->cs, left);
    }
    if (q->item) {
        memcpy(buf, q->buf + q->head * q->item, q->item);
        q->head = (q->head + 1) % q->cap;
    }
    q->count--;
    LeaveCriticalSection(&q->cs);
    WakeConditionVariable(&q->can_send);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t wait)
{
    return queue_get(q, buf, wait);
}

BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *buf, BaseType_t *woken)
{
    if (woken) {
        *woken = pdFALSE;
    }
    return queue_get(q, buf, 0);
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    EnterCriticalSection(&q->cs);
    UBaseType_t n = (UBaseType_t)q->count;
    LeaveCriticalSection(&q->cs);
    return n;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    EnterCriticalSection(&q->cs);
    q->head = q->count = 0;
    LeaveCriticalSection(&q->cs);
    WakeAllConditionVariable(&q->can_send);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t q)
{
    if (!q) {
        return;
    }
    DeleteCriticalSection(&q->cs);
    free(q->buf);
    free(q);
}

// A semaphore is a queue of empty items: count = available tokens.
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial)
{
    struct sim_queue *q = queue_new(max, 0);
    q->count = initial;
    return q;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateCounting(1, 0);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct sim_queue *q = xSemaphoreCreateCounting(1, 1);
    q->mutex = true;
    return q;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    struct sim_queue *q = xSemaphoreCreateMutex();
    q->recursive = true;
    return q;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    if (queue_get(s, NULL, wait) != pdTRUE) {
        return pdFALSE;
    }
    if (s->mutex) {
        s->owner = self();
    }
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s->mutex) {
        s->owner = NULL;
    }
    return queue_put(s, NULL, 0, false);
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait)
{
    if (s->owner == self()) {
        s->depth++;
        return pdTRUE;
    }
    if (xSemaphoreTake(s, wait) != pdTRUE) {
        return pdFALSE;
    }
    s->depth = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    if (s->owner != self()) {
        return pdFALSE;
    }
    if (--s->depth > 0) {
        return pdTRUE;
    }
    return xSemaphoreGive(s);
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    if (woken) {
        *woken = pdFALSE;
    }
    return queue_put(s, NULL, 0, false);
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s)
{
    return uxQueueMessagesWaiting(s);
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    vQueueDelete(s);
}

// ---------- task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S, panics like the chip) ----------

static HANDLE s_wdt_thread;

static DWORD WINAPI wdt_main(void *arg)
{
    for (;;) {
        Sleep(250);
        int64_t now = esp_timer_get_time();
        struct sim_task *late = NULL;
        EnterCriticalSection(&s_tasks_lock);
        for (struct sim_task *t = s_tasks; t; t = t->next) {
            if (t->wdt && now - t->wdt_last_us > CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000000LL) {
                late = t;
                break;
            }
        }
        LeaveCriticalSection(&s_tasks_lock);
        if (late) {
            ESP_LOGE("task_wdt", "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:");
            ESP_LOGE("task_wdt", " - %s (CPU 0/1)", late->name);
            ESP_LOGE("task_wdt", "Aborting.");
            sim_exit_reset(ESP_RST_TASK_WDT);
        }
    }
    return 0;
}

esp_err_t esp_task_wdt_add(TaskHandle_t task)
{
    struct sim_task *t = task ? task : self();
    if (t->wdt) {
        return ESP_ERR_INVALID_ARG;
    }
    t->wdt_last_us = esp_timer_get_time();
    t->wdt = true;
    if (!s_wdt_thread) {
        s_wdt_thread = CreateThread(NULL, 0, wdt_main, NULL, 0, NULL);
    }
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void)
{
    struct sim_task *t = self();
    if (!t->wdt) {
        return ESP_ERR_NOT_FOUND;
    }
    t->wdt_last_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task)
{
    struct sim_task *t = task ? task : self();
    if (!t->wdt) {
        return ESP_ERR_INVALID_ARG;
    }
    t->wdt = false;
    return ESP_OK;
}
