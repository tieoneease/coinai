#include "clipstore.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
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
        if (sscanf(e->d_name, "%5u.%7s", &id, ext) != 2 || strcmp(ext, "ogg") != 0 || id == 0 || id > 65535) {
            continue;
        }
        char path[32];
        struct stat st;
        clipstore_path((uint16_t)id, path, sizeof path);
        out[n].id = (uint16_t)id;
        out[n].size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        n++;
    }
    closedir(dir);
    qsort(out, n, sizeof *out, cmp_clip);
    return n;
}

esp_err_t clipstore_remove(uint16_t id)
{
    char path[32];
    clipstore_path(id, path, sizeof path);
    return remove(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t clipstore_usage(size_t *total, size_t *used)
{
    return esp_littlefs_info(PARTITION, total, used);
}
