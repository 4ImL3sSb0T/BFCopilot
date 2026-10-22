#pragma once

#include <stddef.h>

#include "lib/tools/common_def.h"
#include "esp_err.h"

/*
 * 乐谱播放：把音符序列展开成 bsp_buzzer_tone() / bsp_buzzer_stop() 的时序。
 *
 * 只支持单声部（同时只能响一个音），音色取决于蜂鸣器本身：无源蜂鸣器是方波，
 * 有源蜂鸣器无论给什么频率都按自身谐振响，旋律听不出来。
 *
 * melody_play() 是阻塞的，每个音符靠 vTaskDelay 撑时长。放进主循环会把采样
 * 节奏一起拖慢，调用方应自己开 task 播放。
 */

/// @brief 音名，按十二平均律编号；休止符用 MELODY_REST
#define MELODY_C  0u
#define MELODY_CS 1u
#define MELODY_D  2u
#define MELODY_DS 3u
#define MELODY_E  4u
#define MELODY_F  5u
#define MELODY_FS 6u
#define MELODY_G  7u
#define MELODY_GS 8u
#define MELODY_A  9u
#define MELODY_AS 10u
#define MELODY_B  11u
#define MELODY_REST 0xFFu

typedef struct {
    u8 pitch;       ///< MELODY_C..MELODY_B，或 MELODY_REST
    u8 octave;      ///< 八度，C4 的中央 C 是 octave=4；休止时忽略
    u16 duration_ms;
} melody_note_t;

/// @brief 按顺序播放整段乐谱，返回时蜂鸣器已静音
/// @param notes 音符数组；pitch 越界或音符发声失败都会记日志并中止
/// @param count 音符个数
esp_err_t melody_play(const melody_note_t *notes, size_t count);
