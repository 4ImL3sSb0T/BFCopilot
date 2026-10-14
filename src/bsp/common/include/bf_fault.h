#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared fault vocabulary. Every fault is a bit so the whole set fits one word and
 * can be reported wholesale in telemetry. The concrete detection thresholds and
 * response actions live in the safety component, not here.
 */
typedef enum {
    BF_FAULT_LINK_LOST = 0,      /* last valid control frame too old, or upstream lease expired */
    BF_FAULT_ESTOP,              /* operator emergency stop, latched locally */
    BF_FAULT_SERVO_OVERCURRENT,  /* servo current above calibrated limit */
    BF_FAULT_SERVO_STALL,        /* suspected stall / no motion with load applied */
    BF_FAULT_UNDERVOLTAGE,       /* pack voltage below calibrated limit */
    BF_FAULT_OVERVOLTAGE,        /* pack voltage above allowed input */
    BF_FAULT_ADC_SATURATED,      /* an analog channel pinned at full scale */
    BF_FAULT_ADC_UNRELIABLE,     /* readings inconsistent / not calibrated */
    BF_FAULT_IMU_LOST,           /* IMU device absent or bus error */
    BF_FAULT_IMU_UNHEALTHY,      /* IMU present but data age/saturation/estimator invalid */
    BF_FAULT_BARO_LOST,          /* barometer absent or data stale */
    BF_FAULT_TASK_WDT,           /* a supervised task missed its deadline */
    BF_FAULT_RESET_ABNORMAL,     /* previous boot ended in panic/watchdog/brownout */
    BF_FAULT_PARAM_CORRUPT,      /* stored parameters failed CRC/version/range checks */
    BF_FAULT_NOT_CALIBRATED,     /* required calibration missing or marked invalid */
    BF_FAULT_INTERNAL,           /* catch-all for unexpected internal state */
    BF_FAULT_COUNT
} bf_fault_bit_t;

typedef uint32_t bf_fault_mask_t;

#define BF_FAULT_BIT(bit) ((bf_fault_mask_t)1u << (bit))
#define BF_FAULT_MASK_NONE ((bf_fault_mask_t)0u)

/* Faults that can only be cleared by an explicit operator CLEAR_FAULT once the
 * underlying condition is gone. The rest clear automatically when the condition
 * disappears. */
bool bf_fault_is_latching(bf_fault_bit_t bit);

const char *bf_fault_name(bf_fault_bit_t bit);

/* One-line response hint, mirroring the PLAN fault table. */
const char *bf_fault_action(bf_fault_bit_t bit);

/* Render a mask as "A|B|C" into buf; always NUL-terminated. Returns buf. */
char *bf_fault_mask_str(bf_fault_mask_t mask, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
