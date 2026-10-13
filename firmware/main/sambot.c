#include "sambot.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sambot";

#define AUTH_HEADER     "X-Exedev-Authorization"
#define BOUNDARY        "coinai-7d4a1f0c93b2"
#define READ_SLICE_MS   500        // stream reads return at least this often, to check deadlines and aborts
#define LIVE_WAIT_MS    15000      // GET /events until host_stream_live
#define TRANSCRIBE_MS   45000      // per network wait while SamBot transcribes
#define SSE_MAX         (512 * 1024)  // a bigger frame is skipped
#define TRANSCRIBE_BODY_MAX (256 * 1024)  // the JSON with the transcript: hours of speech
#define POLL_MS         2500       // after the prompt: how often the coin looks whether the turn is over
#define WINDOW_MAX      400        // durable events read back for one turn, at most

static char s_base[128] = "https://sambot.exe.xyz";
static char s_auth[600];
static volatile bool s_abort;
static size_t s_rx;  // event-stream bytes read during the current sambot_ask

const char *sambot_rc_name(sambot_rc_t rc)
{
    static const char *const names[] = {"ok", "network", "auth", "refused", "unknown", "aborted"};
    return rc <= SAMBOT_ABORTED ? names[rc] : "?";
}

void sambot_configure(const char *base_url, const char *token)
{
    snprintf(s_base, sizeof s_base, "%s", base_url);
    size_t n = strlen(s_base);
    while (n && s_base[n - 1] == '/') {
        s_base[--n] = 0;
    }
    snprintf(s_auth, sizeof s_auth, "Bearer %s", token ? token : "");
}

void sambot_abort(void)
{
    s_abort = true;
}

