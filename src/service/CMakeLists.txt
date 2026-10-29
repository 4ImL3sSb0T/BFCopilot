idf_component_register(
    SRCS
        "control/control.c"
        "melody/melody.c"
        "melody/melody_songs.c"
    # 让使用方可以按参考项目的写法 #include "service/melody/melody.h"
    INCLUDE_DIRS ".."
    REQUIRES bsp lib
)
