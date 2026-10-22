/*
 * 乐谱 → 蜂鸣器时序。
 *
 * 频率按十二平均律从 MIDI 编号算出来，再夹到 BSP 支持的音域里；这样乐谱里只写
 * 音名和八度，不用手工维护频率表。
 */

#include "melody.h"

#include <math.h>

#include "bsp_buzzer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "melody";

// MIDI 69 = A4 = 440 Hz，60 = C4。超出 BSP 音域的音夹到边界，宁可走调也不要静音。
static u16 melody_note_freq(u8 pitch, u8 octave) {
    int midi = ((int)octave + 1) * 12 + (int)pitch;
    double hz = 440.0 * pow(2.0, (double)(midi - 69) / 12.0);
    if (hz < (double)BSP_BUZZER_FREQ_MIN_HZ) {
        return BSP_BUZZER_FREQ_MIN_HZ;
    }
    if (hz > (double)BSP_BUZZER_FREQ_MAX_HZ) {
        return BSP_BUZZER_FREQ_MAX_HZ;
    }
    return (u16)(hz + 0.5);
}

esp_err_t melody_play(const melody_note_t *notes, size_t count) {
    if (notes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; i++) {
        const melody_note_t *note = &notes[i];
        if (note->duration_ms == 0) {
            continue;
        }
        if (note->pitch != MELODY_REST && note->pitch > MELODY_B) {
            ESP_LOGW(TAG, "note %u: bad pitch %u", (unsigned)i, note->pitch);
            bsp_buzzer_stop();
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t rc = (note->pitch == MELODY_REST)
                           ? bsp_buzzer_stop()
                           : bsp_buzzer_tone(melody_note_freq(note->pitch, note->octave));
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "note %u failed: %s", (unsigned)i, esp_err_to_name(rc));
            bsp_buzzer_stop();
            return rc;
        }

        // 时长按音符给的整拍算；相邻同音会连成一声，需要断奏就在乐谱里插休止符。
        vTaskDelay(pdMS_TO_TICKS(note->duration_ms));
    }

    return bsp_buzzer_stop();
}
