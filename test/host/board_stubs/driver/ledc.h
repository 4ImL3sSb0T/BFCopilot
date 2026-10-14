#pragma once
#include <stdint.h>
#include "esp_err.h"
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_TIMER_10_BIT 10
#define LEDC_AUTO_CLK 0
typedef struct { int speed_mode, duty_resolution, timer_num, clk_cfg; uint32_t freq_hz; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, timer_sel; uint32_t duty, hpoint; } ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
esp_err_t ledc_set_freq(int mode, int timer, uint32_t freq);
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty);
esp_err_t ledc_update_duty(int mode, int channel);
