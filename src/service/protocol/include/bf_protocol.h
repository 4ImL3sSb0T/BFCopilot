#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "flapping.h"

#define BF_FRAME_SIZE 60u
#define BF_PAYLOAD_SIZE 32u
#define BF_LEASE_MS 150u
#define BF_MAX_TTL_MS 100u

enum { BF_HELLO = 1, BF_CONTROL, BF_ARM, BF_DISARM, BF_ESTOP, BF_CLEAR, BF_STATUS, BF_ACK };

typedef struct {
    uint8_t type;
    uint32_t session;
    uint32_t sequence;
    uint32_t request;
    uint8_t payload[BF_PAYLOAD_SIZE];
} bf_frame_t;

uint16_t bf_get_u16(const uint8_t *data);
uint32_t bf_get_u32(const uint8_t *data);
void bf_put_u16(uint8_t *data, uint16_t value);
void bf_put_u32(uint8_t *data, uint32_t value);
bool bf_frame_decode(const uint8_t *data, size_t length, bf_frame_t *frame);
void bf_frame_encode(const bf_frame_t *frame, uint8_t data[BF_FRAME_SIZE]);
bool bf_control_decode(const bf_frame_t *frame, flapping_command_t *command,
                       uint16_t *remaining_ms, uint32_t *lease);
size_t bf_cobs_encode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity);
size_t bf_cobs_decode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity);
