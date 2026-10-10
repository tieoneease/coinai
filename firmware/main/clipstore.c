#include "clipstore.h"

#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"

static const char *TAG = "clips";

#define BASE      "/c"
#define PARTITION "storage"

esp_err_t clipstore_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE,
        .partition_label = PARTITION,
        .format_if_mount_failed = true,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf), TAG, "mount");
    DIR *dir = opendir(BASE);  // discarded clips whose file was busy until the reboot
    if (dir) {
        uint16_t ids[16];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < 16) {
            unsigned id;
            char ext[8];
            if (sscanf(e->d_name, "%5u.%7s", &id, ext) == 2 && strcmp(ext, "dis") == 0 && id && id <= 65535) {
                ids[n++] = (uint16_t)id;
            }
        }
        closedir(dir);
        for (int i = 0; i < n; i++) {
            clipstore_remove(ids[i]);
            ESP_LOGI(TAG, "clip %u: discarded before the reboot, deleted", ids[i]);
        }
    }
    size_t total = 0, used = 0;
    clipstore_usage(&total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u KB used of %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

uint16_t clipstore_next_id(void)
{
    nvs_handle_t nvs;
    uint16_t id = 1;
    if (nvs_open("clips", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u16(nvs, "next", &id);
        if (id == 0) {
            id = 1;
        }
        nvs_set_u16(nvs, "next", id == 65535 ? 1 : id + 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return id;
}

void clipstore_path(uint16_t id, char *buf, size_t len)
{
    snprintf(buf, len, BASE "/%05u.ogg", id);
}

static int cmp_clip(const void *a, const void *b)
{
    return (int)((const clip_info_t *)a)->id - (int)((const clip_info_t *)b)->id;
}

static volatile uint16_t s_discarded;  // the latest discarded clip whose file could not be removed yet

static void dis_path(uint16_t id, char *buf, size_t len)
{
    snprintf(buf, len, BASE "/%05u.dis", id);
}

bool clipstore_discarded(uint16_t id)
{
    if (!id) {
        return false;
    }
    if (id == s_discarded) {
        return true;
    }
    char path[32];
    struct stat st;
    dis_path(id, path, sizeof path);
    return stat(path, &st) == 0;
}

bool clipstore_discard_pending(uint16_t id)
{
    return id && id == s_discarded;
}

int clipstore_list(clip_info_t *out, int max)
{
    DIR *dir = opendir(BASE);
    if (!dir) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < max) {
        unsigned id;
        char ext[8];
        if (sscanf(e->d_name, "%5u.%7s", &id, ext) != 2 || strcmp(ext, "ogg") != 0 || id == 0 || id > 65535 ||
            clipstore_discarded((uint16_t)id)) {
            continue;
        }
        if (out) {
            char path[32];
            struct stat st;
            clipstore_path((uint16_t)id, path, sizeof path);
            out[n].id = (uint16_t)id;
            out[n].size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        }
        n++;
    }
    closedir(dir);
    if (out) {
        qsort(out, n, sizeof *out, cmp_clip);
    }
    return n;
}

int clipstore_count(void)
{
    return clipstore_list(NULL, 65535);
}

static void meta_path(uint16_t id, char *buf, size_t len)
{
    snprintf(buf, len, BASE "/%05u.met", id);
}

esp_err_t clipstore_remove(uint16_t id)
{
    char path[32];
    meta_path(id, path, sizeof path);
    remove(path);
    clipstore_path(id, path, sizeof path);
    bool gone = remove(path) == 0;
    if (!gone && errno != ENOENT) {
        return ESP_ERR_INVALID_STATE;  // still open: LittleFS refuses with EBUSY
    }
    dis_path(id, path, sizeof path);
    remove(path);
    if (id == s_discarded) {
        s_discarded = 0;
    }
    return gone ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void clipstore_discard(uint16_t id)
{
    uint16_t prev = s_discarded;
    s_discarded = id;  // first, so a transfer cannot rebuild its metadata in between
    if (prev && prev != id) {
        clipstore_remove(prev);  // an earlier discard whose file was busy (its tombstone stays if it still is)
    }
    if (clipstore_remove(id) == ESP_ERR_INVALID_STATE) {
        char path[32];
        dis_path(id, path, sizeof path);
        FILE *f = fopen(path, "wb");  // outlives a reboot: clipstore_init finishes the job
        if (f) {
            fclose(f);
        }
        ESP_LOGW(TAG, "clip %u is open: deleted when its transfer stops", id);
    }
}

#define META_MAGIC 0x314D4F43u  // "COM1"

void clipstore_drop_meta(uint16_t id)
{
    char path[32];
    meta_path(id, path, sizeof path);
    remove(path);
}

esp_err_t clipstore_put_meta(uint16_t id, const clip_meta_t *meta)
{
    char path[32];
    meta_path(id, path, sizeof path);
    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", path);
    uint32_t magic = META_MAGIC;
    bool ok = fwrite(&magic, sizeof magic, 1, f) == 1 && fwrite(meta, sizeof *meta, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    return ok ? ESP_OK : ESP_FAIL;
}

// Size, CRC and duration (last whole page's granule position) of a clip without metadata.
static esp_err_t recompute_meta(uint16_t id, clip_meta_t *meta)
{
    char path[32];
    clipstore_path(id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "no clip %u", id);
    memset(meta, 0, sizeof *meta);
    meta->voiced_ms = CLIP_VOICED_UNKNOWN;
    uint8_t hdr[27 + 255];
    uint8_t buf[512];
    uint64_t granule = 0;
    uint16_t pre_skip = 0;
    uint32_t crc = 0, pos = 0;
    bool first = true;
    while (fread(hdr, 1, 27, f) == 27 && memcmp(hdr, "OggS", 4) == 0) {
        uint8_t nseg = hdr[26];
        if (fread(hdr + 27, 1, nseg, f) != nseg) {
            break;
        }
        uint32_t body = 0;
        for (int i = 0; i < nseg; i++) {
            body += hdr[27 + i];
        }
        uint32_t page_crc = esp_rom_crc32_le(crc, hdr, 27 + nseg);
        bool whole = true;
        while (body > 0) {
            size_t chunk = body > sizeof buf ? sizeof buf : body;
            if (fread(buf, 1, chunk, f) != chunk) {
                whole = false;
                break;
            }
            if (first && chunk >= 12 && memcmp(buf, "OpusHead", 8) == 0) {
                pre_skip = (uint16_t)(buf[10] | (buf[11] << 8));
            }
            first = false;
            page_crc = esp_rom_crc32_le(page_crc, buf, chunk);
            body -= chunk;
        }
        if (!whole) {
            break;  // torn last page: the clip ends at the last whole one
        }
        uint32_t page_len = 27 + nseg;
        for (int i = 0; i < nseg; i++) {
            page_len += hdr[27 + i];
        }
        pos += page_len;
        crc = page_crc;
        uint64_t g = 0;
        for (int i = 0; i < 8; i++) {
            g |= (uint64_t)hdr[6 + i] << (8 * i);
        }
        if (g != UINT64_MAX) {
            granule = g;
        }
    }
    fclose(f);
    meta->size = pos;
    meta->crc32 = crc;
    meta->duration_ms = granule > pre_skip ? (uint32_t)((granule - pre_skip) / 48) : 0;
    if (pos > 0) {
        // Drop a torn tail so the file, its size and its CRC agree.
        truncate(path, pos);
    }
    return ESP_OK;
}

esp_err_t clipstore_get_meta(uint16_t id, uint16_t recording, clip_meta_t *meta, bool *complete)
{
    if (clipstore_discarded(id)) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[32];
    meta_path(id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t magic = 0;
        bool ok = fread(&magic, sizeof magic, 1, f) == 1 && magic == META_MAGIC && fread(meta, sizeof *meta, 1, f) == 1;
        fclose(f);
        if (ok) {
            *complete = true;
            return ESP_OK;
        }
    }
    struct stat st;
    clipstore_path(id, path, sizeof path);
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0, ESP_ERR_NOT_FOUND, TAG, "no clip %u", id);
    if (id == recording) {
        memset(meta, 0, sizeof *meta);
        meta->size = (uint32_t)st.st_size;
        *complete = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(recompute_meta(id, meta), TAG, "recompute %u", id);
    ESP_LOGW(TAG, "clip %u was cut off; recovered %lu bytes, %lu ms", id, (unsigned long)meta->size,
             (unsigned long)meta->duration_ms);
    clipstore_put_meta(id, meta);
    *complete = true;
    return ESP_OK;
}

esp_err_t clipstore_usage(size_t *total, size_t *used)
{
    return esp_littlefs_info(PARTITION, total, used);
}
