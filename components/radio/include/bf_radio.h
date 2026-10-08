#pragma once

#include "bf_protocol.h"
#include "esp_err.h"

typedef struct {
    bf_frame_t frame;
    int64_t received_us;
} bf_radio_message_t;

esp_err_t bf_radio_init(void);
esp_err_t bf_radio_send(const bf_frame_t *frame);
bool bf_radio_receive(bf_radio_message_t *message);
bool bf_radio_take_estop(void);
uint32_t bf_radio_dropped(void);
