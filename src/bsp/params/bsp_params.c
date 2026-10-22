#include "bsp_params.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_params_table.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bsp_params";

static nvs_handle_t s_handle;
static bool s_ready;

/* -------------------------------------------------------------------------- */
/* 初始化与命名空间                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_params_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    // NVS 是全局资源：bsp_wifi_init() 也会调用 nvs_flash_init()，后调用的那个
    // 拿到 ESP_ERR_INVALID_STATE 属于正常情况，不能当失败。
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase: %s", esp_err_to_name(rc));
        rc = nvs_flash_erase();
        if (rc == ESP_OK) {
            rc = nvs_flash_init();
        }
    }
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
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
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
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

esp_err_t bsp_params_set_str(const char *key, const char *value) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = nvs_set_str(s_handle, key, value);
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

esp_err_t bsp_params_get_str(const char *key, char *buf, size_t buf_size) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';
    return nvs_get_str(s_handle, key, buf, &buf_size);
}

esp_err_t bsp_params_set_blob(const char *key, const void *data, size_t len) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = nvs_set_blob(s_handle, key, data, len);
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

esp_err_t bsp_params_get_blob(const char *key, void *buf, size_t *len) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_get_blob(s_handle, key, buf, len);
}

esp_err_t bsp_params_erase(const char *key) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = nvs_erase_key(s_handle, key);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        // 键本来就不存在，目标状态已经达成，不当作错误。
        return ESP_OK;
    }
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

esp_err_t bsp_params_erase_all(void) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = nvs_erase_all(s_handle);
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

/* -------------------------------------------------------------------------- */
/* 参数表                                                                     */
/* -------------------------------------------------------------------------- */

static bool type_is_numeric(bsp_params_type_t type) {
    return type >= BSP_PARAMS_TYPE_U8 && type <= BSP_PARAMS_TYPE_F32;
}

const bsp_params_spec_t *bsp_params_find(const char *key) {
    if (key == NULL) {
        return NULL;
    }

    size_t table_count = 0;
    const bsp_params_table_getter_t *getters = bsp_params_tables(&table_count);
    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        const bsp_params_spec_t *specs = getters[t](&count);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(specs[i].key, key) == 0) {
                return &specs[i];
            }
        }
    }
    return NULL;
}

size_t bsp_params_count(void) {
    size_t table_count = 0;
    const bsp_params_table_getter_t *getters = bsp_params_tables(&table_count);

    size_t total = 0;
    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        getters[t](&count);
        total += count;
    }
    return total;
}

