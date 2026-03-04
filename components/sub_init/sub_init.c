#include "sub_init.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "sdkconfig.h"

#if CONFIG_SUB_UART_ENABLE

static const char *TAG = "sub_uart_bridge";
static TaskHandle_t s_bridge_task = NULL;
static bool s_started = false;

static void uart_bridge_task(void *arg)
{
    const uart_port_t console_uart = (uart_port_t)CONFIG_CONSOLE_UART_PORT_NUM;
    const uart_port_t sub_uart     = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;

    const int buf_sz = CONFIG_UART_BRIDGE_BUF_SIZE;

    uint8_t *buf_console = (uint8_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_DEFAULT);
    uint8_t *buf_sub     = (uint8_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_DEFAULT);

    if (!buf_console || !buf_sub) {
        ESP_LOGE(TAG, "malloc failed (buf_sz=%d)", buf_sz);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART bridge running: console UART%d <-> sub UART%d",
             (int)console_uart, (int)sub_uart);

    while (1) {
        // 1) console -> sub
        int n1 = uart_read_bytes(console_uart, buf_console, buf_sz, pdMS_TO_TICKS(10));
        if (n1 > 0) {
            // write exactly what was read
            int w1 = uart_write_bytes(sub_uart, (const char *)buf_console, n1);
            if (w1 < 0) {
                ESP_LOGW(TAG, "uart_write_bytes sub failed");
            }
        }

        // 2) sub -> console
        int n2 = uart_read_bytes(sub_uart, buf_sub, buf_sz, pdMS_TO_TICKS(10));
        if (n2 > 0) {
            int w2 = uart_write_bytes(console_uart, (const char *)buf_sub, n2);
            if (w2 < 0) {
                ESP_LOGW(TAG, "uart_write_bytes console failed");
            }
        }
        taskYIELD();
    }
}

static esp_err_t uart_setup_console(void)
{
    const uart_port_t console_uart = (uart_port_t)CONFIG_CONSOLE_UART_PORT_NUM;

    uart_config_t cfg = {
        .baud_rate = CONFIG_SUB_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = (CONFIG_SUB_UART_RTS_GPIO >= 0 && CONFIG_SUB_UART_CTS_GPIO >= 0)
                        ? UART_HW_FLOWCTRL_CTS_RTS
                        : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 64,
        .source_clk = UART_SCLK_DEFAULT
    };

    esp_err_t err = uart_param_config(console_uart, &cfg);
    if (err != ESP_OK) return err;

    err = uart_driver_install(console_uart, 2048, 2048, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t uart_setup_sub(void)
{
    const uart_port_t sub_uart = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;

    uart_config_t cfg = {
        .baud_rate = CONFIG_SUB_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = (CONFIG_SUB_UART_RTS_GPIO >= 0 && CONFIG_SUB_UART_CTS_GPIO >= 0)
                        ? UART_HW_FLOWCTRL_CTS_RTS
                        : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 64,
        .source_clk = UART_SCLK_DEFAULT
    };

    esp_err_t err = uart_param_config(sub_uart, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(sub_uart,
                       CONFIG_SUB_UART_TX_GPIO,
                       CONFIG_SUB_UART_RX_GPIO,
                       (CONFIG_SUB_UART_RTS_GPIO >= 0) ? CONFIG_SUB_UART_RTS_GPIO : UART_PIN_NO_CHANGE,
                       (CONFIG_SUB_UART_CTS_GPIO >= 0) ? CONFIG_SUB_UART_CTS_GPIO : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    // Buffers RX/TX para el driver
    err = uart_driver_install(sub_uart, 2048, 2048, 0, NULL, 0);
    return err;
}

bool sub_uart_bridge_init(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "already started");
        return true;
    }

    esp_err_t err;

    err = uart_setup_console();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_setup_console failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_setup_sub();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_setup_sub failed: %s", esp_err_to_name(err));
        return false;
    }

    BaseType_t ok = xTaskCreate(
        uart_bridge_task,
        "uart_bridge",
        CONFIG_UART_BRIDGE_TASK_STACK,
        NULL,
        CONFIG_UART_BRIDGE_TASK_PRIO,
        &s_bridge_task
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_bridge_task = NULL;
        return false;
    }

    s_started = true;
    return true;
}

void sub_uart_bridge_deinit(void)
{
    if (!s_started) return;

    if (s_bridge_task) {
        vTaskDelete(s_bridge_task);
        s_bridge_task = NULL;
    }

    const uart_port_t sub_uart = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;
    uart_driver_delete(sub_uart);

    s_started = false;
}

#else

bool sub_uart_bridge_init(void)
{
    return false;
}

void sub_uart_bridge_deinit(void)
{
}

#endif