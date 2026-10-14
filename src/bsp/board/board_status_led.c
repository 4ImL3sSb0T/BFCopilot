#include "board_status_led.h"

#include "board_pins.h"

#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_led";

/* WS2812 bit timing at 10 MHz (1 tick = 0.1 us). */
#define LED_RMT_RESOLUTION_HZ   10000000
#define LED_RMT_MEM_SYMBOLS     64
#define WS2812_T0H_TICKS        3
#define WS2812_T0L_TICKS        9
#define WS2812_T1H_TICKS        6
#define WS2812_T1L_TICKS        6
#define WS2812_RESET_TICKS      500 /* 50 us idle-low latch */

#define WS2812_BYTES            3 /* GRB */

typedef struct {
    uint8_t r, g, b;
} led_rgb_t;

static const led_rgb_t s_patterns[BOARD_LED_COUNT] = {
    [BOARD_LED_OFF]    = {   0,   0,   0 },
    [BOARD_LED_BOOT]   = {   8,   8,   8 },
    [BOARD_LED_LOCKED] = {   0,   0,  32 },
    [BOARD_LED_READY]  = {  32,  24,   0 },
    [BOARD_LED_ARMED]  = {   0,  32,   0 },
    [BOARD_LED_CALIB]  = {  24,   0,  24 },
    [BOARD_LED_FAULT]  = {  48,   0,   0 },
};

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static board_led_pattern_t s_current = BOARD_LED_OFF;

static size_t ws2812_encode(const void *data, size_t data_size, size_t symbols_written,
                            size_t symbols_free, rmt_symbol_word_t *symbols, bool *done,
                            void *arg)
{
    (void)arg;
    const uint8_t *bytes = (const uint8_t *)data;
    const size_t total = 1 + data_size * 8; /* one reset symbol, then 8 symbols per byte */

    size_t produced = 0;
    size_t pos = symbols_written;

    while (pos < total && produced < symbols_free) {
        rmt_symbol_word_t sym;
        if (pos == 0) {
            sym.level0 = 0;
            sym.duration0 = WS2812_RESET_TICKS / 2;
            sym.level1 = 0;
            sym.duration1 = WS2812_RESET_TICKS / 2;
        } else {
            size_t bit_index = pos - 1;
            size_t byte_index = bit_index / 8;
            int bit = 7 - (int)(bit_index % 8);
            bool one = (bytes[byte_index] >> bit) & 0x1;
            sym.level0 = 1;
            sym.duration0 = one ? WS2812_T1H_TICKS : WS2812_T0H_TICKS;
            sym.level1 = 0;
            sym.duration1 = one ? WS2812_T1L_TICKS : WS2812_T0L_TICKS;
        }
        symbols[produced++] = sym;
        pos++;
    }

    *done = (pos >= total);
    return produced;
}

esp_err_t board_led_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = BOARD_PIN_WS2812,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = LED_RMT_MEM_SYMBOLS,
        .trans_queue_depth = 4,
        .flags.init_level = 0,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_simple_encoder_config_t enc_cfg = {
        .callback = ws2812_encode,
        .arg = NULL,
        .min_chunk_size = LED_RMT_MEM_SYMBOLS,
    };
    err = rmt_new_simple_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_simple_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    return board_led_set_rgb(0, 0, 0);
}

esp_err_t board_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* WS2812 wants GRB order. */
    static uint8_t frame[WS2812_BYTES];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    rmt_transmit_config_t tx_cfg = { .loop_count = 0, .flags.eot_level = 0 };
    esp_err_t err = rmt_tx_wait_all_done(s_chan, 20);
    if (err == ESP_OK) {
        frame[0] = g;
        frame[1] = r;
        frame[2] = b;
        err = rmt_transmit(s_chan, s_encoder, frame, sizeof(frame), &tx_cfg);
    }
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, 20);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void board_led_set(board_led_pattern_t pattern)
{
    if ((unsigned)pattern >= BOARD_LED_COUNT) {
        pattern = BOARD_LED_FAULT;
    }
    const led_rgb_t *rgb = &s_patterns[pattern];
    if (board_led_set_rgb(rgb->r, rgb->g, rgb->b) == ESP_OK) {
        s_current = pattern;
    }
}

board_led_pattern_t board_led_get(void)
{
    return s_current;
}
