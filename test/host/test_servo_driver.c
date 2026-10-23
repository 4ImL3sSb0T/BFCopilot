#include <assert.h>
#include <stdio.h>
#include "servo.h"
#include "board_pins.h"
#include "driver/mcpwm_prelude.h"

static mock_timer_t mock_timer;
static mock_comparator_t comparators[SERVO_COUNT];
static mock_generator_t generators[SERVO_COUNT];
static unsigned comparator_count, generator_count;
static esp_err_t force_error;

esp_err_t mcpwm_new_timer(const mcpwm_timer_config_t *config, mcpwm_timer_handle_t *timer)
{
    assert(config->resolution_hz == 1000000 && config->period_ticks == 5000);
    *timer = &mock_timer;
    return ESP_OK;
}
esp_err_t mcpwm_new_operator(const mcpwm_operator_config_t *config, mcpwm_oper_handle_t *oper)
{
    assert(config->group_id == 0);
    *oper = &mock_timer;
    return ESP_OK;
}
esp_err_t mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper, mcpwm_timer_handle_t timer)
{
    assert(oper && timer);
    return ESP_OK;
}
esp_err_t mcpwm_new_comparator(mcpwm_oper_handle_t oper, const mcpwm_comparator_config_t *config, mcpwm_cmpr_handle_t *comparator)
{
    assert(oper && config->flags.update_cmp_on_tez && comparator_count < SERVO_COUNT);
    *comparator = &comparators[comparator_count++];
    return ESP_OK;
}
esp_err_t mcpwm_new_generator(mcpwm_oper_handle_t oper, const mcpwm_generator_config_t *config, mcpwm_gen_handle_t *generator)
{
    assert(oper && generator_count < SERVO_COUNT);
    *generator = &generators[generator_count++];
    (*generator)->gpio = config->gen_gpio_num;
    (*generator)->continuous_level = -1;
    return ESP_OK;
}
esp_err_t mcpwm_generator_set_action_on_timer_event(mcpwm_gen_handle_t generator, mcpwm_gen_timer_event_action_t action)
{
    assert(generator && action.event == MCPWM_TIMER_EVENT_EMPTY && action.action == MCPWM_GEN_ACTION_HIGH);
    return ESP_OK;
}
esp_err_t mcpwm_generator_set_action_on_compare_event(mcpwm_gen_handle_t generator, mcpwm_gen_compare_event_action_t action)
{
    assert(generator && action.comparator && action.action == MCPWM_GEN_ACTION_LOW);
    return ESP_OK;
}
esp_err_t mcpwm_generator_set_force_level(mcpwm_gen_handle_t generator, int level, bool hold_on)
{
    if (force_error != ESP_OK) return force_error;
    if (hold_on) generator->continuous_level = level;
    else generator->noncontinuous = level >= 0;
    return ESP_OK;
}
esp_err_t mcpwm_comparator_set_compare_value(mcpwm_cmpr_handle_t comparator, uint32_t value)
{
    comparator->compare = value;
    return ESP_OK;
}
esp_err_t mcpwm_timer_enable(mcpwm_timer_handle_t timer)
{
    assert(timer);
    return ESP_OK;
}
esp_err_t mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command)
{
    assert(command == MCPWM_TIMER_START_NO_STOP);
    timer->running = true;
    return ESP_OK;
}

int main(void)
{
    bf_servo_cal_t calibration[SERVO_COUNT] = {0};
    assert(servo_init(calibration) == ESP_OK);
    assert(mock_timer.running);
    assert(generators[0].gpio == BOARD_PIN_SERVO_LEFT_PWM);
    assert(generators[1].gpio == BOARD_PIN_SERVO_RIGHT_PWM);
    assert(generators[0].continuous_level == 0 && generators[1].continuous_level == 0);
    force_error = ESP_FAIL;
    assert(servo_debug_set_pulse_us(SERVO_LEFT, 1550) == ESP_FAIL);
    float reported_pulse = 0;
    assert(servo_debug_get_pulse_us(SERVO_LEFT, &reported_pulse) == ESP_ERR_INVALID_STATE);
    force_error = ESP_OK;
    assert(servo_debug_set_pulse_us(SERVO_LEFT, 1550) == ESP_OK);
    assert(comparators[0].compare == 1550 && generators[0].continuous_level == -1);
    force_error = ESP_FAIL;
    assert(servo_debug_off(SERVO_LEFT) == ESP_FAIL);
    assert(servo_debug_get_pulse_us(SERVO_LEFT, &reported_pulse) == ESP_OK);
    assert(reported_pulse == 1550);
    force_error = ESP_OK;
    assert(generators[1].continuous_level == 0);
    assert(servo_debug_set_pulse_us(SERVO_RIGHT, 1600) == ESP_OK);
    assert(comparators[1].compare == 1600 && generators[1].continuous_level == -1);
    assert(servo_debug_off(SERVO_LEFT) == ESP_OK);
    assert(generators[0].continuous_level == 0 && generators[1].continuous_level == -1);
    assert(servo_debug_set_pulse_us(SERVO_LEFT, 1500) == ESP_OK);
    assert(comparators[0].compare == 1500 && generators[0].continuous_level == -1);
    servo_force_output_off();
    assert(generators[0].continuous_level == 0 && generators[1].continuous_level == 0);
    assert(servo_enable(true) == ESP_ERR_INVALID_STATE);
    assert(generators[0].continuous_level == 0 && generators[1].continuous_level == 0);
    for (unsigned side = 0; side < SERVO_COUNT; side++) {
        calibration[side] = (bf_servo_cal_t){
            .calibrated = 1, .direction = 1, .angle_min_deg = -90, .angle_max_deg = 90,
            .angle_min_us = 1000, .angle_max_us = 2000, .max_speed_dps = 100,
        };
    }
    assert(servo_reload_params() == ESP_OK);
    assert(servo_enable(true) == ESP_OK);
    assert(comparators[0].compare == 1500 && comparators[1].compare == 1500);
    assert(generators[0].continuous_level == -1 && generators[1].continuous_level == -1);
    assert(servo_enable(false) == ESP_OK);
    assert(generators[0].continuous_level == 0 && generators[1].continuous_level == 0);
    assert(servo_enable(true) == ESP_OK);
    assert(generators[0].continuous_level == -1 && generators[1].continuous_level == -1);
    puts("servo driver tests passed");
    return 0;
}
