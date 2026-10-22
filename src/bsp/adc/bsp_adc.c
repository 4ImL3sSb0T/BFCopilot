#include "bsp_adc.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define BSP_ADC_UNIT            ADC_UNIT_1
#define BSP_ADC_ATTEN           ADC_ATTEN_DB_12
#define BSP_ADC_BITWIDTH        ADC_BITWIDTH_DEFAULT
#define BSP_ADC_AVERAGE_SAMPLES 8

static const char *TAG = "bsp_adc";

static const adc_channel_t s_channel[BSP_ADC_COUNT] = {
    [BSP_ADC_VBAT] = ADC_CHANNEL_0,
    [BSP_ADC_ISERVO] = ADC_CHANNEL_1,
};

static adc_oneshot_unit_handle_t s_unit;
// C6 支持逐通道曲线拟合校准；建立失败时退回线性换算，并在日志里说明。
static adc_cali_handle_t s_cali[BSP_ADC_COUNT];
static bool s_ready;

static bool bsp_adc_valid(BSP_ADC_CH ch) {
    return ch >= 0 && ch < BSP_ADC_COUNT;
}

esp_err_t bsp_adc_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BSP_ADC_UNIT,
    };
    esp_err_t rc = adc_oneshot_new_unit(&unit_config, &s_unit);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(rc));
        s_unit = NULL;
        return rc;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = BSP_ADC_ATTEN,
        .bitwidth = BSP_ADC_BITWIDTH,
    };
    for (int ch = 0; ch < BSP_ADC_COUNT; ch++) {
        s_cali[ch] = NULL;
        rc = adc_oneshot_config_channel(s_unit, s_channel[ch], &chan_config);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "channel %d config failed", ch);
            adc_oneshot_del_unit(s_unit);
            s_unit = NULL;
            return rc;
        }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = BSP_ADC_UNIT,
            .chan = s_channel[ch],
            .atten = BSP_ADC_ATTEN,
            .bitwidth = BSP_ADC_BITWIDTH,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali[ch]) != ESP_OK) {
            s_cali[ch] = NULL;
            ESP_LOGW(TAG, "channel %d calibration unavailable, using linear conversion", ch);
        }
#endif
    }

    s_ready = true;
    ESP_LOGI(TAG, "ADC1 ready: VBAT=GPIO%d, ISERVO=GPIO%d, 12 dB",
             BSP_ADC_VBAT_GPIO, BSP_ADC_ISERVO_GPIO);
    return ESP_OK;
}

esp_err_t bsp_adc_read_raw(BSP_ADC_CH ch, u16 *raw) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_adc_valid(ch) || raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int value = 0;
    esp_err_t rc = adc_oneshot_read(s_unit, s_channel[ch], &value);
    if (rc != ESP_OK) {
        return rc;
    }
    *raw = (u16)value;
    return ESP_OK;
}

esp_err_t bsp_adc_read_mv(BSP_ADC_CH ch, u16 *millivolt) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_adc_valid(ch) || millivolt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    u32 sum = 0;
    for (u32 i = 0; i < BSP_ADC_AVERAGE_SAMPLES; i++) {
        u16 value = 0;
        esp_err_t rc = bsp_adc_read_raw(ch, &value);
        if (rc != ESP_OK) {
            return rc;
        }
        sum += value;
    }
    const int raw = (int)(sum / BSP_ADC_AVERAGE_SAMPLES);

    int voltage = 0;
    if (s_cali[ch] != NULL &&
        adc_cali_raw_to_voltage(s_cali[ch], raw, &voltage) == ESP_OK) {
        *millivolt = (u16)voltage;
        return ESP_OK;
    }
    // 校准不可用时的保守线性换算：12 bit、3.3 V 参考。
    *millivolt = (u16)((u32)raw * 3300u / 4095u);
    return ESP_OK;
}
