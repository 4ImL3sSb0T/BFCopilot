#pragma once

#include <stddef.h>

#include "melody.h"

/*
 * 内置曲库，全部是公有领域的传统曲目，只取主旋律第一段，用来做蜂鸣器自检。
 * 音名按 C 大调写在 C5 附近：蜂鸣器在 500 Hz–2 kHz 一带最响，低于 C4 效率明显变差。
 */

typedef struct {
    const char *name;
    const melody_note_t *notes;
    size_t count;
} melody_song_t;

extern const melody_song_t melody_songs[];
extern const size_t melody_song_count;
