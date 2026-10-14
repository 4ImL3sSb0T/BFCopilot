#include "board_buzzer.h"

#include "board_pins.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_buzzer";

#define BUZZER_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER     LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BUZZER_LEDC_RES       LEDC_TIMER_10_BIT
#define BUZZER_DUTY_HALF      512 /* 50% of 10-bit */

typedef struct {
    uint32_t freq_hz;
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t remaining;
    bool on_phase;
    esp_timer_handle_t timer;
    bool ready;
} buzzer_ctx_t;

static buzzer_ctx_t s_ctx;
static SemaphoreHandle_t s_lock;

static esp_err_t buzzer_stop_sequence(void);

static esp_err_t buzzer_set_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    return err == ESP_OK ? ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL) : err;
}

static void buzzer_step(void *arg)
{
    buzzer_ctx_t *ctx = (buzzer_ctx_t *)arg;

    if (ctx->on_phase) {
        if (buzzer_set_duty(0) != ESP_OK) {
            buzzer_stop_sequence();
            return;
        }
        ctx->on_phase = false;
        if (ctx->remaining > 0) {
            ctx->remaining--;
        }
        if (ctx->remaining == 0) {
            buzzer_stop_sequence();
            return;
        }
    } else {
        if (buzzer_set_duty(BUZZER_DUTY_HALF) != ESP_OK) {
            buzzer_stop_sequence();
            return;
        }
        ctx->on_phase = true;
    }

    uint32_t next_ms = ctx->on_phase ? ctx->on_ms : ctx->off_ms;
    if (next_ms == 0) {
        next_ms = 1;
    }
    if (esp_timer_start_once(ctx->timer, (uint64_t)next_ms * 1000) != ESP_OK) {
        buzzer_stop_sequence();
    }
}

static void buzzer_timer_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctx.remaining && !esp_timer_is_active(s_ctx.timer)) buzzer_step(arg);
    xSemaphoreGive(s_lock);
}

static esp_err_t buzzer_stop_sequence(void)
{
    if (s_ctx.timer && esp_timer_is_active(s_ctx.timer)) {
        esp_err_t err = esp_timer_stop(s_ctx.timer);
        if (err != ESP_OK) return err;
    }
    s_ctx.remaining = 0;
    s_ctx.on_phase = false;
    if (s_ctx.ready) {
        return buzzer_set_duty(0);
    }
    return ESP_OK;
}

esp_err_t board_buzzer_init(void)
{
    if (s_ctx.ready) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    ledc_timer_config_t timer = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_LEDC_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t chan = {
        .gpio_num = BOARD_PIN_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_timer_create_args_t args = {
        .callback = buzzer_timer_cb,
        .arg = &s_ctx,
        .name = "buzzer",
    };
    err = esp_timer_create(&args, &s_ctx.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ctx.ready = true;
    return ESP_OK;
}

static esp_err_t buzzer_tone(uint32_t freq_hz)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = buzzer_stop_sequence();
    if (err != ESP_OK) return err;

    if (freq_hz == 0) {
        return ESP_OK;
    }
    err = ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    return buzzer_set_duty(BUZZER_DUTY_HALF);
}

static esp_err_t buzzer_beep(uint32_t freq_hz, uint32_t on_ms, uint32_t off_ms, uint32_t count)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = buzzer_stop_sequence();
    if (err != ESP_OK) return err;

    if (count == 0 || freq_hz == 0 || on_ms == 0) {
        return ESP_OK;
    }

    err = ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.freq_hz = freq_hz;
    s_ctx.on_ms = on_ms;
    s_ctx.off_ms = off_ms;
    s_ctx.remaining = count;
    s_ctx.on_phase = true;

    err = buzzer_set_duty(BUZZER_DUTY_HALF);
    if (err == ESP_OK) err = esp_timer_start_once(s_ctx.timer, (uint64_t)on_ms * 1000);
    if (err != ESP_OK) buzzer_stop_sequence();
    return err;
}

esp_err_t board_buzzer_tone(uint32_t freq_hz)
{
    if (!s_ctx.ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = buzzer_tone(freq_hz);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t board_buzzer_beep(uint32_t freq_hz, uint32_t on_ms, uint32_t off_ms, uint32_t count)
{
    if (!s_ctx.ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = buzzer_beep(freq_hz, on_ms, off_ms, count);
    xSemaphoreGive(s_lock);
    return err;
}
