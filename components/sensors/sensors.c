#include "sensors.h"

#include "board_pins.h"
#include "parameter.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "sensors";

#define ADC_UNIT_USED      ADC_UNIT_1
#define ADC_ATTEN_USED     ADC_ATTEN_DB_12
#define ADC_BITWIDTH_USED  ADC_BITWIDTH_DEFAULT
#define SATURATION_MV      3200 /* near the 12 dB full-scale ceiling */
#define FILTER_LEN         16

typedef struct {
    adc_channel_t chan;
    adc_cali_handle_t cali;
    bool cali_ok;
    int32_t history[FILTER_LEN];
    size_t head;
    size_t count;
} adc_channel_ctx_t;

static adc_oneshot_unit_handle_t s_unit;
static adc_channel_ctx_t s_vbat = { .chan = ADC_CHANNEL_0 };
static adc_channel_ctx_t s_iservo = { .chan = ADC_CHANNEL_1 };
static bool s_ready;
static sensors_reading_t s_latest;

static void push(adc_channel_ctx_t *ctx, int32_t mv)
{
    ctx->history[ctx->head] = mv;
    ctx->head = (ctx->head + 1) % FILTER_LEN;
    if (ctx->count < FILTER_LEN) {
        ctx->count++;
    }
}

static int32_t average(const adc_channel_ctx_t *ctx)
{
    if (ctx->count == 0) {
        return 0;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        sum += ctx->history[i];
    }
    return (int32_t)(sum / (int64_t)ctx->count);
}

static bool setup_channel(adc_channel_ctx_t *ctx)
{
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_USED,
    };
    esp_err_t err = adc_oneshot_config_channel(s_unit, ctx->chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config channel %d: %s", ctx->chan, esp_err_to_name(err));
        return false;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_USED,
        .chan = ctx->chan,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_USED,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &ctx->cali);
    ctx->cali_ok = (err == ESP_OK);
    if (!ctx->cali_ok) {
        /* Raw counts are useless without calibration; report and continue with the
         * channel marked invalid rather than inventing a curve. */
        ESP_LOGW(TAG, "no calibration scheme for channel %d: %s", ctx->chan, esp_err_to_name(err));
    }
    return true;
}

esp_err_t sensors_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_USED,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    if (!setup_channel(&s_vbat) || !setup_channel(&s_iservo)) {
        adc_oneshot_del_unit(s_unit);
        s_unit = NULL;
        return ESP_FAIL;
    }

    s_ready = true;
    return ESP_OK;
}

bool sensors_calibration_valid(void)
{
    return parameter_get()->sensors.calibrated != 0;
}

esp_err_t sensors_reload_params(void)
{
    s_vbat.head = s_vbat.count = 0;
    s_iservo.head = s_iservo.count = 0;
    memset(&s_latest, 0, sizeof(s_latest));
    return ESP_OK;
}

static bool read_mv(adc_channel_ctx_t *ctx, int32_t *out_mv, int *out_raw)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_unit, ctx->chan, &raw);
    if (err != ESP_OK) {
        return false;
    }
    if (out_raw) {
        *out_raw = raw;
    }

    int mv = 0;
    if (!ctx->cali_ok) {
        return false;
    }
    if (adc_cali_raw_to_voltage(ctx->cali, raw, &mv) != ESP_OK) {
        return false;
    }
    *out_mv = mv;
    push(ctx, mv);
    return true;
}

esp_err_t sensors_sample(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const bf_sensors_cfg_t *cfg = &parameter_get()->sensors;
    bool cal_valid = sensors_calibration_valid();

    int32_t vbat_pin_mv = 0;
    int32_t iservo_pin_mv = 0;
    bool vbat_ok = read_mv(&s_vbat, &vbat_pin_mv, NULL);
    bool iservo_ok = read_mv(&s_iservo, &iservo_pin_mv, NULL);

    int32_t vbat_avg = average(&s_vbat);
    int32_t iservo_avg = average(&s_iservo);

    bool saturated = (vbat_ok && vbat_pin_mv >= SATURATION_MV) ||
                     (iservo_ok && iservo_pin_mv >= SATURATION_MV);

    /* Battery: divider scales the pack voltage down to the pin. */
    if (vbat_ok && cfg->vbat_div_den != 0 && cfg->vbat_div_num != 0) {
        s_latest.vbat_mv = (int32_t)(((int64_t)vbat_avg * cfg->vbat_div_num) / cfg->vbat_div_den);
    }
    s_latest.vbat_valid = vbat_ok && cal_valid && cfg->vbat_div_den != 0;

    /* Servo current: I(mA) = 1000 * (Vout_mV - zero_mV) / (gain * shunt_mOhm). */
    if (iservo_ok && cfg->ina199_gain != 0 && cfg->iservo_shunt_mohm != 0) {
        int32_t delta_mv = iservo_avg - cfg->iservo_zero_mv;
        int64_t denom = (int64_t)cfg->ina199_gain * cfg->iservo_shunt_mohm;
        s_latest.iservo_ma = (int32_t)(((int64_t)delta_mv * 1000) / denom);
        s_latest.iservo_peak_ma = (int32_t)(((int64_t)(iservo_pin_mv - cfg->iservo_zero_mv) * 1000) / denom);
    }
    s_latest.iservo_valid = iservo_ok && cal_valid &&
                            cfg->ina199_gain != 0 && cfg->iservo_shunt_mohm != 0;
    s_latest.saturated = saturated;

    return ESP_OK;
}

void sensors_get_latest(sensors_reading_t *out)
{
    if (out) {
        *out = s_latest;
    }
}
