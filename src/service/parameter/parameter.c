#include "parameter.h"

#include "bf_crc.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "parameter";

#define BF_PARAM_NVS_NAMESPACE "bfcfg"
#define BF_PARAM_KEY_SLOT0     "cfg0"
#define BF_PARAM_KEY_SLOT1     "cfg1"
#define BF_PARAM_KEY_META      "meta"
#define BF_PARAM_MAGIC         0x31465042u /* "BFP1" little-endian */
#define BF_PARAM_NUM_SLOTS     2

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_len;
    uint32_t generation;
    uint32_t payload_crc;
} bf_param_blob_hdr_t;

static const char *const s_slot_keys[BF_PARAM_NUM_SLOTS] = { BF_PARAM_KEY_SLOT0, BF_PARAM_KEY_SLOT1 };

/* Descriptor table. Order must match bf_param_id_t exactly. */
static const bf_param_desc_t s_desc[BF_PARAM_COUNT] = {
    { BF_PARAM_SERVO_L_DIRECTION,      "servo_l.direction",      "",   BF_PT_I8,  offsetof(bf_params_t, servo[0].direction),      -1,   1,    true },
    { BF_PARAM_SERVO_L_CALIBRATED,     "servo_l.calibrated",     "",   BF_PT_U8,  offsetof(bf_params_t, servo[0].calibrated),      0,   1,    true },
    { BF_PARAM_SERVO_L_ZERO_US,        "servo_l.zero_us",        "us", BF_PT_U16, offsetof(bf_params_t, servo[0].zero_us),         0, 10000,  true },
    { BF_PARAM_SERVO_L_ANGLE_MIN_US,   "servo_l.angle_min_us",   "us", BF_PT_U16, offsetof(bf_params_t, servo[0].angle_min_us),    0, 10000,  true },
    { BF_PARAM_SERVO_L_ANGLE_MAX_US,   "servo_l.angle_max_us",   "us", BF_PT_U16, offsetof(bf_params_t, servo[0].angle_max_us),    0, 10000,  true },
    { BF_PARAM_SERVO_L_ANGLE_MIN_DEG,  "servo_l.angle_min_deg",  "deg",BF_PT_F32, offsetof(bf_params_t, servo[0].angle_min_deg), -180,  180,  true },
    { BF_PARAM_SERVO_L_ANGLE_MAX_DEG,  "servo_l.angle_max_deg",  "deg",BF_PT_F32, offsetof(bf_params_t, servo[0].angle_max_deg), -180,  180,  true },
    { BF_PARAM_SERVO_L_CENTER_DEG,     "servo_l.center_deg",     "deg",BF_PT_F32, offsetof(bf_params_t, servo[0].center_deg),    -180,  180,  true },
    { BF_PARAM_SERVO_L_SAFE_DEG,       "servo_l.safe_deg",       "deg",BF_PT_F32, offsetof(bf_params_t, servo[0].safe_deg),      -180,  180,  true },
    { BF_PARAM_SERVO_L_MAX_SPEED_DPS,  "servo_l.max_speed_dps",  "d/s",BF_PT_F32, offsetof(bf_params_t, servo[0].max_speed_dps),    0,10000,  true },

    { BF_PARAM_SERVO_R_DIRECTION,      "servo_r.direction",      "",   BF_PT_I8,  offsetof(bf_params_t, servo[1].direction),      -1,   1,    true },
    { BF_PARAM_SERVO_R_CALIBRATED,     "servo_r.calibrated",     "",   BF_PT_U8,  offsetof(bf_params_t, servo[1].calibrated),      0,   1,    true },
    { BF_PARAM_SERVO_R_ZERO_US,        "servo_r.zero_us",        "us", BF_PT_U16, offsetof(bf_params_t, servo[1].zero_us),         0, 10000,  true },
    { BF_PARAM_SERVO_R_ANGLE_MIN_US,   "servo_r.angle_min_us",   "us", BF_PT_U16, offsetof(bf_params_t, servo[1].angle_min_us),    0, 10000,  true },
    { BF_PARAM_SERVO_R_ANGLE_MAX_US,   "servo_r.angle_max_us",   "us", BF_PT_U16, offsetof(bf_params_t, servo[1].angle_max_us),    0, 10000,  true },
    { BF_PARAM_SERVO_R_ANGLE_MIN_DEG,  "servo_r.angle_min_deg",  "deg",BF_PT_F32, offsetof(bf_params_t, servo[1].angle_min_deg), -180,  180,  true },
    { BF_PARAM_SERVO_R_ANGLE_MAX_DEG,  "servo_r.angle_max_deg",  "deg",BF_PT_F32, offsetof(bf_params_t, servo[1].angle_max_deg), -180,  180,  true },
    { BF_PARAM_SERVO_R_CENTER_DEG,     "servo_r.center_deg",     "deg",BF_PT_F32, offsetof(bf_params_t, servo[1].center_deg),    -180,  180,  true },
    { BF_PARAM_SERVO_R_SAFE_DEG,       "servo_r.safe_deg",       "deg",BF_PT_F32, offsetof(bf_params_t, servo[1].safe_deg),      -180,  180,  true },
    { BF_PARAM_SERVO_R_MAX_SPEED_DPS,  "servo_r.max_speed_dps",  "d/s",BF_PT_F32, offsetof(bf_params_t, servo[1].max_speed_dps),    0,10000,  true },

    { BF_PARAM_SAFETY_CALIBRATED,      "safety.calibrated",      "",   BF_PT_U8,  offsetof(bf_params_t, safety.calibrated),        0,   1,    true },
    { BF_PARAM_SAFETY_VBAT_WARN_MV,    "safety.vbat_warn_mv",    "mV", BF_PT_I32, offsetof(bf_params_t, safety.vbat_warn_mv),      0,12000,  true },
    { BF_PARAM_SAFETY_VBAT_CRIT_MV,    "safety.vbat_crit_mv",    "mV", BF_PT_I32, offsetof(bf_params_t, safety.vbat_crit_mv),      0,12000,  true },
    { BF_PARAM_SAFETY_VBAT_MAX_MV,     "safety.vbat_max_mv",     "mV", BF_PT_I32, offsetof(bf_params_t, safety.vbat_max_mv),       0,14000,  true },
    { BF_PARAM_SAFETY_ISERVO_MAX_MA,   "safety.iservo_max_ma",   "mA", BF_PT_I32, offsetof(bf_params_t, safety.iservo_max_ma),      0,20000,  true },

    { BF_PARAM_SENSORS_CALIBRATED,     "sensors.calibrated",     "",   BF_PT_U8,  offsetof(bf_params_t, sensors.calibrated),        0,   1,    true },
    { BF_PARAM_SENSORS_VBAT_DIV_NUM,   "sensors.vbat_div_num",   "",   BF_PT_U16, offsetof(bf_params_t, sensors.vbat_div_num),      1, 65535,  true },
    { BF_PARAM_SENSORS_VBAT_DIV_DEN,   "sensors.vbat_div_den",   "",   BF_PT_U16, offsetof(bf_params_t, sensors.vbat_div_den),      1, 65535,  true },
    { BF_PARAM_SENSORS_ISERVO_SHUNT_MOHM, "sensors.iservo_shunt_mohm", "mOhm", BF_PT_U16, offsetof(bf_params_t, sensors.iservo_shunt_mohm), 1, 1000, true },
    { BF_PARAM_SENSORS_INA199_GAIN,    "sensors.ina199_gain",    "V/V",BF_PT_U16, offsetof(bf_params_t, sensors.ina199_gain),        1, 1000,  true },
    { BF_PARAM_SENSORS_ISERVO_ZERO_MV, "sensors.iservo_zero_mv", "mV", BF_PT_I32, offsetof(bf_params_t, sensors.iservo_zero_mv), -1000, 3000, true },

    { BF_PARAM_SYSTEM_CONTROL_PERIOD_US, "system.control_period_us", "us", BF_PT_U32, offsetof(bf_params_t, system.control_period_us), 3003, 3003, true },
    { BF_PARAM_SYSTEM_TELEMETRY_PERIOD_MS, "system.telemetry_period_ms", "ms", BF_PT_U32, offsetof(bf_params_t, system.telemetry_period_ms), 50, 50, true },
    { BF_PARAM_SERVO_L_MAX_ACCELERATION, "servo_l.max_acceleration_dps2", "d/s2", BF_PT_F32, offsetof(bf_params_t, servo[0].max_acceleration_dps2), 1, 100000, true },
    { BF_PARAM_SERVO_R_MAX_ACCELERATION, "servo_r.max_acceleration_dps2", "d/s2", BF_PT_F32, offsetof(bf_params_t, servo[1].max_acceleration_dps2), 1, 100000, true },
    { BF_PARAM_SYSTEM_MAX_FLAPPING_HZ, "system.max_flapping_hz", "Hz", BF_PT_F32, offsetof(bf_params_t, system.max_flapping_hz), 0.1, 20, true },
    { BF_PARAM_SYSTEM_RAMP_S, "system.ramp_s", "s", BF_PT_F32, offsetof(bf_params_t, system.ramp_s), 0.05, 10, true },
};

