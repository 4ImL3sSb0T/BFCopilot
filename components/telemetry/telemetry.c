#include "telemetry.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "telemetry";

static telemetry_snapshot_t s_snapshot;
static telemetry_event_t s_events[TELEMETRY_EVENT_LOG_LEN];
static size_t s_event_head; /* next write index */
static size_t s_event_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t telemetry_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_events, 0, sizeof(s_events));
    s_event_head = 0;
    s_event_count = 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void telemetry_publish(const telemetry_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_snapshot = *snapshot;
    portEXIT_CRITICAL(&s_lock);
}

void telemetry_get(telemetry_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

void telemetry_log_event(bf_fault_bit_t fault, const char *detail)
{
    telemetry_event_t ev = {
        .uptime_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .fault = fault,
    };
    if (detail != NULL) {
        strncpy(ev.detail, detail, sizeof(ev.detail) - 1);
    }

    portENTER_CRITICAL(&s_lock);
    s_events[s_event_head] = ev;
    s_event_head = (s_event_head + 1) % TELEMETRY_EVENT_LOG_LEN;
    if (s_event_count < TELEMETRY_EVENT_LOG_LEN) {
        s_event_count++;
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "event %" PRIu64 "ms %s: %s", ev.uptime_ms,
             bf_fault_name(fault), ev.detail);
}

size_t telemetry_get_events(telemetry_event_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    size_t count = (s_event_count < max) ? s_event_count : max;
    size_t start = (s_event_head + TELEMETRY_EVENT_LOG_LEN - count) % TELEMETRY_EVENT_LOG_LEN;
    for (size_t i = 0; i < count; i++) {
        out[i] = s_events[(start + i) % TELEMETRY_EVENT_LOG_LEN];
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

void telemetry_log_snapshot(const telemetry_snapshot_t *s)
{
    if (s == NULL) {
        return;
    }
    char faults[128];
    bf_fault_mask_str(s->faults, faults, sizeof(faults));
    ESP_LOGI(TAG,
             "state=%" PRId32 " armed=%d vbat=%" PRId32 "mV(%d) iservo=%" PRId32 "mA(%d) "
             "faults=%s sat=%d dt=%" PRIu32 "us max=%" PRIu32 "us overruns=%" PRIu32,
             s->safety_state, s->armed, s->vbat_mv, s->vbat_valid, s->iservo_ma,
             s->iservo_valid, faults, s->adc_saturated, s->control_dt_us,
             s->control_dt_max_us, s->control_overruns);
    ESP_LOGI(TAG, "imu=%d accel=[%d,%d,%d]mg gyro=[%" PRId32 ",%" PRId32 ",%" PRId32 "]mdps "
             "baro=%d pressure=%" PRId32 "Pa temperature=%" PRId32 "mdegC",
             s->imu_valid, s->accel_mg[0], s->accel_mg[1], s->accel_mg[2],
             s->gyro_mdps[0], s->gyro_mdps[1], s->gyro_mdps[2], s->baro_valid,
             s->pressure_pa, s->temperature_mdegc);
}
