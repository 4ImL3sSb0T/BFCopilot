#include "bf_console.h"
#include "bf_runtime.h"
#include "flight.h"
#include <errno.h>
#include <math.h>

#include "board.h"
#include "parameter.h"
#include "safety.h"
#include "sensors.h"
#include "servo.h"
#include "telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "console";

static const char *servo_token(servo_idx_t s)
{
    return (s == SERVO_LEFT) ? "l" : "r";
}

static int parse_servo(const char *tok, servo_idx_t *out)
{
    if (strcmp(tok, "l") == 0 || strcmp(tok, "left") == 0) {
        *out = SERVO_LEFT;
        return 1;
    }
    if (strcmp(tok, "r") == 0 || strcmp(tok, "right") == 0) {
        *out = SERVO_RIGHT;
        return 1;
    }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    telemetry_snapshot_t s;
    telemetry_get(&s);
    char faults[160];
    bf_fault_mask_str(s.faults, faults, sizeof(faults));

    printf("safety   : %s (armed=%d)\n", safety_state_str(safety_state()), s.armed);
    printf("faults   : %s\n", faults);
    printf("power    : vbat=%ld mV(%s) iservo=%ld mA(%s) sat=%d\n",
           (long)s.vbat_mv, s.vbat_valid ? "ok" : "invalid",
           (long)s.iservo_ma, s.iservo_valid ? "ok" : "invalid", s.adc_saturated);
    printf("servos   : enabled=%d\n", s.servo_enabled);
    for (int i = 0; i < TELEMETRY_SERVO_COUNT; i++) {
        printf("  %s: cal=%d target=%.2f out=%.2f pulse=%.1f us\n",
               servo_token((servo_idx_t)i), s.servo_calibrated[i],
               s.servo_target_deg[i], s.servo_output_deg[i], s.servo_pulse_us[i]);
    }
    printf("sensors  : calibrated=%d  params_valid=%d\n",
           s.sensors_calibrated, s.params_valid);
    printf("timing   : uptime=%lu ms  dt=%lu us  max=%lu us  overruns=%lu\n",
           (unsigned long)s.uptime_ms, (unsigned long)s.control_dt_us,
           (unsigned long)s.control_dt_max_us, (unsigned long)s.control_overruns);
    return 0;
}

static int cmd_params(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < parameter_count(); i++) {
        const bf_param_desc_t *d = parameter_descriptor(i);
        double v = 0;
        parameter_get_scalar(d->id, &v);
        printf("%-28s %10.3f %-5s [%.3f, %.3f]%s\n",
               d->name, v, d->unit, d->min, d->max, d->calibration ? " cal" : "");
    }
    return 0;
}

static int cmd_param(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "get") != 0 &&
        (servo_is_enabled() || safety_state() == SAFETY_ARMED ||
         safety_state() == SAFETY_SAFE_RETURN_LOCKED || flight_session_active())) {
        printf("refused: disable outputs and close radio session before changing parameters\n");
        return 1;
    }
    if (argc < 2) {
        printf("usage: param get <name> | param set <name> <value> | param reset\n");
        return 1;
    }
    if (strcmp(argv[1], "reset") == 0) {
        esp_err_t err = parameter_reset_to_defaults();
        servo_reload_params();
        sensors_reload_params();
        printf("reset: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 3) {
        printf("usage: param get <name> | param set <name> <value>\n");
        return 1;
    }
    const bf_param_desc_t *d = parameter_find(argv[2]);
    if (d == NULL) {
        printf("unknown parameter '%s'\n", argv[2]);
        return 1;
    }
    if (strcmp(argv[1], "get") == 0) {
        double v = 0;
        parameter_get_scalar(d->id, &v);
        printf("%s = %.3f %s\n", d->name, v, d->unit);
        return 0;
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            printf("usage: param set <name> <value>\n");
            return 1;
        }
        if (d->calibration && safety_state() == SAFETY_ARMED) {
            printf("refused: parameters are read-only while armed\n");
            return 1;
        }
        char *end;
        errno = 0;
        double v = strtod(argv[3], &end);
        if (errno || end == argv[3] || *end || !isfinite(v)) return 1;
        esp_err_t err = parameter_set_scalar(d->id, v);
        if (err != ESP_OK) {
            printf("set failed: %s (out of range or invalid)\n", esp_err_to_name(err));
            return 1;
        }
        servo_reload_params();
        sensors_reload_params();
        printf("%s = %.3f %s\n", d->name, v, d->unit);
        return 0;
    }
    printf("usage: param get|set ...\n");
    return 1;
}

static int cmd_events(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    telemetry_event_t evs[TELEMETRY_EVENT_LOG_LEN];
    size_t n = telemetry_get_events(evs, TELEMETRY_EVENT_LOG_LEN);
    for (size_t i = 0; i < n; i++) {
        printf("%8llu ms  %-18s %s\n", (unsigned long long)evs[i].uptime_ms,
               bf_fault_name(evs[i].fault), evs[i].detail);
    }
    if (n == 0) {
        printf("(no events)\n");
    }
    return 0;
}

