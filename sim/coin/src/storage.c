// LittleFS at "/c" as <data>/littlefs, and NVS as <data>/nvs.txt (one "namespace key type hex"
// line per entry), both surviving power-off like the coin's flash.
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/time.h>
#include "sim_esp.h"
#include "simlink.h"

// ---------- files ----------

#define MOUNT "/c"
#define PARTITION_BYTES 0x13D0000  // the coin's storage partition (20288 KB)

static const char *map_path(const char *path, char *out, size_t len)
{
    size_t m = strlen(MOUNT);
    if (strncmp(path, MOUNT, m) == 0 && (path[m] == '/' || path[m] == '\0')) {
        snprintf(out, len, "%s/littlefs%s", sim_data_dir(), path + m);
        return out;
    }
    return path;
}

FILE *sim_fopen(const char *path, const char *mode)
{
    char p[MAX_PATH], m[8];
    snprintf(m, sizeof m, "%s%s", mode, strchr(mode, 'b') ? "" : "b");  // never text mode
    return fopen(map_path(path, p, sizeof p), m);
}

DIR *sim_opendir(const char *path)
{
    char p[MAX_PATH];
    return opendir(map_path(path, p, sizeof p));
}

int sim_stat(const char *path, struct stat *st)
{
    char p[MAX_PATH];
    return stat(map_path(path, p, sizeof p), st);
}

int sim_remove(const char *path)
{
    char p[MAX_PATH];
    return remove(map_path(path, p, sizeof p));
}

int sim_rename(const char *from, const char *to)
{
    char a[MAX_PATH], b[MAX_PATH];
    map_path(to, b, sizeof b);
    remove(b);  // POSIX rename replaces the target; Windows' does not
    return rename(map_path(from, a, sizeof a), b);
}

int sim_truncate(const char *path, off_t len)
{
    char p[MAX_PATH];
    int fd = _open(map_path(path, p, sizeof p), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        return -1;
    }
    int rc = _chsize_s(fd, len) == 0 ? 0 : -1;
    _close(fd);
    return rc;
}

int sim_fsync(int fd)
{
    return _commit(fd);
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/littlefs", sim_data_dir());
    _mkdir(sim_data_dir());
    _mkdir(p);
    DWORD attr = GetFileAttributesA(p);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_vfs_littlefs_unregister(const char *partition_label)
{
    return ESP_OK;
}

esp_err_t esp_littlefs_info(const char *partition_label, size_t *total, size_t *used)
{
    // LittleFS keeps 4 KB blocks: count each file's blocks plus the two superblocks.
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/littlefs", sim_data_dir());
    size_t blocks = 2;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char f[MAX_PATH];
        struct stat st;
        snprintf(f, sizeof f, "%s/%s", p, e->d_name);
        if (e->d_name[0] != '.' && stat(f, &st) == 0) {
            blocks += (st.st_size + 4095) / 4096 + 1;
        }
    }
    if (d) {
        closedir(d);
    }
    if (total) {
        *total = PARTITION_BYTES;
    }
    if (used) {
        *used = blocks * 4096;
    }
    return ESP_OK;
}

// ---------- NVS ----------

typedef enum { T_U8 = 1, T_I8, T_U16, T_I16, T_U32, T_I32, T_U64, T_I64, T_STR, T_BLOB } nvs_type_t;

typedef struct {
    char ns[16];
    char key[16];
    nvs_type_t type;
    size_t len;
    uint8_t *data;
} entry_t;

static entry_t *s_entries;
static int s_count, s_cap;
static struct {
    char ns[16];
    nvs_open_mode_t mode;
    bool used;
} s_handles[32];
static CRITICAL_SECTION s_nvs_lock;
static bool s_nvs_ready;

static void nvs_path(char *out, size_t len)
{
    snprintf(out, len, "%s/nvs.txt", sim_data_dir());
}

