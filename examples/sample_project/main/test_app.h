/*
 * End-to-end test for the CH9434 SPI-to-4xUART bridge.
 *
 * Hardware wiring expected:
 *   - UART0: RX0 <-> TX0 (loopback)
 *   - UART1: TX1  ->  UART2: RX2 (cross-link)
 */
#ifndef TEST_APP_H
#define TEST_APP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the full end-to-end test in a continuous loop.
 *   1. Sends a marker on UART0, expects it back on UART0 RX (loopback).
 *   2. Sends a marker on UART1, expects it back on UART2 RX (cross).
 * Prints test results to the console. Never returns.
 */
void test_app_run(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_APP_H */
