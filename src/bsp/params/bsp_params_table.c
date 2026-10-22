#include "bsp_params_table.h"

/*
 * 参数表汇总。
 *
 * 新增一个模块的参数表：
 *   1. 模块里定义 static const bsp_params_spec_t 数组；
 *   2. 数组后面写 BSP_PARAMS_TABLE_GETTER(模块_get, 数组名)；
 *   3. 在这里的 s_tables 里加一行 getter。
 *
 * 汇总表本身也是 const，链接后只占 flash；参数数组不会被复制，也没有容量上限。
 */
static const bsp_params_table_getter_t s_tables[] = {
    /* 例：bsp_servo_params_get, */
};

const bsp_params_table_getter_t *bsp_params_tables(size_t *count) {
    *count = sizeof(s_tables) / sizeof(s_tables[0]);
    return s_tables;
}
