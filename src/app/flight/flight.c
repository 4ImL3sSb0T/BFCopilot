#include "flight.h"

#include <limits.h>
#include <string.h>
#include "bf_radio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "flapping.h"
#include "parameter.h"
#include "safety.h"
#include "sensors.h"
#include "servo.h"

#define STATUS_INTERVAL_MS 20
#define LEASE_SLOTS ((BF_LEASE_MS + STATUS_INTERVAL_MS - 1) / STATUS_INTERVAL_MS)

static flapping_t s_flapping;
static flapping_command_t s_command;
static uint32_t s_session;
static uint32_t s_sequence;
static uint32_t s_rejected;
static uint32_t s_nonce;
static uint32_t s_tx_sequence;
static int64_t s_deadline_us;
static int64_t s_last_control_us;
static int64_t s_status_us;
static bool s_control_valid;
static bf_frame_t s_ack;
static struct { uint32_t id; int64_t deadline_us; } s_leases[LEASE_SLOTS];
static unsigned s_lease_slot;

static uint32_t random_id(void)
{
    uint32_t value;
    do { value = esp_random(); } while (!value);
    return value;
}

static flapping_limits_t limits(void)
{
    const bf_params_t *params = parameter_get();
    flapping_limits_t result = { .max_frequency_hz = params->system.max_flapping_hz,
                                 .ramp_s = params->system.ramp_s };
    for (unsigned side = 0; side < 2; side++) {
        result.min_deg[side] = params->servo[side].angle_min_deg;
        result.max_deg[side] = params->servo[side].angle_max_deg;
        result.speed_dps[side] = params->servo[side].max_speed_dps;
        result.acceleration_dps2[side] = params->servo[side].max_acceleration_dps2;
    }
    return result;
}

void flight_init(void)
{
    flapping_reset(&s_flapping);
}

void flight_end_session(void)
{
    safety_set_session(false);
    s_session = 0;
    s_control_valid = false;
    s_deadline_us = 0;
    memset(s_leases, 0, sizeof(s_leases));
    memset(&s_ack, 0, sizeof(s_ack));
    flapping_reset(&s_flapping);
}

bool flight_session_active(void) { return s_session != 0; }
float flight_phase_deg(void) { return s_flapping.phase_rad * 57.2957795f; }
uint32_t flight_rejected_frames(void) { return s_rejected + bf_radio_dropped(); }

int32_t flight_rx_age_ms(void)
{
    if (!s_control_valid) return -1;
    int64_t age = (esp_timer_get_time() - s_last_control_us) / 1000;
    return age > INT32_MAX ? INT32_MAX : (int32_t)age;
}

static void send_status(int64_t now)
{
    bf_frame_t frame = { .type = BF_STATUS, .session = s_session,
                        .sequence = ++s_tx_sequence, .request = s_nonce };
    unsigned slot = s_lease_slot++ % LEASE_SLOTS;
    s_leases[slot].id = random_id();
    s_leases[slot].deadline_us = now + BF_LEASE_MS * 1000;
    bf_put_u32(frame.payload, s_leases[slot].id);
    bf_put_u16(frame.payload + 4, BF_LEASE_MS);
    frame.payload[6] = (uint8_t)safety_state();
    frame.payload[7] = safety_state() == SAFETY_ARMED;
    bf_put_u32(frame.payload + 8, safety_faults());
    sensors_reading_t reading;
    sensors_get_latest(&reading);
    bf_put_u32(frame.payload + 12, reading.vbat_valid ? (uint32_t)reading.vbat_mv : UINT32_MAX);
    bf_put_u32(frame.payload + 16, reading.iservo_valid ? (uint32_t)reading.iservo_ma : UINT32_MAX);
    for (unsigned side = 0; side < 2; side++) {
        float output = 0;
        servo_get_output_deg((servo_idx_t)side, &output);
        bf_put_u16(frame.payload + 20 + side * 2, (uint16_t)(int16_t)(output * 100));
    }
    bf_put_u16(frame.payload + 24, (uint16_t)(flight_phase_deg() * 100));
    int32_t age = flight_rx_age_ms();
    bf_put_u16(frame.payload + 26, age < 0 || age > 65534 ? 65535 : (uint16_t)age);
    bf_put_u32(frame.payload + 28, flight_rejected_frames());
    bf_radio_send(&frame);
    s_status_us = now;
}

static bool empty_payload(const bf_frame_t *frame)
{
    for (unsigned index = 0; index < BF_PAYLOAD_SIZE; index++) {
        if (frame->payload[index]) return false;
    }
    return true;
}

