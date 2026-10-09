// GPIO and I2C through the host, and the console REPL on stdin/stdout (the coin's USB serial).
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "sim_esp.h"
#include "simlink.h"

// ---------- GPIO ----------

#define PINS 49

static volatile uint8_t s_level[PINS];
static int8_t s_out[PINS];  // last level sent, -1 = never
static gpio_int_type_t s_intr[PINS];
static gpio_isr_t s_isr[PINS];
static void *s_isr_arg[PINS];

void sim_gpio_defaults(void)
{
    // Pull-ups at rest: BOOT (GPIO0) and the touch INT read 1; PWR (GPIO3) has a pull-down.
    s_level[0] = 1;
    s_level[11] = 1;
    memset(s_out, -1, sizeof s_out);
    const char *spec = sim_env("COINSIM_GPIO");
    while (spec && *spec) {
        int pin, level;
        if (sscanf(spec, "%d=%d", &pin, &level) == 2 && pin >= 0 && pin < PINS) {
            s_level[pin] = level ? 1 : 0;
        }
        spec = strchr(spec, ',');
        spec = spec ? spec + 1 : NULL;
    }
}

void sim_gpio_input(uint8_t pin, uint8_t level)
{
    if (pin >= PINS) {
        return;
    }
    uint8_t old = s_level[pin];
    s_level[pin] = level ? 1 : 0;
    gpio_int_type_t t = s_intr[pin];
    bool fire = (t == GPIO_INTR_ANYEDGE && old != s_level[pin]) || (t == GPIO_INTR_POSEDGE && !old && level) ||
                (t == GPIO_INTR_NEGEDGE && old && !level) || (t == GPIO_INTR_HIGH_LEVEL && level) ||
                (t == GPIO_INTR_LOW_LEVEL && !level);
    if (fire && s_isr[pin]) {
        s_isr[pin](s_isr_arg[pin]);  // the "interrupt" runs on the link's reader thread
    }
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    for (int pin = 0; pin < PINS; pin++) {
        if (cfg->pin_bit_mask & (1ULL << pin)) {
            s_intr[pin] = cfg->intr_type;
        }
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    return pin >= 0 && pin < PINS ? s_level[pin] : 0;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin < 0 || pin >= PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = level ? 1 : 0;
    if (s_out[pin] != v) {
        s_out[pin] = v;
        uint8_t msg[2] = {(uint8_t)pin, v};
        sim_send(IPC_GPIO_OUT, msg, 2);
    }
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { return ESP_OK; }

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t isr, void *arg)
{
    if (pin < 0 || pin >= PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_isr_arg[pin] = arg;
    s_isr[pin] = isr;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    if (pin >= 0 && pin < PINS) {
        s_isr[pin] = NULL;
    }
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{
    if (pin >= 0 && pin < PINS) {
        s_intr[pin] = type;
    }
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t pin) { return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { return ESP_OK; }
esp_err_t gpio_hold_en(gpio_num_t pin) { return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t pin) { return ESP_OK; }
void gpio_deep_sleep_hold_en(void) {}
void gpio_deep_sleep_hold_dis(void) {}
esp_err_t rtc_gpio_deinit(gpio_num_t pin) { return ESP_OK; }

// ---------- I2C ----------

struct sim_i2c_bus {
    int unused;
};
struct sim_i2c_dev {
    uint8_t addr;
};
static struct sim_i2c_bus s_bus;

esp_err_t bsp_i2c_init(void)
{
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return &s_bus;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out)
{
    struct sim_i2c_dev *d = calloc(1, sizeof *d);
    d->addr = (uint8_t)cfg->device_address;
    *out = d;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    free(dev);
    return ESP_OK;
}

static esp_err_t xfer(uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms)
{
    int rc = sim_i2c(addr, w, wlen, r, rlen, timeout_ms);
    return rc == 0 ? ESP_OK : rc == 1 ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen, int timeout_ms)
{
    return xfer(dev->addr, w, wlen, NULL, 0, timeout_ms);
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *r, size_t rlen, int timeout_ms)
{
    return xfer(dev->addr, NULL, 0, r, rlen, timeout_ms);
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen, uint8_t *r,
                                      size_t rlen, int timeout_ms)
{
    return xfer(dev->addr, w, wlen, r, rlen, timeout_ms);
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    int rc = sim_i2c((uint8_t)address, NULL, 0, NULL, 0, timeout_ms);
    return rc == 0 ? ESP_OK : rc == 1 ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
}

// ---------- console ----------

#define MAX_CMDS 64

static esp_console_cmd_t s_cmds[MAX_CMDS];
static int s_ncmds;
static esp_console_repl_config_t s_repl_cfg;
static esp_console_repl_t s_repl;

esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd)
{
    if (s_ncmds == MAX_CMDS) {
        return ESP_ERR_NO_MEM;
    }
    s_cmds[s_ncmds] = *cmd;
    s_cmds[s_ncmds].command = _strdup(cmd->command);
    s_cmds[s_ncmds].help = cmd->help ? _strdup(cmd->help) : NULL;
    s_ncmds++;
    return ESP_OK;
}

static int cmd_help(int argc, char **argv)
{
    for (int i = 0; i < s_ncmds; i++) {
        printf("%s\n  %s\n\n", s_cmds[i].command, s_cmds[i].help ? s_cmds[i].help : "");
    }
    return 0;
}

esp_err_t esp_console_register_help_command(void)
{
    esp_console_cmd_t help = {.command = "help", .help = "Print the list of registered commands", .func = cmd_help};
    return esp_console_cmd_register(&help);
}

static esp_err_t repl_del(esp_console_repl_t *repl)
{
    return ESP_OK;
}

esp_err_t esp_console_new_repl_usb_serial_jtag(const esp_console_dev_usb_serial_jtag_config_t *dev,
                                               const esp_console_repl_config_t *cfg, esp_console_repl_t **out)
{
    s_repl_cfg = *cfg;
    s_repl.del = repl_del;
    *out = &s_repl;
    return ESP_OK;
}

esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t *dev, const esp_console_repl_config_t *cfg,
                                    esp_console_repl_t **out)
{
    return esp_console_new_repl_usb_serial_jtag(NULL, cfg, out);
}

// esp_console_split_argv: whitespace separates, double quotes group, backslash escapes.
static int split_argv(char *line, char **argv, int max)
{
    int argc = 0;
    char *src = line, *dst = line;
    while (*src && argc < max - 1) {
        while (isspace((unsigned char)*src)) {
            src++;
        }
        if (!*src) {
            break;
        }
        argv[argc++] = dst;
        bool quoted = false;
        while (*src && (quoted || !isspace((unsigned char)*src))) {
            if (*src == '"') {
                quoted = !quoted;
                src++;
            } else if (*src == '\\' && src[1]) {
                *dst++ = src[1];
                src += 2;
            } else {
                *dst++ = *src++;
            }
        }
        if (*src) {
            src++;
        }
        *dst++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void repl_task(void *arg)
{
    size_t max = s_repl_cfg.max_cmdline_length ? s_repl_cfg.max_cmdline_length : 256;
    char *line = malloc(max + 2);
    const char *prompt = s_repl_cfg.prompt ? s_repl_cfg.prompt : "esp> ";
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(line, (int)max + 1, stdin)) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // stdin closed: the host is not attached
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';  // no echo: the host logs what it types
        char *argv[32];
        int argc = split_argv(line, argv, 32);
        if (!argc) {
            continue;
        }
        int i = 0;
        while (i < s_ncmds && strcmp(s_cmds[i].command, argv[0]) != 0) {
            i++;
        }
        if (i == s_ncmds) {
            printf("Unrecognized command\n");
            continue;
        }
        int ret = s_cmds[i].func ? s_cmds[i].func(argc, argv)
                                 : s_cmds[i].func_w_context(s_cmds[i].context, argc, argv);
        if (ret != 0) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        }
    }
}

esp_err_t esp_console_start_repl(esp_console_repl_t *repl)
{
    return xTaskCreate(repl_task, "console_repl", 8192, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
