/*
 * ADC1 连续采样封装。
 *
 * 初始化后硬件按固定速率一直采，DMA 把结果送进驱动的环形缓冲；读接口只
 * 从缓冲里取最近一个样本。平滑交给芯片内置 IIR 滤波器，不再做软件平均，
 * 避免额外延迟和削掉舵机电流的短时峰值。
 *
 * 只返回 raw / mV，分压比和采样电阻等板级换算留给上层：这些量尚未标定，
 * 写进 BSP 容易被误当成确定值。
 */

#include "bsp_adc.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_filter.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#define BSP_ADC_UNIT            ADC_UNIT_1
#define BSP_ADC_ATTEN           ADC_ATTEN_DB_12
#define BSP_ADC_FRAME_BYTES     256     // 每帧 64 个样本，必须是 4 字节样本的整数倍
#define BSP_ADC_POOL_BYTES      8192    // 池约存 100 ms 数据，够 100 ms 一次的读取周期
// 排空积压时的轮数上限，防止驱动持续产出时循环不退出。
#define BSP_ADC_POLL_MAX_ROUNDS 64
#define BSP_ADC_READ_TIMEOUT_MS 20

static const char *TAG = "bsp_adc";

static const adc_channel_t s_channel[BSP_ADC_COUNT] = {
    [BSP_ADC_VBAT] = ADC_CHANNEL_0,
    [BSP_ADC_ISERVO] = ADC_CHANNEL_1,
};

static adc_continuous_handle_t s_handle;
// C6 支持逐通道曲线拟合校准；建立失败时退回线性换算，并在日志里说明。
static adc_cali_handle_t s_cali[BSP_ADC_COUNT];
static adc_iir_filter_handle_t s_filter[BSP_ADC_COUNT];
static adc_digi_pattern_config_t s_pattern[BSP_ADC_COUNT];
static u16 s_latest_raw[BSP_ADC_COUNT];
static bool s_has_sample[BSP_ADC_COUNT];
static bool s_ready;

static bool bsp_adc_valid(BSP_ADC_CH ch) {
    return ch >= 0 && ch < BSP_ADC_COUNT;
}

// 头文件用数值宏，避免公共头暴露 adc_filter.h 的驱动枚举。
static adc_digi_iir_filter_coeff_t bsp_adc_iir_coeff(void) {
    switch (BSP_ADC_IIR_FILTER_COEFF) {
        case 2: return ADC_DIGI_IIR_FILTER_COEFF_2;
        case 4: return ADC_DIGI_IIR_FILTER_COEFF_4;
        case 8: return ADC_DIGI_IIR_FILTER_COEFF_8;
        case 32: return ADC_DIGI_IIR_FILTER_COEFF_32;
        case 64: return ADC_DIGI_IIR_FILTER_COEFF_64;
        default: return ADC_DIGI_IIR_FILTER_COEFF_16;
    }
}

// 排空环形缓冲，只保留每个通道的最后一个样本。
// 连续采样期间缓冲里积压的帧是旧的，全部取走丢弃，最后覆盖上去的才是最新的。
static esp_err_t bsp_adc_poll(uint32_t timeout_ms) {
    u8 frame[BSP_ADC_FRAME_BYTES];
    adc_continuous_data_t parsed[BSP_ADC_FRAME_BYTES / SOC_ADC_DIGI_RESULT_BYTES];

    for (int round = 0; round < BSP_ADC_POLL_MAX_ROUNDS; round++) {
        uint32_t got = 0;
        esp_err_t rc = adc_continuous_read(s_handle, frame, sizeof(frame), &got, timeout_ms);
        if (rc == ESP_ERR_TIMEOUT) {
            return ESP_OK;  // 积压已排空
        }
        if (rc != ESP_OK) {
            return rc;
        }

        uint32_t count = 0;
        rc = adc_continuous_parse_data(s_handle, frame, got, parsed, &count);
        if (rc != ESP_OK) {
            return rc;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!parsed[i].valid) {
                continue;
            }
            for (int ch = 0; ch < BSP_ADC_COUNT; ch++) {
                if (parsed[i].channel == s_channel[ch]) {
                    s_latest_raw[ch] = (u16)parsed[i].raw_data;
                    s_has_sample[ch] = true;
                }
            }
        }
        // 首轮可以把刚启动时的等待用掉，之后只取已有数据，不再阻塞。
        timeout_ms = 0;
    }
    return ESP_OK;
}

