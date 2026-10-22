/*
 * 双路舵机（MCPWM）。
 * 50 Hz、1 µs 分辨率，比较值直接就是高电平宽度（µs）。
 * 上电和断电都以"先停波形、再动电源"为准，避免开关瞬间的窄脉冲顶到舵机。
 */

#include "bsp_servo.h"

#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"

#define BSP_SERVO_MCPWM_GROUP       0
#define BSP_SERVO_TIMER_RESOLUTION  1000000   // 1 MHz → 1 tick = 1 µs
#define BSP_SERVO_TIMER_PERIOD      BSP_SERVO_PERIOD_US

static const char *TAG = "bsp_servo";

static const int s_gpio[BSP_SERVO_COUNT] = {
    [BSP_SERVO_LEFT] = BSP_SERVO_LEFT_GPIO,
    [BSP_SERVO_RIGHT] = BSP_SERVO_RIGHT_GPIO,
};

static mcpwm_timer_handle_t s_timer;
static mcpwm_oper_handle_t s_operator;
static mcpwm_cmpr_handle_t s_comparator[BSP_SERVO_COUNT];
static mcpwm_gen_handle_t s_generator[BSP_SERVO_COUNT];
static bool s_power_enabled;
static bool s_ready;

static bool bsp_servo_valid(BSP_SERVO_CH ch) {
    return ch >= 0 && ch < BSP_SERVO_COUNT;
}

static esp_err_t bsp_servo_force_idle(BSP_SERVO_CH ch) {
    // hold_on = true：强制电平会一直保持，直到显式用 -1 解除
    return mcpwm_generator_set_force_level(s_generator[ch], 0, true);
}

static void bsp_servo_release(bool timer_enabled) {
    for (int ch = 0; ch < BSP_SERVO_COUNT; ch++) {
        if (s_generator[ch] != NULL) {
            mcpwm_del_generator(s_generator[ch]);
            s_generator[ch] = NULL;
        }
        if (s_comparator[ch] != NULL) {
            mcpwm_del_comparator(s_comparator[ch]);
            s_comparator[ch] = NULL;
        }
    }
    if (s_operator != NULL) {
        mcpwm_del_operator(s_operator);
        s_operator = NULL;
    }
    if (s_timer != NULL) {
        if (timer_enabled) {
            mcpwm_timer_disable(s_timer);
        }
        mcpwm_del_timer(s_timer);
        s_timer = NULL;
    }
    s_power_enabled = false;
    s_ready = false;
}

esp_err_t bsp_servo_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    bool timer_enabled = false;

    // 第一件事就是把舵机电源关掉：SERVO_EN 悬空时 Q3 状态不确定，
    // 这里把它明确拉低，确保上电后舵机不带电。
    gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << BSP_SERVO_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t rc = gpio_config(&power_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "servo power pin config failed");
        return rc;
    }
    gpio_set_level(BSP_SERVO_POWER_GPIO, 0);
    s_power_enabled = false;

    mcpwm_timer_config_t timer_config = {
        .group_id = BSP_SERVO_MCPWM_GROUP,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = BSP_SERVO_TIMER_RESOLUTION,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = BSP_SERVO_TIMER_PERIOD,
    };
    rc = mcpwm_new_timer(&timer_config, &s_timer);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm timer alloc failed");
        goto fail;
    }
    rc = mcpwm_timer_enable(s_timer);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm timer enable failed");
        goto fail;
    }
    timer_enabled = true;

    mcpwm_operator_config_t operator_config = {
        .group_id = BSP_SERVO_MCPWM_GROUP,
    };
    rc = mcpwm_new_operator(&operator_config, &s_operator);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm operator alloc failed");
        goto fail;
    }
    rc = mcpwm_operator_connect_timer(s_operator, s_timer);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm operator connect failed");
        goto fail;
    }

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    for (int ch = 0; ch < BSP_SERVO_COUNT; ch++) {
        rc = mcpwm_new_comparator(s_operator, &comparator_config, &s_comparator[ch]);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "mcpwm comparator %d alloc failed", ch);
            goto fail;
        }
        // 比较值先放在中位；输出仍是强制低电平，所以不会出脉冲
        rc = mcpwm_comparator_set_compare_value(
            s_comparator[ch], BSP_SERVO_PULSE_NEUTRAL_US);
        if (rc != ESP_OK) {
            goto fail;
        }

        mcpwm_generator_config_t generator_config = {
            .gen_gpio_num = s_gpio[ch],
        };
        rc = mcpwm_new_generator(s_operator, &generator_config, &s_generator[ch]);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "mcpwm generator %d alloc failed", ch);
            goto fail;
        }
        // 周期起点拉高、比较值处拉低：高电平宽度 = 比较值
        rc = mcpwm_generator_set_action_on_timer_event(
            s_generator[ch],
            MCPWM_GEN_TIMER_EVENT_ACTION(
                MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
        if (rc != ESP_OK) {
            goto fail;
        }
        rc = mcpwm_generator_set_action_on_compare_event(
            s_generator[ch],
            MCPWM_GEN_COMPARE_EVENT_ACTION(
                MCPWM_TIMER_DIRECTION_UP, s_comparator[ch], MCPWM_GEN_ACTION_LOW));
        if (rc != ESP_OK) {
            goto fail;
        }
        rc = bsp_servo_force_idle((BSP_SERVO_CH)ch);
        if (rc != ESP_OK) {
            goto fail;
        }
    }

    rc = mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm timer start failed");
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "servo ready: left=GPIO%d right=GPIO%d, 50 Hz, outputs idle, power off",
             BSP_SERVO_LEFT_GPIO, BSP_SERVO_RIGHT_GPIO);
    return ESP_OK;

fail:
    bsp_servo_release(timer_enabled);
    return rc;
}

esp_err_t bsp_servo_set_pulse_us(BSP_SERVO_CH ch, u16 pulse_us) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_servo_valid(ch)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 端点未标定：这里只做绝对安全限位，真实行程由上层决定
    if (pulse_us < BSP_SERVO_PULSE_MIN_US) {
        pulse_us = BSP_SERVO_PULSE_MIN_US;
    } else if (pulse_us > BSP_SERVO_PULSE_MAX_US) {
        pulse_us = BSP_SERVO_PULSE_MAX_US;
    }
    return mcpwm_comparator_set_compare_value(s_comparator[ch], pulse_us);
}

esp_err_t bsp_servo_output_enable(BSP_SERVO_CH ch, bool enable) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_servo_valid(ch)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!enable) {
        return bsp_servo_force_idle(ch);
    }
    // -1 表示解除强制电平，交还给定时器/比较器事件
    return mcpwm_generator_set_force_level(s_generator[ch], -1, true);
}

esp_err_t bsp_servo_power_enable(bool enable) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enable) {
        // 断电前先把波形停掉，避免开关瞬间的窄脉冲顶到舵机
        for (int ch = 0; ch < BSP_SERVO_COUNT; ch++) {
            const esp_err_t rc = bsp_servo_force_idle((BSP_SERVO_CH)ch);
            if (rc != ESP_OK) {
                return rc;
            }
        }
    }

    gpio_set_level(BSP_SERVO_POWER_GPIO, enable ? 1 : 0);
    s_power_enabled = enable;
    return ESP_OK;
}

bool bsp_servo_power_enabled(void) {
    return s_power_enabled;
}
