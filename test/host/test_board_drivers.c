#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_status_led.h"
#include "board_buzzer.h"
#include "board_pins.h"
#include "driver/rmt_tx.h"
#include "driver/ledc.h"
#include "esp_timer.h"

static rmt_simple_encoder_config_t encoder_config;
static esp_timer_create_args_t timer_config;
static const uint8_t *pending_frame;
static unsigned wait_calls, fail_wait_at, transmit_calls;
static bool timer_active;
static uint32_t applied_duty;
static esp_err_t duty_error, timer_error;

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *channel)
{
    assert(config->gpio_num == BOARD_PIN_WS2812 && config->resolution_hz == 10000000);
    *channel = &encoder_config;
    return ESP_OK;
}
esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *config, rmt_encoder_handle_t *encoder)
{
    encoder_config = *config;
    *encoder = &encoder_config;
    return ESP_OK;
}
esp_err_t rmt_enable(rmt_channel_handle_t channel) { assert(channel); return ESP_OK; }
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder, const void *data, size_t size, const rmt_transmit_config_t *config)
{
    assert(channel && encoder && size == 3 && config->flags.eot_level == 0);
    pending_frame = data;
    transmit_calls++;
    return ESP_OK;
}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout)
{
    assert(channel && timeout == 20);
    return ++wait_calls == fail_wait_at ? ESP_FAIL : ESP_OK;
}
esp_err_t ledc_timer_config(const ledc_timer_config_t *config) { assert(config->duty_resolution == 10); return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *config) { assert(config->gpio_num == BOARD_PIN_BUZZER); return ESP_OK; }
esp_err_t ledc_set_freq(int mode, int timer, uint32_t freq) { (void)mode; (void)timer; assert(freq); return ESP_OK; }
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty)
{
    (void)mode;
    (void)channel;
    if (duty_error != ESP_OK) return duty_error;
    applied_duty = duty;
    return ESP_OK;
}
esp_err_t ledc_update_duty(int mode, int channel) { (void)mode; (void)channel; return ESP_OK; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *timer)
{
    timer_config = *config;
    *timer = &timer_config;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { assert(timer); timer_active = false; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us)
{
    assert(timer && us > 0);
    if (timer_error != ESP_OK) return timer_error;
    timer_active = true;
    return ESP_OK;
}
bool esp_timer_is_active(esp_timer_handle_t timer) { assert(timer); return timer_active; }

int main(void)
{
    assert(board_led_init() == ESP_OK);
    assert(board_led_set_rgb(0x80, 0x01, 0x55) == ESP_OK);
    assert(pending_frame[0] == 0x01 && pending_frame[1] == 0x80 && pending_frame[2] == 0x55);
    rmt_symbol_word_t symbols[25];
    bool done = false;
    assert(encoder_config.callback(pending_frame, 3, 0, 25, symbols, &done, NULL) == 25 && done);
    assert(symbols[0].duration0 + symbols[0].duration1 == 500);
    for (unsigned index = 0; index < 25; index++) {
        assert(symbols[index].duration0 > 0 && symbols[index].duration1 > 0);
    }
    assert(symbols[1].duration0 == 3 && symbols[8].duration0 == 6 && symbols[9].duration0 == 6);
    fail_wait_at = wait_calls + 2;
    assert(board_led_set_rgb(1, 2, 3) == ESP_FAIL);
    uint8_t preserved[3];
    memcpy(preserved, pending_frame, 3);
    unsigned previous_transmits = transmit_calls;
    fail_wait_at = wait_calls + 1;
    assert(board_led_set_rgb(9, 8, 7) == ESP_FAIL);
    assert(transmit_calls == previous_transmits && memcmp(preserved, pending_frame, 3) == 0);
    fail_wait_at = 0;
    board_led_set((board_led_pattern_t)-1);
    assert(board_led_get() == BOARD_LED_FAULT);

    assert(board_buzzer_init() == ESP_OK);
    assert(board_buzzer_tone(2000) == ESP_OK && applied_duty == 512);
    assert(board_buzzer_tone(0) == ESP_OK && applied_duty == 0);
    duty_error = ESP_FAIL;
    assert(board_buzzer_tone(0) == ESP_FAIL);
    assert(board_buzzer_beep(2000, 100, 100, 1) == ESP_FAIL);
    duty_error = ESP_OK;
    timer_error = ESP_FAIL;
    assert(board_buzzer_beep(2000, 100, 100, 1) == ESP_FAIL && applied_duty == 0);
    timer_error = ESP_OK;
    assert(board_buzzer_beep(2000, 100, 100, 2) == ESP_OK && timer_active && applied_duty == 512);
    timer_config.callback(timer_config.arg);
    assert(applied_duty == 512 && timer_active);
    timer_active = false;
    timer_config.callback(timer_config.arg);
    assert(applied_duty == 0 && timer_active);
    assert(board_buzzer_tone(0) == ESP_OK && !timer_active);
    timer_config.callback(timer_config.arg);
    assert(applied_duty == 0 && !timer_active);
    puts("board driver tests passed");
    return 0;
}
