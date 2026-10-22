# 乐谱播放：把音符序列展开成蜂鸣器时序（单声部）
idf_component_register(
    SRCS "melody.c" "melody_songs.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lib
    PRIV_REQUIRES freertos
)