#undef FIELD_SERVO

static bf_params_t s_params;
static bool s_valid;
static uint32_t s_generation;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void apply_defaults(bf_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->schema_version = BF_PARAM_SCHEMA_VERSION;

    for (int i = 0; i < BF_PARAM_SERVO_COUNT; i++) {
        p->servo[i].direction = (i == BF_SERVO_LEFT) ? 1 : -1;
        p->servo[i].calibrated = 0; /* stays 0 until this side is measured on the bench */
        /* Nameplate starting points from servo_config.h. These are NOT measured travel:
         * the factory neutral (1520 us) is not the midpoint of 500..2500 us, so the
         * two-point map must be replaced with measured endpoints before calibrated=1. */
        p->servo[i].zero_us = SERVO_SPEC_CENTER_US;
        p->servo[i].angle_min_us = SERVO_SPEC_PULSE_MIN_US;
        p->servo[i].angle_max_us = SERVO_SPEC_PULSE_MAX_US;
        p->servo[i].angle_min_deg = -90.0f; /* nominal half-travel, not the fitted linkage */
        p->servo[i].angle_max_deg = 90.0f;
        p->servo[i].center_deg = 0.0f;
        p->servo[i].safe_deg = 0.0f;
        p->servo[i].max_speed_dps = SERVO_SPEC_RATE_DPS_74V; /* no-load ceiling at 7.4 V */
        p->servo[i].max_acceleration_dps2 = 360.0f; /* no datasheet figure - unmeasured */
    }

    p->safety.calibrated = 0;
    p->safety.vbat_warn_mv = 6800; /* placeholder, 2S nominal region */
    p->safety.vbat_crit_mv = 6400;
    p->safety.vbat_max_mv = 9000;
    p->safety.iservo_max_ma = 0; /* 0 = not established */

    p->sensors.calibrated = 0;
    p->sensors.vbat_div_num = 5;   /* placeholder divider, unmeasured */
    p->sensors.vbat_div_den = 1;
    p->sensors.iservo_shunt_mohm = 30; /* R11 value is disputed; unverified */
    p->sensors.ina199_gain = 50;
    p->sensors.iservo_zero_mv = 0; /* INA199 zero-current offset, unmeasured */

    p->system.control_period_us = 3003; /* ~333 Hz, not a flight parameter */
    p->system.telemetry_period_ms = 50;
    p->system.max_flapping_hz = 1.0f;
    p->system.ramp_s = 1.0f;
}