static void *ps_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void sambot_begin(void)
{
    // A transcript can be tens of KB; its JSON copies stay out of the internal RAM Wi-Fi needs.
    static cJSON_Hooks hooks = {.malloc_fn = ps_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    s_abort = false;
}

static void set_err(sambot_err_t *err, int status, const char *fmt, const char *arg)
{
    if (err) {
        err->status = status;
        snprintf(err->detail, sizeof err->detail, fmt, arg ? arg : "");
    }
}

static esp_http_client_handle_t client(const char *path, esp_http_client_method_t method, int timeout_ms)
{
    char url[256];
    snprintf(url, sizeof url, "%s%s", s_base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,  // an unauthenticated request is redirected to the login page
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "coinai",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h) {
        esp_http_client_set_header(h, AUTH_HEADER, s_auth);
    }
    return h;
}

// The proxy redirects a request without a valid token to its login page (or may answer 401/403).
static bool auth_status(int status)
{
    return (status >= 300 && status < 400) || status == 401 || status == 403;
}

// Reads a small response body; returns its length (-1 on error).
static int read_body(esp_http_client_handle_t h, char *buf, size_t len)
{
    if (len == 0) {
        return -1;
    }
    int total = 0;
    int64_t deadline = esp_timer_get_time() + 20 * 1000000LL;
    while ((size_t)total < len - 1) {
        int n = esp_http_client_read(h, buf + total, len - 1 - total);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (s_abort || esp_timer_get_time() > deadline) {
                break;
            }
            continue;
        }
        if (n < 0) {
            buf[total] = 0;
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = 0;
    return total;
}

// Reads a response body of up to `max` bytes into a PSRAM buffer the caller frees. Returns its
// length; -1 on a read error, -2 when it is longer than `max` (*out stays NULL in both cases).
static int read_body_alloc(esp_http_client_handle_t h, size_t max, char **out)
{
    *out = NULL;
    size_t cap = 4096, total = 0;
    char *buf = ps_malloc(cap);
    if (!buf) {
        return -1;
    }
    int64_t deadline = esp_timer_get_time() + 20 * 1000000LL;
    for (;;) {
        if (total + 1 == cap) {
            if (cap >= max) {
                free(buf);
                return -2;
            }
            char *q = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!q) {
                free(buf);
                return -1;
            }
            buf = q;
            cap *= 2;
        }
        int n = esp_http_client_read(h, buf + total, cap - 1 - total);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (s_abort || esp_timer_get_time() > deadline) {
                break;
            }
            continue;
        }
        if (n < 0) {
            free(buf);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    if (total > max) {
        free(buf);
        return -2;
    }
    buf[total] = 0;
    *out = buf;
    return (int)total;
}

// SamBot's {"error": "..."} text, if any.

static void error_detail(const char *body, sambot_err_t *err, int status, const char *what)
{
    cJSON *j = cJSON_Parse(body);
    const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
    if (cJSON_IsString(e)) {
        set_err(err, status, "%s", e->valuestring);
    } else {
        char s[48];
        snprintf(s, sizeof s, "%s: HTTP %d", what, status);
        set_err(err, status, "%s", s);
    }
    cJSON_Delete(j);
}

static bool write_all(esp_http_client_handle_t h, const char *buf, int len)
{
    while (len > 0) {
        int n = esp_http_client_write(h, buf, len);
        if (n <= 0) {
            return false;
        }
        buf += n;
        len -= n;
    }
    return true;
}

sambot_rc_t sambot_check(sambot_err_t *err)
{
    set_err(err, 0, "", NULL);
    esp_http_client_handle_t h = client("/health", HTTP_METHOD_GET, 10000);
    if (!h) {
        set_err(err, 0, "no HTTP client%s", NULL);
        return SAMBOT_NET;
    }
    sambot_rc_t rc = SAMBOT_NET;
    esp_err_t e = esp_http_client_open(h, 0);
    if (e == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
        int status = esp_http_client_get_status_code(h);
        rc = status == 200 ? SAMBOT_OK : auth_status(status) ? SAMBOT_AUTH : SAMBOT_NET;
        set_err(err, status, "HTTP status%s", NULL);
    } else {
        set_err(err, 0, "%s", esp_err_to_name(e == ESP_OK ? ESP_FAIL : e));
    }
    esp_http_client_cleanup(h);
    return rc;
}

sambot_rc_t sambot_transcribe(const char *path, char **text, sambot_progress_t progress, void *ctx,
                              sambot_err_t *err)
{
    *text = NULL;
    set_err(err, 0, "", NULL);
    if (s_abort) {
        return SAMBOT_ABORTED;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, 0, "clip file missing", NULL);
        return SAMBOT_REFUSED;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char head[200], tail[48];
    int hl = snprintf(head, sizeof head,
                      "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"clip.ogg\"\r\n"
                      "Content-Type: audio/ogg\r\n\r\n");
    int tl = snprintf(tail, sizeof tail, "\r\n--" BOUNDARY "--\r\n");
    uint32_t total = hl + size + tl;

    sambot_rc_t rc = SAMBOT_NET;
    char *buf = ps_malloc(4096);
    esp_http_client_handle_t h = client("/voice/transcribe", HTTP_METHOD_POST, TRANSCRIBE_MS);
    if (!buf || !h) {
        set_err(err, 0, "out of memory", NULL);
        goto out;
    }
    esp_http_client_set_header(h, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
    esp_err_t e = esp_http_client_open(h, total);
    if (e != ESP_OK) {
        set_err(err, 0, "connect: %s", esp_err_to_name(e));
        goto out;
    }
    if (!write_all(h, head, hl)) {
        set_err(err, 0, "upload failed", NULL);
        goto out;
    }
    uint32_t sent = 0;
    size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (s_abort) {
            rc = SAMBOT_ABORTED;
            goto out;
        }
        if (!write_all(h, buf, (int)n)) {
            set_err(err, 0, "upload failed", NULL);
            goto out;
        }
        sent += n;
        if (progress) {
            progress(sent, (uint32_t)size, ctx);
        }
    }
    if (!write_all(h, tail, tl)) {
        set_err(err, 0, "upload failed", NULL);
        goto out;
    }
    if (esp_http_client_fetch_headers(h) < 0) {
        set_err(err, 0, s_abort ? "aborted" : "no answer", NULL);
        rc = s_abort ? SAMBOT_ABORTED : SAMBOT_NET;
        goto out;
    }
    int status = esp_http_client_get_status_code(h);
    if (status >= 200 && status < 300) {
        char *body = NULL;
        int blen = read_body_alloc(h, TRANSCRIBE_BODY_MAX, &body);
        cJSON *j = blen >= 0 ? cJSON_Parse(body) : NULL;
        const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "text") : NULL;
        if (cJSON_IsString(t) && (*text = ps_malloc(strlen(t->valuestring) + 1)) != NULL) {
            strcpy(*text, t->valuestring);
            rc = SAMBOT_OK;
        } else if (blen == -2) {
            set_err(err, status, "transcribe: answer too long", NULL);
            rc = SAMBOT_REFUSED;  // the phone has no such limit
        } else {
            set_err(err, status, blen == -1 ? "transcribe: answer cut off" : "transcribe: unexpected answer", NULL);
        }
        cJSON_Delete(j);
        free(body);
        goto out;
    }
    int len = read_body(h, buf, 4096);
    if (auth_status(status)) {
        set_err(err, status, "token rejected", NULL);
        rc = SAMBOT_AUTH;
    } else if (status >= 400 && status < 500 && status != 408 && status != 429) {
        error_detail(buf, err, status, "transcribe");
        rc = SAMBOT_REFUSED;
    } else {
        error_detail(len >= 0 ? buf : "", err, status, "transcribe");  // 5xx, 408, 429: later
    }
out:
    if (h) {
        esp_http_client_cleanup(h);
    }
    free(buf);
    fclose(f);
    return rc;
}

// ---------- server-sent events ----------

typedef struct {
    char *line;
    size_t len, cap;
    char *data;
    size_t dlen, dcap;
    bool has_data, overflow;
    bool stopped;  // on_frame returned false
} sse_t;

static bool grow(char **p, size_t *cap, size_t need)
{
    if (need <= *cap) {
        return true;
    }
    if (need > SSE_MAX) {
        return false;
    }
    size_t c = *cap ? *cap : 1024;
    while (c < need) {
        c *= 2;
    }
    char *q = heap_caps_realloc(*p, c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q) {
        return false;
    }
    *p = q;
    *cap = c;
    return true;
}

static void sse_free(sse_t *s)
{
    free(s->line);
    free(s->data);
}

// Feeds bytes; calls on_frame(data) for each complete event. on_frame returns false to stop; the
// return value is the number of bytes used (n unless stopped), so the caller can keep the rest.
static size_t sse_feed(sse_t *s, const char *buf, size_t n, bool (*on_frame)(const char *data, void *ctx), void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c != '\n') {
            if (!s->overflow && grow(&s->line, &s->cap, s->len + 2)) {
                s->line[s->len++] = c;
            } else {
                s->overflow = true;
            }
            continue;
        }
        if (s->len && s->line[s->len - 1] == '\r') {
            s->len--;
        }
        if (s->len == 0) {  // end of an event
            if (s->has_data && !s->overflow) {
                s->data[s->dlen] = 0;
                if (!on_frame(s->data, ctx)) {
                    s->has_data = s->overflow = false;
                    s->dlen = 0;
                    s->len = 0;
                    s->stopped = true;
                    return i + 1;
                }
            }
            s->has_data = s->overflow = false;
            s->dlen = 0;
        } else if (!s->overflow && s->len >= 5 && memcmp(s->line, "data:", 5) == 0) {
            const char *v = s->line + 5;
            size_t vl = s->len - 5;
            if (vl && *v == ' ') {
                v++, vl--;
            }
            if (grow(&s->data, &s->dcap, s->dlen + vl + 2)) {
                if (s->has_data) {
                    s->data[s->dlen++] = '\n';
                }
                memcpy(s->data + s->dlen, v, vl);
                s->dlen += vl;
                s->has_data = true;
            } else {
                s->overflow = true;
            }
        }
        // event:, id:, retry: and comments carry nothing SamBot uses. An oversized line ends here;
        // its event stays marked as overflowed until the blank line.
        s->len = 0;
    }
    return n;
}

