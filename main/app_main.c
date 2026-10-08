#include "board.h"
#include "bf_console.h"
#include "bf_fault.h"
#include "parameter.h"
#include "safety.h"
#include "sensors.h"
#include "servo.h"
#include "telemetry.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

#define CONTROL_TASK_STACK  4096
#define CONTROL_TASK_PRIO   8
#define MONITOR_TASK_STACK  4096
#define MONITOR_TASK_PRIO   5
#define TELEMETRY_TASK_STACK 4096
#define TELEMETRY_TASK_PRIO 3

#define MONITOR_PERIOD_MS   10   /* sensor sampling, ~100 Hz */
#define TELEMETRY_PERIOD_MS 50   /* ~20 Hz snapshot */

/* Servo frame / control tick period in microseconds (~333 Hz). The PLAN forbids using
 * the RTOS tick as the 1/333 s time base, so the tick is driven by a high-resolution
 * esp_timer that notifies the control task. */
#define CONTROL_PERIOD_US   3003

static TaskHandle_t s_control_task;
static uint32_t s_period_us_last;
static uint32_t s_period_us_max;
static uint32_t s_overruns;
static bool s_param_corrupt_reported;

static void control_timer_cb(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_control_task, &hp);
    portYIELD_FROM_ISR(hp);
}

static void wdt_subscribe_self(void)
{
    esp_task_wdt_add(NULL);
}

static void publish_telemetry(void)
{
    telemetry_snapshot_t s = { 0 };
    s.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s.safety_state = safety_state();
    s.faults = safety_faults();
    s.armed = (safety_state() == SAFETY_ARMED);
    s.servo_enabled = servo_is_enabled();

    sensors_reading_t r;
    sensors_get_latest(&r);
    s.vbat_mv = r.vbat_mv;
    s.vbat_valid = r.vbat_valid;
    s.iservo_ma = r.iservo_ma;
    s.iservo_valid = r.iservo_valid;
    s.adc_saturated = r.saturated;

    for (int i = 0; i < TELEMETRY_SERVO_COUNT; i++) {
        servo_idx_t idx = (servo_idx_t)i;
        s.servo_calibrated[i] = servo_calibration_valid(idx);
        servo_get_target_deg(idx, &s.servo_target_deg[i]);
        servo_get_output_deg(idx, &s.servo_output_deg[i]);
        float pulse = 0.0f;
        if (servo_get_pulse_us(idx, &pulse) == ESP_OK) {
            s.servo_pulse_us[i] = pulse;
        }
    }

    s.sensors_calibrated = sensors_calibration_valid();
    s.params_valid = parameter_is_valid();
    s.control_dt_us = s_period_us_last;
    s.control_dt_max_us = s_period_us_max;
    s.control_overruns = s_overruns;

    telemetry_publish(&s);
}

static void control_task(void *arg)
{
    (void)arg;
    wdt_subscribe_self();

    s_control_task = xTaskGetCurrentTaskHandle();

    esp_timer_create_args_t timer_args = {
        .callback = control_timer_cb,
        .arg = NULL,
        .name = "ctrl_tick",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, CONTROL_PERIOD_US));

    int64_t last = esp_timer_get_time();

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t now = esp_timer_get_time();
        uint32_t period_us = (uint32_t)(now - last);
        last = now;
        s_period_us_last = period_us;
        if (period_us > s_period_us_max) {
            s_period_us_max = period_us;
        }
        /* A tick that takes more than two periods means we are not keeping up. */
        if (period_us > 2 * CONTROL_PERIOD_US) {
            s_overruns++;
        }

        float dt = (float)period_us / 1e6f;
        safety_tick(dt);
        servo_update(dt);

        esp_task_wdt_reset();
    }
}

/* Evaluate the power path and raise/lower the voltage faults with hysteresis:
 * enter at crit/max, recover at warn. */
