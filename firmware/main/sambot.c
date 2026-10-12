#include "sambot.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sambot";

#define AUTH_HEADER     "X-Exedev-Authorization"
#define BOUNDARY        "coinai-7d4a1f0c93b2"
#define READ_SLICE_MS   500        // stream reads return at least this often, to check deadlines and aborts
#define LIVE_WAIT_MS    15000      // GET /events until host_stream_live
#define TRANSCRIBE_MS   45000      // per network wait while SamBot transcribes
#define SSE_MAX         (512 * 1024)  // a bigger frame is skipped
#define TRANSCRIBE_BODY_MAX (256 * 1024)  // the JSON with the transcript: hours of speech
// History replayed before host_stream_live. Without `tail` SamBot replays the lane's whole
// transcript (119 MB in 2026-10). `tail=0` would mean live-only but SamBot reads it as everything
// (`slice(-0)`), so one event it is; anything before host_stream_live is discarded anyway.
#define REPLAY_TAIL     "1"

static char s_base[128] = "https://sambot.exe.xyz";
static char s_auth[600];
static volatile bool s_abort;

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
    bool live;
    bool settled;
    char *reply;
    size_t reply_len, reply_used;
} ask_state_t;

static const char *type_of(const cJSON *j)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

static bool frame_until_live(const char *data, void *ctx)
{
    ask_state_t *st = ctx;
    // Replayed frames can be big: only the one that may be the boundary is parsed.
    if (!strstr(data, "host_stream_live")) {
        return true;
    }
    cJSON *j = cJSON_Parse(data);
    st->live = j && strcmp(type_of(j), "host_stream_live") == 0;
    cJSON_Delete(j);
    return !st->live;  // stop at the boundary: later frames in the same chunk belong to the turn
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

static bool frame_turn(const char *data, void *ctx)
{
    ask_state_t *st = ctx;
    cJSON *j = cJSON_Parse(data);
    if (!j) {
        return true;
    }
    const char *type = type_of(j);
    if (strcmp(type, "agent_settled") == 0) {
        st->settled = true;
    } else if (strcmp(type, "message_end") == 0 || strcmp(type, "message") == 0) {
        assistant_text(j, st);
    }
    cJSON_Delete(j);
    return !st->settled;
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

sambot_rc_t sambot_ask(const char *text, const char *lane, uint32_t wait_ms, char *reply, size_t reply_len,
                       bool *settled, const sambot_ask_hooks_t *hooks, sambot_err_t *err)
{
    reply[0] = 0;
    *settled = false;
    set_err(err, 0, "", NULL);
    ask_state_t st = {.reply = reply, .reply_len = reply_len};
    sse_t sse = {0};
    pump_t pb = {.buf = ps_malloc(1024)};
    char path[96];
    snprintf(path, sizeof path, "/events?lane=%s&tail=" REPLAY_TAIL, lane);
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
    // 1. Everything up to host_stream_live is replay.
    int r = pump(h, &sse, &pb, esp_timer_get_time() + LIVE_WAIT_MS * 1000LL, frame_until_live, &st);
    if (r != 1) {
        rc = r == -2 ? SAMBOT_ABORTED : SAMBOT_NET;
        set_err(err, 0, r == 0 ? "events: not live in time" : "events: stream ended", NULL);
        goto out;
    }
    // 2. Only then the prompt.
    if (s_abort || (hooks && hooks->before_prompt && !hooks->before_prompt(hooks->ctx))) {
        set_err(err, 0, "aborted before the prompt", NULL);
        rc = SAMBOT_ABORTED;
        goto out;
    }
    rc = post_prompt(text, lane, err);
    if (rc != SAMBOT_OK) {
        goto out;
    }
    if (hooks && hooks->prompted) {
        hooks->prompted(hooks->ctx);
    }
    // 3. Assistant text until agent_settled (the stream stays open from before the prompt).
    r = pump(h, &sse, &pb, esp_timer_get_time() + (int64_t)wait_ms * 1000, frame_turn, &st);
    *settled = st.settled;
    if (r < 0) {
        ESP_LOGW(TAG, "event stream %s after the prompt", r == -2 ? "aborted" : "ended");
    }
out:
    if (h) {
        esp_http_client_cleanup(h);
    }
    sse_free(&sse);
    free(pb.buf);
    return rc;
}