static void bsp_adc_release(void) {
    for (int ch = 0; ch < BSP_ADC_COUNT; ch++) {
#if SOC_ADC_DIG_IIR_FILTER_SUPPORTED
        if (s_filter[ch] != NULL) {
            adc_del_continuous_iir_filter(s_filter[ch]);
            s_filter[ch] = NULL;
        }
#endif
        if (s_cali[ch] != NULL) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(s_cali[ch]);
#endif
            s_cali[ch] = NULL;
        }
    }
    if (s_handle != NULL) {
        adc_continuous_deinit(s_handle);
        s_handle = NULL;
    }
}

esp_err_t bsp_adc_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    adc_continuous_handle_cfg_t handle_config = {
        .max_store_buf_size = BSP_ADC_POOL_BYTES,
        .conv_frame_size = BSP_ADC_FRAME_BYTES,
        // 读取周期长于池容量时丢掉旧帧，保证读到的始终是最新数据。
        .flags.flush_pool = 1,
    };
    esp_err_t rc = adc_continuous_new_handle(&handle_config, &s_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "continuous handle init failed: %s", esp_err_to_name(rc));
        s_handle = NULL;
        return rc;
    }

    for (int ch = 0; ch < BSP_ADC_COUNT; ch++) {
        s_pattern[ch].atten = BSP_ADC_ATTEN;
        s_pattern[ch].channel = (u8)s_channel[ch];
        s_pattern[ch].unit = BSP_ADC_UNIT;
        s_pattern[ch].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    }

    adc_continuous_config_t dig_config = {
        .pattern_num = BSP_ADC_COUNT,
        .adc_pattern = s_pattern,
        .sample_freq_hz = BSP_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    rc = adc_continuous_config(s_handle, &dig_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "continuous config failed: %s", esp_err_to_name(rc));
        bsp_adc_release();
        return rc;
    }

    for (int ch = 0; ch < BSP_ADC_COUNT; ch++) {
        s_cali[ch] = NULL;
        s_filter[ch] = NULL;
        s_has_sample[ch] = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = BSP_ADC_UNIT,
            .chan = s_channel[ch],
            .atten = BSP_ADC_ATTEN,
            .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali[ch]) != ESP_OK) {
            s_cali[ch] = NULL;
            ESP_LOGW(TAG, "channel %d calibration unavailable, using linear conversion", ch);
        }
#endif

#if SOC_ADC_DIG_IIR_FILTER_SUPPORTED
        adc_continuous_iir_filter_config_t filter_config = {
            .unit = BSP_ADC_UNIT,
            .channel = s_channel[ch],
            .coeff = bsp_adc_iir_coeff(),
        };
        if (adc_new_continuous_iir_filter(s_handle, &filter_config, &s_filter[ch]) != ESP_OK ||
            adc_continuous_iir_filter_enable(s_filter[ch]) != ESP_OK) {
            if (s_filter[ch] != NULL) {
                adc_del_continuous_iir_filter(s_filter[ch]);
                s_filter[ch] = NULL;
            }
            ESP_LOGW(TAG, "channel %d IIR filter unavailable, smoothing degraded", ch);
        }
#endif
    }

    rc = adc_continuous_start(s_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "continuous start failed: %s", esp_err_to_name(rc));
        bsp_adc_release();
        return rc;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ADC1 sampling: VBAT=GPIO%d, ISERVO=GPIO%d, %d Hz total, 12 dB",
             BSP_ADC_VBAT_GPIO, BSP_ADC_ISERVO_GPIO, BSP_ADC_SAMPLE_FREQ_HZ);
    return ESP_OK;
}

esp_err_t bsp_adc_read_raw(BSP_ADC_CH ch, u16 *raw) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_adc_valid(ch) || raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = bsp_adc_poll(BSP_ADC_READ_TIMEOUT_MS);
    if (rc != ESP_OK) {
        return rc;
    }
    if (!s_has_sample[ch]) {
        return ESP_ERR_NOT_FINISHED;
    }

    *raw = s_latest_raw[ch];
    return ESP_OK;
}

esp_err_t bsp_adc_read_mv(BSP_ADC_CH ch, u16 *millivolt) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_adc_valid(ch) || millivolt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    u16 raw = 0;
    esp_err_t rc = bsp_adc_read_raw(ch, &raw);
    if (rc != ESP_OK) {
        return rc;
    }

    int voltage = 0;
    if (s_cali[ch] != NULL &&
        adc_cali_raw_to_voltage(s_cali[ch], (int)raw, &voltage) == ESP_OK) {
        *millivolt = (u16)voltage;
        return ESP_OK;
    }
    // 校准不可用时的保守线性换算：12 bit、3.3 V 参考。
    *millivolt = (u16)((u32)raw * 3300u / 4095u);
    return ESP_OK;
}
