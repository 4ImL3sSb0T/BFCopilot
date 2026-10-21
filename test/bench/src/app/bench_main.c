/*
 * Hardware test firmware for the butterfly PCB (ESP32-C6).
 *
 * It deliberately runs none of the flight logic: no safety state machine, no session, no
 * arming, no control loop, no telemetry publishing. The board is being brought up part by
 * part, so nothing here may be gated on an arming or fault state - the console drives the
 * hardware directly, and the only thing that fails is the bus or the chip itself.
 *
 * The one exception is the flapping service (`flap/`): it generates the product's own
 * trajectory and pushes it through the calibrated servo path, but only while a `flap`
 * command asks it to, and it consults no arming or fault state to do so.
 *
 * Not the flight firmware. Never flash it on a vehicle that is meant to fly, and never
 * leave it driving servos unattended.
 */
#include "esp_log.h"

#include "board.h"
#include "bf_runtime.h"
#include "flap.h"
#include "hwtest.h"
#include "net.h"
#include "parameter.h"
#include "sensors.h"
#include "servo.h"

static const char *TAG = "bench";

void app_main(void)
{
    bf_runtime_init();
    ESP_LOGI(TAG, "hardware test firmware on %s rev %s", board_caps()->hw_name,
             board_caps()->hw_revision);
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(parameter_init());
    ESP_ERROR_CHECK(servo_init(parameter_get()->servo));
    ESP_ERROR_CHECK(sensors_init(&parameter_get()->sensors));
    servo_reload_params();

    /* The flapping service runner. It holds no pulse until a `flap` command arrives. */
    ESP_ERROR_CHECK(bf_flap_init());

    /* Wi-Fi is the preferred link, but it must never keep the board from coming up:
     * unconfigured credentials or an unreachable AP fall back to the USB-Serial-JTAG
     * console, which is always running. */
    esp_err_t link_err = bf_net_wifi_start();
    if (link_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi link unavailable (%s); using the USB serial console",
                 esp_err_to_name(link_err));
    }
    ESP_ERROR_CHECK(bf_net_console_init());
    ESP_ERROR_CHECK(bf_hwtest_register());

    ESP_LOGI(TAG, "ready; type 'hw' for the hardware commands");
}
