#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

esp_err_t sub_uart_init(void);

esp_err_t sub_rx_mirror_start(uart_port_t console_uart);
esp_err_t sub_rx_mirror_stop(void);

esp_err_t sub_send_line(const char *line);

esp_err_t sub_reset_pulse(int pulse_ms);