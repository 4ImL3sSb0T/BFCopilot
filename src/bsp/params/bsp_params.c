#include "bsp_params.h"

#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_params_table.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bsp_params";

static nvs_handle_t s_handle;
static bool s_ready;

#define BSP_PARAMS_ENSURE_READY()  do { if (!s_ready) return ESP_ERR_INVALID_STATE; } while (0)
#define BSP_PARAMS_ENSURE_ARG(cond) do { if (cond) return ESP_ERR_INVALID_ARG; } while (0)

/* -------------------------------------------------------------------------- */
/* 初始化与命名空间                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_nvs_init(void) {
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase: %s", esp_err_to_name(rc));
        rc = nvs_flash_erase();
        if (rc == ESP_OK) {
            rc = nvs_flash_init();
        }
    }
    // 已经初始化过时 ESP-IDF 返回 INVALID_STATE，对调用方来说是成功。
    return (rc == ESP_ERR_INVALID_STATE) ? ESP_OK : rc;
}

esp_err_t bsp_params_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = bsp_nvs_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(rc));
        return rc;
    }

    rc = nvs_open(BSP_PARAMS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "open \"%s\" failed: %s", BSP_PARAMS_NAMESPACE, esp_err_to_name(rc));
        return rc;
    }

    s_ready = true;
    ESP_LOGI(TAG, "namespace \"%s\" ready", BSP_PARAMS_NAMESPACE);
    return ESP_OK;
}

esp_err_t bsp_params_deinit(void) {
    BSP_PARAMS_ENSURE_READY();
    nvs_close(s_handle);
    s_ready = false;
    return ESP_OK;
}

bool bsp_params_ready(void) {
    return s_ready;
}

/* -------------------------------------------------------------------------- */
/* 通用键值：字符串、二进制块、删除                                           */
/* -------------------------------------------------------------------------- */

// 写入成功才 commit：漏 commit 会在掉电时静默丢值，多写一次 flash 的代价可以接受。
static esp_err_t commit_if_ok(esp_err_t rc) {
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

esp_err_t bsp_params_set_str(const char *key, const char *value) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || value == NULL);
    return commit_if_ok(nvs_set_str(s_handle, key, value));
}

esp_err_t bsp_params_get_str(const char *key, char *buf, size_t buf_size) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || buf == NULL || buf_size == 0);

    buf[0] = '\0';
    return nvs_get_str(s_handle, key, buf, &buf_size);
}

esp_err_t bsp_params_set_blob(const char *key, const void *data, size_t len) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || data == NULL || len == 0);
    return commit_if_ok(nvs_set_blob(s_handle, key, data, len));
}

esp_err_t bsp_params_get_blob(const char *key, void *buf, size_t *len) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || len == NULL);
    return nvs_get_blob(s_handle, key, buf, len);
}

esp_err_t bsp_params_erase(const char *key) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL);

    esp_err_t rc = nvs_erase_key(s_handle, key);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        // 键本来就不存在，目标状态已经达成，不当作错误。
        return ESP_OK;
    }
    return commit_if_ok(rc);
}

esp_err_t bsp_params_erase_all(void) {
    BSP_PARAMS_ENSURE_READY();
    return commit_if_ok(nvs_erase_all(s_handle));
}

/* -------------------------------------------------------------------------- */
/* 参数表                                                                     */
/* -------------------------------------------------------------------------- */

static bool type_is_numeric(bsp_params_type_t type) {
    return type >= BSP_PARAMS_TYPE_U8 && type <= BSP_PARAMS_TYPE_F32;
}

// 遍历所有模块的参数表。表是编译期汇总的 const 数组，这里不复制、不加锁。
typedef struct {
    const bsp_params_table_getter_t *getters;
    size_t table_count;
    size_t table;
    size_t index;
    size_t count;
    const bsp_params_spec_t *specs;
} spec_iter_t;

static spec_iter_t spec_iter_begin(void) {
    spec_iter_t it = { 0 };
    it.getters = bsp_params_tables(&it.table_count);
    if (it.table_count > 0) {
        it.specs = it.getters[0](&it.count);
    }
    return it;
}

static const bsp_params_spec_t *spec_iter_next(spec_iter_t *it) {
    while (it->table < it->table_count) {
        if (it->index < it->count) {
            return &it->specs[it->index++];
        }
        if (++it->table == it->table_count) {
            break;
        }
        it->index = 0;
        it->specs = it->getters[it->table](&it->count);
    }
    return NULL;
}

