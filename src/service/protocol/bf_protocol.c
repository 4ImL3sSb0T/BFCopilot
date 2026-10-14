#include "bf_protocol.h"
#include "bf_crc.h"

#include <string.h>

uint16_t bf_get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t bf_get_u32(const uint8_t *data)
{
    return (uint32_t)bf_get_u16(data) | ((uint32_t)bf_get_u16(data + 2) << 16);
}

void bf_put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

void bf_put_u32(uint8_t *data, uint32_t value)
{
    bf_put_u16(data, (uint16_t)value);
    bf_put_u16(data + 2, (uint16_t)(value >> 16));
}

bool bf_frame_decode(const uint8_t *data, size_t length, bf_frame_t *frame)
{
    if (!data || !frame || length != BF_FRAME_SIZE || bf_get_u16(data) != 0x4642 ||
        data[2] != 1 || data[3] < BF_HELLO || data[3] > BF_ACK ||
        bf_get_u32(data + 4) != 1 || bf_get_u16(data + 20) != BF_PAYLOAD_SIZE ||
        bf_get_u16(data + 22) != 0 || bf_get_u32(data + 56) != bf_crc32(data, 56)) {
        return false;
    }
    frame->type = data[3];
    frame->session = bf_get_u32(data + 8);
    frame->sequence = bf_get_u32(data + 12);
    frame->request = bf_get_u32(data + 16);
    memcpy(frame->payload, data + 24, BF_PAYLOAD_SIZE);
    return true;
}

void bf_frame_encode(const bf_frame_t *frame, uint8_t data[BF_FRAME_SIZE])
{
    memset(data, 0, BF_FRAME_SIZE);
    bf_put_u16(data, 0x4642);
    data[2] = 1;
    data[3] = frame->type;
    bf_put_u32(data + 4, 1);
    bf_put_u32(data + 8, frame->session);
    bf_put_u32(data + 12, frame->sequence);
    bf_put_u32(data + 16, frame->request);
    bf_put_u16(data + 20, BF_PAYLOAD_SIZE);
    memcpy(data + 24, frame->payload, BF_PAYLOAD_SIZE);
    bf_put_u32(data + 56, bf_crc32(data, 56));
}

bool bf_control_decode(const bf_frame_t *frame, flapping_command_t *command,
                       uint16_t *remaining_ms, uint32_t *lease)
{
    if (!frame || !command || !remaining_ms || !lease || frame->type != BF_CONTROL) {
        return false;
    }
    const uint8_t *payload = frame->payload;
    uint16_t ttl = bf_get_u16(payload + 2);
    uint16_t age = bf_get_u16(payload + 4);
    if (payload[1] || bf_get_u16(payload + 6) || ttl == 0 || ttl > BF_MAX_TTL_MS || age >= ttl) {
        return false;
    }
    *remaining_ms = ttl - age;
    *lease = bf_get_u32(payload + 8);
    memset(command, 0, sizeof(*command));
    command->mode = payload[0];
    command->frequency_hz = bf_get_u32(payload + 12) / 1000.0f;
    for (unsigned side = 0; side < 2; side++) {
        command->amplitude_deg[side] = bf_get_u16(payload + 16 + side * 2) / 100.0f;
        command->center_deg[side] = ((int16_t)bf_get_u16(payload + 20 + side * 2) +
            (int16_t)bf_get_u16(payload + 26 + side * 2)) / 100.0f;
    }
    command->phase_offset_deg = (int16_t)bf_get_u16(payload + 24) / 100.0f;
    command->differential = (int16_t)bf_get_u16(payload + 30) / 1000.0f;
    return true;
}

size_t bf_cobs_encode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity)
{
    if (!data || !out || capacity < length + length / 254 + 1) {
        return 0;
    }
    size_t write = 1;
    size_t marker = 0;
    uint8_t code = 1;
    for (size_t index = 0; index < length; index++) {
        if (data[index] == 0) {
            out[marker] = code;
            marker = write++;
            code = 1;
        } else {
            out[write++] = data[index];
            if (++code == 255) {
                out[marker] = code;
                marker = write++;
                code = 1;
            }
        }
    }
    out[marker] = code;
    return write;
}

size_t bf_cobs_decode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity)
{
    if (!data || !out || length == 0) {
        return 0;
    }
    size_t read = 0;
    size_t write = 0;
    while (read < length) {
        uint8_t code = data[read++];
        if (code == 0 || read + code - 1 > length || write + code - 1 > capacity) {
            return 0;
        }
        for (unsigned count = 1; count < code; count++) {
            if (data[read] == 0) {
                return 0;
            }
            out[write++] = data[read++];
        }
        if (code != 255 && read < length) {
            if (write == capacity) {
                return 0;
            }
            out[write++] = 0;
        }
    }
    return write;
}