static int cmd_servo(int argc, char **argv)
{
    if (flight_session_active() || safety_state() == SAFETY_ARMED ||
        safety_state() == SAFETY_SAFE_RETURN_LOCKED || safety_estop_latched() ||
        safety_state() == SAFETY_FAULT_LATCHED) {
        if (argc > 1 && strcmp(argv[1], "off") == 0) {
            safety_emergency_stop();
            flight_end_session();
            servo_force_output_off();
            return 0;
        }
        printf("refused: servo jog requires locked bench mode without faults\n");
        return 1;
    }
    if (argc < 2) {
        printf("usage: servo <l|r|both> <deg> | servo enable | servo disable | servo off\n");
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        servo_force_output_off();
        printf("output off\n");
        return 0;
    }
    if (strcmp(argv[1], "enable") == 0) {
        esp_err_t err = servo_enable(true);
        printf("output enable: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "disable") == 0) {
        servo_enable(false);
        printf("output disabled\n");
        return 0;
    }
    if (safety_state() == SAFETY_ARMED) {
        printf("refused: disarm before jogging servos\n");
        return 1;
    }
    if (argc < 3) {
        printf("usage: servo <l|r|both> <deg>\n");
        return 1;
    }

    char *end;
    errno = 0;
    float deg = strtof(argv[2], &end);
    if (errno || end == argv[2] || *end || !isfinite(deg)) return 1;
    if (strcmp(argv[1], "both") == 0) {
        servo_set_target_deg(SERVO_LEFT, deg);
        servo_set_target_deg(SERVO_RIGHT, deg);
    } else {
        servo_idx_t s;
        if (!parse_servo(argv[1], &s)) {
            printf("unknown servo '%s'\n", argv[1]);
            return 1;
        }
        servo_set_target_deg(s, deg);
    }
    if (!servo_is_enabled()) {
        printf("note: output not enabled; run 'servo enable' first\n");
    }
    return 0;
}

static int cmd_session(int argc, char **argv)
{
    if (argc < 2) {
        printf("session is %s\n", safety_session_established() ? "on" : "off");
        return 0;
    }
    if (strcmp(argv[1], "off") != 0) {
        printf("session on requires authenticated radio HELLO\n");
        return 1;
    }
    flight_end_session();
    return 0;
}

static int cmd_arm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("arm requires a fresh radio command and explicit remote ARM\n");
    return 1;
}

static int cmd_disarm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = safety_disarm();
    printf("disarm: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_estop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    safety_emergency_stop();
    flight_end_session();
    printf("emergency stop latched\n");
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = safety_clear_faults();
    printf("clear: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static const esp_console_cmd_t s_cmds[] = {
    { .command = "status",  .help = "Show the current telemetry snapshot", .func = cmd_status },
    { .command = "params",  .help = "List all parameters",                 .func = cmd_params },
    { .command = "param",   .help = "Get/set/reset a parameter",           .func = cmd_param },
    { .command = "events",  .help = "Show recent fault events",            .func = cmd_events },
    { .command = "servo",   .help = "Jog a servo angle (bench only)",      .func = cmd_servo },
    { .command = "session", .help = "Set/clear the control session",       .func = cmd_session },
    { .command = "arm",     .help = "Request arming",                      .func = cmd_arm },
    { .command = "disarm",  .help = "Disarm and return to safe angle",     .func = cmd_disarm },
    { .command = "estop",   .help = "Latched emergency stop",              .func = cmd_estop },
    { .command = "clear",   .help = "Clear resolved faults",               .func = cmd_clear },
};

static int dispatch(int argc, char **argv)
{
    if (argc == 0) return 1;
    bool read_only = strcmp(argv[0], "status") == 0 || strcmp(argv[0], "params") == 0 ||
                     strcmp(argv[0], "events") == 0;
    if (!read_only) bf_runtime_lock();
    int result = 1;
    for (size_t index = 0; index < sizeof(s_cmds) / sizeof(s_cmds[0]); index++) {
        if (strcmp(argv[0], s_cmds[index].command) == 0) {
            result = s_cmds[index].func(argc, argv);
            break;
        }
    }
    if (!read_only) bf_runtime_unlock();
    return result;
}

esp_err_t bf_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "bf> ";
    repl_cfg.max_cmdline_length = 160;
    repl_cfg.max_cmdline_args = 8;
    repl_cfg.task_stack_size = 4096;

    esp_err_t err = esp_console_register_help_command();
    if (err != ESP_OK) {
        return err;
    }

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t dev_cfg = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&dev_cfg, &repl_cfg, &repl);
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "repl create failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        esp_console_cmd_t command = s_cmds[i];
        command.func = dispatch;
        err = esp_console_cmd_register(&command);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", s_cmds[i].command, esp_err_to_name(err));
            return err;
        }
    }

    return esp_console_start_repl(repl);
}
