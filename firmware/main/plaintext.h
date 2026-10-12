#pragma once
// SamBot's markdown reply as text for the coin's screen, the C twin of the app's
// PlainText.forCoin (android/app/src/main/kotlin/dev/coinai/clips/PlainText.kt) for replies that
// come over Wi-Fi: markup removed, only printable ASCII and newlines (PROTOCOL.md rule 7 mappings,
// accents folded, everything else dropped), space runs and blank lines collapsed. The result is cut
// at `out_len - 1` bytes, with "..." marking a cut. Returns the length written.

#include <stddef.h>

size_t plaintext_for_coin(const char *markdown, char *out, size_t out_len);
