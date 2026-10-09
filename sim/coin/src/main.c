// coinsim.exe: the coin's firmware as a Windows process. Started by sim/host/coinsim.py, which owns
// the hardware around it (sim/IPC.md). One process lifetime is one boot: deep sleep, restart, a
// watchdog or a panic end the process and the host starts the next boot.
#include <windows.h>
#include <stdio.h>
#include "sim_esp.h"
#include "simlink.h"

void app_main(void);
void sim_gpio_defaults(void);

static void main_task(void *arg)
{
    app_main();
    vTaskDelete(NULL);
}

// A crash in firmware code is a panic on the coin: report it and let the host restart the coin.
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    printf("Guru Meditation Error: Core 0 panic'ed (exception 0x%08lx at %p)\n",
           (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    sim_exit_reset(ESP_RST_PANIC);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(on_crash);
    sim_gpio_defaults();
    sim_link_start();
    printf("coinsim %s: reset reason %d, wake causes 0x%x\n", esp_app_get_description()->version,
           (int)esp_reset_reason(), (unsigned)esp_sleep_get_wakeup_causes());
    xTaskCreatePinnedToCore(main_task, "main", CONFIG_ESP_MAIN_TASK_STACK_SIZE, NULL, 1, NULL, 0);
    for (;;) {
        Sleep(INFINITE);  // the process ends through sim_exit_*() or the host closing the link
    }
}
