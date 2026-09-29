/**
 * @file weight_uart.c
 *
 * Mirrors the Arduino logic from the user's Uno R4 sketch:
 *   - 9600 baud (configurable)
 *   - read bytes, mask with 0x7F (strip parity / MSB)
 *   - accumulate into a small line buffer
 *   - on '\n', parse and emit
 *
 * Float parser is intentionally tolerant: it scans the line for the first
 * run of [-+]?digits(.digits)? and converts that. Handles formats like:
 *   "+001234.5\n"
 *   " 1234.5 g\r\n"
 *   "ST,GS,+001234.5 kg\r\n"
 *
 * A line longer than LINE_BUF_LEN is discarded entirely (up to the next
 * '\n') instead of emitting its tail as a bogus reading.
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

#define LINE_BUF_LEN 48
#define RX_BUF_SIZE 1024 /* driver ring buffer, must be > UART FIFO (128) */
#define READ_CHUNK 64
#define SAMPLE_QUEUE_LEN 16

static QueueHandle_t s_queue;

/* Find first numeric token, sign-aware. Returns false if none found. */
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
    bool seen_dot = false;
    while (*p && i < sizeof(buf) - 1)
    {
        if (isdigit((unsigned char)*p))
        {
            buf[i++] = *p++;
            any_digit = true;
        }
        else if (*p == '.' && !seen_dot)
        {
            seen_dot = true;
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
    s.ts = time(NULL); /* may be < WEIGHT_TIME_VALID_EPOCH before SNTP sync */

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
        .baud_rate = (int)cfg->uart_baud,
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
    bool discarding = false;
    uint8_t chunk[READ_CHUNK];

    while (1)
    {
        /* Read whatever is available (up to 64 bytes) with a short timeout,
         * instead of one driver call per byte. 20 ms at 9600 baud ~ 19 chars. */
        int n = uart_read_bytes(WEIGHT_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n <= 0)
            continue;

        for (int k = 0; k < n; k++)
        {
            uint8_t byte = chunk[k] & 0x7F; /* match Arduino: strip 8th bit */

            if (byte == '\n')
            {
                if (!discarding && li > 0)
                {
                    line[li] = '\0';
                    emit_line(line);
                }
                li = 0;
                discarding = false;
            }
            else if (byte == '\r')
            {
                /* skip CR */
            }
            else if (discarding)
            {
                /* drop the rest of an over-long line */
            }
            else if (li < sizeof(line) - 1)
            {
                line[li++] = (char)byte;
            }
            else
            {
                ESP_LOGD(TAG, "line overflow - discarding until newline");
                li = 0;
                discarding = true;
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