const bsp_params_spec_t *bsp_params_find(const char *key) {
    if (key == NULL) {
        return NULL;
    }

    spec_iter_t it = spec_iter_begin();
    for (const bsp_params_spec_t *s = spec_iter_next(&it); s != NULL; s = spec_iter_next(&it)) {
        if (strcmp(s->key, key) == 0) {
            return s;
        }
    }
    return NULL;
}

size_t bsp_params_count(void) {
    size_t total = 0;
    spec_iter_t it = spec_iter_begin();
    while (spec_iter_next(&it) != NULL) {
        total++;
    }
    return total;
}

const bsp_params_spec_t *bsp_params_spec(size_t index) {
    spec_iter_t it = spec_iter_begin();
    const bsp_params_spec_t *s;
    while ((s = spec_iter_next(&it)) != NULL) {
        if (index-- == 0) {
            return s;
        }
    }
    return NULL;
}

// 数值类型与 NVS 存取函数的对应表。读写两个 switch 共用这一份，新增类型只改这里。
#define BSP_PARAMS_NUMERIC_TYPES(X)                                                 \
    X(BSP_PARAMS_TYPE_U8, u8, uint8_t)                                              \
    X(BSP_PARAMS_TYPE_I8, i8, int8_t)                                               \
    X(BSP_PARAMS_TYPE_U16, u16, uint16_t)                                           \
    X(BSP_PARAMS_TYPE_I16, i16, int16_t)                                            \
    X(BSP_PARAMS_TYPE_U32, u32, uint32_t)                                           \
    X(BSP_PARAMS_TYPE_I32, i32, int32_t)                                            \
    X(BSP_PARAMS_TYPE_U64, u64, uint64_t)                                           \
    X(BSP_PARAMS_TYPE_I64, i64, int64_t)

