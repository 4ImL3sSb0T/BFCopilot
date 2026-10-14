#include "board.h"

#include "board_buzzer.h"
#include "board_pins.h"
#include "board_status_led.h"

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "board";

static const board_caps_t s_caps = {
    .hw_name = BOARD_HW_NAME,
    .hw_revision = BOARD_HW_REVISION,
    .has_servo_power_switch = false,
    .has_imu = true,        /* fitted per hand-off BOM; verify on bench */
    .has_baro = true,       /* fitted per hand-off BOM; verify on bench */
    .has_vbat_adc = true,
    .has_current_adc = true,
    .has_ws2812 = true,
    .has_buzzer = true,
    .usb_native = false,    /* module/USB path not verified yet */
};

static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;

const board_caps_t *board_caps(void)
{
    return &s_caps;
}

esp_err_t board_servo_power_enable(void)
{
    /* No servo power MOS on this board. Never fake it by stopping PWM. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_servo_power_disable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

board_servo_power_t board_servo_power_state(void)
{
    return BOARD_SERVO_POWER_UNSUPPORTED;
}

const char *board_reset_reason_str(void)
{
    switch (s_reset_reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

esp_err_t board_init(void)
{
    s_reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "board %s rev %s, reset reason: %s",
             s_caps.hw_name, s_caps.hw_revision, board_reset_reason_str());

    if (!s_caps.has_servo_power_switch) {
        ESP_LOGW(TAG, "no servo power switch: PWM off does NOT remove servo power");
    }

    esp_err_t err = board_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status LED init failed: %s", esp_err_to_name(err));
    } else {
        board_led_set(BOARD_LED_BOOT);
    }

    err = board_buzzer_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "buzzer init failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}
