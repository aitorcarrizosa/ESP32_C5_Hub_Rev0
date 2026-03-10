#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_console.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "driver/uart.h"

#include "led_init.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"

#include "sub_init.h"
#include "hub_pairing.h"

/* ---------------------------- Variables-- ------------------------- */
esp_netif_t *hub_eth_get_netif(void);
esp_eth_handle_t hub_eth_get_handle(void);

/* ------------------------- Info command ------------------------- */
static int cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Zensor-Hub_Rev0 console OK\r\n");
    return 0;
}

/* ------------------------- Reset command ------------------------- */
static int cmd_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("System resetting...\r\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return 0;
}

/* ------------------------- LED command ------------------------- */
static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\r\n");
        printf("  led off\r\n");
        printf("  led rgb <r> <g> <b>\r\n");
        printf("  led red on|off\r\n");
        printf("  led grn on|off\r\n");
        printf("  led blu on|off\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "off")) {
        status_rgb_off();
        return 0;
    }

    if (!strcmp(argv[1], "rgb")) {
        if (argc < 5) {
            printf("Usage: led rgb <r> <g> <b>\r\n");
            return 0;
        }

        int r = atoi(argv[2]);
        int g = atoi(argv[3]);
        int b = atoi(argv[4]);

        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;

        status_rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
        return 0;
    }

    if (argc >= 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        int on = !strcmp(argv[2], "on");

        if (!strcmp(argv[1], "red"))      status_rgb_set(on ? 255 : 0, 0, 0);
        else if (!strcmp(argv[1], "grn")) status_rgb_set(0, on ? 255 : 0, 0);
        else if (!strcmp(argv[1], "blu")) status_rgb_set(0, 0, on ? 255 : 0);
        else printf("Unknown channel. Use red|grn|blu\r\n");

        return 0;
    }

    printf("Unknown led command. Try: led off | led rgb ...\r\n");
    return 0;
}

/* ------------------------- ETH command ------------------------- */
static void print_ip_info(void)
{
    esp_netif_t *netif = hub_eth_get_netif();
    if (!netif) {
        printf("netif: NULL\r\n");
        return;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        printf("IP   : " IPSTR "\n", IP2STR(&ip.ip));
        printf("MASK : " IPSTR "\n", IP2STR(&ip.netmask));
        printf("GW   : " IPSTR "\n", IP2STR(&ip.gw));
    } else {
        printf("IP   : (not set)\r\n");
    }

    const ip_addr_t *dns0 = dns_getserver(0);
    const ip_addr_t *dns1 = dns_getserver(1);

    if (dns0 && !ip_addr_isany(dns0)) printf("DNS0 : %s\n", ipaddr_ntoa(dns0));
    if (dns1 && !ip_addr_isany(dns1)) printf("DNS1 : %s\r\n", ipaddr_ntoa(dns1));
}

static int cmd_eth(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\r\n");
        printf("  eth info\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "info")) {
        esp_eth_handle_t eth = hub_eth_get_handle();
        if (eth) {
            uint8_t mac[6] = {0};
            if (esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
                printf("MAC  : %02x:%02x:%02x:%02x:%02x:%02x\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
        } else {
            printf("MAC  : (eth not started)\r\n");
        }

        print_ip_info();
        return 0;
    }

    printf("Unknown eth subcommand\r\n");
    return 0;
}

/* ----------------------------- Command Sub-GHz --------------------------- */
static int console_readline_echo(uart_port_t uart, char *out, int out_sz)
{
    int n = 0;

    while (1) {
        uint8_t c;
        int r = uart_read_bytes(uart, &c, 1, pdMS_TO_TICKS(50));
        if (r <= 0) continue;

        if (c == '\r' || c == '\n') {
            const char *nl = "\r\n";
            uart_write_bytes(uart, nl, 2);
            out[n] = 0;
            return n;
        }

        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                const char bs_seq[3] = { '\b', ' ', '\b' };
                uart_write_bytes(uart, bs_seq, 3);
            }
            continue;
        }

        if (c < 0x20) continue;

        if (n < (out_sz - 1)) {
            out[n++] = (char)c;
            uart_write_bytes(uart, (const char *)&c, 1);
        }
    }
}