typedef struct {
    bool live;      // host_stream_live reached
    bool busy;      // its `busy`: the lane is in a turn
    long total;     // its `historyTotal`: durable events in the lane's transcript (-1: unknown)
    bool collect;   // gather the replayed window's assistant text and agent_settled
    bool settled;
    char *reply;
    size_t reply_len, reply_used;
} ask_state_t;

static const char *type_of(const cJSON *j)
{
    const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "type") : NULL;
    return cJSON_IsString(t) ? t->valuestring : "";
}

static void append(ask_state_t *st, const char *s)
{
    size_t n = strlen(s);
    if (st->reply_used + n >= st->reply_len) {
        n = st->reply_len - 1 - st->reply_used;
    }
    memcpy(st->reply + st->reply_used, s, n);
    st->reply_used += n;
    st->reply[st->reply_used] = 0;
}

// The app's assistantText(): message = event.message ?: event; role "assistant"; string content
// as is, array content: the `text` of its {"type": "text"} blocks.
static void assistant_text(const cJSON *event, ask_state_t *st)
{
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(event, "message");
    if (!m || cJSON_IsNull(m)) {
        m = event;
    } else if (!cJSON_IsObject(m)) {
        return;
    }
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
    if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0) {
        return;
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
    if (cJSON_IsString(content)) {
        append(st, content->valuestring);
    } else if (cJSON_IsArray(content)) {
        const cJSON *block;
        cJSON_ArrayForEach(block, content)
        {
            const cJSON *bt = cJSON_GetObjectItemCaseSensitive(block, "type");
            const cJSON *tx = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(bt) && strcmp(bt->valuestring, "text") == 0 && cJSON_IsString(tx)) {
                append(st, tx->valuestring);
            }
        }
    }
}

