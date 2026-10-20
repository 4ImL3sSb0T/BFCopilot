#include "filter.h"
#include "module/sys/sys_log.h"

/**
 * @brief 获取滤波器库版本字符串
 */
const char* filter_get_version(void)
{
    return "1.0.0";
}

/**
 * @brief 滤波器库初始化
 */
exit_code_t filter_lib_init(void)
{
    sys_log_text(info, "Filter: Initializing filter library (version=%s)...", filter_get_version());
    // 目前无需特殊初始化
    sys_log_text(info, "Filter: Filter library initialized");
    return EXIT_OK;
}
