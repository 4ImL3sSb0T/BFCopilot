#pragma once

#include <stddef.h>

#include "bsp_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 各模块参数表的汇总点。
 *
 * 表本身是 const 数组，链接后直接留在 flash：没有数量上限，也不复制到 RAM，
 * 更没有运行时注册。新增模块时只需在 bsp_params_table.c 的列表里加一行 getter。
 */
const bsp_params_table_getter_t *bsp_params_tables(size_t *count);

#ifdef __cplusplus
}
#endif