static esp_err_t read_scalar(const bf_params_t *p, const bf_param_desc_t *d, double *out)
{
    const uint8_t *base = (const uint8_t *)p;
    const void *field = base + d->offset;

    switch (d->type) {
    case BF_PT_U8:  *out = *(const uint8_t *)field;  break;
    case BF_PT_I8:  *out = *(const int8_t *)field;   break;
    case BF_PT_U16: *out = *(const uint16_t *)field; break;
    case BF_PT_I16: *out = *(const int16_t *)field;  break;
    case BF_PT_U32: *out = *(const uint32_t *)field; break;
    case BF_PT_I32: *out = *(const int32_t *)field;  break;
    case BF_PT_F32: *out = *(const float *)field;    break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t write_scalar(bf_params_t *p, const bf_param_desc_t *d, double v)
{
    uint8_t *base = (uint8_t *)p;
    void *field = base + d->offset;

    switch (d->type) {
    case BF_PT_U8:  *(uint8_t *)field = (uint8_t)v;   break;
    case BF_PT_I8:  *(int8_t *)field = (int8_t)v;     break;
    case BF_PT_U16: *(uint16_t *)field = (uint16_t)v; break;
    case BF_PT_I16: *(int16_t *)field = (int16_t)v;   break;
    case BF_PT_U32: *(uint32_t *)field = (uint32_t)v; break;
    case BF_PT_I32: *(int32_t *)field = (int32_t)v;   break;
    case BF_PT_F32: *(float *)field = (float)v;       break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool params_in_range(const bf_params_t *p)
{
    for (size_t i = 0; i < BF_PARAM_COUNT; i++) {
        double v;
        if (read_scalar(p, &s_desc[i], &v) != ESP_OK) {
            return false;
        }
        if (!(v >= s_desc[i].min && v <= s_desc[i].max)) {
            ESP_LOGW(TAG, "param %s out of range: %g not in [%g, %g]",
                     s_desc[i].name, v, s_desc[i].min, s_desc[i].max);
            return false;
        }
    }
    /* Semantic checks that ranges cannot express. */
    for (int i = 0; i < BF_PARAM_SERVO_COUNT; i++) {
        if (p->servo[i].direction == 0) {
            ESP_LOGW(TAG, "servo[%d] direction must be +1 or -1", i);
            return false;
        }
        const bf_servo_cal_t *cal = &p->servo[i];
        if (cal->angle_min_deg >= cal->angle_max_deg ||
            cal->center_deg < cal->angle_min_deg || cal->center_deg > cal->angle_max_deg ||
            (cal->calibrated && (cal->angle_min_us < 200 || cal->angle_min_us > 2800 ||
             cal->angle_max_us < 200 || cal->angle_max_us > 2800 ||
             cal->angle_min_us == cal->angle_max_us || cal->max_speed_dps <= 0))) {
            return false;
        }
        if (p->servo[i].safe_deg < p->servo[i].angle_min_deg ||
            p->servo[i].safe_deg > p->servo[i].angle_max_deg) {
            ESP_LOGW(TAG, "servo[%d] safe angle outside travel limits", i);
            return false;
        }
    }
    if (p->safety.vbat_crit_mv >= p->safety.vbat_warn_mv ||
        p->safety.vbat_warn_mv >= p->safety.vbat_max_mv ||
        (p->safety.calibrated && p->safety.iservo_max_ma <= 0)) {
        return false;
    }
    return true;
}

/* --- persistence ------------------------------------------------------------- */

static bool blob_slot_valid(nvs_handle_t h, const char *key, bf_params_t *out, uint32_t *out_gen)
{
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK) {
        return false;
    }
    if (len != sizeof(bf_param_blob_hdr_t) + sizeof(bf_params_t)) {
        return false;
    }

    uint8_t buf[sizeof(bf_param_blob_hdr_t) + sizeof(bf_params_t)];
    if (nvs_get_blob(h, key, buf, &len) != ESP_OK) {
        return false;
    }

    bf_param_blob_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != BF_PARAM_MAGIC) {
        return false;
    }
    if (hdr.schema_version != BF_PARAM_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "slot %s schema %u != %u, ignoring", key, hdr.schema_version, BF_PARAM_SCHEMA_VERSION);
        return false;
    }
    if (hdr.payload_len != sizeof(bf_params_t)) {
        return false;
    }
    const uint8_t *payload = buf + sizeof(bf_param_blob_hdr_t);
    if (bf_crc32(payload, hdr.payload_len) != hdr.payload_crc) {
        ESP_LOGW(TAG, "slot %s CRC mismatch, ignoring", key);
        return false;
    }

    memcpy(out, payload, sizeof(bf_params_t));
    if (!params_in_range(out)) {
        ESP_LOGW(TAG, "slot %s failed range validation, ignoring", key);
        return false;
    }
    if (out_gen) {
        *out_gen = hdr.generation;
    }
    return true;
}

static esp_err_t blob_write(nvs_handle_t h, const char *key, const bf_params_t *p, uint32_t generation)
{
    uint8_t buf[sizeof(bf_param_blob_hdr_t) + sizeof(bf_params_t)];
    bf_param_blob_hdr_t hdr = {
        .magic = BF_PARAM_MAGIC,
        .schema_version = BF_PARAM_SCHEMA_VERSION,
        .payload_len = sizeof(bf_params_t),
        .generation = generation,
        .payload_crc = bf_crc32(p, sizeof(bf_params_t)),
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), p, sizeof(bf_params_t));

    esp_err_t err = nvs_set_blob(h, key, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(h);
}

static esp_err_t nvs_open_ready(nvs_handle_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_open(BF_PARAM_NVS_NAMESPACE, NVS_READWRITE, out);
}

esp_err_t parameter_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The descriptor table is indexed by id, so order must match exactly. */
    for (size_t i = 0; i < BF_PARAM_COUNT; i++) {
        if (s_desc[i].id != (bf_param_id_t)i) {
            ESP_LOGE(TAG, "descriptor %zu has id %d; table out of order", i, s_desc[i].id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    apply_defaults(&s_params);
    s_valid = false;
    s_generation = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open_ready(&h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s; running on defaults", esp_err_to_name(err));
        return ESP_OK; /* keep running locked on defaults */
    }

    uint8_t active = 0;
    bool have_meta = (nvs_get_u8(h, BF_PARAM_KEY_META, &active) == ESP_OK) && (active < BF_PARAM_NUM_SLOTS);

    bf_params_t candidate;
    uint32_t gen = 0;
    bool found = false;

    /* Try the recorded slot first, then the other one. */
    for (int attempt = 0; attempt < BF_PARAM_NUM_SLOTS; attempt++) {
        uint8_t idx = have_meta ? (uint8_t)((active + attempt) % BF_PARAM_NUM_SLOTS) : (uint8_t)attempt;
        if (blob_slot_valid(h, s_slot_keys[idx], &candidate, &gen)) {
            s_params = candidate;
            s_generation = gen;
            s_valid = true;
            found = true;
            if (have_meta && idx != active) {
                ESP_LOGW(TAG, "active slot unreadable; fell back to slot %u", idx);
            }
            break;
        }
    }

    nvs_close(h);

    if (!found) {
        ESP_LOGW(TAG, "no valid stored parameters; using safe defaults");
        s_valid = false;
    } else {
        ESP_LOGI(TAG, "parameters loaded (generation %" PRIu32 ")", s_generation);
    }
    return ESP_OK;
}

bool parameter_is_valid(void)
{
    return s_valid;
}

const bf_params_t *parameter_get(void)
{
    return &s_params;
}

static esp_err_t persist(const bf_params_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_ready(&h);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t active = 0;
    bool have_meta = (nvs_get_u8(h, BF_PARAM_KEY_META, &active) == ESP_OK) && (active < BF_PARAM_NUM_SLOTS);
    uint8_t target = have_meta ? (uint8_t)((active + 1) % BF_PARAM_NUM_SLOTS) : 0;

    uint32_t next_gen = s_generation + 1;
    err = blob_write(h, s_slot_keys[target], p, next_gen);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    /* Read back and verify before flipping the active pointer. */
    bf_params_t verify;
    uint32_t vgen = 0;
    if (!blob_slot_valid(h, s_slot_keys[target], &verify, &vgen)) {
        nvs_close(h);
        ESP_LOGE(TAG, "readback verification failed");
        return ESP_ERR_INVALID_CRC;
    }

    err = nvs_set_u8(h, BF_PARAM_KEY_META, target);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    s_generation = next_gen;
    s_valid = true;
    return ESP_OK;
}

esp_err_t parameter_commit(const bf_params_t *next)
{
    if (next == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!params_in_range(next)) {
        return ESP_ERR_INVALID_ARG;
    }

    bf_params_t copy = *next;
    copy.schema_version = BF_PARAM_SCHEMA_VERSION;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = persist(&copy);
    if (err == ESP_OK) {
        s_params = copy;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t parameter_reset_to_defaults(void)
{
    bf_params_t fresh;
    apply_defaults(&fresh);
    return parameter_commit(&fresh);
}

size_t parameter_count(void)
{
    return BF_PARAM_COUNT;
}

const bf_param_desc_t *parameter_descriptor(size_t index)
{
    return (index < BF_PARAM_COUNT) ? &s_desc[index] : NULL;
}

const bf_param_desc_t *parameter_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < BF_PARAM_COUNT; i++) {
        if (strcmp(s_desc[i].name, name) == 0) {
            return &s_desc[i];
        }
    }
    return NULL;
}

esp_err_t parameter_get_scalar(bf_param_id_t id, double *out)
{
    if (out == NULL || (unsigned)id >= BF_PARAM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_scalar(&s_params, &s_desc[id], out);
}

esp_err_t parameter_set_scalar(bf_param_id_t id, double value)
{
    if ((unsigned)id >= BF_PARAM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const bf_param_desc_t *d = &s_desc[id];
    if (!(value >= d->min && value <= d->max)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (d->type != BF_PT_F32 && trunc(value) != value) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bf_params_t staged = s_params;
    write_scalar(&staged, d, value);
    if (!params_in_range(&staged)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = persist(&staged);
    if (err == ESP_OK) {
        s_params = staged;
    }
    xSemaphoreGive(s_lock);
    return err;
}
