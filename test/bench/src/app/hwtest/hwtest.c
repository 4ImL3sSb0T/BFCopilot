/*
 * Hardware test commands for the bench firmware.
 *
 * The firmware this belongs to runs no flight logic and no safety state machine on
 * purpose: the point is to poke one device at a time, at the register level, on a board
 * that is being brought up. Everything here therefore talks to real hardware directly -
 * bus registers, ADC counts, pulse widths, LED, buzzer and bare GPIO - and nothing here
 * consults an arming or fault state.
 *
 * Bus access goes through sensors_debug_*, which uses the same pins, clock and mode as the
 * flight firmware: a probe that only worked on some other configuration would prove
 * nothing about whether the board works.
 *
 * A register write can leave a device in a state the driver did not configure. Power-cycle
 * or `hw imu reset` afterwards.
 */
#include "hwtest.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "board_buzzer.h"
#include "board_pins.h"
#include "board_status_led.h"
#include "driver/gpio.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "parameter.h"
#include "sensors.h"
#include "sensors_debug.h"
#include "servo.h"

/* Registers touched directly here. Kept as literals rather than taken from the ST driver
 * because the point of these commands is to work when the driver is not driving. */
#define IMU_REG_STATUS 0x1eu
#define IMU_REG_OUT_G 0x22u
#define IMU_REG_OUT_A 0x28u
#define BARO_REG_STATUS 0x08u
#define BARO_REG_PRESSURE 0x00u
#define BARO_REG_TEMPERATURE 0x03u

#define STREAM_MAX_S 30

#define out(...) bf_net_write(__VA_ARGS__)

static bool parse_byte(const char *text, uint8_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (!text[0] || !end || *end || parsed > 0xff) return false;
    *value = (uint8_t)parsed;
    return true;
}

static bool parse_float(const char *text, float *value)
{
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (!text[0] || !end || *end) return false;
    *value = parsed;
    return true;
}

static bool parse_seconds(int argc, char **argv, unsigned *seconds)
{
    *seconds = 3;
    if (argc < 3) return true;
    char *end = NULL;
    unsigned long parsed = strtoul(argv[2], &end, 0);
    if (!argv[2][0] || !end || *end) return false;
    if (parsed == 0) parsed = 1;
    if (parsed > STREAM_MAX_S) parsed = STREAM_MAX_S;
    *seconds = (unsigned)parsed;
    return true;
}

