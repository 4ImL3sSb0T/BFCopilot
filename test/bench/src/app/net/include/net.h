#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The bench firmware's link and console.
 *
 * Wi-Fi joins the configured AP; the hardware console is then served both on TCP (so the
 * laptop drives tests without a cable) and on the programming port (the fallback when the
 * network is misconfigured).
 *
 * Call bf_net_wifi_start() first, then bf_net_console_init(), then register commands - the
 * console has to exist before esp_console_cmd_register() accepts anything.
 */
esp_err_t bf_net_wifi_start(void);
esp_err_t bf_net_console_init(void);

/*
 * Output for every command. Writes to stdout and mirrors to the connected TCP client, so a
 * remote console sees the same text the programming port does.
 */
int bf_net_write(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
