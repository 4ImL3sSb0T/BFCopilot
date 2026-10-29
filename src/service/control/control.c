#include "control.h"


static bool is_ready = false;
static f32 service_servo_max_speed = 0, service_servo_max_acc = 0;

static void control_task(void* param) {
    
}

esp_err_t control_service_init(f32 servo_max_speed, f32 servo_max_acc) {
    if (is_ready == true) return ESP_OK;
    esp_err_t rc = bsp_servo_init();
    if (rc != ESP_OK) return rc;

    bsp_servo_power_enable(false);

    if (servo_max_speed >= 0 && servo_max_acc >= 0) {
        service_servo_max_speed = servo_max_speed;
        service_servo_max_acc = servo_max_acc;
    }

    is_ready = true;
    return ESP_OK;
}

esp_err_t control_service_excute(control_command_t command);

esp_err_t control_service_stop();

esp_err_t control_service_start();