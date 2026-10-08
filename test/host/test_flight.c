#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bf_radio.h"
#include "board_buzzer.h"
#include "board_status_led.h"
#include "flight.h"
#include "parameter.h"
#include "safety.h"
#include "sensors.h"
#include "servo.h"
#include "telemetry.h"

static int64_t s_now;
static uint32_t s_random = 100;
static bf_params_t s_params;
static bf_radio_message_t s_incoming;
static bool s_pending;
static bool s_estop;
static bool s_enabled;
static bool s_calibrated = true;
static bool s_enable_failure;
static float s_output[2];
static float s_target[2];
static bf_frame_t s_status;
static bf_frame_t s_ack;
static unsigned s_arm_beeps;
static unsigned s_sequence;
static uint32_t s_session;
static unsigned s_checks;

#define CHECK(condition) do { s_checks++; if (!(condition)) { \
    fprintf(stderr, "failed line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int64_t esp_timer_get_time(void) { return s_now; }
uint32_t esp_random(void) { return ++s_random; }
const bf_params_t *parameter_get(void) { return &s_params; }
bool parameter_is_valid(void) { return true; }
void board_led_set(board_led_pattern_t pattern) { (void)pattern; }
esp_err_t board_buzzer_beep(uint32_t hz, uint32_t on, uint32_t off, uint32_t count)
{
    (void)hz; (void)on; (void)off;
    if (count == 1) s_arm_beeps++;
    return ESP_OK;
}
void telemetry_log_event(bf_fault_bit_t bit, const char *detail) { (void)bit; (void)detail; }
bool servo_is_enabled(void) { return s_enabled; }
bool servo_calibration_valid(servo_idx_t side) { (void)side; return s_calibrated; }
esp_err_t servo_enable(bool enable)
{
    if (enable && s_enable_failure) return ESP_FAIL;
    s_enabled = enable;
    return ESP_OK;
}
void servo_force_output_off(void) { s_enabled = false; }
esp_err_t servo_set_target_deg(servo_idx_t side, float angle) { s_target[side] = angle; return ESP_OK; }
esp_err_t servo_get_output_deg(servo_idx_t side, float *out) { *out = s_output[side]; return ESP_OK; }
esp_err_t servo_get_safe_deg(servo_idx_t side, float *out) { *out = s_params.servo[side].safe_deg; return ESP_OK; }
esp_err_t servo_return_to_safe(void)
{
    for (unsigned side = 0; side < 2; side++) s_target[side] = s_params.servo[side].safe_deg;
    return ESP_OK;
}
void sensors_get_latest(sensors_reading_t *out) { memset(out, 0, sizeof(*out)); }
uint32_t bf_radio_dropped(void) { return 0; }
bool bf_radio_take_estop(void) { bool value = s_estop; s_estop = false; return value; }
bool bf_radio_receive(bf_radio_message_t *out)
{
    if (!s_pending) return false;
    *out = s_incoming;
    s_pending = false;
    return true;
}
esp_err_t bf_radio_send(const bf_frame_t *frame)
{
    if (frame->type == BF_STATUS) s_status = *frame;
    if (frame->type == BF_ACK) s_ack = *frame;
    return ESP_OK;
}

static void tick(void)
{
    flight_tick(0.003003f);
    safety_tick(0.003003f);
}

static void receive(bf_frame_t frame)
{
    s_incoming = (bf_radio_message_t){ .frame = frame, .received_us = s_now };
    s_pending = true;
    tick();
}

static void hello(void)
{
    receive((bf_frame_t){ .type = BF_HELLO, .request = 42 });
    s_session = s_status.session;
    s_sequence = 0;
}

static bf_frame_t control(void)
{
    bf_frame_t frame = { .type = BF_CONTROL, .session = s_session, .sequence = ++s_sequence };
    bf_put_u16(frame.payload + 2, 100);
    bf_put_u32(frame.payload + 8, bf_get_u32(s_status.payload));
    return frame;
}

static void request(uint8_t type, uint32_t identifier)
{
    receive((bf_frame_t){ .type = type, .session = s_session,
        .sequence = ++s_sequence, .request = identifier });
}

int main(void)
{
    s_params.safety.calibrated = s_params.sensors.calibrated = 1;
    s_params.system.max_flapping_hz = 5;
    s_params.system.ramp_s = 0.1f;
    for (unsigned side = 0; side < 2; side++) {
        s_params.servo[side].angle_min_deg = -90;
        s_params.servo[side].angle_max_deg = 90;
        s_params.servo[side].max_speed_dps = 1000;
        s_params.servo[side].max_acceleration_dps2 = 10000;
    }
    safety_init();
    flight_init();
    safety_boot_complete(true);
    safety_set_power_ok(true);
    CHECK(safety_request_arm() == SAFETY_ARM_ERR_NO_SESSION);
    hello();
    CHECK(s_session != 0 && !s_enabled);
    request(BF_ARM, 1);
    CHECK(bf_get_u32(s_ack.payload) == 1 && safety_state() != SAFETY_ARMED);
    receive(control());
    request(BF_ARM, 2);
    CHECK(bf_get_u32(s_ack.payload) == 0 && safety_state() == SAFETY_ARMED && s_enabled);
    CHECK(safety_clear_faults() == ESP_ERR_INVALID_STATE);
    unsigned beeps = s_arm_beeps;
    request(BF_ARM, 2);
    CHECK(s_arm_beeps == beeps && bf_get_u32(s_ack.payload) == 0);
    bf_frame_t flap = control();
    flap.payload[0] = 2;
    bf_put_u32(flap.payload + 12, 1000);
    bf_put_u16(flap.payload + 16, 10 * 100);
    bf_put_u16(flap.payload + 18, 10 * 100);
    receive(flap);
    CHECK(flight_phase_deg() > 0);
    uint32_t rejected = flight_rejected_frames();
    receive(flap);
    CHECK(flight_rejected_frames() == rejected + 1);
    s_now = 100000;
    tick();
    CHECK(!flight_session_active() && safety_state() != SAFETY_ARMED && safety_has_fault(BF_FAULT_LINK_LOST));
    CHECK(flight_phase_deg() == 0);
    hello();
    CHECK(safety_state() != SAFETY_ARMED);
    receive(control());
    request(BF_CLEAR, 3);
    CHECK(bf_get_u32(s_ack.payload) == 0);
    request(BF_ARM, 4);
    CHECK(safety_state() == SAFETY_ARMED);
    s_estop = true;
    s_incoming = (bf_radio_message_t){ .frame = control(), .received_us = s_now };
    s_pending = true;
    tick();
    CHECK(safety_estop_latched() && !flight_session_active() && !s_pending);
    CHECK(safety_state() != SAFETY_ARMED);
    hello();
    receive(control());
    request(BF_CLEAR, 5);
    CHECK(!safety_estop_latched());
    request(BF_ARM, 6);
    CHECK(safety_state() == SAFETY_ARMED);
    s_output[0] = 20;
    safety_emergency_stop();
    CHECK(safety_state() == SAFETY_SAFE_RETURN_LOCKED);
    safety_raise_fault(BF_FAULT_SERVO_OVERCURRENT, "injected");
    CHECK(safety_state() == SAFETY_FAULT_LATCHED && !s_enabled);
    CHECK(safety_clear_faults() == ESP_ERR_INVALID_STATE);
    safety_lower_fault(BF_FAULT_SERVO_OVERCURRENT);
    CHECK(safety_clear_faults() == ESP_OK);
    s_enable_failure = true;
    CHECK(safety_request_arm() != SAFETY_ARM_OK && safety_state() != SAFETY_ARMED);
    s_enable_failure = false;
    s_calibrated = false;
    CHECK(safety_request_arm() == SAFETY_ARM_ERR_NOT_CALIBRATED);
    s_calibrated = true;
    s_params.safety.calibrated = 0;
    CHECK(safety_request_arm() == SAFETY_ARM_ERR_POWER);
    s_params.safety.calibrated = 1;
    s_output[0] = 0;
    flight_end_session();
    hello();
    bf_frame_t stale = control();
    bf_put_u16(stale.payload + 4, 100);
    rejected = flight_rejected_frames();
    receive(stale);
    CHECK(flight_rejected_frames() == rejected + 1);
    request(BF_ARM, 7);
    CHECK(safety_state() != SAFETY_ARMED);
    stale = control();
    bf_put_u32(stale.payload + 8, 0xdeadbeef);
    receive(stale);
    request(BF_ARM, 8);
    CHECK(safety_state() != SAFETY_ARMED);
    receive(control());
    bf_frame_t old_arm = { .type = BF_ARM, .session = s_session - 1,
        .sequence = ++s_sequence, .request = 9 };
    receive(old_arm);
    CHECK(safety_state() != SAFETY_ARMED);
    puts("flight/safety injection tests passed");
    printf("%u checks\n", s_checks);
    return 0;
}