static void evaluate_power(const sensors_reading_t *r)
{
    const bf_safety_cfg_t *cfg = &parameter_get()->safety;
    bool ok = false;

    if (r->vbat_valid && !r->saturated) {
        if (r->vbat_mv < cfg->vbat_crit_mv) {
            safety_raise_fault(BF_FAULT_UNDERVOLTAGE, "pack below critical");
            ok = false;
        } else if (r->vbat_mv > cfg->vbat_max_mv) {
            safety_raise_fault(BF_FAULT_OVERVOLTAGE, "pack above maximum");
            ok = false;
        } else {
            if (r->vbat_mv > cfg->vbat_warn_mv) {
                safety_lower_fault(BF_FAULT_UNDERVOLTAGE);
            }
            safety_lower_fault(BF_FAULT_OVERVOLTAGE);
            ok = !safety_has_fault(BF_FAULT_UNDERVOLTAGE);
        }
    }

    if (r->saturated) {
        safety_raise_fault(BF_FAULT_ADC_SATURATED, "analog channel saturated");
        ok = false;
    } else {
        safety_lower_fault(BF_FAULT_ADC_SATURATED);
    }

    /* Without calibrated sensors the power path cannot be trusted, so arming stays
     * blocked even though no voltage fault is raised. */
    if (!sensors_calibration_valid()) {
        ok = false;
    }

    safety_set_power_ok(ok);
}

static void monitor_task(void *arg)
{
    (void)arg;
    wdt_subscribe_self();

    TickType_t last = xTaskGetTickCount();
    while (true) {
        sensors_sample();
        sensors_reading_t r;
        sensors_get_latest(&r);
        evaluate_power(&r);

        if (!parameter_is_valid()) {
            if (!s_param_corrupt_reported) {
                telemetry_log_event(BF_FAULT_PARAM_CORRUPT, "stored params unusable; defaults active");
                s_param_corrupt_reported = true;
            }
            safety_raise_fault(BF_FAULT_PARAM_CORRUPT, "stored params unusable");
        } else {
            safety_lower_fault(BF_FAULT_PARAM_CORRUPT);
        }

        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;
    while (true) {
        publish_telemetry();
        if (++tick % 20 == 0) { /* ~1 Hz snapshot to the log */
            telemetry_snapshot_t s;
            telemetry_get(&s);
            telemetry_log_snapshot(&s);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

static bool run_self_check(void)
{
    bool ok = true;

    if (!parameter_is_valid()) {
        ESP_LOGW(TAG, "self-check: stored parameters unusable, defaults in effect");
    }
    if (!servo_calibration_valid(SERVO_LEFT) || !servo_calibration_valid(SERVO_RIGHT)) {
        ESP_LOGW(TAG, "self-check: servo calibration missing; arming will be refused");
    }
    if (!sensors_calibration_valid()) {
        ESP_LOGW(TAG, "self-check: sensor calibration missing; power path marked invalid");
    }
    if (!board_caps()->has_servo_power_switch) {
        ESP_LOGW(TAG, "self-check: NO_POWER_CUTOFF - PWM off does not remove servo power");
    }

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT) {
        ESP_LOGE(TAG, "self-check: abnormal previous reset (%s)", board_reset_reason_str());
        telemetry_log_event(BF_FAULT_RESET_ABNORMAL, board_reset_reason_str());
    }

    /* Hard failures would set ok = false here; none exist yet at this stage. */
    return ok;
}

static void init_watchdog(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 3000,
        .idle_core_mask = 1 << 0,
        .trigger_panic = true,
    };
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&cfg));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BFCopilot starting (%s rev %s)", board_caps()->hw_name,
             board_caps()->hw_revision);

    board_init();
    parameter_init();
    telemetry_init();
    servo_init();
    sensors_init();

    servo_reload_params();
    safety_init();

    init_watchdog();

    bool self_ok = run_self_check();
    safety_boot_complete(self_ok);

    xTaskCreate(control_task, "control", CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIO, NULL);
    xTaskCreate(monitor_task, "monitor", MONITOR_TASK_STACK, NULL, MONITOR_TASK_PRIO, NULL);
    xTaskCreate(telemetry_task, "telemetry", TELEMETRY_TASK_STACK, NULL, TELEMETRY_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "started, state=%s", safety_state_str(safety_state()));
    ESP_ERROR_CHECK(bf_console_start());
}
