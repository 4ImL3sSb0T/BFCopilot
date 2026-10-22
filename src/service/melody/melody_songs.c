/*
 * 内置曲库。每首只保留主旋律第一段：蜂鸣器是单声部方波，长曲子听感会很单调。
 */

#include "melody_songs.h"

#define SONG_LEN(song) (sizeof(song) / sizeof((song)[0]))

/* 一闪一闪亮晶晶：C C G G A A G | F F E E D D C */
static const melody_note_t SONG_TWINKLE[] = {
    {MELODY_C, 5, 300}, {MELODY_C, 5, 300}, {MELODY_G, 5, 300}, {MELODY_G, 5, 300},
    {MELODY_A, 5, 300}, {MELODY_A, 5, 300}, {MELODY_G, 5, 600},
    {MELODY_F, 5, 300}, {MELODY_F, 5, 300}, {MELODY_E, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_D, 5, 300}, {MELODY_D, 5, 300}, {MELODY_C, 5, 600},
};

/* 两只老虎：1 2 3 1 | 1 2 3 1 | 3 4 5 | 3 4 5 | 5 6 5 4 3 1 | 5 6 5 4 3 1 | 1 5 1 | 1 5 1 */
static const melody_note_t SONG_TWO_TIGERS[] = {
    {MELODY_C, 5, 300}, {MELODY_D, 5, 300}, {MELODY_E, 5, 300}, {MELODY_C, 5, 300},
    {MELODY_C, 5, 300}, {MELODY_D, 5, 300}, {MELODY_E, 5, 300}, {MELODY_C, 5, 300},
    {MELODY_E, 5, 300}, {MELODY_F, 5, 300}, {MELODY_G, 5, 600},
    {MELODY_E, 5, 300}, {MELODY_F, 5, 300}, {MELODY_G, 5, 600},
    {MELODY_G, 5, 300}, {MELODY_A, 5, 300}, {MELODY_G, 5, 300}, {MELODY_F, 5, 300},
    {MELODY_E, 5, 300}, {MELODY_C, 5, 300},
    {MELODY_G, 5, 300}, {MELODY_A, 5, 300}, {MELODY_G, 5, 300}, {MELODY_F, 5, 300},
    {MELODY_E, 5, 300}, {MELODY_C, 5, 300},
    {MELODY_C, 5, 300}, {MELODY_G, 4, 300}, {MELODY_C, 5, 600},
    {MELODY_C, 5, 300}, {MELODY_G, 4, 300}, {MELODY_C, 5, 600},
};

/* 欢乐颂：3 3 4 5 | 5 4 3 2 | 1 1 2 3 | 3 2 2，再来一遍收在 2 1 1 */
static const melody_note_t SONG_ODE_TO_JOY[] = {
    {MELODY_E, 5, 300}, {MELODY_E, 5, 300}, {MELODY_F, 5, 300}, {MELODY_G, 5, 300},
    {MELODY_G, 5, 300}, {MELODY_F, 5, 300}, {MELODY_E, 5, 300}, {MELODY_D, 5, 300},
    {MELODY_C, 5, 300}, {MELODY_C, 5, 300}, {MELODY_D, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_E, 5, 400}, {MELODY_D, 5, 200}, {MELODY_D, 5, 600},
    {MELODY_E, 5, 300}, {MELODY_E, 5, 300}, {MELODY_F, 5, 300}, {MELODY_G, 5, 300},
    {MELODY_G, 5, 300}, {MELODY_F, 5, 300}, {MELODY_E, 5, 300}, {MELODY_D, 5, 300},
    {MELODY_C, 5, 300}, {MELODY_C, 5, 300}, {MELODY_D, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_D, 5, 400}, {MELODY_C, 5, 200}, {MELODY_C, 5, 600},
};

/* 铃儿响叮当副歌：E E E | E E E | E G C D E | F F F F | F E E E | E D D E | D G */
static const melody_note_t SONG_JINGLE_BELLS[] = {
    {MELODY_E, 5, 300}, {MELODY_E, 5, 300}, {MELODY_E, 5, 600},
    {MELODY_E, 5, 300}, {MELODY_E, 5, 300}, {MELODY_E, 5, 600},
    {MELODY_E, 5, 300}, {MELODY_G, 5, 300}, {MELODY_C, 5, 300}, {MELODY_D, 5, 300},
    {MELODY_E, 5, 600},
    {MELODY_F, 5, 300}, {MELODY_F, 5, 300}, {MELODY_F, 5, 300}, {MELODY_F, 5, 300},
    {MELODY_F, 5, 300}, {MELODY_E, 5, 300}, {MELODY_E, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_E, 5, 300}, {MELODY_D, 5, 300}, {MELODY_D, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_D, 5, 300}, {MELODY_G, 5, 600},
};

/* 生日快乐：G G A G C B | G G A G D C | G G G' E C B A | F F E C D C */
static const melody_note_t SONG_HAPPY_BIRTHDAY[] = {
    {MELODY_G, 4, 300}, {MELODY_G, 4, 300}, {MELODY_A, 4, 300}, {MELODY_G, 4, 300},
    {MELODY_C, 5, 300}, {MELODY_B, 4, 600},
    {MELODY_G, 4, 300}, {MELODY_G, 4, 300}, {MELODY_A, 4, 300}, {MELODY_G, 4, 300},
    {MELODY_D, 5, 300}, {MELODY_C, 5, 600},
    {MELODY_G, 4, 300}, {MELODY_G, 4, 300}, {MELODY_G, 5, 300}, {MELODY_E, 5, 300},
    {MELODY_C, 5, 300}, {MELODY_B, 4, 300}, {MELODY_A, 4, 600},
    {MELODY_F, 5, 300}, {MELODY_F, 5, 300}, {MELODY_E, 5, 300}, {MELODY_C, 5, 300},
    {MELODY_D, 5, 300}, {MELODY_C, 5, 600},
};

const melody_song_t melody_songs[] = {
    {"小星星", SONG_TWINKLE, SONG_LEN(SONG_TWINKLE)},
    {"两只老虎", SONG_TWO_TIGERS, SONG_LEN(SONG_TWO_TIGERS)},
    {"欢乐颂", SONG_ODE_TO_JOY, SONG_LEN(SONG_ODE_TO_JOY)},
    {"铃儿响叮当", SONG_JINGLE_BELLS, SONG_LEN(SONG_JINGLE_BELLS)},
    {"生日快乐", SONG_HAPPY_BIRTHDAY, SONG_LEN(SONG_HAPPY_BIRTHDAY)},
};

const size_t melody_song_count = SONG_LEN(melody_songs);