// 不走公开 set 函数：apply_defaults / reset 需要把多次写入合并成一次 commit，
// 否则恢复出厂设置会按参数个数写很多次 flash。
static esp_err_t read_numeric_nvs(const bsp_params_spec_t *spec, f64 *out) {
    switch (spec->type) {
#define BSP_PARAMS_READ_CASE(type_enum, suffix, ctype)                              \
    case type_enum: {                                                               \
        ctype raw = 0;                                                              \
        esp_err_t rc = nvs_get_##suffix(s_handle, spec->key, &raw);                 \
        if (rc == ESP_OK) {                                                         \
            *out = (f64)raw;                                                        \
        }                                                                           \
        return rc;                                                                  \
    }
        BSP_PARAMS_NUMERIC_TYPES(BSP_PARAMS_READ_CASE)
#undef BSP_PARAMS_READ_CASE
        case BSP_PARAMS_TYPE_F32: {
            uint32_t bits = 0;
            esp_err_t rc = nvs_get_u32(s_handle, spec->key, &bits);
            if (rc == ESP_OK) {
                f32 raw;
                memcpy(&raw, &bits, sizeof(raw));
                *out = raw;
            }
            return rc;
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t write_numeric_nvs(const bsp_params_spec_t *spec, f64 value) {
    if (!isfinite(value)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (spec->type == BSP_PARAMS_TYPE_F32) {
        if (value < -FLT_MAX || value > FLT_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        if (trunc(value) != value) {
            return ESP_ERR_INVALID_ARG;
        }
        switch (spec->type) {
            case BSP_PARAMS_TYPE_U8:
                if (value < 0 || value > UINT8_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_I8:
                if (value < INT8_MIN || value > INT8_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_U16:
                if (value < 0 || value > UINT16_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_I16:
                if (value < INT16_MIN || value > INT16_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_U32:
                if (value < 0 || value > UINT32_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_I32:
                if (value < INT32_MIN || value > INT32_MAX) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_U64:
                if (value < 0 || value >= 0x1p64) return ESP_ERR_INVALID_ARG;
                break;
            case BSP_PARAMS_TYPE_I64:
                if (value < -0x1p63 || value >= 0x1p63) return ESP_ERR_INVALID_ARG;
                break;
            default:
                return ESP_ERR_INVALID_ARG;
        }
    }
    switch (spec->type) {
#define BSP_PARAMS_WRITE_CASE(type_enum, suffix, ctype)                             \
    case type_enum:                                                                 \
        return nvs_set_##suffix(s_handle, spec->key, (ctype)value);
        BSP_PARAMS_NUMERIC_TYPES(BSP_PARAMS_WRITE_CASE)
#undef BSP_PARAMS_WRITE_CASE
        case BSP_PARAMS_TYPE_F32: {
            f32 raw = (f32)value;
            uint32_t bits;
            memcpy(&bits, &raw, sizeof(bits));
            return nvs_set_u32(s_handle, spec->key, bits);
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

#undef BSP_PARAMS_NUMERIC_TYPES

static bool spec_has_default(const bsp_params_spec_t *s) {
    switch (s->type) {
    case BSP_PARAMS_TYPE_STR:
        return s->def_str != NULL;
    case BSP_PARAMS_TYPE_BLOB:
        return s->def_blob != NULL && s->def_blob_len > 0;
    default:
        return true;
    }
}

static esp_err_t spec_write_default(const bsp_params_spec_t *s) {
    switch (s->type) {
    case BSP_PARAMS_TYPE_STR:
        return nvs_set_str(s_handle, s->key, s->def_str);
    case BSP_PARAMS_TYPE_BLOB:
        return nvs_set_blob(s_handle, s->key, s->def_blob, s->def_blob_len);
    default:
        return write_numeric_nvs(s, s->def_num);
    }
}

// 只判断键是否存在：不关心值，也不落盘。
static esp_err_t spec_exists(const bsp_params_spec_t *s) {
    if (type_is_numeric(s->type)) {
        f64 ignored;
        return read_numeric_nvs(s, &ignored);
    }
    size_t len = 0;
    return (s->type == BSP_PARAMS_TYPE_STR)
               ? nvs_get_str(s_handle, s->key, NULL, &len)
               : nvs_get_blob(s_handle, s->key, NULL, &len);
}

esp_err_t bsp_params_apply_defaults(void) {
    BSP_PARAMS_ENSURE_READY();

    bool written = false;
    esp_err_t first_error = ESP_OK;

    spec_iter_t it = spec_iter_begin();
    for (const bsp_params_spec_t *s = spec_iter_next(&it); s != NULL; s = spec_iter_next(&it)) {
        if (!spec_has_default(s)) {
            continue;
        }
        esp_err_t rc = spec_exists(s);
        if (rc == ESP_ERR_NVS_NOT_FOUND) {
            rc = spec_write_default(s);
            written = written || (rc == ESP_OK);
        }
        if (rc != ESP_OK && first_error == ESP_OK) {
            ESP_LOGW(TAG, "default for \"%s\" failed: %s", s->key, esp_err_to_name(rc));
            first_error = rc;
        }
    }

    if (written) {
        esp_err_t rc = nvs_commit(s_handle);
        if (rc != ESP_OK) {
            return rc;
        }
    }
    return first_error;
}

esp_err_t bsp_params_reset(void) {
    BSP_PARAMS_ENSURE_READY();

    // 已汇总参数直接删掉再按默认值写回；无论原值是否存在都写默认值，
    // 这样 reset 的语义是“回到出厂值”，而不是“补上缺的”。
    spec_iter_t it = spec_iter_begin();
    for (const bsp_params_spec_t *s = spec_iter_next(&it); s != NULL; s = spec_iter_next(&it)) {
        nvs_erase_key(s_handle, s->key);
    }

    it = spec_iter_begin();
    for (const bsp_params_spec_t *s = spec_iter_next(&it); s != NULL; s = spec_iter_next(&it)) {
        if (!spec_has_default(s)) {
            continue;
        }
        esp_err_t rc = spec_write_default(s);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "reset \"%s\" failed: %s", s->key, esp_err_to_name(rc));
            return rc;
        }
    }

    return nvs_commit(s_handle);
}

esp_err_t bsp_params_get_scalar(const char *key, f64 *out) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || out == NULL);

    const bsp_params_spec_t *s = bsp_params_find(key);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return type_is_numeric(s->type) ? read_numeric_nvs(s, out) : ESP_ERR_INVALID_ARG;
}

esp_err_t bsp_params_set_scalar(const char *key, f64 value) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || !isfinite(value));

    const bsp_params_spec_t *s = bsp_params_find(key);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!type_is_numeric(s->type)) {
        return ESP_ERR_INVALID_ARG;
    }
    // min == max 表示不校验范围（见头文件约定）。
    if (s->max > s->min && (value < s->min || value > s->max)) {
        return ESP_ERR_INVALID_ARG;
    }
    // 整型参数不接受 6800.5 这种值，否则会被静默截断。
    if (s->type != BSP_PARAMS_TYPE_F32 && trunc(value) != value) {
        return ESP_ERR_INVALID_ARG;
    }

    return commit_if_ok(write_numeric_nvs(s, value));
}

/* -------------------------------------------------------------------------- */
/* 带版本和 CRC 的结构体                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    u32 magic;
    u16 version;
    u16 reserved;
    u32 length;
    u32 crc;
} bsp_params_struct_hdr_t;

_Static_assert(sizeof(bsp_params_struct_hdr_t) == 16, "struct header must be 16 bytes");

// CRC-32/ISO-HDLC（多项式 0xEDB88320，初值/终值取反）。逐位实现，只在存取结构体
// 时跑，不占表，也不依赖 ESP-IDF 版本里的 ROM 实现。
static u32 crc32_iso_hdlc(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

esp_err_t bsp_params_save_struct(const char *key, const void *data, size_t len, u16 version) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || data == NULL || len == 0);

    size_t total = sizeof(bsp_params_struct_hdr_t) + len;
    u8 *buf = malloc(total);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bsp_params_struct_hdr_t hdr = {
        .magic = BSP_PARAMS_STRUCT_MAGIC,
        .version = version,
        .reserved = 0,
        .length = (u32)len,
        .crc = crc32_iso_hdlc(data, len),
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), data, len);

    esp_err_t rc = bsp_params_set_blob(key, buf, total);
    free(buf);
    return rc;
}

esp_err_t bsp_params_load_struct(const char *key, void *data, size_t len,
                                 u16 expected_version, u16 *out_version) {
    BSP_PARAMS_ENSURE_READY();
    BSP_PARAMS_ENSURE_ARG(key == NULL || data == NULL || len == 0);

    // 先问长度，避免把调用方的缓冲区当成任意大小来写。
    size_t blob_len = 0;
    esp_err_t rc = nvs_get_blob(s_handle, key, NULL, &blob_len);
    if (rc != ESP_OK) {
        return rc;
    }
    if (blob_len < sizeof(bsp_params_struct_hdr_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    u8 *buf = malloc(blob_len);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rc = nvs_get_blob(s_handle, key, buf, &blob_len);
    if (rc != ESP_OK) {
        goto out;
    }
    if (blob_len < sizeof(bsp_params_struct_hdr_t)) {
        rc = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    bsp_params_struct_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (out_version != NULL) {
        *out_version = hdr.version;
    }
    // 版本先于长度/CRC 检查：结构体布局变了，调用方需要知道旧版本号做迁移，
    // 这时候长度必然对不上，报 INVALID_VERSION 比 INVALID_SIZE 更有用。
    if (hdr.magic != BSP_PARAMS_STRUCT_MAGIC) {
        rc = ESP_ERR_INVALID_STATE;
    } else if (hdr.version != expected_version) {
        rc = ESP_ERR_INVALID_VERSION;
    } else if (hdr.length != len || blob_len - sizeof(hdr) != len) {
        rc = ESP_ERR_INVALID_SIZE;
    } else if (hdr.crc != crc32_iso_hdlc(buf + sizeof(hdr), len)) {
        rc = ESP_ERR_INVALID_CRC;
    } else {
        memcpy(data, buf + sizeof(hdr), len);
        rc = ESP_OK;
    }

out:
    free(buf);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* 存储状态                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_params_get_stats(bsp_params_stats_t *out) {
    BSP_PARAMS_ENSURE_ARG(out == NULL);
    BSP_PARAMS_ENSURE_READY();

    nvs_stats_t stats;
    esp_err_t rc = nvs_get_stats(NULL, &stats);
    if (rc != ESP_OK) {
        return rc;
    }

    out->total_entries = (u32)stats.total_entries;
    out->used_entries = (u32)stats.used_entries;
    out->free_entries = (u32)stats.free_entries;
    out->available_entries = (u32)stats.available_entries;
    return ESP_OK;
}