// A replayed frame, up to host_stream_live. SamBot writes `{"type":"..."` first, so the type is
// read without parsing; only host_stream_live and (when collecting) message_end are parsed.
static bool frame_window(const char *data, void *ctx)
{
    ask_state_t *st = ctx;
    static const char PRE[] = "{\"type\":\"";
    char type[32] = "";
    cJSON *j = NULL;
    if (strncmp(data, PRE, sizeof PRE - 1) == 0) {
        const char *t = data + sizeof PRE - 1, *q = strchr(t, '"');
        if (q && (size_t)(q - t) < sizeof type) {
            memcpy(type, t, q - t);
            type[q - t] = 0;
        }
    } else {  // laid out some other way: parse to find out
        j = cJSON_Parse(data);
        snprintf(type, sizeof type, "%s", type_of(j));
    }
    bool live = strcmp(type, "host_stream_live") == 0;
    bool text = st->collect && (strcmp(type, "message_end") == 0 || strcmp(type, "message") == 0);
    if ((live || text) && !j) {
        j = cJSON_Parse(data);
    }
    if (live && j) {
        const cJSON *total = cJSON_GetObjectItemCaseSensitive(j, "historyTotal");
        st->busy = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "busy"));
        st->total = cJSON_IsNumber(total) ? (long)total->valuedouble : -1;
        st->live = true;
    } else if (text && j) {
        assistant_text(j, st);
    } else if (st->collect && strcmp(type, "agent_settled") == 0) {
        st->settled = true;
    }
    cJSON_Delete(j);
    return !st->live;  // stop at the boundary: what follows is live
}

typedef struct {
    char *buf;            // 1 KB read buffer
    size_t rest, rest_len;  // bytes after the frame that stopped the last pump(), not yet fed
} pump_t;

