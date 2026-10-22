#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lib/tools/common_def.h"

/*
 * NVS 参数存储封装。
 *
 * 参数表在编译期汇总：各模块把自己的 const 数组放在 flash 里，用
 * BSP_PARAMS_TABLE_GETTER 生成一个 getter，再在 bsp_params_table.c 的列表里
 * 加一行。没有固定上限、不占 RAM，也没有运行时注册。
 *
 * 能力分三层：
 *   1. 通用键值：字符串、二进制块、删除。
 *   2. 参数表：按名字读写数值、自动写默认值、一键恢复出厂值，并把整张表交给
 *      控制台打印。
 *   3. 结构化数据：把一个结构体连同版本号和 CRC 存进单个键，读取时整块校验，
 *      适合标定数据——要么完整读到，要么明确失败，不会读到半新半旧的字段。
 *
 * 约定：
 *   - 键名和命名空间都不得超过 15 个字符（NVS 上限），区分大小写。
 *   - set 成功即已落盘（内部 commit）。
 *   - 键不存在返回 ESP_ERR_NVS_NOT_FOUND。
 *   - NVS 内部有锁，本模块不再加互斥；但写入会写 flash（期间关 cache、
 *     上百微秒量级），不要放在控制循环或中断里。
 *   - 参数表只存元数据，不含任何飞行策略；某个参数该不该影响飞控，由上层决定。
 */

#define BSP_PARAMS_NAMESPACE "bfparams"
#define BSP_PARAMS_KEY_MAX_LEN 15
#define BSP_PARAMS_STRUCT_MAGIC 0x31465042u /* "BFP1" little-endian */

/// @brief 初始化 NVS 分区（分区没有空页或格式版本变化时先擦除再重试）
/// @note 全局资源，重复调用返回 ESP_OK；各模块可以各自调用，不必约定顺序。
esp_err_t bsp_nvs_init(void);

/// @brief 初始化参数存储并打开命名空间（每个 boot 调一次）
/// @note 内部先调用 bsp_nvs_init()；重复调用返回 ESP_ERR_INVALID_STATE。
esp_err_t bsp_params_init(void);

/// @brief 关闭命名空间句柄；之后读写返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_params_deinit(void);

/// @brief 命名空间是否已打开
bool bsp_params_ready(void);

/* -------------------------------------------------------------------------- */
/* 通用键值                                                                   */
/* -------------------------------------------------------------------------- */

/// @brief 存字符串（包含结尾 '\0'）
esp_err_t bsp_params_set_str(const char *key, const char *value);

/// @brief 读字符串；buf_size 是缓冲区总大小（含 '\0'）
/// @return 缓冲区不够时返回 ESP_ERR_NVS_INVALID_LENGTH
esp_err_t bsp_params_get_str(const char *key, char *buf, size_t buf_size);

/// @brief 存任意二进制块
esp_err_t bsp_params_set_blob(const char *key, const void *data, size_t len);

/// @brief 读二进制块；*len 入参是缓冲区容量，出参是实际长度
/// @note buf 传 NULL 时只回读长度，和 nvs_get_blob 的语义一致
esp_err_t bsp_params_get_blob(const char *key, void *buf, size_t *len);

/// @brief 删除一个键
/// @note 键本来就不存在时返回 ESP_OK，重复删除是安全的
esp_err_t bsp_params_erase(const char *key);

/// @brief 清空本模块命名空间下的所有键（其他命名空间不受影响）
esp_err_t bsp_params_erase_all(void);

/* -------------------------------------------------------------------------- */
/* 参数表                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    BSP_PARAMS_TYPE_U8 = 0,
    BSP_PARAMS_TYPE_I8,
    BSP_PARAMS_TYPE_U16,
    BSP_PARAMS_TYPE_I16,
    BSP_PARAMS_TYPE_U32,
    BSP_PARAMS_TYPE_I32,
    BSP_PARAMS_TYPE_U64,
    BSP_PARAMS_TYPE_I64,
    BSP_PARAMS_TYPE_F32,
    BSP_PARAMS_TYPE_STR,
    BSP_PARAMS_TYPE_BLOB,
} bsp_params_type_t;

/*
 * 参数元数据。
 *
 *   - 数值类型用 min/max/def_num；min == max 表示不做范围校验。
 *   - STR 用 def_str，BLOB 用 def_blob/def_blob_len；默认值为 NULL 表示
 *     “这个参数没有默认值”，apply_defaults 时跳过。
 *   - key / unit / def_str / def_blob 必须指向静态存储，链接后一直有效。
 */
