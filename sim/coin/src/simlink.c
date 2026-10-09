// The connection to the simulator host (sim/IPC.md), the launch environment, RTC memory and the
// ways the process ends.
#include "simlink.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_esp.h"

#define I2C_SLOTS 8

static SOCKET s_sock = INVALID_SOCKET;
static CRITICAL_SECTION s_send_lock;
static CRITICAL_SECTION s_i2c_lock;
static LARGE_INTEGER s_qpc_freq, s_qpc_start;
static uint64_t s_rtc_at_start;
static char s_data_dir[MAX_PATH];

static struct {
    bool busy;
    uint16_t seq;
    HANDLE done;
    int status;
    uint8_t *r;
    size_t rlen;
} s_i2c[I2C_SLOTS];
static uint16_t s_i2c_seq;

// ---------- environment ----------

const char *sim_env(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

uint64_t sim_env_u64(const char *name, uint64_t def)
{
    const char *v = sim_env(name);
    return v ? strtoull(v, NULL, 0) : def;
}

const char *sim_data_dir(void)
{
    return s_data_dir;
}

uint64_t sim_rtc_us(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return s_rtc_at_start + (uint64_t)((now.QuadPart - s_qpc_start.QuadPart) * 1000000 / s_qpc_freq.QuadPart);
}

int64_t esp_timer_get_time(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart - s_qpc_start.QuadPart) * 1000000 / s_qpc_freq.QuadPart);
}

// ---------- RTC memory: the .rtcdat section of this executable ----------

static uint8_t *rtc_section(size_t *len)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".rtcdat", 8) == 0) {
            *len = sec->Misc.VirtualSize;
            return base + sec->VirtualAddress;
        }
    }
    *len = 0;
    return NULL;
}

static void rtc_restore(void)
{
    // As the bootloader: RTC_DATA_ATTR variables keep their values only through deep sleep; every
    // other boot starts them from their initial values.
    const char *path = sim_env("COINSIM_RTC");
    size_t len;
    uint8_t *rtc = rtc_section(&len);
    if (!path || !rtc || sim_env_u64("COINSIM_RESET", 1) != ESP_RST_DEEPSLEEP) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    uint8_t *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len + 1, f);
    fclose(f);
    if (n == len) {
        DWORD old;
        VirtualProtect(rtc, len, PAGE_READWRITE, &old);
        memcpy(rtc, buf, len);
    } else {
        fprintf(stderr, "coinsim: RTC memory image is %u bytes, this build has %u; starting with it zeroed\n",
                (unsigned)n, (unsigned)len);
    }
    free(buf);
}

// ---------- frames ----------