const bsp_params_spec_t *bsp_params_spec(size_t index) {
    size_t table_count = 0;
    const bsp_params_table_getter_t *getters = bsp_params_tables(&table_count);

    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        const bsp_params_spec_t *specs = getters[t](&count);
        if (index < count) {
            return &specs[index];
        }
        index -= count;
    }
    return NULL;
}

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
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_U8, u8, uint8_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_I8, i8, int8_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_U16, u16, uint16_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_I16, i16, int16_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_U32, u32, uint32_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_I32, i32, int32_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_U64, u64, uint64_t)
        BSP_PARAMS_READ_CASE(BSP_PARAMS_TYPE_I64, i64, int64_t)
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
#undef BSP_PARAMS_READ_CASE
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t write_numeric_nvs(const bsp_params_spec_t *spec, f64 value) {
    switch (spec->type) {
#define BSP_PARAMS_WRITE_CASE(type_enum, suffix, ctype)                             \
    case type_enum:                                                                 \
        return nvs_set_##suffix(s_handle, spec->key, (ctype)value);
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_U8, u8, uint8_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_I8, i8, int8_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_U16, u16, uint16_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_I16, i16, int16_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_U32, u32, uint32_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_I32, i32, int32_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_U64, u64, uint64_t)
        BSP_PARAMS_WRITE_CASE(BSP_PARAMS_TYPE_I64, i64, int64_t)
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

esp_err_t bsp_params_apply_defaults(void) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    bool written = false;
    esp_err_t first_error = ESP_OK;

    size_t table_count = 0;
    const bsp_params_table_getter_t *getters = bsp_params_tables(&table_count);
    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        const bsp_params_spec_t *specs = getters[t](&count);
        for (size_t i = 0; i < count; i++) {
            const bsp_params_spec_t *s = &specs[i];
            esp_err_t rc;

            if (type_is_numeric(s->type)) {
                f64 dummy;
                rc = read_numeric_nvs(s, &dummy);
                if (rc == ESP_ERR_NVS_NOT_FOUND) {
                    rc = write_numeric_nvs(s, s->def_num);
                    if (rc == ESP_OK) {
                        written = true;
                    }
                }
            } else if (s->type == BSP_PARAMS_TYPE_STR) {
                if (s->def_str == NULL) {
                    continue;
                }
                size_t len = 0;
                rc = nvs_get_str(s_handle, s->key, NULL, &len);
                if (rc == ESP_ERR_NVS_NOT_FOUND) {
                    rc = nvs_set_str(s_handle, s->key, s->def_str);
                    if (rc == ESP_OK) {
                        written = true;
                    }
                }
            } else { /* BSP_PARAMS_TYPE_BLOB */
                if (s->def_blob == NULL || s->def_blob_len == 0) {
                    continue;
                }
                size_t len = 0;
                rc = nvs_get_blob(s_handle, s->key, NULL, &len);
                if (rc == ESP_ERR_NVS_NOT_FOUND) {
                    rc = nvs_set_blob(s_handle, s->key, s->def_blob, s->def_blob_len);
                    if (rc == ESP_OK) {
                        written = true;
                    }
                }
            }

            if (rc != ESP_OK && first_error == ESP_OK) {
                ESP_LOGW(TAG, "default for \"%s\" failed: %s", s->key, esp_err_to_name(rc));
                first_error = rc;
            }
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
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t table_count = 0;
    const bsp_params_table_getter_t *getters = bsp_params_tables(&table_count);

    // 已汇总参数直接删掉再按默认值写回；无论原值是否存在都写默认值，
    // 这样 reset 的语义是“回到出厂值”，而不是“补上缺的”。
    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        const bsp_params_spec_t *specs = getters[t](&count);
        for (size_t i = 0; i < count; i++) {
            nvs_erase_key(s_handle, specs[i].key);
        }
    }

    for (size_t t = 0; t < table_count; t++) {
        size_t count = 0;
        const bsp_params_spec_t *specs = getters[t](&count);
        for (size_t i = 0; i < count; i++) {
            const bsp_params_spec_t *s = &specs[i];
            esp_err_t rc;
            if (type_is_numeric(s->type)) {
                rc = write_numeric_nvs(s, s->def_num);
            } else if (s->type == BSP_PARAMS_TYPE_STR) {
                rc = (s->def_str != NULL) ? nvs_set_str(s_handle, s->key, s->def_str) : ESP_OK;
            } else {
                rc = (s->def_blob != NULL && s->def_blob_len > 0)
                         ? nvs_set_blob(s_handle, s->key, s->def_blob, s->def_blob_len)
                         : ESP_OK;
            }
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "reset \"%s\" failed: %s", s->key, esp_err_to_name(rc));
                return rc;
            }
        }
    }

    return nvs_commit(s_handle);
}

esp_err_t bsp_params_get_scalar(const char *key, f64 *out) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const bsp_params_spec_t *s = bsp_params_find(key);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!type_is_numeric(s->type)) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_numeric_nvs(s, out);
}

esp_err_t bsp_params_set_scalar(const char *key, f64 value) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || !isfinite(value)) {
        return ESP_ERR_INVALID_ARG;
    }

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

    esp_err_t rc = write_numeric_nvs(s, value);
    return (rc == ESP_OK) ? nvs_commit(s_handle) : rc;
}

/* -------------------------------------------------------------------------- */
/* 第三层：带版本和 CRC 的结构体                                              */
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
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

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
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先问长度，避免把调用方的缓冲区当成任意大小来写。
    size_t blob_len = 0;
    esp_err_t rc = nvs_get_blob(s_handle, key, NULL, &blob_len);
    if (rc != ESP_OK) {
        return rc;
    }
    if (blob_len != sizeof(bsp_params_struct_hdr_t) + len) {
        return ESP_ERR_INVALID_SIZE;
    }

    u8 *buf = malloc(blob_len);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rc = nvs_get_blob(s_handle, key, buf, &blob_len);
    if (rc != ESP_OK) {
        free(buf);
        return rc;
    }

    bsp_params_struct_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (out_version != NULL) {
        *out_version = hdr.version;
    }
    if (hdr.magic != BSP_PARAMS_STRUCT_MAGIC) {
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    // 版本先于长度/CRC 检查：结构体布局变了，调用方需要知道旧版本号做迁移，
    // 这时候长度必然对不上，报 INVALID_VERSION 比 INVALID_SIZE 更有用。
    if (hdr.version != expected_version) {
        free(buf);
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr.length != len) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    if (hdr.crc != crc32_iso_hdlc(buf + sizeof(hdr), len)) {
        free(buf);
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(data, buf + sizeof(hdr), len);
    free(buf);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* 存储状态                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_params_get_stats(bsp_params_stats_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

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

