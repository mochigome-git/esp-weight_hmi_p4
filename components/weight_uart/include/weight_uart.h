/**
 * @file weight_uart.h
 * @brief RS232 weight reader.
 *
 * Pins on Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1 (40-pin header):
 *   - GPIO 37 = TX (to external MAX3232 module)
 *   - GPIO 38 = RX (from MAX3232)
 *
 * Reads ASCII lines terminated by '\n' (matching the Arduino code's protocol).
 * Each character is masked with 0x7F (7-bit). Parses values like
 * "+001234.5", " -0123.45 g", "ST,GS,+001234.5 kg" etc. Pushes a
 * weight_uart_sample_t onto an internal queue, exposed via _take().
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEIGHT_UART_PIN_TX   22    // header pin 19
#define WEIGHT_UART_PIN_RX   21    // header pin 21
#define WEIGHT_UART_PORT     UART_NUM_1

typedef struct {
    float    value;        /* parsed numeric value, sign preserved */
    time_t   ts;            /* unix epoch sec when line completed */
    char     raw[24];       /* trimmed original line, for debug */
} weight_uart_sample_t;

esp_err_t weight_uart_start(void);

/* Block up to ticks_to_wait for the next parsed sample. */
bool weight_uart_take(weight_uart_sample_t *out, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
