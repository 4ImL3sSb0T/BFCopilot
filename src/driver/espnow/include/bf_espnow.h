#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*bf_espnow_receiver_t)(const uint8_t *data, size_t length, int64_t received_us);

esp_err_t bf_espnow_init(bf_espnow_receiver_t receiver);
esp_err_t bf_espnow_send(const uint8_t *data, size_t length);
