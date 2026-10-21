/*
 * Console link for the bench firmware.
 *
 * The console is the ESP32-C6 USB-Serial-JTAG port: the same cable used for flashing
 * carries the commands, so bring-up does not depend on a working Wi-Fi network and the
 * firmware needs no credentials. There is no TCP path and no secondary console.
 */
#include "link.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_console.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "link";

#define INPUT_MAX 160

#if !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#error "the bench console must be CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG"
#endif

int bf_link_write(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vprintf(format, args);
    va_end(args);
    return written;
}

esp_err_t bf_link_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hw> ";
    repl_cfg.max_cmdline_length = INPUT_MAX;
    /* The parser keeps at most max_cmdline_args - 1 tokens and drops the rest without a word,
     * so this has to clear the longest command: `flap` takes 9 (name + 8 values). The ESP-IDF
     * default is 32; the 8 that used to be here silently truncated `flap` to 7 arguments. */
    repl_cfg.max_cmdline_args = 16;
    repl_cfg.task_stack_size = 4096;

    esp_err_t err = esp_console_register_help_command();
    if (err != ESP_OK) return err;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console repl: %s", esp_err_to_name(err));
        return err;
    }
    return esp_console_start_repl(repl);
}
