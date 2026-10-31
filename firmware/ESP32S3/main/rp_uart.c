#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rp_uart.h"

static const char *TAG = "rp_uart";

#define RP_UART_LINE_GAP_MS 100

esp_err_t rp_uart_start(void)
{
    uart_config_t cfg = {
        .baud_rate = RP_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret;

    ret = uart_driver_install(RP_UART_PORT_NUM, 4096, 4096, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ESP_ERROR_CHECK(uart_param_config(RP_UART_PORT_NUM, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        RP_UART_PORT_NUM,
        RP_UART_TX_GPIO,
        RP_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    uart_flush_input(RP_UART_PORT_NUM);

    ESP_LOGI(TAG, "UART started TX=%d RX=%d baud=%d",
             RP_UART_TX_GPIO,
             RP_UART_RX_GPIO,
             RP_UART_BAUD_RATE);

    return ESP_OK;
}

esp_err_t rp_uart_command(const char *cmd, char *response, int response_len, int timeout_ms)
{
    if (!cmd || !response || response_len <= 1) {
        return ESP_ERR_INVALID_ARG;
    }

    response[0] = '\0';

    uart_flush_input(RP_UART_PORT_NUM);

    uart_write_bytes(RP_UART_PORT_NUM, cmd, strlen(cmd));
    uart_write_bytes(RP_UART_PORT_NUM, "\n", 1);
    uart_wait_tx_done(RP_UART_PORT_NUM, pdMS_TO_TICKS(50));

    /*
       timeout_ms bounds the wait for the reply to start. Once bytes arrive
       the reply may still take up to RP_UART_LINE_GAP_MS between bytes: a
       busy ESP32 can leave this task unscheduled for tens of milliseconds
       while the RP's line sits in the driver buffer, and a long STATUS line
       must not be cut short by that.
    */
    int64_t now = esp_timer_get_time();
    int64_t deadline = now + ((int64_t)timeout_ms * 1000);
    int pos = 0;

    while (now < deadline && pos < response_len - 1) {
        uint8_t buf[64];
        size_t avail = 0;
        int n;

        uart_get_buffered_data_len(RP_UART_PORT_NUM, &avail);
        if (avail > sizeof(buf)) {
            avail = sizeof(buf);
        }

        if (avail > 0) {
            n = uart_read_bytes(RP_UART_PORT_NUM, buf, avail, 0);
        } else {
            n = uart_read_bytes(RP_UART_PORT_NUM, buf, 1, pdMS_TO_TICKS(10));
        }

        for (int i = 0; i < n; i++) {
            uint8_t ch = buf[i];

            if (ch == '\n') {
                response[pos] = '\0';
                return ESP_OK;
            }

            if (ch != '\r' && pos < response_len - 1) {
                response[pos++] = (char)ch;
            }
        }

        now = esp_timer_get_time();
        if (n > 0) {
            int64_t gap_deadline = now + (int64_t)RP_UART_LINE_GAP_MS * 1000;
            if (gap_deadline > deadline) {
                deadline = gap_deadline;
            }
        }
    }

    response[pos] = '\0';
    return ESP_ERR_TIMEOUT;
}

esp_err_t rp_uart_read_line(char *response, int response_len, int timeout_ms)
{
    if (!response || response_len <= 1) {
        return ESP_ERR_INVALID_ARG;
    }

    response[0] = '\0';

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int pos = 0;

    while (xTaskGetTickCount() < deadline && pos < response_len - 1) {
        uint8_t buf[64];

        int n = uart_read_bytes(
            RP_UART_PORT_NUM,
            buf,
            sizeof(buf),
            pdMS_TO_TICKS(2)
        );

        for (int i = 0; i < n && pos < response_len - 1; i++) {
            uint8_t ch = buf[i];

            if (ch == '\n') {
                response[pos] = '\0';
                return ESP_OK;
            }

            if (ch != '\r') {
                response[pos++] = (char)ch;
            }
        }
    }

    response[pos] = '\0';
    return ESP_ERR_TIMEOUT;
}
