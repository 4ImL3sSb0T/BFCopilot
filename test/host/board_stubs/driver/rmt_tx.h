#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef struct { unsigned duration0, level0, duration1, level1; } rmt_symbol_word_t;
typedef size_t (*rmt_encode_callback_t)(const void *, size_t, size_t, size_t, rmt_symbol_word_t *, bool *, void *);
typedef struct { rmt_encode_callback_t callback; void *arg; size_t min_chunk_size; } rmt_simple_encoder_config_t;
typedef struct { int gpio_num, clk_src, resolution_hz, mem_block_symbols, trans_queue_depth; struct { int init_level; } flags; } rmt_tx_channel_config_t;
typedef struct { int loop_count; struct { int eot_level; } flags; } rmt_transmit_config_t;
#define RMT_CLK_SRC_DEFAULT 0
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *channel);
esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *config, rmt_encoder_handle_t *encoder);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder, const void *data, size_t size, const rmt_transmit_config_t *config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout);
