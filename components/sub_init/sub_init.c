#include "sub_init.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/uart.h"

static const char *TAG = "sub";

/* ------------------------- internal state ------------------------- */
static TaskHandle_t s_rx_task = NULL;
static volatile bool s_rx_running = false;
static uart_port_t s_console_uart = UART_NUM_0;
static sub_line_rx_cb_t s_line_cb = NULL;
static void *s_line_cb_ctx = NULL;

#if CONFIG_SUB_UART_ENABLE

esp_err_t sub_uart_init(void)
{
    const uart_port_t sub_uart = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;

    const uart_config_t cfg = {
        .baud_rate  = CONFIG_SUB_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(sub_uart, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(sub_uart,
                       CONFIG_SUB_UART_TX_GPIO,
                       CONFIG_SUB_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    // RX buffer only needed if we read from it. Keep some margin.
    err = uart_driver_install(sub_uart, 4096, 0, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) return err;

    // nRST
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_SUB_NRST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(CONFIG_SUB_NRST_GPIO, 1);

    return ESP_OK;
}

static void rx_mirror_task(void *arg)
{
    (void)arg;
    const uart_port_t sub_uart = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;

    uint8_t buf[128];
    char line[256];
    size_t line_len = 0;

    ESP_LOGI(TAG, "RX mirror ON: sub UART%d -> console UART%d (baud=%d)",
             (int)sub_uart, (int)s_console_uart, (int)CONFIG_SUB_UART_BAUD);

    while (s_rx_running) {
        int n = uart_read_bytes(sub_uart, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            uart_write_bytes(s_console_uart, (const char *)buf, n);

            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];

                if (c == '\r' || c == '\n') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        if (s_line_cb) {
                            s_line_cb(line, s_line_cb_ctx);
                        }
                        line_len = 0;
                    }
                    continue;
                }

                if (line_len < (sizeof(line) - 1)) {
                    line[line_len++] = c;
                } else {
                    line[sizeof(line) - 1] = '\0';
                    if (s_line_cb) {
                        s_line_cb(line, s_line_cb_ctx);
                    }
                    line_len = 0;
                }
            }
        }
        taskYIELD();
    }

    ESP_LOGI(TAG, "RX mirror OFF");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sub_rx_mirror_start(uart_port_t console_uart)
{
    if (s_rx_running) return ESP_OK;

    s_console_uart = console_uart;
    s_rx_running = true;

    BaseType_t ok = xTaskCreate(rx_mirror_task, "sub_rx_mirror", 4096, NULL, 5, &s_rx_task);
    if (ok != pdPASS) {
        s_rx_running = false;
        s_rx_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sub_set_line_callback(sub_line_rx_cb_t cb, void *ctx)
{
    s_line_cb = cb;
    s_line_cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t sub_rx_mirror_stop(void)
{
    if (!s_rx_running) return ESP_OK;

    s_rx_running = false;

    // wait task exits to avoid race on restart
    for (int i = 0; i < 30; i++) { // 300ms
        if (s_rx_task == NULL) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "sub_rx_mirror_stop: timeout waiting task exit");
    return ESP_OK;
}

esp_err_t sub_send_line(const char *line)
{
    if (!line) return ESP_ERR_INVALID_ARG;

    const uart_port_t sub_uart = (uart_port_t)CONFIG_SUB_UART_PORT_NUM;

    int w1 = uart_write_bytes(sub_uart, line, (int)strlen(line));
    int w2 = uart_write_bytes(sub_uart, "\r", 1);

    if (w1 < 0 || w2 < 0) return ESP_FAIL;

    esp_err_t err = uart_wait_tx_done(sub_uart, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_wait_tx_done failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t sub_reset_pulse(int pulse_ms)
{
    if (pulse_ms <= 0) pulse_ms = 50;

    gpio_set_level(CONFIG_SUB_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    gpio_set_level(CONFIG_SUB_NRST_GPIO, 1);

    return ESP_OK;
}

#else

esp_err_t sub_uart_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sub_rx_mirror_start(uart_port_t console_uart) { (void)console_uart; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sub_rx_mirror_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sub_set_line_callback(sub_line_rx_cb_t cb, void *ctx) { (void)cb; (void)ctx; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sub_send_line(const char *line) { (void)line; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sub_reset_pulse(int pulse_ms) { (void)pulse_ms; return ESP_ERR_NOT_SUPPORTED; }

#endif