static void stream_tick(unsigned ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* --------------------------------------------------------------------- bus reporting */

static void report_imu_bus(void)
{
    if (!sensors_debug_imu_bus_ready()) {
        out("spi2     : NOT UP\n");
        return;
    }
    int miso = -1, scl = -1, sda = -1;
    sensors_debug_line_levels(&miso, &scl, &sda);
    out("spi2     : SCK=%d MOSI=%d MISO=%d CS=%d, MISO idle=%d\n",
        BOARD_PIN_IMU_SCK, BOARD_PIN_IMU_MOSI, BOARD_PIN_IMU_MISO, BOARD_PIN_IMU_CS, miso);
    uint8_t id = 0;
    esp_err_t err = sensors_debug_imu_read(SENSORS_DEBUG_IMU_ID_REG, &id, 1);
    out("imu      : WHO_AM_I=0x%02x (expected 0x%02x), transfer=%s\n",
        id, SENSORS_DEBUG_IMU_ID, esp_err_to_name(err));
}

static void report_i2c_bus(void)
{
    if (!sensors_debug_i2c_bus_ready()) {
        out("i2c0     : NOT UP\n");
        return;
    }
    int miso = -1, scl = -1, sda = -1;
    sensors_debug_line_levels(&miso, &scl, &sda);
    out("i2c0     : SCL=%d SDA=%d, idle SCL=%d SDA=%d\n",
        BOARD_PIN_BARO_SCL, BOARD_PIN_BARO_SDA, scl, sda);

    unsigned acknowledged = 0;
    esp_err_t bus_error = ESP_OK;
    for (uint16_t address = 0x08; address <= 0x77; address++) {
        esp_err_t err = sensors_debug_i2c_probe((uint8_t)address);
        if (err != ESP_OK) {
            /* A NACK means "no device here"; anything else means the bus itself failed,
             * which must not be read as a missing part. */
            if (err != ESP_ERR_NOT_FOUND && bus_error == ESP_OK) bus_error = err;
            continue;
        }
        uint8_t id = 0;
        esp_err_t id_err = sensors_debug_i2c_read((uint8_t)address, SENSORS_DEBUG_BARO_ID_REG, &id, 1);
        acknowledged++;
        out("  ack    : 0x%02x id(0x%02x)=0x%02x%s\n", address, SENSORS_DEBUG_BARO_ID_REG, id,
            id_err == ESP_OK && id == SENSORS_DEBUG_BARO_ID ? "  SPA06-003" : "");
    }
    out("i2c ack  : %u address(es)\n", acknowledged);
    if (bus_error != ESP_OK) {
        out("            probe reported %s - a bus fault, not a missing device\n",
            esp_err_to_name(bus_error));
    } else if (!acknowledged) {
        out("            nothing answered: check 3V3 on the parts, then SCL/SDA continuity\n");
    }
}

static void print_imu_sample(void)
{
    uint8_t status = 0, raw[6];
    esp_err_t err = sensors_debug_imu_read(IMU_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        out("  status read failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (sensors_debug_imu_read(IMU_REG_OUT_A, raw, sizeof(raw)) == ESP_OK) {
        out("  accel raw [%6d, %6d, %6d]  gyro raw ", (int16_t)(raw[0] | (raw[1] << 8)),
            (int16_t)(raw[2] | (raw[3] << 8)), (int16_t)(raw[4] | (raw[5] << 8)));
    } else {
        out("  accel read failed          gyro raw ");
    }
    if (sensors_debug_imu_read(IMU_REG_OUT_G, raw, sizeof(raw)) == ESP_OK) {
        out("[%6d, %6d, %6d]  status 0x%02x (xlda=%d gda=%d)\n",
            (int16_t)(raw[0] | (raw[1] << 8)), (int16_t)(raw[2] | (raw[3] << 8)),
            (int16_t)(raw[4] | (raw[5] << 8)), status, (status & 1) != 0, (status & 2) != 0);
    } else {
        out("read failed\n");
    }
}

/* ---------------------------------------------------------------- hw subcommands */

static int hw_imu(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "rd") == 0) {
        uint8_t reg = 0;
        if (argc < 3 || !parse_byte(argv[2], &reg)) {
            out("usage: hw imu rd <reg>\n");
            return 1;
        }
        uint8_t value = 0;
        esp_err_t err = sensors_debug_imu_read(reg, &value, 1);
        out("imu reg  : 0x%02x = 0x%02x (transfer=%s)\n", reg, value, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "wr") == 0) {
        uint8_t reg = 0, value = 0;
        if (argc < 4 || !parse_byte(argv[2], &reg) || !parse_byte(argv[3], &value)) {
            out("usage: hw imu wr <reg> <value>\n");
            return 1;
        }
        esp_err_t err = sensors_debug_imu_write(reg, value);
        out("imu reg  : 0x%02x <- 0x%02x (%s); power-cycle or reset to restore the defaults\n",
            reg, value, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        /* CTRL3_C bit 0 is SW_RESET; the datasheet allows 10 ms for it to clear. */
        esp_err_t err = sensors_debug_imu_write(0x12, 0x01);
        if (err != ESP_OK) {
            out("imu reset: write failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        stream_tick(20);
        uint8_t ctrl3 = 0, id = 0;
        sensors_debug_imu_read(0x12, &ctrl3, 1);
        sensors_debug_imu_read(SENSORS_DEBUG_IMU_ID_REG, &id, 1);
        out("imu reset: CTRL3_C=0x%02x (sw_reset=%d), WHO_AM_I=0x%02x\n", ctrl3, ctrl3 & 1, id);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stream") == 0) {
        unsigned seconds = 0;
        if (!parse_seconds(argc, argv, &seconds)) {
            out("usage: hw imu stream [seconds]\n");
            return 1;
        }
        if (!sensors_debug_imu_bus_ready()) {
            out("imu      : SPI2 bus not up\n");
            return 1;
        }
        out("streaming raw IMU for %u s (10 Hz)\n", seconds);
        for (unsigned tick = 0; tick < seconds * 10; tick++) {
            print_imu_sample();
            stream_tick(100);
        }
        return 0;
    }
    if (argc >= 2) {
        out("usage: hw imu [rd <reg> | wr <reg> <value> | reset | stream [seconds]]\n");
        return 1;
    }
    if (!sensors_debug_imu_bus_ready()) {
        out("imu      : SPI2 bus not up\n");
        return 1;
    }
    uint8_t id = 0;
    esp_err_t err = sensors_debug_imu_read(SENSORS_DEBUG_IMU_ID_REG, &id, 1);
    out("WHO_AM_I : 0x%02x (expected 0x%02x), transfer=%s\n",
        id, SENSORS_DEBUG_IMU_ID, esp_err_to_name(err));
    print_imu_sample();
    out("           at 104 Hz, +/-2 g (0.061 mg/LSB), +/-250 dps (8.75 mdps/LSB)\n");
    return 0;
}

static int hw_baro(int argc, char **argv)
{
    uint8_t address = BOARD_BARO_I2C_ADDR;
    if (argc >= 2 && strcmp(argv[1], "rd") == 0) {
        if (argc >= 4 && !parse_byte(argv[3], &address)) {
            out("usage: hw baro rd <reg> [address]\n");
            return 1;
        }
        uint8_t reg = 0;
        if (argc < 3 || !parse_byte(argv[2], &reg)) {
            out("usage: hw baro rd <reg> [address]\n");
            return 1;
        }
        uint8_t value = 0;
        esp_err_t err = sensors_debug_i2c_read(address, reg, &value, 1);
        out("baro reg : 0x%02x @0x%02x = 0x%02x (%s)\n", reg, address, value, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "wr") == 0) {
        uint8_t reg = 0, value = 0;
        if (argc < 4 || !parse_byte(argv[2], &reg) || !parse_byte(argv[3], &value)) {
            out("usage: hw baro wr <reg> <value> [address]\n");
            return 1;
        }
        if (argc >= 5 && !parse_byte(argv[4], &address)) {
            out("usage: hw baro wr <reg> <value> [address]\n");
            return 1;
        }
        esp_err_t err = sensors_debug_i2c_write(address, reg, value);
        out("baro reg : 0x%02x @0x%02x <- 0x%02x (%s); power-cycle to restore the defaults\n",
            reg, address, value, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "stream") == 0) {
        unsigned seconds = 0;
        if (!parse_seconds(argc, argv, &seconds)) {
            out("usage: hw baro stream [seconds]\n");
            return 1;
        }
        out("streaming raw barometer for %u s (5 Hz); one address only\n", seconds);
        for (unsigned tick = 0; tick < seconds * 5; tick++) {
            uint8_t id = 0, status = 0, raw[3];
            esp_err_t err = sensors_debug_i2c_read(address, SENSORS_DEBUG_BARO_ID_REG, &id, 1);
            if (err != ESP_OK) {
                out("  0x%02x unreachable: %s\n", address, esp_err_to_name(err));
                return 1;
            }
            sensors_debug_i2c_read(address, BARO_REG_STATUS, &status, 1);
            int32_t pressure = 0, temperature = 0;
            if (sensors_debug_i2c_read(address, BARO_REG_PRESSURE, raw, sizeof(raw)) == ESP_OK) {
                pressure = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
                pressure = (pressure << 8) >> 8;
            }
            if (sensors_debug_i2c_read(address, BARO_REG_TEMPERATURE, raw, sizeof(raw)) == ESP_OK) {
                temperature = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
                temperature = (temperature << 8) >> 8;
            }
            out("  status 0x%02x  press raw %9ld  temp raw %9ld\n", status, (long)pressure,
                (long)temperature);
            stream_tick(200);
        }
        return 0;
    }
    if (argc >= 2) {
        out("usage: hw baro [rd <reg> [address] | wr <reg> <value> [address] | stream [seconds]]\n");
        return 1;
    }
    if (!sensors_debug_i2c_bus_ready()) {
        out("baro     : I2C0 bus not up\n");
        return 1;
    }
    bool found = false;
    for (unsigned index = 0; index < 2; index++) {
        uint8_t candidate = BOARD_BARO_I2C_ADDR ^ index;
        esp_err_t err = sensors_debug_i2c_probe(candidate);
        if (err != ESP_OK) {
            out("0x%02x     : no ACK (%s)\n", candidate, esp_err_to_name(err));
            continue;
        }
        uint8_t id = 0;
        err = sensors_debug_i2c_read(candidate, SENSORS_DEBUG_BARO_ID_REG, &id, 1);
        if (err != ESP_OK) {
            out("0x%02x     : ACK, id read failed (%s)\n", candidate, esp_err_to_name(err));
            continue;
        }
        out("0x%02x     : ACK, id=0x%02x (expected 0x%02x)%s\n", candidate, id,
            SENSORS_DEBUG_BARO_ID, id == SENSORS_DEBUG_BARO_ID ? "  SPA06-003" : "");
        found = true;
        uint8_t status = 0;
        if (sensors_debug_i2c_read(candidate, BARO_REG_STATUS, &status, 1) == ESP_OK) {
            out("  status : 0x%02x (prs_rdy=%d tmp_rdy=%d)\n", status,
                (status & 0x10) != 0, (status & 0x20) != 0);
        }
        uint8_t raw[3];
        if (sensors_debug_i2c_read(candidate, BARO_REG_PRESSURE, raw, sizeof(raw)) == ESP_OK) {
            int32_t value = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
            out("  press  : raw %ld (24-bit signed)\n", (long)((value << 8) >> 8));
        }
        if (sensors_debug_i2c_read(candidate, BARO_REG_TEMPERATURE, raw, sizeof(raw)) == ESP_OK) {
            int32_t value = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
            out("  temp   : raw %ld (24-bit signed)\n", (long)((value << 8) >> 8));
        }
    }
    if (!found) out("           no device readable at either configured address\n");
    return found ? 0 : 1;
}

static int hw_i2c(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "scan") == 0) {
        report_i2c_bus();
        return 0;
    }
    if (strcmp(argv[1], "rd") == 0) {
        uint8_t address = 0, reg = 0;
        if (argc < 4 || !parse_byte(argv[2], &address) || !parse_byte(argv[3], &reg)) {
            out("usage: hw i2c rd <addr> <reg>\n");
            return 1;
        }
        uint8_t value = 0;
        esp_err_t err = sensors_debug_i2c_read(address, reg, &value, 1);
        out("i2c      : 0x%02x reg 0x%02x = 0x%02x (%s)\n", address, reg, value,
            esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "wr") == 0) {
        uint8_t address = 0, reg = 0, value = 0;
        if (argc < 5 || !parse_byte(argv[2], &address) || !parse_byte(argv[3], &reg) ||
            !parse_byte(argv[4], &value)) {
            out("usage: hw i2c wr <addr> <reg> <value>\n");
            return 1;
        }
        esp_err_t err = sensors_debug_i2c_write(address, reg, value);
        out("i2c      : 0x%02x reg 0x%02x <- 0x%02x (%s)\n", address, reg, value,
            esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    out("usage: hw i2c [scan | rd <addr> <reg> | wr <addr> <reg> <value>]\n");
    return 1;
}

static void print_adc(void)
{
    int vbat_raw = 0, iservo_raw = 0, vbat_mv = -1, iservo_mv = -1;
    esp_err_t err = sensors_debug_adc_raw(&vbat_raw, &iservo_raw, &vbat_mv, &iservo_mv);
    if (err != ESP_OK) {
        out("adc read failed: %s\n", esp_err_to_name(err));
        return;
    }
    out("  vbat   GPIO%d raw=%4d pin=%4d mV\n", BOARD_PIN_VBAT_ADC, vbat_raw, vbat_mv);
    out("  iservo GPIO%d raw=%4d pin=%4d mV\n", BOARD_PIN_ISERVO_ADC, iservo_raw, iservo_mv);
}

static int hw_adc(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stream") == 0) {
        unsigned seconds = 0;
        if (!parse_seconds(argc, argv, &seconds)) {
            out("usage: hw adc stream [seconds]\n");
            return 1;
        }
        out("streaming raw ADC for %u s (10 Hz)\n", seconds);
        for (unsigned tick = 0; tick < seconds * 10; tick++) {
            print_adc();
            stream_tick(100);
        }
        return 0;
    }
    if (argc >= 2) {
        out("usage: hw adc [stream [seconds]]\n");
        return 1;
    }
    print_adc();
    out("  raw ADC / pin voltage test; no parameter calibration required\n");
    return 0;
}

static bool parse_servo_index(const char *token, servo_idx_t *out)
{
    if (strcmp(token, "l") == 0 || strcmp(token, "left") == 0) {
        *out = SERVO_LEFT;
        return true;
    }
    if (strcmp(token, "r") == 0 || strcmp(token, "right") == 0) {
        *out = SERVO_RIGHT;
        return true;
    }
    return false;
}

static int hw_pulse(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        if (argc >= 3 && strcmp(argv[2], "both") != 0) {
            servo_idx_t index;
            if (!parse_servo_index(argv[2], &index)) {
                out("usage: hw pulse off [l|r|both]\n");
                return 1;
            }
            esp_err_t err = servo_debug_off(index);
            out("pulse off: %s\n", esp_err_to_name(err));
            return err == ESP_OK ? 0 : 1;
        }
        servo_debug_off(SERVO_LEFT);
        servo_debug_off(SERVO_RIGHT);
        out("pulse off: both sides\n");
        return 0;
    }
    float pulse = 0;
    if (argc < 3) {
        out("usage: hw pulse <l|r|both> <us>  |  hw pulse off [l|r|both]\n");
        return 1;
    }
    if (!parse_float(argv[2], &pulse)) {
        out("pulse must be a number of microseconds\n");
        return 1;
    }
    if (strcmp(argv[1], "both") == 0) {
        esp_err_t left = servo_debug_set_pulse_us(SERVO_LEFT, pulse);
        esp_err_t right = servo_debug_set_pulse_us(SERVO_RIGHT, pulse);
        out("pulse    : both <- %.0f us (%s / %s)\n", pulse, esp_err_to_name(left),
            esp_err_to_name(right));
        return left == ESP_OK && right == ESP_OK ? 0 : 1;
    }
    servo_idx_t index;
    if (!parse_servo_index(argv[1], &index)) {
        out("usage: hw pulse <l|r|both> <us>\n");
        return 1;
    }
    esp_err_t err = servo_debug_set_pulse_us(index, pulse);
    out("pulse    : %s <- %.0f us (%s)\n", argv[1], pulse, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int hw_led(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        board_led_set(BOARD_LED_OFF);
        out("led      : off\n");
        return 0;
    }
    if (argc < 4) {
        out("usage: hw led <r> <g> <b>  |  hw led off   (0..255 each)\n");
        return 1;
    }
    uint8_t r = 0, g = 0, b = 0;
    if (!parse_byte(argv[1], &r) || !parse_byte(argv[2], &g) || !parse_byte(argv[3], &b)) {
        out("led values must be 0..255\n");
        return 1;
    }
    /* Stop the pattern animation first, or it overwrites the colour on its next tick. */
    board_led_set(BOARD_LED_OFF);
    esp_err_t err = board_led_set_rgb(r, g, b);
    out("led      : rgb(%u, %u, %u) %s\n", r, g, b, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int hw_buzzer(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "off") == 0) {
        board_buzzer_tone(0);
        out("buzzer   : off\n");
        return 0;
    }
    uint32_t frequency = 0;
    char *end = NULL;
    frequency = (uint32_t)strtoul(argv[1], &end, 0);
    if (!argv[1][0] || !end || *end || frequency == 0) {
        out("usage: hw buzzer <freq_hz> [ms]  |  hw buzzer off\n");
        return 1;
    }
    if (argc >= 3) {
        unsigned long ms = strtoul(argv[2], &end, 0);
        if (!argv[2][0] || !end || *end) {
            out("usage: hw buzzer <freq_hz> [ms]\n");
            return 1;
        }
        esp_err_t err = board_buzzer_beep(frequency, (uint32_t)ms, (uint32_t)ms, 1);
        out("buzzer   : %u Hz for %lu ms (%s)\n", (unsigned)frequency, ms, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    esp_err_t err = board_buzzer_tone(frequency);
    out("buzzer   : %u Hz steady (%s); 'hw buzzer off' stops it\n", (unsigned)frequency,
        esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int hw_gpio(int argc, char **argv)
{
    if (argc < 2) {
        out("usage: hw gpio <pin>                    read the level\n");
        out("       hw gpio <pin> in                 input, no pulls\n");
        out("       hw gpio <pin> out <0|1>          drive low or high\n");
        out("       pins owned by a peripheral will fight this - expect bus errors\n");
        return 1;
    }
    uint8_t pin = 0;
    if (!parse_byte(argv[1], &pin) || pin > 30) {
        out("gpio     : pin must be 0..30 on this chip\n");
        return 1;
    }
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (argc >= 4 && strcmp(argv[2], "out") == 0) {
        uint8_t level = 0;
        if (!parse_byte(argv[3], &level) || level > 1) {
            out("usage: hw gpio <pin> out <0|1>\n");
            return 1;
        }
        config.mode = GPIO_MODE_OUTPUT;
        esp_err_t err = gpio_config(&config);
        if (err != ESP_OK) {
            out("gpio     : config failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        gpio_set_level(pin, level);
        out("gpio %-2u  : output %u\n", pin, level);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "in") == 0) {
        config.mode = GPIO_MODE_INPUT;
    } else if (argc >= 3) {
        out("usage: hw gpio <pin> [in | out <0|1>]\n");
        return 1;
    }
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        out("gpio     : config failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    out("gpio %-2u  : input, level %d\n", pin, gpio_get_level(pin));
    return 0;
}

static int hw_servo_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int side = 0; side < 2; side++) {
        servo_idx_t index = (servo_idx_t)side;
        float commanded = 0;
        bool driving = servo_debug_get_pulse_us(index, &commanded) == ESP_OK;
        out("  %-5s : GPIO%-2d U1 pad=%d %s.%d PWM=%s commanded=%.0f us frequency=%d Hz\n",
            side == SERVO_LEFT ? "left" : "right",
            side == SERVO_LEFT ? BOARD_PIN_SERVO_LEFT_PWM : BOARD_PIN_SERVO_RIGHT_PWM,
            side == SERVO_LEFT ? BOARD_PAD_SERVO_LEFT_PWM : BOARD_PAD_SERVO_RIGHT_PWM,
            side == SERVO_LEFT ? BOARD_SERVO_LEFT_CONNECTOR : BOARD_SERVO_RIGHT_CONNECTOR,
            BOARD_SERVO_CONNECTOR_PWM_PIN,
            driving ? "on" : "off", driving ? commanded : 0.0f, SERVO_FRAME_HZ);
    }
    out("           use 'hw pulse <l|r|both> <us>' / 'hw pulse off'; no calibration required\n");
    out("           scope at H1/H2 pin 3 vs pin 1 (GND); reported state is not waveform feedback\n");
    return 0;
}

static int cmd_hw(int argc, char **argv)
{
    if (argc < 2) {
        out("hw scan                 probe both buses and report identity\n");
        out("hw imu [rd <reg> | wr <reg> <val> | reset | stream [s]]\n");
        out("hw baro [rd <reg> [addr] | wr <reg> <val> [addr] | stream [s]]\n");
        out("hw i2c [scan | rd <addr> <reg> | wr <addr> <reg> <val>]\n");
        out("hw adc [stream [s]]     raw and converted power channels\n");
        out("hw pulse <l|r|both> <us> | hw pulse off [l|r|both]\n");
        out("hw servo                servo state\n");
        out("hw led <r> <g> <b> | hw led off\n");
        out("hw buzzer <hz> [ms] | hw buzzer off\n");
        out("hw gpio <pin> [in | out <0|1>]\n");
        return 1;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "scan") == 0) {
        report_imu_bus();
        report_i2c_bus();
        return 0;
    }
    if (strcmp(sub, "imu") == 0) return hw_imu(argc - 1, argv + 1);
    if (strcmp(sub, "baro") == 0) return hw_baro(argc - 1, argv + 1);
    if (strcmp(sub, "i2c") == 0) return hw_i2c(argc - 1, argv + 1);
    if (strcmp(sub, "adc") == 0) return hw_adc(argc - 1, argv + 1);
    if (strcmp(sub, "pulse") == 0) return hw_pulse(argc - 1, argv + 1);
    if (strcmp(sub, "servo") == 0) return hw_servo_state(argc - 1, argv + 1);
    if (strcmp(sub, "led") == 0) return hw_led(argc - 1, argv + 1);
    if (strcmp(sub, "buzzer") == 0) return hw_buzzer(argc - 1, argv + 1);
    if (strcmp(sub, "gpio") == 0) return hw_gpio(argc - 1, argv + 1);
    out("unknown subcommand '%s'; run 'hw' for the list\n", sub);
    return 1;
}

/* ------------------------------------------------------------------ top-level */

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const board_caps_t *caps = board_caps();

    out("board    : %s rev %s\n", caps->hw_name, caps->hw_revision);
    print_adc();
    report_imu_bus();
    print_imu_sample();
    char *baro_args[] = { "baro" };
    hw_baro(1, baro_args);
    hw_servo_state(0, NULL);
    out("reset    : %s\n", board_reset_reason_str());
    return 0;
}

static int cmd_params(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t index = 0; index < parameter_count(); index++) {
        const bf_param_desc_t *desc = parameter_descriptor(index);
        double value = 0;
        parameter_get_scalar(desc->id, &value);
        out("%-28s %10.3f %-5s [%.3f, %.3f]%s\n", desc->name, value, desc->unit, desc->min,
            desc->max, desc->calibration ? " cal" : "");
    }
    return 0;
}

static int cmd_param(int argc, char **argv)
{
    if (argc < 2) {
        out("usage: param get <name> | param set <name> <value> | param reset\n");
        return 1;
    }
    if (strcmp(argv[1], "reset") == 0) {
        esp_err_t err = parameter_reset_to_defaults();
        servo_reload_params();
        sensors_reload_params();
        out("reset    : %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 3) {
        out("usage: param get <name> | param set <name> <value>\n");
        return 1;
    }
    const bf_param_desc_t *desc = parameter_find(argv[2]);
    if (desc == NULL) {
        out("unknown parameter '%s'\n", argv[2]);
        return 1;
    }
    if (strcmp(argv[1], "get") == 0) {
        double value = 0;
        parameter_get_scalar(desc->id, &value);
        out("%s = %.3f %s\n", desc->name, value, desc->unit);
        return 0;
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            out("usage: param set <name> <value>\n");
            return 1;
        }
        if (servo_is_enabled()) {
            out("refused: turn the servo output off first ('servo off')\n");
            return 1;
        }
        char *end = NULL;
        double value = strtod(argv[3], &end);
        if (!argv[3][0] || !end || *end) {
            out("value must be a number\n");
            return 1;
        }
        esp_err_t err = parameter_set_scalar(desc->id, value);
        if (err != ESP_OK) {
            out("set failed: %s (out of range?)\n", esp_err_to_name(err));
            return 1;
        }
        servo_reload_params();
        sensors_reload_params();
        out("%s = %.3f %s\n", desc->name, value, desc->unit);
        return 0;
    }
    out("usage: param get|set|reset ...\n");
    return 1;
}

static int cmd_servo(int argc, char **argv)
{
    if (argc < 2) {
        out("usage: servo <l|r|both> <deg> | servo off\n");
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        servo_force_output_off();
        out("output   : both sides off\n");
        return 0;
    }
    if (argc < 3) {
        out("usage: servo <l|r|both> <deg>\n");
        return 1;
    }
    float degrees = 0;
    if (!parse_float(argv[2], &degrees)) {
        out("angle must be a number\n");
        return 1;
    }
    if (strcmp(argv[1], "both") == 0) {
        esp_err_t left = servo_debug_set_angle_deg(SERVO_LEFT, degrees);
        esp_err_t right = servo_debug_set_angle_deg(SERVO_RIGHT, degrees);
        out("angle    : both <- %.2f deg (%s / %s)\n", degrees, esp_err_to_name(left),
            esp_err_to_name(right));
        if (left != ESP_OK || right != ESP_OK) {
            out("           both sides need calibration; 'hw pulse' works without it\n");
            return 1;
        }
        return 0;
    }
    servo_idx_t index;
    if (!parse_servo_index(argv[1], &index)) {
        out("usage: servo <l|r|both> <deg>\n");
        return 1;
    }
    esp_err_t err = servo_debug_set_angle_deg(index, degrees);
    out("angle    : %s <- %.2f deg (%s)\n", argv[1], degrees, esp_err_to_name(err));
    if (err != ESP_OK) {
        out("           that side needs calibration; 'hw pulse <l|r> <us>' works without it\n");
    }
    return err == ESP_OK ? 0 : 1;
}

esp_err_t bf_hwtest_register(void)
{
    const esp_console_cmd_t commands[] = {
        { .command = "hw", .help = "Hardware tests: scan, imu, baro, i2c, adc, pulse, servo, led, buzzer, gpio", .func = cmd_hw },
        { .command = "status", .help = "One live sample of every peripheral", .func = cmd_status },
        { .command = "params", .help = "List all parameters", .func = cmd_params },
        { .command = "param", .help = "Get/set/reset a parameter", .func = cmd_param },
        { .command = "servo", .help = "Jog a servo angle (needs calibration)", .func = cmd_servo },
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
        esp_err_t err = esp_console_cmd_register(&commands[index]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
