/*
 * Hardware test firmware for the butterfly PCB (ESP32-C6).
 *
 * It deliberately runs none of the flight logic: no safety state machine, no session, no
 * arming, no control loop, no telemetry publishing. The board is being brought up part by
 * part, so nothing here may be gated on an arming or fault state - the console drives the
 * hardware directly, and the only thing that fails is the bus or the chip itself.
 *
 * Not the flight firmware. Never flash it on a vehicle that is meant to fly, and never
 * leave it driving servos unattended.
 */
#include "esp_log.h"

#include "board.h"
#include "bf_runtime.h"
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

    /* The link first: it only waits for association, which takes far longer than the
     * console setup below. */
    ESP_ERROR_CHECK(bf_net_wifi_start());
    ESP_ERROR_CHECK(bf_net_console_init());
    ESP_ERROR_CHECK(bf_hwtest_register());

    ESP_LOGI(TAG, "ready; type 'hw' for the hardware commands");
}
