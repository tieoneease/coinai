#pragma once
// SamBot's HTTP API for the Wi-Fi path: the C twin of the app's SambotClient
// (android/app/src/main/kotlin/dev/coinai/sambot/SambotClient.kt). POST /voice/transcribe,
// GET /events (server-sent events) and POST /prompt through the exe.dev proxy, with a VM-scoped
// bearer token in X-Exedev-Authorization. Blocking; one call at a time (the net task).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SAMBOT_OK = 0,
    SAMBOT_NET,      // not reached, or failed before SamBot took anything: try again later
    SAMBOT_AUTH,     // the proxy wants a login: the token is missing, wrong or revoked
    SAMBOT_REFUSED,  // SamBot answered and refused (4xx; 409: the agent is not running)
    SAMBOT_UNKNOWN,  // the prompt may or may not have arrived: never send it again
    SAMBOT_ABORTED,  // sambot_abort() (standby)
} sambot_rc_t;

typedef struct {
    int status;        // last HTTP status, 0 = none
    char detail[96];   // SamBot's "error" text, or what failed
} sambot_err_t;

const char *sambot_rc_name(sambot_rc_t rc);

// base_url without a trailing slash, e.g. "https://sambot.exe.xyz".
void sambot_configure(const char *base_url, const char *token);
// Makes the call in progress return SAMBOT_ABORTED within about a second; cleared by the next call.
void sambot_abort(void);

typedef void (*sambot_progress_t)(uint32_t sent, uint32_t total, void *ctx);

// Uploads the clip file unchanged; `text` gets the transcript (empty: nothing heard).
sambot_rc_t sambot_transcribe(const char *path, char *text, size_t text_len, sambot_progress_t progress, void *ctx,
                              sambot_err_t *err);

// Sends `text` to `lane` and collects the assistant's reply (markdown) until agent_settled or
// `wait_ms` after the prompt was accepted. `prompted` runs as soon as SamBot accepted the prompt.
// SAMBOT_OK means the prompt went out; `*settled` says whether the turn finished in time (the text
// may be partial otherwise). Failing before the prompt is sent gives SAMBOT_NET, SAMBOT_AUTH or
// SAMBOT_REFUSED; a prompt with an unknown outcome gives SAMBOT_UNKNOWN.
sambot_rc_t sambot_ask(const char *text, const char *lane, uint32_t wait_ms, char *reply, size_t reply_len,
                       bool *settled, void (*prompted)(void *ctx), void *ctx, sambot_err_t *err);
