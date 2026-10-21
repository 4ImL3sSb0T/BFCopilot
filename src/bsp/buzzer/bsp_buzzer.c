#include "bsp_buzzer.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_log.h"

#define BSP_BUZZER_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BSP_BUZZER_LEDC_TIMER      LEDC_TIMER_0
#define BSP_BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BSP_BUZZER_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define BSP_BUZZER_LEDC_DUTY_MAX   ((1u << 10) - 1u)

static const char *TAG = "bsp_buzzer";
static bool s_ready;

esp_err_t bsp_buzzer_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    // 先把引脚配成 0 占空比输出，消掉复位期间栅极浮空的不确定状态。
    ledc_timer_config_t timer_config = {
        .speed_mode = BSP_BUZZER_LEDC_MODE,
        .duty_resolution = BSP_BUZZER_LEDC_RESOLUTION,
        .timer_num = BSP_BUZZER_LEDC_TIMER,
        .freq_hz = BSP_BUZZER_FREQ_DEFAULT_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t rc = ledc_timer_config(&timer_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer config failed");
        return rc;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = BSP_BUZZER_GPIO,
        .speed_mode = BSP_BUZZER_LEDC_MODE,
        .channel = BSP_BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    rc = ledc_channel_config(&channel_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed");
        return rc;
    }

    s_ready = true;
    ESP_LOGI(TAG, "buzzer ready on GPIO%d (silent)", BSP_BUZZER_GPIO);
    return ESP_OK;
}

esp_err_t bsp_buzzer_tone(u32 freq_hz) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < BSP_BUZZER_FREQ_MIN_HZ || freq_hz > BSP_BUZZER_FREQ_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先归零再改频率：LEDC 改频时按旧占空比过渡会在喇叭上产生一声爆音。
    esp_err_t rc = ledc_set_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL, 0);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = ledc_update_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = ledc_set_freq(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_TIMER, freq_hz);
    if (rc != ESP_OK) {
        return rc;
    }
    rc = ledc_set_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL,
                       BSP_BUZZER_LEDC_DUTY_MAX / 2u);
    if (rc != ESP_OK) {
        return rc;
    }
    return ledc_update_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL);
}

esp_err_t bsp_buzzer_stop(void) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = ledc_set_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL, 0);
    if (rc != ESP_OK) {
        return rc;
    }
    return ledc_update_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL);
}
