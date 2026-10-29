/*
 * BFCopilot bring-up app.
 *
 * 当前阶段只做板级自检：按顺序初始化 BSP 各模块，然后周期采样并打印。
 * 没有任何飞行逻辑，也不会自动给舵机上电——bsp_servo_init() 之后舵机电源
 * 保持关断、两路输出没有脉冲。
 */
#include "bsp_adc.h"
#include "bsp_baro.h"
#include "bsp_buzzer.h"
#include "bsp_imu.h"
#include "bsp_servo.h"
#include "bsp_com.h"
#include "service/melody/melody.h"
#include "service/melody/melody_songs.h"
#include "net_test.h"

#include "lib/tools/common_def.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "app";

// 上电自检：顺序放一遍内置曲库，确认蜂鸣器能按音高发声
static void melody_test_task(void *arg) {
    (void)arg;
    for (size_t i = 0; i < melody_song_count; i++) {
        ESP_LOGI(TAG, "melody %u/%u: %s", (unsigned)(i + 1), (unsigned)melody_song_count,
                 melody_songs[i].name);
        melody_play(melody_songs[i].notes, melody_songs[i].count);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}

static void report_init(const char *name, esp_err_t rc) {
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "%s init ok", name);
    } else {
        ESP_LOGW(TAG, "%s init failed: %s", name, esp_err_to_name(rc));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "BFCopilot bring-up app (no flight logic)");

    // 舵机先初始化：把 SERVO_EN 拉低，确保上电阶段舵机不带电
    report_init("servo", bsp_servo_init());
    esp_err_t buzzer_rc = bsp_buzzer_init();
    report_init("buzzer", buzzer_rc);
    report_init("adc", bsp_adc_init());
    report_init("imu", bsp_imu_init());
    report_init("baro", bsp_baro_init());

#if CONFIG_BF_WIFI_ENABLED
    esp_err_t wifi_rc = bsp_wifi_init();
    report_init("wifi", wifi_rc);
    if (wifi_rc == ESP_OK) {
        wifi_rc = bsp_wifi_connect(CONFIG_BF_WIFI_CONNECT_TIMEOUT_MS);
        if (wifi_rc == ESP_OK) {
            bsp_wifi_status_t status;
            bsp_wifi_get_status(&status);
            ESP_LOGI(TAG, "wifi connected, ip %s", status.ip);
#if CONFIG_BF_NET_TEST_ENABLED
            report_init("net test", app_net_test_start());
#endif
        } else {
            ESP_LOGW(TAG, "wifi connect failed: %s", esp_err_to_name(wifi_rc));
        }
    }
#endif

    u32 tick = 0;
    for (;;) {
        // 返回 NOT_FINISHED 表示本轮还没有新样本，跳过即可，不影响下一轮
        bsp_imu_sample_t imu;
        if (bsp_imu_read(&imu) == ESP_OK) {
            ESP_LOGD(TAG, "accel %.2f %.2f %.2f g | gyro %.1f %.1f %.1f dps",
                     imu.accel_g.x, imu.accel_g.y, imu.accel_g.z,
                     imu.gyro_dps.x, imu.gyro_dps.y, imu.gyro_dps.z);
        }

        bsp_baro_sample_t baro;
        if (bsp_baro_read(&baro) == ESP_OK) {
            ESP_LOGD(TAG, "baro %.0f Pa %.2f C", baro.pressure_pa, baro.temperature_c);
        }

        // 电压/电流变化慢，每 10 轮（约 1 s）打印一次就够，避免刷屏
        if (++tick % 10 == 0) {
            u16 vbat_mv = 0;
            u16 iservo_mv = 0;
            bsp_adc_read_mv(BSP_ADC_VBAT, &vbat_mv);
            bsp_adc_read_mv(BSP_ADC_ISERVO, &iservo_mv);
            ESP_LOGI(TAG, "vbat=%u mV iservo=%u mV", vbat_mv, iservo_mv);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
