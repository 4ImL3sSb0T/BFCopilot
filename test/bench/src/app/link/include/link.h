#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The bench firmware's console, served on the ESP32-C6 USB-Serial-JTAG port.
 *
 * The board is driven over that cable: the console REPL is the USB Serial/JTAG
 * peripheral itself, so the laptop needs no network and the firmware needs no
 * credentials. `scripts/idf.sh monitor` and the host tools see the same port.
 *
 * Register commands only after bf_link_console_init() - esp_console_cmd_register()
 * rejects commands before the REPL exists.
 */
esp_err_t bf_link_console_init(void);

/*
 * Output for every command. Writes to stdout, which the console configuration
 * routes to the USB-Serial-JTAG port.
 */
int bf_link_write(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
