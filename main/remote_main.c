#include "bf_radio.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bf_radio_init());
    esp_log_level_set("*", ESP_LOG_NONE);
    uart_config_t config = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &config));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0));
    uint8_t encoded[BF_FRAME_SIZE + 1];
    size_t used = 0;
    bool overflow = false;
    int64_t started = 0;
    while (true) {
        uint8_t incoming[64];
        int count = uart_read_bytes(UART_NUM_0, incoming, sizeof(incoming), pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time();
        if (used && now - started >= 100000) {
            used = 0;
            overflow = true;
        }
        for (int index = 0; index < count; index++) {
            uint8_t value = incoming[index];
            if (value == 0) {
                uint8_t raw[BF_FRAME_SIZE];
                bf_frame_t frame;
                if (!overflow && bf_cobs_decode(encoded, used, raw, sizeof(raw)) == BF_FRAME_SIZE &&
                    bf_frame_decode(raw, sizeof(raw), &frame) && frame.type <= BF_CLEAR) {
                    if (frame.type == BF_CONTROL) {
                        uint16_t age = bf_get_u16(frame.payload + 4);
                        uint32_t elapsed = (uint32_t)((now - started + 999) / 1000);
                        uint32_t total = age + elapsed;
                        bf_put_u16(frame.payload + 4, total > 65535 ? 65535 : (uint16_t)total);
                    }
                    bf_radio_send(&frame);
                }
                used = 0;
                overflow = false;
            } else if (!overflow) {
                if (!used) started = now;
                if (used == sizeof(encoded)) overflow = true;
                else encoded[used++] = value;
            }
        }
        bf_radio_message_t message;
        for (unsigned count = 0; count < 8 && bf_radio_receive(&message); count++) {
            if (message.frame.type != BF_STATUS && message.frame.type != BF_ACK) continue;
            uint8_t raw[BF_FRAME_SIZE];
            uint8_t output[BF_FRAME_SIZE + 2];
            bf_frame_encode(&message.frame, raw);
            size_t length = bf_cobs_encode(raw, sizeof(raw), output, sizeof(output) - 1);
            output[length++] = 0;
            uart_write_bytes(UART_NUM_0, output, length);
        }
    }
}
