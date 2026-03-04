#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_init.h" 

static int cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Zensor-Hub_Rev0 console OK\r\n");
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Rebooting...\r\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return 0;
}

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

        if (r < 0) { r = 0; }
        if (r > 255) { r = 255; }

        if (g < 0) { g = 0; }
        if (g > 255) { g = 255; }

        if (b < 0) { b = 0; }
        if (b > 255) { b = 255; }

        status_rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
        return 0;
    }

    if (argc >= 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        int on = !strcmp(argv[2], "on");

        if (!strcmp(argv[1], "red")) status_rgb_set(on ? 255 : 0, 0, 0);
        else if (!strcmp(argv[1], "grn")) status_rgb_set(0, on ? 255 : 0, 0);
        else if (!strcmp(argv[1], "blu")) status_rgb_set(0, 0, on ? 255 : 0);
        else printf("Unknown channel. Use red|grn|blu\r\n");

        return 0;
    }

    printf("Unknown led command. Try: led off | led rgb ...\r\n");
    return 0;
}

static void register_commands(void)
{
    esp_console_register_help_command();

    const esp_console_cmd_t cmd = {
        .command = "info",
        .help = "Print basic info",
        .hint = NULL,
        .func = &cmd_info,
        .argtable = NULL,
    };

    const esp_console_cmd_t cmd_reset_def = {
        .command = "reset",
        .help = "Reboot the device",
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
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_reset_def));
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_led_def));
    
}

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

        if (c < 0x20) {
            continue;
        }

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