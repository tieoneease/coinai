#include "ogg.h"

#include <string.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_rom_crc.h"

static const char *TAG = "ogg";

// Ogg's page checksum: CRC-32, polynomial 0x04C11DB7, MSB first, init 0, no final xor.
static uint32_t s_crc_table[256];

static void crc_table_init(void)
{
    if (s_crc_table[1]) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) {
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        }
        s_crc_table[i] = r;
    }
}

static uint32_t page_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc = (crc << 8) ^ s_crc_table[((crc >> 24) ^ *p++) & 0xFF];
    }
    return crc;
}

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static esp_err_t write_page(ogg_opus_writer_t *w, uint8_t header_type, uint64_t granule)
{
    uint8_t hdr[27 + 255];
    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;
    hdr[5] = header_type;
    put_le(hdr + 6, granule, 8);
    put_le(hdr + 14, w->serial, 4);
    put_le(hdr + 18, w->seq++, 4);
    put_le(hdr + 22, 0, 4);
    hdr[26] = (uint8_t)w->nsegs;
    memcpy(hdr + 27, w->segs, w->nsegs);
    size_t hlen = 27 + w->nsegs;
    put_le(hdr + 22, page_crc(page_crc(0, hdr, hlen), w->body, w->body_len), 4);

    ESP_RETURN_ON_FALSE(fwrite(hdr, 1, hlen, w->f) == hlen, ESP_FAIL, TAG, "write header");
    ESP_RETURN_ON_FALSE(fwrite(w->body, 1, w->body_len, w->f) == w->body_len, ESP_FAIL, TAG, "write body");
    w->crc32 = esp_rom_crc32_le(w->crc32, hdr, hlen);
    w->crc32 = esp_rom_crc32_le(w->crc32, w->body, w->body_len);
    w->bytes += hlen + w->body_len;
    w->nsegs = 0;
    w->body_len = 0;
    w->packets = 0;
    return ESP_OK;
}

static esp_err_t sync_file(ogg_opus_writer_t *w)
{
    ESP_RETURN_ON_FALSE(fflush(w->f) == 0, ESP_FAIL, TAG, "fflush");
    ESP_RETURN_ON_FALSE(fsync(fileno(w->f)) == 0, ESP_FAIL, TAG, "fsync");
    return ESP_OK;
}

static void add_packet(ogg_opus_writer_t *w, const uint8_t *pkt, size_t len)
{
    size_t left = len;
    do {
        uint8_t seg = left >= 255 ? 255 : (uint8_t)left;
        w->segs[w->nsegs++] = seg;
        left -= seg;
        if (seg < 255) {
            break;
        }
    } while (1);
    memcpy(w->body + w->body_len, pkt, len);
    w->body_len += len;
    w->packets++;
}

esp_err_t ogg_opus_begin(ogg_opus_writer_t *w, FILE *f, uint32_t serial, uint16_t pre_skip, uint32_t input_rate)
{
    crc_table_init();
    memset(w, 0, sizeof *w);
    w->f = f;
    w->serial = serial;

    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;  // version
    head[9] = 1;  // channels
    put_le(head + 10, pre_skip, 2);
    put_le(head + 12, input_rate, 4);
    put_le(head + 16, 0, 2);  // output gain
    head[18] = 0;             // channel mapping family
    add_packet(w, head, sizeof head);
    ESP_RETURN_ON_ERROR(write_page(w, 0x02, 0), TAG, "OpusHead");

    static const char vendor[] = "coinai";
    uint8_t tags[8 + 4 + sizeof vendor - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le(tags + 8, sizeof vendor - 1, 4);
    memcpy(tags + 12, vendor, sizeof vendor - 1);
    put_le(tags + 12 + sizeof vendor - 1, 0, 4);  // no user comments
    add_packet(w, tags, sizeof tags);
    ESP_RETURN_ON_ERROR(write_page(w, 0x00, 0), TAG, "OpusTags");
    return sync_file(w);
}

esp_err_t ogg_opus_packet(ogg_opus_writer_t *w, const uint8_t *pkt, size_t len, uint32_t samples48k)
{
    int segs_needed = (int)(len / 255) + 1;
    ESP_RETURN_ON_FALSE(len <= sizeof w->body && segs_needed <= 255, ESP_ERR_INVALID_SIZE, TAG, "packet too large");
    if (w->nsegs + segs_needed > 255 || w->body_len + len > sizeof w->body) {
        ESP_RETURN_ON_ERROR(ogg_opus_flush(w), TAG, "flush");
    }
    add_packet(w, pkt, len);
    w->granule += samples48k;
    return ESP_OK;
}

esp_err_t ogg_opus_flush(ogg_opus_writer_t *w)
{
    if (w->packets == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(write_page(w, 0x00, w->granule), TAG, "page");
    return sync_file(w);
}

esp_err_t ogg_opus_end(ogg_opus_writer_t *w)
{
    ESP_RETURN_ON_FALSE(w->packets > 0, ESP_ERR_INVALID_STATE, TAG, "EOS page needs a packet");
    ESP_RETURN_ON_ERROR(write_page(w, 0x04, w->granule), TAG, "EOS page");
    return sync_file(w);
}

int ogg_opus_pending_packets(const ogg_opus_writer_t *w)
{
    return w->packets;
}

static bool read_page_header(ogg_reader_t *r)
{
    uint8_t h[27];
    if (fread(h, 1, sizeof h, r->f) != sizeof h || memcmp(h, "OggS", 4) != 0) {
        return false;
    }
    r->nsegs = h[26];
    r->seg_idx = 0;
    return fread(r->segs, 1, r->nsegs, r->f) == (size_t)r->nsegs;
}

int ogg_read_packet(ogg_reader_t *r, uint8_t *buf, size_t cap)
{
    size_t len = 0;
    for (;;) {
        if (r->seg_idx >= r->nsegs) {
            if (!read_page_header(r)) {
                return len ? -1 : 0;
            }
            continue;
        }
        uint8_t seg = r->segs[r->seg_idx++];
        if (len + seg > cap || fread(buf + len, 1, seg, r->f) != seg) {
            return -1;
        }
        len += seg;
        if (seg < 255) {
            return (int)len;
        }
    }
}
