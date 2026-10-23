#include "board.h"
#include "bf_console.h"
#include "bf_fault.h"
#include "parameter.h"
#include "safety.h"
#include "sensors.h"
#include "servo.h"
#include "telemetry.h"
#include "bf_runtime.h"
#include "bf_radio.h"
#include "flight.h"

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

/* Control tick period comes from system.control_period_us (200 Hz / 5000 us by default,
 * the same frame as the servo PWM). The PLAN forbids using the RTOS tick as the time base,
 * so the tick is driven by a high-resolution esp_timer that notifies the control task. */
#define CONTROL_PERIOD_FALLBACK_US 5000

static TaskHandle_t s_control_task;
static uint32_t s_period_us_last;
static uint32_t s_period_us_max;
static uint32_t s_overruns;
static bool s_param_corrupt_reported;

static void control_timer_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_control_task);
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
    s.imu_valid = r.imu_valid;
    s.imu_sample_us = r.imu_sample_us;
    for (unsigned axis = 0; axis < 3; axis++) {
        s.accel_mg[axis] = r.accel_mg[axis];
        s.gyro_mdps[axis] = r.gyro_mdps[axis];
    }
    s.baro_valid = r.baro_valid;
    s.pressure_pa = r.pressure_pa;
    s.temperature_mdegc = r.temperature_mdegc;
    s.baro_sample_us = r.baro_sample_us;

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
    s.flap_phase_deg = flight_phase_deg();
    s.rx_age_ms = flight_rx_age_ms();
    s.dropped_frames = flight_rejected_frames();

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

    uint32_t target_period_us = parameter_get()->system.control_period_us;
    if (target_period_us == 0) {
        target_period_us = CONTROL_PERIOD_FALLBACK_US;
    }
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, target_period_us));

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
        if (period_us > 2 * target_period_us) {
            s_overruns++;
        }

        float dt = (float)period_us / 1e6f;
        bf_runtime_lock();
        if (period_us > 2 * target_period_us) {
            if (servo_is_enabled() || safety_state() == SAFETY_ARMED || flight_session_active()) {
                safety_raise_fault(BF_FAULT_TASK_WDT, "control deadline missed");
                flight_end_session();
            }
        } else {
            safety_lower_fault(BF_FAULT_TASK_WDT);
            flight_tick(dt);
        }
        safety_tick(dt);
        if (servo_update(dt > 0.02f ? 0.02f : dt) != ESP_OK) {
            safety_raise_fault(BF_FAULT_INTERNAL, "servo update failed");
        }
        bf_runtime_unlock();

        esp_task_wdt_reset();
    }
}

/* Evaluate the power path and raise/lower the voltage faults with hysteresis:
 * enter at crit/max, recover at warn. */
static void evaluate_power(const sensors_reading_t *r)
{
    const bf_safety_cfg_t *cfg = &parameter_get()->safety;
    bool ok = false;

    if (cfg->calibrated && r->vbat_valid && !r->saturated) {
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

    bool reliable = r->vbat_valid && r->iservo_valid && !r->saturated;
    if (sensors_calibration_valid() && !reliable) {
        safety_raise_fault(BF_FAULT_ADC_UNRELIABLE, "power measurement invalid");
    } else {
        safety_lower_fault(BF_FAULT_ADC_UNRELIABLE);
    }
    if (cfg->calibrated && r->iservo_valid &&
        (r->iservo_ma > cfg->iservo_max_ma || r->iservo_peak_ma > cfg->iservo_max_ma)) {
        safety_raise_fault(BF_FAULT_SERVO_OVERCURRENT, "servo branch above limit");
        ok = false;
    } else {
        safety_lower_fault(BF_FAULT_SERVO_OVERCURRENT);
    }
    safety_set_power_ok(ok && reliable && cfg->calibrated);
}

static void monitor_task(void *arg)
{
    (void)arg;
    wdt_subscribe_self();

    TickType_t last = xTaskGetTickCount();
    while (true) {
        sensors_sample_motion();
        bf_runtime_lock();
        sensors_sample();
        sensors_reading_t r;
        sensors_get_latest(&r);
        evaluate_power(&r);
        /* Open-loop build: no flight path consumes attitude or pressure, so a missing
         * sensor is reported through telemetry and the log, never latched as a fault. */
        safety_lower_fault(BF_FAULT_IMU_LOST);
        safety_lower_fault(BF_FAULT_BARO_LOST);

        if (!parameter_is_valid()) {
            if (!s_param_corrupt_reported) {
                telemetry_log_event(BF_FAULT_PARAM_CORRUPT, "stored params unusable; defaults active");
                s_param_corrupt_reported = true;
            }
            safety_raise_fault(BF_FAULT_PARAM_CORRUPT, "stored params unusable");
        } else {
            safety_lower_fault(BF_FAULT_PARAM_CORRUPT);
        }
        bf_runtime_unlock();

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
        bf_runtime_lock();
        publish_telemetry();
        bf_runtime_unlock();
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
    sensors_reading_t reading = {0};
    int64_t deadline = esp_timer_get_time() + 400000;
    do {
        sensors_sample_motion();
        sensors_get_latest(&reading);
        if ((!board_caps()->has_imu || reading.imu_valid) &&
            (!board_caps()->has_baro || reading.baro_valid)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
    if (board_caps()->has_imu && !reading.imu_valid) {
        ESP_LOGW(TAG, "self-check: IMU unavailable (open-loop build; attitude features disabled)");
    }
    if (board_caps()->has_baro && !reading.baro_valid) {
        ESP_LOGW(TAG, "self-check: barometer unavailable (open-loop build; altitude features disabled)");
    }

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

    bool sensors_ready = (!board_caps()->has_imu || reading.imu_valid) &&
                         (!board_caps()->has_baro || reading.baro_valid);
    ESP_LOGI(TAG, "self-check: motion sensors %s",
             sensors_ready ? "ready" : "unavailable (open loop)");
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
    bf_runtime_init();
    ESP_LOGI(TAG, "BFCopilot starting (%s rev %s)", board_caps()->hw_name,
             board_caps()->hw_revision);

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(parameter_init());
    ESP_ERROR_CHECK(telemetry_init());
    ESP_ERROR_CHECK(servo_init(parameter_get()->servo));
    ESP_ERROR_CHECK(sensors_init(&parameter_get()->sensors));

    servo_reload_params();
    ESP_ERROR_CHECK(safety_init());
    flight_init();
    esp_err_t radio_err = bf_radio_init();
    if (radio_err != ESP_OK && radio_err != ESP_ERR_NOT_SUPPORTED) {
        safety_raise_fault(BF_FAULT_INTERNAL, "radio provisioning invalid");
    }

    init_watchdog();

    bool self_ok = run_self_check();
    safety_boot_complete(self_ok);

    ESP_ERROR_CHECK(xTaskCreate(control_task, "control", CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIO, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(monitor_task, "monitor", MONITOR_TASK_STACK, NULL, MONITOR_TASK_PRIO, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(telemetry_task, "telemetry", TELEMETRY_TASK_STACK, NULL, TELEMETRY_TASK_PRIO, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "started, state=%s", safety_state_str(safety_state()));
    ESP_ERROR_CHECK(bf_console_start());
}
