#include "bf_fault.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *action;
    bool latching;
} fault_desc_t;

static const fault_desc_t s_faults[BF_FAULT_COUNT] = {
    [BF_FAULT_LINK_LOST]         = { "LINK_LOST",        "stop flapping, slew to safe angle, latch", true },
    [BF_FAULT_ESTOP]             = { "ESTOP",            "stop flapping, slew to safe angle, latch", true },
    [BF_FAULT_SERVO_OVERCURRENT] = { "SERVO_OVERCURRENT", "stop driving the load; do not force a return", true },
    [BF_FAULT_SERVO_STALL]       = { "SERVO_STALL",      "hold/disable by bench policy, prompt for power cut", true },
    [BF_FAULT_UNDERVOLTAGE]      = { "UNDERVOLTAGE",     "warn then latch; return only while power allows", true },
    [BF_FAULT_OVERVOLTAGE]       = { "OVERVOLTAGE",      "reject powered start; software cannot protect the pin", true },
    [BF_FAULT_ADC_SATURATED]     = { "ADC_SATURATED",    "treat reading as invalid, block powered arm", true },
    [BF_FAULT_ADC_UNRELIABLE]    = { "ADC_UNRELIABLE",   "block powered arm until recalibrated", true },
    [BF_FAULT_IMU_LOST]          = { "IMU_LOST",         "warn; disable closed loop; open-loop per verified mode", false },
    [BF_FAULT_IMU_UNHEALTHY]     = { "IMU_UNHEALTHY",    "stop closed loop, fall back to a pre-validated mode", false },
    [BF_FAULT_BARO_LOST]         = { "BARO_LOST",        "warn; disable altitude features", false },
    [BF_FAULT_TASK_WDT]          = { "TASK_WDT",         "restart still locked; report previous fault", true },
    [BF_FAULT_RESET_ABNORMAL]    = { "RESET_ABNORMAL",   "start locked; report previous fault", false },
    [BF_FAULT_PARAM_CORRUPT]     = { "PARAM_CORRUPT",    "run diagnostics on defaults; never auto-arm", false },
    [BF_FAULT_NOT_CALIBRATED]    = { "NOT_CALIBRATED",   "no powered arm with wings until calibrated", false },
    [BF_FAULT_INTERNAL]          = { "INTERNAL",         "latch and report", true },
};

bool bf_fault_is_latching(bf_fault_bit_t bit)
{
    return (bit < BF_FAULT_COUNT) ? s_faults[bit].latching : true;
}

const char *bf_fault_name(bf_fault_bit_t bit)
{
    return (bit < BF_FAULT_COUNT) ? s_faults[bit].name : "UNKNOWN";
}

const char *bf_fault_action(bf_fault_bit_t bit)
{
    return (bit < BF_FAULT_COUNT) ? s_faults[bit].action : "";
}

char *bf_fault_mask_str(bf_fault_mask_t mask, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return buf;
    }
    buf[0] = '\0';
    if (mask == BF_FAULT_MASK_NONE) {
        snprintf(buf, buf_len, "none");
        return buf;
    }

    size_t used = 0;
    for (int i = 0; i < BF_FAULT_COUNT; i++) {
        if (!(mask & BF_FAULT_BIT(i))) {
            continue;
        }
        int written = snprintf(buf + used, buf_len - used, "%s%s",
                               used ? "|" : "", bf_fault_name((bf_fault_bit_t)i));
        if (written < 0 || (size_t)written >= buf_len - used) {
            break;
        }
        used += (size_t)written;
    }
    return buf;
}