static bool send_all(const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        int n = send(s_sock, p, len > 0x10000000 ? 0x10000000 : (int)len, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

void sim_send2(uint8_t type, const void *head, size_t head_len, const void *body, size_t body_len)
{
    uint8_t hdr[5] = {type};
    sim_put32(hdr + 1, (uint32_t)(head_len + body_len));
    EnterCriticalSection(&s_send_lock);
    bool ok = send_all(hdr, 5) && (!head_len || send_all(head, head_len)) && (!body_len || send_all(body, body_len));
    LeaveCriticalSection(&s_send_lock);
    if (!ok) {
        ExitProcess(1);  // the host is gone
    }
}

void sim_send(uint8_t type, const void *payload, size_t len)
{
    sim_send2(type, payload, len, NULL, 0);
}

static bool recv_all(void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        int n = recv(s_sock, p, (int)len, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

static void i2c_result(const uint8_t *p, size_t len)
{
    if (len < 3) {
        return;
    }
    uint16_t seq = sim_get16(p);
    EnterCriticalSection(&s_i2c_lock);
    for (int i = 0; i < I2C_SLOTS; i++) {
        if (s_i2c[i].busy && s_i2c[i].seq == seq) {
            s_i2c[i].status = p[2];
            size_t n = len - 3 < s_i2c[i].rlen ? len - 3 : s_i2c[i].rlen;
            if (n) {
                memcpy(s_i2c[i].r, p + 3, n);
            }
            SetEvent(s_i2c[i].done);
            break;
        }
    }
    LeaveCriticalSection(&s_i2c_lock);
}

static DWORD WINAPI reader(void *arg)
{
    uint8_t hdr[5];
    uint8_t *buf = NULL;
    size_t cap = 0;
    while (recv_all(hdr, 5)) {
        size_t len = sim_get32(hdr + 1);
        if (len > cap) {
            buf = realloc(buf, len);
            cap = len;
        }
        if (len && !recv_all(buf, len)) {
            break;
        }
        switch (hdr[0]) {
        case IPC_I2C_RESULT:
            i2c_result(buf, len);
            break;
        case IPC_GPIO_IN:
            if (len >= 2) {
                sim_gpio_input(buf[0], buf[1]);
            }
            break;
        case IPC_TOUCH:
            if (len >= 5) {
                sim_touch_input(buf[0], sim_get16(buf + 1), sim_get16(buf + 3));
            }
            break;
        case IPC_MIC:
            sim_mic_input((const int16_t *)buf, len / 2);
            break;
        default:
            if (hdr[0] >= 0x90 && hdr[0] <= 0x9F) {
                sim_ble_input(hdr[0], buf, len);
            }
            break;
        }
    }
    // The host closed the connection: it has stopped the simulation.
    ExitProcess(0);
    return 0;
}

int sim_i2c(uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms)
{
    int slot = -1;
    uint16_t seq;
    EnterCriticalSection(&s_i2c_lock);
    for (int i = 0; i < I2C_SLOTS; i++) {
        if (!s_i2c[i].busy) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LeaveCriticalSection(&s_i2c_lock);
        return -1;
    }
    seq = ++s_i2c_seq;
    s_i2c[slot].busy = true;
    s_i2c[slot].seq = seq;
    s_i2c[slot].r = r;
    s_i2c[slot].rlen = rlen;
    s_i2c[slot].status = -1;
    ResetEvent(s_i2c[slot].done);
    LeaveCriticalSection(&s_i2c_lock);

    uint8_t head[7];
    sim_put16(head, seq);
    head[2] = addr;
    sim_put16(head + 3, (uint16_t)wlen);
    sim_put16(head + 5, (uint16_t)rlen);
    sim_send2(IPC_I2C, head, sizeof head, w, wlen);
    // The host answers from its event loop; allow for a busy host beyond the bus timeout.
    DWORD wait = WaitForSingleObject(s_i2c[slot].done, timeout_ms < 1000 ? 1000 : timeout_ms);

    EnterCriticalSection(&s_i2c_lock);
    int status = wait == WAIT_OBJECT_0 ? s_i2c[slot].status : -1;
    s_i2c[slot].busy = false;
    LeaveCriticalSection(&s_i2c_lock);
    return status;
}

// ---------- start and end ----------

void sim_link_start(void)
{
    QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&s_qpc_start);
    s_rtc_at_start = sim_env_u64("COINSIM_RTC_US", 0);
    InitializeCriticalSection(&s_send_lock);
    InitializeCriticalSection(&s_i2c_lock);
    for (int i = 0; i < I2C_SLOTS; i++) {
        s_i2c[i].done = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
    const char *dir = sim_env("COINSIM_DATA");
    snprintf(s_data_dir, sizeof s_data_dir, "%s", dir ? dir : ".");
    rtc_restore();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    uint16_t port = (uint16_t)sim_env_u64("COINSIM_IPC", 0);
    if (!port) {
        fprintf(stderr, "coinsim: COINSIM_IPC is not set; start the coin from sim/host/coinsim.py\n");
        ExitProcess(2);
    }
    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "coinsim: cannot connect to the host on port %u\n", port);
        ExitProcess(2);
    }
    BOOL nodelay = TRUE;
    setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);

    const char *ver = esp_app_get_description()->version;
    uint8_t hello[4 + 32];
    sim_put32(hello, 1);
    size_t n = strlen(ver);
    memcpy(hello + 4, ver, n);
    sim_send(IPC_HELLO, hello, 4 + n);
    CreateThread(NULL, 256 * 1024, reader, NULL, 0, NULL);
}

static void leave(void)
{
    fflush(stdout);
    fflush(stderr);
    shutdown(s_sock, SD_SEND);
    ExitProcess(0);
    __builtin_unreachable();
}

void sim_exit_reset(int reason)
{
    uint8_t r = (uint8_t)reason;
    sim_send(IPC_RESET, &r, 1);
    leave();
}

void sim_exit_sleep(uint64_t timer_us, uint64_t ext1_mask, int ext1_mode)
{
    size_t len;
    uint8_t *rtc = rtc_section(&len);
    uint8_t head[21];
    sim_put64(head, timer_us);
    sim_put64(head + 8, ext1_mask);
    head[16] = (uint8_t)ext1_mode;
    sim_put32(head + 17, (uint32_t)len);
    sim_send2(IPC_SLEEP, head, sizeof head, rtc, len);
    leave();
}