static void process(const bf_radio_message_t *message, int64_t now)
{
    const bf_frame_t *frame = &message->frame;
    if (frame->type == BF_HELLO) {
        if (frame->session || !frame->request || !empty_payload(frame) ||
            now - message->received_us >= 100000) {
            s_rejected++;
            return;
        }
        flight_end_session();
        s_session = random_id();
        s_nonce = frame->request;
        s_sequence = 0;
        s_deadline_us = now + BF_LEASE_MS * 1000;
        safety_set_session(true);
        send_status(now);
        return;
    }
    if (!s_session || frame->session != s_session || frame->sequence <= s_sequence ||
        frame->sequence >= UINT32_MAX - 16 || now - message->received_us >= 100000) {
        s_rejected++;
        return;
    }
    s_sequence = frame->sequence;
    if (frame->type == BF_CONTROL) {
        flapping_command_t command;
        uint16_t remaining;
        uint32_t lease;
        flapping_limits_t bounds = limits();
        if (!bf_control_decode(frame, &command, &remaining, &lease) ||
            !flapping_validate(&command, &bounds)) {
            s_rejected++;
            return;
        }
        int64_t deadline = message->received_us + remaining * 1000;
        bool valid_lease = false;
        for (unsigned slot = 0; slot < LEASE_SLOTS; slot++) {
            if (lease && s_leases[slot].id == lease && now < s_leases[slot].deadline_us) {
                if (s_leases[slot].deadline_us < deadline) deadline = s_leases[slot].deadline_us;
                valid_lease = true;
                break;
            }
        }
        if (!valid_lease || now >= deadline) {
            s_rejected++;
            return;
        }
        s_command = command;
        s_control_valid = true;
        s_last_control_us = message->received_us;
        s_deadline_us = deadline;
        return;
    }
    if (!empty_payload(frame) || !frame->request ||
        (frame->type != BF_ARM && frame->type != BF_CLEAR && frame->type != BF_DISARM)) {
        s_rejected++;
        return;
    }
    if (s_ack.request == frame->request) {
        if (s_ack.payload[4] == frame->type) bf_radio_send(&s_ack);
        else s_rejected++;
        return;
    }
    bool success = false;
    bool fresh = s_control_valid && now < s_deadline_us;
    if (frame->type == BF_DISARM) {
        success = safety_disarm() == ESP_OK;
        flapping_reset(&s_flapping);
    } else if (frame->type == BF_CLEAR && fresh) {
        success = safety_clear_faults() == ESP_OK;
    } else if (frame->type == BF_ARM && fresh && s_command.mode != 2) {
        success = safety_request_arm() == SAFETY_ARM_OK;
        flapping_reset(&s_flapping);
    }
    s_ack = (bf_frame_t){ .type = BF_ACK, .session = s_session,
        .sequence = ++s_tx_sequence, .request = frame->request };
    bf_put_u32(s_ack.payload, success ? 0 : 1);
    s_ack.payload[4] = frame->type;
    bf_radio_send(&s_ack);
}

void flight_tick(float dt_s)
{
    int64_t now = esp_timer_get_time();
    bf_radio_message_t message;
    if (bf_radio_take_estop()) {
        safety_emergency_stop();
        flight_end_session();
        for (unsigned count = 0; count < 8 && bf_radio_receive(&message); count++) { }
    } else {
        if (s_session && now >= s_deadline_us) {
            if (safety_state() == SAFETY_ARMED) {
                safety_raise_fault(BF_FAULT_LINK_LOST, "control TTL or lease expired");
            }
            flight_end_session();
        }
        for (unsigned count = 0; count < 8 && bf_radio_receive(&message); count++) {
            process(&message, now);
        }
    }
    if (safety_state() == SAFETY_ARMED && s_session && s_control_valid && now < s_deadline_us) {
        flapping_limits_t bounds = limits();
        float output[2];
        if (!flapping_command(&s_flapping, &s_command, &bounds) ||
            !flapping_step(&s_flapping, &bounds, dt_s, output)) {
            safety_raise_fault(BF_FAULT_INTERNAL, "trajectory invalid");
        } else {
            for (unsigned side = 0; side < 2; side++) {
                servo_set_target_deg((servo_idx_t)side, output[side]);
            }
        }
    } else {
        flapping_reset(&s_flapping);
    }
    if (s_session && now - s_status_us >= STATUS_INTERVAL_MS * 1000) send_status(now);
}