// Reads the event stream until `on_frame` stops it or the deadline. Returns 1 when stopped by the
// callback, 0 at the deadline, -1 when the stream failed or ended, -2 on abort.
static int pump(esp_http_client_handle_t h, sse_t *sse, pump_t *p, int64_t deadline_us,
                bool (*on_frame)(const char *, void *), void *ctx)
{
    sse->stopped = false;
    if (p->rest_len) {
        size_t len = p->rest_len;
        p->rest_len = 0;
        size_t used = sse_feed(sse, p->buf + p->rest, len, on_frame, ctx);
        if (used < len) {
            p->rest += used;
            p->rest_len = len - used;
        }
        if (sse->stopped) {
            return 1;
        }
    }
    while (esp_timer_get_time() < deadline_us) {
        if (s_abort) {
            return -2;
        }
        int n = esp_http_client_read(h, p->buf, 1024);
        if (n > 0) {
            s_rx += (size_t)n;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        size_t used = sse_feed(sse, p->buf, (size_t)n, on_frame, ctx);
        if (used < (size_t)n) {
            p->rest = used;
            p->rest_len = (size_t)n - used;
        }
        if (sse->stopped) {
            return 1;  // also when the stopping frame was the chunk's last
        }
    }
    return 0;
}

static sambot_rc_t post_prompt(const char *text, const char *lane, sambot_err_t *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "lane", lane);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) {
        set_err(err, 0, "out of memory", NULL);
        return SAMBOT_NET;
    }
    sambot_rc_t rc = SAMBOT_UNKNOWN;
    char resp[512];
    esp_http_client_handle_t h = client("/prompt", HTTP_METHOD_POST, 20000);
    if (!h) {
        set_err(err, 0, "out of memory", NULL);
        rc = SAMBOT_NET;
        goto out;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json; charset=utf-8");
    esp_err_t e = esp_http_client_open(h, (int)strlen(body));
    if (e != ESP_OK) {
        set_err(err, 0, "prompt: connect: %s", esp_err_to_name(e));  // no body went out
        rc = SAMBOT_NET;
        goto out;
    }
    if (!write_all(h, body, (int)strlen(body)) || esp_http_client_fetch_headers(h) < 0) {
        set_err(err, 0, "prompt: no answer", NULL);
        goto out;  // unknown: it may have arrived
    }
    int status = esp_http_client_get_status_code(h);
    int len = read_body(h, resp, sizeof resp);
    if (auth_status(status)) {
        set_err(err, status, "token rejected", NULL);
        rc = SAMBOT_AUTH;
    } else if (status == 409) {
        set_err(err, status, "agent is not running", NULL);
        rc = SAMBOT_REFUSED;
    } else if (status >= 500 || len < 0) {
        error_detail(len >= 0 ? resp : "", err, status, "prompt");  // the proxy, typically: unknown
    } else if (status >= 400) {
        error_detail(resp, err, status, "prompt");
        rc = SAMBOT_REFUSED;
    } else {
        cJSON *j = cJSON_Parse(resp);
        const cJSON *ok = j ? cJSON_GetObjectItemCaseSensitive(j, "ok") : NULL;
        if (!j) {
            set_err(err, status, "prompt: unreadable answer", NULL);
        } else if (cJSON_IsTrue(ok)) {
            rc = SAMBOT_OK;
        } else {
            set_err(err, status, "prompt: ok=false", NULL);
            rc = SAMBOT_REFUSED;
        }
        cJSON_Delete(j);
    }
out:
    if (h) {
        esp_http_client_cleanup(h);
    }
    cJSON_free(body);
    return rc;
}

