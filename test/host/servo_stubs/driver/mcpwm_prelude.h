#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include "esp_err.h"

typedef struct { bool running; } mock_timer_t;
typedef struct { uint32_t compare; } mock_comparator_t;
typedef struct { int gpio; int continuous_level; bool noncontinuous; } mock_generator_t;
typedef mock_timer_t *mcpwm_timer_handle_t;
typedef void *mcpwm_oper_handle_t;
typedef mock_comparator_t *mcpwm_cmpr_handle_t;
typedef mock_generator_t *mcpwm_gen_handle_t;
typedef struct { int group_id, clk_src, count_mode; uint32_t resolution_hz, period_ticks; } mcpwm_timer_config_t;
typedef struct { int group_id; } mcpwm_operator_config_t;
typedef struct { struct { bool update_cmp_on_tez; } flags; } mcpwm_comparator_config_t;
typedef struct { int gen_gpio_num; } mcpwm_generator_config_t;
typedef struct { int direction, event, action; } mcpwm_gen_timer_event_action_t;
typedef struct { int direction; mcpwm_cmpr_handle_t comparator; int action; } mcpwm_gen_compare_event_action_t;

#define MCPWM_TIMER_CLK_SRC_DEFAULT 0
#define MCPWM_TIMER_COUNT_MODE_UP 0
#define MCPWM_TIMER_DIRECTION_UP 0
#define MCPWM_TIMER_EVENT_EMPTY 0
#define MCPWM_GEN_ACTION_LOW 0
#define MCPWM_GEN_ACTION_HIGH 1
#define MCPWM_TIMER_START_NO_STOP 0
#define MCPWM_GEN_TIMER_EVENT_ACTION(dir, evt, act) ((mcpwm_gen_timer_event_action_t){dir, evt, act})
#define MCPWM_GEN_COMPARE_EVENT_ACTION(dir, cmp, act) ((mcpwm_gen_compare_event_action_t){dir, cmp, act})
#define ESP_ERROR_CHECK(result) assert((result) == ESP_OK)

esp_err_t mcpwm_new_timer(const mcpwm_timer_config_t *config, mcpwm_timer_handle_t *timer);
esp_err_t mcpwm_new_operator(const mcpwm_operator_config_t *config, mcpwm_oper_handle_t *oper);
esp_err_t mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper, mcpwm_timer_handle_t timer);
esp_err_t mcpwm_new_comparator(mcpwm_oper_handle_t oper, const mcpwm_comparator_config_t *config, mcpwm_cmpr_handle_t *comparator);
esp_err_t mcpwm_new_generator(mcpwm_oper_handle_t oper, const mcpwm_generator_config_t *config, mcpwm_gen_handle_t *generator);
esp_err_t mcpwm_generator_set_action_on_timer_event(mcpwm_gen_handle_t generator, mcpwm_gen_timer_event_action_t action);
esp_err_t mcpwm_generator_set_action_on_compare_event(mcpwm_gen_handle_t generator, mcpwm_gen_compare_event_action_t action);
esp_err_t mcpwm_generator_set_force_level(mcpwm_gen_handle_t generator, int level, bool hold_on);
esp_err_t mcpwm_comparator_set_compare_value(mcpwm_cmpr_handle_t comparator, uint32_t value);
esp_err_t mcpwm_timer_enable(mcpwm_timer_handle_t timer);
esp_err_t mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command);
