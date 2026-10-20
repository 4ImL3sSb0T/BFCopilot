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
static mcpwm_cmpr_handle_t s_comparator[BSP_SERVO_COUNT];
static mcpwm_gen_handle_t s_generator[BSP_SERVO_COUNT];
static bool s_power_enabled;
static bool s_ready;

static exit_code_t bsp_servo_status(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return EXIT_OK;
    case ESP_ERR_INVALID_ARG:
        return EXIT_INVALID_PARAM;
    case ESP_ERR_INVALID_STATE:
        return EXIT_NOT_INITIALIZED;
    case ESP_ERR_NO_MEM:
        return EXIT_NO_MEMORY;
    case ESP_ERR_NOT_FOUND:
        return EXIT_NO_RESOURCE;
    case ESP_ERR_NOT_SUPPORTED:
        return EXIT_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT:
        return EXIT_TIMEOUT;
    default:
        return EXIT_HW_FAILURE;
    }
}

static bool bsp_servo_valid(BSP_SERVO_CH ch) {
    return ch >= 0 && ch < BSP_SERVO_COUNT;
}

static exit_code_t bsp_servo_force_idle(BSP_SERVO_CH ch) {
    // hold_on = true：强制电平会一直保持，直到显式用 -1 解除
    return bsp_servo_status(
        mcpwm_generator_set_force_level(s_generator[ch], 0, true));
}

exit_code_t bsp_servo_init(void) {
    if (s_ready) {
        return EXIT_ALREADY_INITIALIZED;
    }

    // 第一件事就是把舵机电源关掉：SERVO_EN 悬空时 Q3 状态不确定，
    // 这里把它明确拉低，确保上电后舵机不带电。
    gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << BSP_SERVO_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    exit_code_t rc = bsp_servo_status(gpio_config(&power_config));
    if (rc != EXIT_OK) {
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
    rc = bsp_servo_status(mcpwm_new_timer(&timer_config, &s_timer));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "mcpwm timer alloc failed");
        return rc;
    }
    rc = bsp_servo_status(mcpwm_timer_enable(s_timer));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "mcpwm timer enable failed");
        return rc;
    }

    mcpwm_operator_config_t operator_config = {
        .group_id = BSP_SERVO_MCPWM_GROUP,
    };
    mcpwm_oper_handle_t oper = NULL;
    rc = bsp_servo_status(mcpwm_new_operator(&operator_config, &oper));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "mcpwm operator alloc failed");
        return rc;
    }
    rc = bsp_servo_status(mcpwm_operator_connect_timer(oper, s_timer));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "mcpwm operator connect failed");
        return rc;
    }

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    for (int ch = 0; ch < BSP_SERVO_COUNT; ch++) {
        rc = bsp_servo_status(
            mcpwm_new_comparator(oper, &comparator_config, &s_comparator[ch]));
        if (rc != EXIT_OK) {
            ESP_LOGE(TAG, "mcpwm comparator %d alloc failed", ch);
            return rc;
        }
        // 比较值先放在中位；输出仍是强制低电平，所以不会出脉冲
        rc = bsp_servo_status(mcpwm_comparator_set_compare_value(
            s_comparator[ch], BSP_SERVO_PULSE_NEUTRAL_US));
        if (rc != EXIT_OK) {
            return rc;
        }

        mcpwm_generator_config_t generator_config = {
            .gen_gpio_num = s_gpio[ch],
        };
        rc = bsp_servo_status(
            mcpwm_new_generator(oper, &generator_config, &s_generator[ch]));
        if (rc != EXIT_OK) {
            ESP_LOGE(TAG, "mcpwm generator %d alloc failed", ch);
            return rc;
        }
        // 周期起点拉高、比较值处拉低：高电平宽度 = 比较值
        rc = bsp_servo_status(mcpwm_generator_set_action_on_timer_event(
            s_generator[ch],
            MCPWM_GEN_TIMER_EVENT_ACTION(
                MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        if (rc != EXIT_OK) {
            return rc;
        }
        rc = bsp_servo_status(mcpwm_generator_set_action_on_compare_event(
            s_generator[ch],
            MCPWM_GEN_COMPARE_EVENT_ACTION(
                MCPWM_TIMER_DIRECTION_UP, s_comparator[ch], MCPWM_GEN_ACTION_LOW)));
        if (rc != EXIT_OK) {
            return rc;
        }
        rc = bsp_servo_force_idle((BSP_SERVO_CH)ch);
        if (rc != EXIT_OK) {
            return rc;
        }
    }

    rc = bsp_servo_status(
        mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "mcpwm timer start failed");
        return rc;
    }

    s_ready = true;
    ESP_LOGI(TAG, "servo ready: left=GPIO%d right=GPIO%d, 50 Hz, outputs idle, power off",
             BSP_SERVO_LEFT_GPIO, BSP_SERVO_RIGHT_GPIO);
    return EXIT_OK;
}

exit_code_t bsp_servo_set_pulse_us(BSP_SERVO_CH ch, u16 pulse_us) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (!bsp_servo_valid(ch)) {
        return EXIT_INVALID_PARAM;
    }

    if (pulse_us < BSP_SERVO_PULSE_MIN_US) {
        pulse_us = BSP_SERVO_PULSE_MIN_US;
    } else if (pulse_us > BSP_SERVO_PULSE_MAX_US) {
        pulse_us = BSP_SERVO_PULSE_MAX_US;
    }
    return bsp_servo_status(
        mcpwm_comparator_set_compare_value(s_comparator[ch], pulse_us));
}

exit_code_t bsp_servo_output_enable(BSP_SERVO_CH ch, bool enable) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (!bsp_servo_valid(ch)) {
        return EXIT_INVALID_PARAM;
    }

    if (!enable) {
        return bsp_servo_force_idle(ch);
    }
    // -1 表示解除强制电平，交还给定时器/比较器事件
    return bsp_servo_status(
        mcpwm_generator_set_force_level(s_generator[ch], -1, false));
}

exit_code_t bsp_servo_power_enable(bool enable) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }

    if (!enable) {
        // 断电前先把波形停掉，避免开关瞬间的窄脉冲顶到舵机
        for (int ch = 0; ch < BSP_SERVO_COUNT; ch++) {
            exit_code_t rc = bsp_servo_force_idle((BSP_SERVO_CH)ch);
            if (rc != EXIT_OK) {
                return rc;
            }
        }
    }

    gpio_set_level(BSP_SERVO_POWER_GPIO, enable ? 1 : 0);
    s_power_enabled = enable;
    return EXIT_OK;
}

bool bsp_servo_power_enabled(void) {
    return s_power_enabled;
}
