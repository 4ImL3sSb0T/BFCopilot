#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bf_fault.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Telemetry: a fixed-size status snapshot plus a bounded event log.
 *
 * Deliberately decoupled from the safety module: the snapshot carries the safety state
 * as a plain integer so the higher-level modules can depend on telemetry without a
 * dependency cycle. Publishing never blocks and never allocates.
 */

#define TELEMETRY_SERVO_COUNT 2
#define TELEMETRY_EVENT_DETAIL_LEN 32
#define TELEMETRY_EVENT_LOG_LEN 32

typedef struct {
    uint32_t uptime_ms;
    int32_t safety_state;
    bf_fault_mask_t faults;
    bool armed;

    int32_t vbat_mv;
    bool vbat_valid;
    int32_t iservo_ma;
    bool iservo_valid;
    bool adc_saturated;

    bool servo_enabled;
    bool servo_calibrated[TELEMETRY_SERVO_COUNT];
    float servo_target_deg[TELEMETRY_SERVO_COUNT];
    float servo_output_deg[TELEMETRY_SERVO_COUNT];
    float servo_pulse_us[TELEMETRY_SERVO_COUNT];

    bool sensors_calibrated;
    bool params_valid;

    uint32_t control_dt_us;
    uint32_t control_dt_max_us;
    uint32_t control_overruns;

    /* Reserved for the wireless link (Phase 4); zero until the link exists. */
    int rssi;
    int32_t rx_age_ms;
    uint32_t dropped_frames;
    float flap_phase_deg;
} telemetry_snapshot_t;

typedef struct {
    uint64_t uptime_ms;
    bf_fault_bit_t fault;
    char detail[TELEMETRY_EVENT_DETAIL_LEN];
} telemetry_event_t;

esp_err_t telemetry_init(void);

/* Copy a snapshot into the shared slot. Safe to call from any task. */
void telemetry_publish(const telemetry_snapshot_t *snapshot);
void telemetry_get(telemetry_snapshot_t *out);

/* Record a fault/event. Cheap and non-blocking. */
void telemetry_log_event(bf_fault_bit_t fault, const char *detail);

/* Copy up to max events, newest last. Returns the number copied. */
size_t telemetry_get_events(telemetry_event_t *out, size_t max);

/* One-line human-readable dump of a snapshot to the log. */
void telemetry_log_snapshot(const telemetry_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
