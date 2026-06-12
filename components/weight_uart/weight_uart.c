/**
 * @file weight_uart.c
 *
 * Mirrors the Arduino logic from the user's Uno R4 sketch:
 *   - 9600 baud (configurable)
 *   - read byte, mask with 0x7F (strip parity / MSB)
 *   - accumulate into a small line buffer
 *   - on '\n', parse and emit
 *
 * Float parser is intentionally tolerant: it scans the line for the first
 * run of [-+]?digits(.digits)? and atof()'s that. Handles formats like:
 *   "+001234.5\n"
 *   " 1234.5 g\r\n"
 *   "ST,GS,+001234.5 kg\r\n"
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "weight_uart.h"
#include "weight_config.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "uart";

#define LINE_BUF_LEN 32
#define RX_BUF_SIZE 512
#define SAMPLE_QUEUE_LEN 16

static QueueHandle_t s_queue;

/* Find first numeric token, sign-aware. Returns parsed float, or NAN-ish 0 if not found. */
static bool parse_first_number(const char *s, float *out)
{
    const char *p = s;

    /* skip until we see a digit or +/- followed by digit/dot */
    while (*p)
    {
        if (isdigit((unsigned char)*p))
            break;
        if ((*p == '+' || *p == '-') &&
            (isdigit((unsigned char)p[1]) || p[1] == '.'))
            break;
        p++;
    }
    if (!*p)
        return false;

    char buf[24];
    size_t i = 0;
    if (*p == '+' || *p == '-')
    {
        buf[i++] = *p++;
    }
    bool any_digit = false;
    while (*p && i < sizeof(buf) - 1)
    {
        if (isdigit((unsigned char)*p))
        {
            buf[i++] = *p++;
            any_digit = true;
        }
        else if (*p == '.')
        {
            buf[i++] = *p++;
        }
        else
            break;
    }
    buf[i] = '\0';
    if (!any_digit)
        return false;

    *out = strtof(buf, NULL);
    return true;
}

static void emit_line(const char *line)
{
    weight_uart_sample_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.raw, sizeof(s.raw), "%s", line);
    if (!parse_first_number(line, &s.value))
    {
        ESP_LOGD(TAG, "unparseable line: '%s'", line);
        return;
    }
    s.ts = time(NULL);

    /* Non-blocking send - if queue full, drop oldest by reading one */
    if (xQueueSend(s_queue, &s, 0) != pdTRUE)
    {
        weight_uart_sample_t throwaway;
        xQueueReceive(s_queue, &throwaway, 0);
        xQueueSend(s_queue, &s, 0);
    }
}

static void uart_task(void *arg)
{
    const weight_config_t *cfg = weight_config_get();

    uart_config_t uart_cfg = {
        .baud_rate = cfg->uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(WEIGHT_UART_PORT, RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(WEIGHT_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(WEIGHT_UART_PORT,
                                 WEIGHT_UART_PIN_TX, WEIGHT_UART_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART started on TX=%d RX=%d baud=%lu",
             WEIGHT_UART_PIN_TX, WEIGHT_UART_PIN_RX,
             (unsigned long)cfg->uart_baud);

    char line[LINE_BUF_LEN];
    size_t li = 0;
    uint8_t byte;

    while (1)
    {
        int n = uart_read_bytes(WEIGHT_UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0)
            continue;

        byte &= 0x7F; /* match Arduino: strip 8th bit */

        if (byte == '\n')
        {
            line[li] = '\0';
            if (li > 0)
                emit_line(line);
            li = 0;
        }
        else if (byte == '\r')
        {
            /* skip CR */
        }
        else
        {
            if (li < sizeof(line) - 1)
            {
                line[li++] = (char)byte;
            }
            else
            {
                /* overflow - reset, prevent unterminated junk */
                li = 0;
            }
        }
    }
}

esp_err_t weight_uart_start(void)
{
    s_queue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(weight_uart_sample_t));
    if (!s_queue)
        return ESP_ERR_NO_MEM;
    BaseType_t r = xTaskCreatePinnedToCore(uart_task, "uart", 4096, NULL, 10, NULL, 0);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool weight_uart_take(weight_uart_sample_t *out, TickType_t ticks)
{
    return xQueueReceive(s_queue, out, ticks) == pdTRUE;
}