typedef struct {
    const char *key;
    bsp_params_type_t type;
    const char *unit; /* 可为 NULL，仅用于展示 */
    f64 min;
    f64 max;
    f64 def_num;
    const char *def_str;
    const void *def_blob;
    size_t def_blob_len;
} bsp_params_spec_t;

/// @brief 模块参数表的 getter：返回数组首地址，并通过 count 回填元素个数
typedef const bsp_params_spec_t *(*bsp_params_table_getter_t)(size_t *count);

/// @brief 为模块自己的 const 参数数组生成 getter
/// @note 只能传数组名，不能传指针；写在数组定义之后
#define BSP_PARAMS_TABLE_GETTER(getter_name, array_name)                        \
    const bsp_params_spec_t *getter_name(size_t *count) {                       \
        *count = sizeof(array_name) / sizeof((array_name)[0]);                  \
        return array_name;                                                      \
    }

/// @brief 已汇总的参数个数（遍历所有模块的表）
size_t bsp_params_count(void);

/// @brief 按序号取元数据，越界返回 NULL（控制台列表用）
const bsp_params_spec_t *bsp_params_spec(size_t index);

/// @brief 按名字取元数据，未定义返回 NULL
const bsp_params_spec_t *bsp_params_find(const char *key);

/// @brief 给所有“键不存在”的已定义参数写入默认值
/// @note 已存在的键一律不覆盖；没有默认值的参数跳过
esp_err_t bsp_params_apply_defaults(void);

/// @brief 删除所有已定义参数并重新写入默认值（出厂恢复）
esp_err_t bsp_params_reset(void);

/// @brief 按名字读取数值参数（只对已定义的数值类型有效）
/// @note u64/i64 超过 2^53 时 f64 无法精确表示，这类值请改用结构化接口
esp_err_t bsp_params_get_scalar(const char *key, f64 *out);

/// @brief 按名字写入数值参数：范围校验 + 整型不得带小数 + 立即落盘
/// @return 未定义返回 ESP_ERR_NOT_FOUND；越界或类型不符返回 ESP_ERR_INVALID_ARG
esp_err_t bsp_params_set_scalar(const char *key, f64 value);

/* -------------------------------------------------------------------------- */
/* 带版本和 CRC 的结构体                                                       */
/* -------------------------------------------------------------------------- */

/*
 * 存储格式：[16 字节头][结构体原始字节]
 *   头里带 magic、版本号、长度和 payload 的 CRC32(ISO-HDLC)。
 *
 * 读取时四项全部校验，任何一项不符都不会往调用方缓冲区写数据，因此不会出现
 * “结构体改过字段后读到旧记录、部分字段是垃圾”的情况。结构体布局变化时递增
 * version，由调用方决定是迁移还是回默认值。
 */

/// @brief 保存结构体到单个键（整块一次写入）
esp_err_t bsp_params_save_struct(const char *key, const void *data, size_t len, u16 version);

/// @brief 读取结构体
/// @param expected_version 期望的版本号
/// @param out_version      可为 NULL；成功后（或版本不匹配时）回读实际版本号
/// @return 键不存在 ESP_ERR_NVS_NOT_FOUND；magic 不符 ESP_ERR_INVALID_STATE；
///         版本不符 ESP_ERR_INVALID_VERSION；长度不符 ESP_ERR_INVALID_SIZE；
///         CRC 不符 ESP_ERR_INVALID_CRC。data 只在 ESP_OK 时被写入。
esp_err_t bsp_params_load_struct(const char *key, void *data, size_t len,
                                 u16 expected_version, u16 *out_version);

/* -------------------------------------------------------------------------- */
/* 存储状态                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    u32 total_entries;     /* NVS 分区总条目数 */
    u32 used_entries;      /* 已用条目数 */
    u32 free_entries;      /* 空闲条目数 */
    u32 available_entries; /* 还能写入的条目数（含可回收页） */
} bsp_params_stats_t;

/// @brief 查询 NVS 分区用量（整个分区，不只是本命名空间）
esp_err_t bsp_params_get_stats(bsp_params_stats_t *out);