static int cmd_sub(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\r\n");
        printf("  sub send [text]\r\n");
        printf("  sub rst [ms]\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "send")) {

        uart_flush_input((uart_port_t)CONFIG_SUB_UART_PORT_NUM);        // Clean SUB UART RX buffer

        esp_err_t err = sub_send_line("subgig send");
        if (err != ESP_OK) {
            printf("ERROR: sub_send_line(subgig send) failed: %s\r\n", esp_err_to_name(err));
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        err = sub_send_line("ACKNOWLEDGE");
        if (err != ESP_OK) {
            printf("ERROR: sub_send_line(ACKNOWLEDGE) failed: %s\r\n", esp_err_to_name(err));
            return 0;
        }

        return 0;
    }

    if (!strcmp(argv[1], "rst")) {
        int ms = 50;
        if (argc >= 3) ms = atoi(argv[2]);

        esp_err_t err = sub_reset_pulse(ms);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            printf("nRST not configured (menuconfig)\r\n");
        } else if (err != ESP_OK) {
            printf("sub rst failed: %s\r\n", esp_err_to_name(err));
        }
        return 0;
    }

    printf("Unknown sub command\r\n");
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\r\n");
        printf("  pair start\r\n");
        printf("  pair list\r\n");
        printf("  pair clear\r\n");
        printf("  pair status\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "start")) {
        esp_err_t err = hub_pairing_start();
        if (err != ESP_OK) {
            printf("pair start failed: %s\r\n", esp_err_to_name(err));
        }
        return 0;
    }

    if (!strcmp(argv[1], "list")) {
        char ids[CONFIG_HUB_PAIR_MAX_NODES][HUB_NODE_ID_LEN + 1];
        size_t count = hub_pairing_copy_node_ids(ids, CONFIG_HUB_PAIR_MAX_NODES);
        printf("paired nodes: %u\r\n", (unsigned)count);
        for (size_t i = 0; i < count; i++) {
            printf("  %u: %s\r\n", (unsigned)(i + 1), ids[i]);
        }
        return 0;
    }

    if (!strcmp(argv[1], "clear")) {
        esp_err_t err = hub_pairing_clear();
        if (err != ESP_OK) {
            printf("pair clear failed: %s\r\n", esp_err_to_name(err));
        }
        return 0;
    }

    if (!strcmp(argv[1], "status")) {
        printf("pairing: %s\r\n", hub_pairing_is_active() ? "active" : "idle");
        printf("paired nodes: %u\r\n", (unsigned)hub_pairing_get_count());
        return 0;
    }

    printf("Unknown pair subcommand\r\n");
    return 0;
}

/* ------------------------- Command registration ------------------------- */
static void register_commands(void)
{
    esp_console_register_help_command();

    const esp_console_cmd_t cmd_info_def = {
        .command = "info",
        .help = "Print basic info",
        .hint = NULL,
        .func = &cmd_info,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_reset_def = {
        .command = "reset",
        .help = "Reset the device",
        .hint = NULL,
        .func = &cmd_reset,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_led_def = {
        .command = "led",
        .help = "Control status LED (WS2812 on DevKit or discrete RGB on HUB)",
        .hint = NULL,
        .func = &cmd_led,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_eth_def = {
        .command = "eth",
        .help = "Ethernet utilities (info)",
        .hint = NULL,
        .func = &cmd_eth,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_sub_def = {
        .command = "sub",
        .help = "Sub-GHz module utilities (send/rst)",
        .hint = NULL,
        .func = &cmd_sub,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_pair_def = {
        .command = "pair",
        .help = "Pairing utilities (start/list/clear/status)",
        .hint = NULL,
        .func = &cmd_pair,
        .argtable = NULL,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_info_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_reset_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_led_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_eth_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sub_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_pair_def));
}

/* ------------------------- UART console loop (echo) ------------------------- */
static int uart_readline_echo(uart_port_t uart, char *out, int out_sz)
{
    int n = 0;

    while (1) {
        uint8_t c;
        int r = uart_read_bytes(uart, &c, 1, pdMS_TO_TICKS(50));
        if (r <= 0) continue;

        if (c == '\r' || c == '\n') {
            const char *nl = "\r\n";
            uart_write_bytes(uart, nl, 2);
            out[n] = 0;
            return n;
        }

        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                const char bs_seq[3] = { '\b', ' ', '\b' };
                uart_write_bytes(uart, bs_seq, 3);
            }
            continue;
        }

        if (c < 0x20) continue;

        if (n < (out_sz - 1)) {
            out[n++] = (char)c;
            uart_write_bytes(uart, (const char *)&c, 1);
        }
    }
}

void console_start_uart0(void)
{
    const uart_port_t console_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    const uart_config_t uart_cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(console_uart, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(uart_param_config(console_uart, &uart_cfg));

    esp_console_config_t console_cfg = {
        .max_cmdline_args = 8,
        .max_cmdline_length = 256,
    };

    err = esp_console_init(&console_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    register_commands();

    char line[256];

    vTaskDelay(pdMS_TO_TICKS(100));
    printf("\r\nZensor-Hub_Rev0> ");
    fflush(stdout);

    while (true) {
        int len = uart_readline_echo(console_uart, line, sizeof(line));
        if (len <= 0) {
            printf("Zensor-Hub_Rev0> ");
            fflush(stdout);
            continue;
        }

        int ret = 0;
        err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unknown command\r\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            printf("Invalid args\r\n");
        } else if (err != ESP_OK) {
            printf("Console error: %s\r\n", esp_err_to_name(err));
        }

        printf("Zensor-Hub_Rev0> ");
        fflush(stdout);
    }
}
