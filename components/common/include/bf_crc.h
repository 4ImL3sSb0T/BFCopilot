#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CRC-32/ISO-HDLC (a.k.a. CRC-32, zlib): poly 0x04C11DB7 reflected, init 0xFFFFFFFF,
 * final xor 0xFFFFFFFF. Check value of "123456789" is 0xCBF43926.
 *
 * Kept here as a local implementation so the exact same bytes are produced on the
 * target and on host-side tools/tests, independent of ESP-IDF version changes.
 */
uint32_t bf_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
