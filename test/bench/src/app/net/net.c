/*
 * Wi-Fi link and hardware console for the bench firmware.
 *
 * This is the only place the bench firmware talks to the network. It joins the configured
 * AP and serves the console over TCP so the laptop can drive bring-up tests without a
 * cable; the same commands stay available on the programming port as the fallback when the
 * network is wrong.
 */
#include "net.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define SERVER_STACK 4096
#define SERVER_PRIO 3
#define INPUT_MAX 160

/* Client currently awaiting a command's output; -1 when nothing is being served. */
static volatile int s_console_client = -1;

int bf_net_write(const char *format, ...)
{
    /* static: commands run on modest task stacks and a 512-byte frame per call is not
     * worth the headroom. The console is driven by one person at a time. */
    static char line[512];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) return written;
    fputs(line, stdout);
    int client = s_console_client;
    if (client >= 0) send(client, line, strlen(line), 0);
    return written;
}

static void reply(int client, const char *text)
{
    send(client, text, strlen(text), 0);
}

static void run_line(int client, char *line)
{
    s_console_client = client;
    int result = 0;
    esp_err_t err = esp_console_run(line, &result);
    if (err == ESP_ERR_NOT_FOUND) reply(client, "unknown command; try 'help'\n");
    else if (err == ESP_ERR_INVALID_ARG) reply(client, "invalid command\n");
    s_console_client = -1;
}

/* One console client at a time, but a new one takes the port over: a client that vanishes
 * without closing (Wi-Fi drop, killed host process) would otherwise hold it forever. */
static void serve(int listener, int client)
{
    reply(client, "hw> ");
    char line[INPUT_MAX];
    size_t used = 0;
    char chunk[64];
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        FD_SET(client, &readable);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 20000 };
        if (select(client + 1, &readable, NULL, NULL, &timeout) < 0) return;
        if (FD_ISSET(listener, &readable)) return;
        if (!FD_ISSET(client, &readable)) continue;
        int count = recv(client, chunk, sizeof(chunk), 0);
        if (count <= 0) return;
        for (int index = 0; index < count; index++) {
            char value = chunk[index];
            if (value == '\r') continue;
            if (value == '\n') {
                line[used] = '\0';
                if (used) run_line(client, line);
                used = 0;
                reply(client, "hw> ");
                continue;
            }
            if (used < INPUT_MAX - 1) line[used++] = value;
        }
    }
}

static void server_task(void *arg)
{
    (void)arg;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    while (!netif) {
        vTaskDelay(pdMS_TO_TICKS(100));
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    esp_netif_ip_info_t info;
    while (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = { .sin_family = AF_INET,
                                   .sin_addr.s_addr = htonl(INADDR_ANY),
                                   .sin_port = htons(CONFIG_BF_BENCH_PORT) };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen tcp/%d failed: errno %d", CONFIG_BF_BENCH_PORT, errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "console on " IPSTR " tcp/%d", IP2STR(&info.ip), CONFIG_BF_BENCH_PORT);

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        ESP_LOGI(TAG, "console client connected");
        serve(listener, client);
        close(client);
        ESP_LOGI(TAG, "console client disconnected");
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *info = data;
        ESP_LOGW(TAG, "link down (reason %d); reconnecting", info->reason);
        esp_wifi_connect();
    }
}

esp_err_t bf_net_wifi_start(void)
{
    if (strlen(CONFIG_BF_WIFI_STA_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_BF_WIFI_STA_SSID is empty; build with a filled local.defaults");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    wifi_config_t station = { 0 };
    snprintf((char *)station.sta.ssid, sizeof(station.sta.ssid), "%s", CONFIG_BF_WIFI_STA_SSID);
    snprintf((char *)station.sta.password, sizeof(station.sta.password), "%s",
             CONFIG_BF_WIFI_STA_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining \"%s\"", CONFIG_BF_WIFI_STA_SSID);

    if (xTaskCreate(server_task, "console-net", SERVER_STACK, NULL, SERVER_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bf_net_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hw> ";
    repl_cfg.max_cmdline_length = INPUT_MAX;
    repl_cfg.max_cmdline_args = 8;
    repl_cfg.task_stack_size = 4096;

    esp_err_t err = esp_console_register_help_command();
    if (err != ESP_OK) return err;
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t dev_cfg = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&dev_cfg, &repl_cfg, &repl);
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console repl: %s", esp_err_to_name(err));
        return err;
    }
    return esp_console_start_repl(repl);
}