static void nvs_save(void)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    nvs_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    for (int i = 0; i < s_count; i++) {
        fprintf(f, "%s %s %d ", s_entries[i].ns, s_entries[i].key, s_entries[i].type);
        for (size_t k = 0; k < s_entries[i].len; k++) {
            fprintf(f, "%02x", s_entries[i].data[k]);
        }
        fputc('\n', f);
    }
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static entry_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].ns, ns) == 0 && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_flash_init(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }
    InitializeCriticalSection(&s_nvs_lock);
    _mkdir(sim_data_dir());
    char path[MAX_PATH];
    nvs_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    char line[8192];
    while (f && fgets(line, sizeof line, f)) {
        char ns[16], key[16], hex[8000] = "";
        int type;
        if (sscanf(line, "%15s %15s %d %7999s", ns, key, &type, hex) < 3) {
            continue;
        }
        if (s_count == s_cap) {
            s_cap = s_cap ? s_cap * 2 : 32;
            s_entries = realloc(s_entries, s_cap * sizeof *s_entries);
        }
        entry_t *e = &s_entries[s_count++];
        memset(e, 0, sizeof *e);
        snprintf(e->ns, sizeof e->ns, "%s", ns);
        snprintf(e->key, sizeof e->key, "%s", key);
        e->type = type;
        e->len = strlen(hex) / 2;
        e->data = malloc(e->len + 1);
        for (size_t k = 0; k < e->len; k++) {
            unsigned v;
            sscanf(hex + 2 * k, "%2x", &v);
            e->data[k] = (uint8_t)v;
        }
    }
    if (f) {
        fclose(f);
    }
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    char path[MAX_PATH];
    nvs_path(path, sizeof path);
    remove(path);
    s_count = 0;
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_nvs_ready) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    EnterCriticalSection(&s_nvs_lock);
    bool exists = false;
    for (int i = 0; i < s_count; i++) {
        exists |= strcmp(s_entries[i].ns, ns) == 0;
    }
    esp_err_t err = ESP_ERR_NVS_NOT_FOUND;
    if (exists || mode == NVS_READWRITE) {
        err = ESP_ERR_NO_MEM;
        for (int i = 1; i < 32; i++) {
            if (!s_handles[i].used) {
                s_handles[i].used = true;
                s_handles[i].mode = mode;
                snprintf(s_handles[i].ns, sizeof s_handles[i].ns, "%s", ns);
                *out = i;
                err = ESP_OK;
                break;
            }
        }
    }
    LeaveCriticalSection(&s_nvs_lock);
    return err;
}

void nvs_close(nvs_handle_t h)
{
    if (h < 32) {
        s_handles[h].used = false;
    }
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    EnterCriticalSection(&s_nvs_lock);
    nvs_save();
    LeaveCriticalSection(&s_nvs_lock);
    return ESP_OK;
}

static esp_err_t set(nvs_handle_t h, const char *key, nvs_type_t type, const void *v, size_t len)
{
    if (h >= 32 || !s_handles[h].used) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (s_handles[h].mode != NVS_READWRITE) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    if (strlen(key) > 15) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    EnterCriticalSection(&s_nvs_lock);
    entry_t *e = find(s_handles[h].ns, key);
    if (!e) {
        if (s_count == s_cap) {
            s_cap = s_cap ? s_cap * 2 : 32;
            s_entries = realloc(s_entries, s_cap * sizeof *s_entries);
        }
        e = &s_entries[s_count++];
        memset(e, 0, sizeof *e);
        snprintf(e->ns, sizeof e->ns, "%s", s_handles[h].ns);
        snprintf(e->key, sizeof e->key, "%s", key);
    }
    free(e->data);
    e->type = type;
    e->len = len;
    e->data = malloc(len + 1);
    memcpy(e->data, v, len);
    LeaveCriticalSection(&s_nvs_lock);
    return ESP_OK;
}

static esp_err_t get(nvs_handle_t h, const char *key, nvs_type_t type, void *out, size_t *len, bool sized)
{
    if (h >= 32 || !s_handles[h].used) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    EnterCriticalSection(&s_nvs_lock);
    entry_t *e = find(s_handles[h].ns, key);
    esp_err_t err = ESP_OK;
    if (!e || e->type != type) {
        err = ESP_ERR_NVS_NOT_FOUND;
    } else if (sized) {
        if (!out) {
            *len = e->len;
        } else if (*len < e->len) {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            memcpy(out, e->data, e->len);
            *len = e->len;
        }
    } else {
        memcpy(out, e->data, e->len);
    }
    LeaveCriticalSection(&s_nvs_lock);
    return err;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    if (h >= 32 || !s_handles[h].used) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    EnterCriticalSection(&s_nvs_lock);
    entry_t *e = find(s_handles[h].ns, key);
    if (e) {
        free(e->data);
        *e = s_entries[--s_count];
    }
    LeaveCriticalSection(&s_nvs_lock);
    return e ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    if (h >= 32 || !s_handles[h].used) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    EnterCriticalSection(&s_nvs_lock);
    for (int i = 0; i < s_count;) {
        if (strcmp(s_entries[i].ns, s_handles[h].ns) == 0) {
            free(s_entries[i].data);
            s_entries[i] = s_entries[--s_count];
        } else {
            i++;
        }
    }
    LeaveCriticalSection(&s_nvs_lock);
    return ESP_OK;
}

#define NVS_INT(suffix, ctype, tag)                                                \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char *key, ctype v)            \
    {                                                                               \
        return set(h, key, tag, &v, sizeof v);                                      \
    }                                                                               \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char *key, ctype *v)           \
    {                                                                               \
        return get(h, key, tag, v, NULL, false);                                    \
    }
NVS_INT(u8, uint8_t, T_U8)
NVS_INT(i8, int8_t, T_I8)
NVS_INT(u16, uint16_t, T_U16)
NVS_INT(i16, int16_t, T_I16)
NVS_INT(u32, uint32_t, T_U32)
NVS_INT(i32, int32_t, T_I32)
NVS_INT(u64, uint64_t, T_U64)
NVS_INT(i64, int64_t, T_I64)

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v)
{
    return set(h, key, T_STR, v, strlen(v) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    return get(h, key, T_STR, out, len, true);
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    return set(h, key, T_BLOB, v, len);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    return get(h, key, T_BLOB, out, len, true);
}