// One look at a lane: GET /events?lane=&tail=N, the replayed window up to host_stream_live, then
// the connection is closed. Fills st->busy and st->total; with st->collect also the window's
// assistant text (appended to st->reply) and whether it holds agent_settled.
static sambot_rc_t read_window(const char *lane, long tail, ask_state_t *st, sambot_err_t *err)
{
    st->live = st->busy = st->settled = false;
    st->total = -1;
    char path[96];
    snprintf(path, sizeof path, "/events?lane=%s&tail=%ld", lane, tail);
    sse_t sse = {0};
    pump_t pb = {.buf = ps_malloc(1024)};
    esp_http_client_handle_t h = client(path, HTTP_METHOD_GET, 15000);
    sambot_rc_t rc = SAMBOT_NET;
    if (!pb.buf || !h) {
        set_err(err, 0, "out of memory", NULL);
        goto out;
    }
    if (s_abort) {
        rc = SAMBOT_ABORTED;
        goto out;
    }
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) {
        set_err(err, 0, "events: connect: %s", esp_err_to_name(e));
        goto out;
    }
    if (esp_http_client_fetch_headers(h) < 0) {
        set_err(err, 0, "events: no answer", NULL);
        rc = s_abort ? SAMBOT_ABORTED : SAMBOT_NET;
        goto out;
    }
    int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        char body[256];
        read_body(h, body, sizeof body);
        if (auth_status(status)) {
            set_err(err, status, "token rejected", NULL);
            rc = SAMBOT_AUTH;
        } else {
            error_detail(body, err, status, "events");
            // As for transcribe: a client error lasts (409: the agent is not running); 408, 429,
            // 5xx and odd 2xx are worth another try.
            rc = status >= 400 && status < 500 && status != 408 && status != 429 ? SAMBOT_REFUSED : SAMBOT_NET;
        }
        goto out;
    }
    esp_http_client_set_timeout_ms(h, READ_SLICE_MS);  // from here on: short reads, to watch deadlines
    int r = pump(h, &sse, &pb, esp_timer_get_time() + LIVE_WAIT_MS * 1000LL, frame_window, st);
    if (r == 1 && st->total >= 0) {
        rc = SAMBOT_OK;
    } else {
        rc = r == -2 ? SAMBOT_ABORTED : SAMBOT_NET;
        set_err(err, 0, r == 0 ? "events: not live in time" : "events: stream ended", NULL);
    }
out:
    if (h) {
        esp_http_client_cleanup(h);
    }
    sse_free(&sse);
    free(pb.buf);
    return rc;
}

sambot_rc_t sambot_ask(const char *text, const char *lane, uint32_t wait_ms, char *reply, size_t reply_len,
                       bool *settled, const sambot_ask_hooks_t *hooks, sambot_err_t *err)
{
    reply[0] = 0;
    *settled = false;
    set_err(err, 0, "", NULL);
    ask_state_t st = {.reply = reply, .reply_len = reply_len};
    s_rx = 0;
    // 1. Where the lane's transcript ends now: the turn is whatever comes after. (tail=1: SamBot
    // reads tail=0 as the whole transcript.)
    sambot_rc_t rc = read_window(lane, 1, &st, err);
    if (rc != SAMBOT_OK) {
        return rc;
    }
    long base = st.total;
    // 2. The prompt.
    if (s_abort || (hooks && hooks->before_prompt && !hooks->before_prompt(hooks->ctx))) {
        set_err(err, 0, "aborted before the prompt", NULL);
        return SAMBOT_ABORTED;
    }
    rc = post_prompt(text, lane, err);
    if (rc != SAMBOT_OK) {
        return rc;
    }
    if (hooks && hooks->prompted) {
        hooks->prompted(hooks->ctx);
    }
    // 3. Not the live stream: it carries every token delta, each with the whole message so far,
    // megabytes for an answer with thinking. While the lane is busy only its newest durable event is
    // read; once it is idle, the turn's durable events are read back (final messages only).
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    int looks = 0;
    for (bool last = false; !last && !s_abort;) {
        int64_t wake = esp_timer_get_time() + POLL_MS * 1000LL;
        if (wake >= deadline) {
            wake = deadline;
            last = true;  // time is up: whatever the turn has said so far
        }
        while (esp_timer_get_time() < wake && !s_abort) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        sambot_err_t e2;
        st.collect = false;
        looks++;
        if (read_window(lane, 1, &st, &e2) != SAMBOT_OK) {
            continue;  // a failed look: the next one may work
        }
        if (st.total < base) {
            base = 0;  // a new session (its own transcript) since the prompt
        }
        if (st.total <= base || (st.busy && !last)) {
            continue;
        }
        long n = st.total - base;
        st.collect = true;
        st.reply_used = 0;
        reply[0] = 0;
        looks++;
        if (read_window(lane, n < WINDOW_MAX ? n : WINDOW_MAX, &st, &e2) == SAMBOT_OK && st.settled) {
            *settled = true;
            break;
        }
    }
    ESP_LOGI(TAG, "turn %s after %d looks, %u bytes read", *settled ? "settled" : "not settled", looks,
             (unsigned)s_rx);
    return SAMBOT_OK;  // the prompt went out
}
