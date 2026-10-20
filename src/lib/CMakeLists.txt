idf_component_register(
    SRCS
        "tools/common_def.c"
        "tools/vec_math.c"
    # 让使用方可以按参考项目的写法 #include "lib/tools/xxx.h"
    INCLUDE_DIRS ".."
)
