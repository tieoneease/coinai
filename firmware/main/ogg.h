#pragma once
// Minimal Ogg Opus (RFC 7845) writer and packet reader for mono clips.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct {
    FILE *f;
    uint32_t serial;
    uint32_t seq;
    uint64_t granule;      // 48 kHz samples through the last packet added
    uint8_t segs[255];
    int nsegs;
    uint8_t body[8192];
    size_t body_len;
    int packets;           // packets in the pending page
    uint32_t crc32;        // IEEE CRC-32 of every byte written so far (the protocol's file CRC)
    uint32_t bytes;        // bytes written so far
} ogg_opus_writer_t;

// Writes the OpusHead (BOS) and OpusTags pages.
esp_err_t ogg_opus_begin(ogg_opus_writer_t *w, FILE *f, uint32_t serial, uint16_t pre_skip, uint32_t input_rate);
// Buffers one Opus packet covering `samples48k` samples; writes a page first if this one would overflow.
esp_err_t ogg_opus_packet(ogg_opus_writer_t *w, const uint8_t *pkt, size_t len, uint32_t samples48k);
// Writes buffered packets as one page and syncs the file to flash.
esp_err_t ogg_opus_flush(ogg_opus_writer_t *w);
// Writes the buffered packets as the final (EOS) page. At least one packet must be buffered.
esp_err_t ogg_opus_end(ogg_opus_writer_t *w);
int ogg_opus_pending_packets(const ogg_opus_writer_t *w);

typedef struct {
    FILE *f;
    uint8_t segs[255];
    int nsegs;
    int seg_idx;
} ogg_reader_t;

// Next packet into buf. Returns its length, 0 at end of file, -1 on a malformed stream or a too-small buffer.
int ogg_read_packet(ogg_reader_t *r, uint8_t *buf, size_t cap);
