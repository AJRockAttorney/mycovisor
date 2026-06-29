#include "main.h"
#include "lpuart.h"
#include <stdio.h>

static void uart_write(const char *s) {
    while (*s) {
        while (!LL_LPUART_IsActiveFlag_TXE(LPUART1)) { }  // wait for TX empty
        LL_LPUART_TransmitData8(LPUART1, (uint8_t)*s++);
    }
}

static void echo_handler(const uint8_t *data, size_t len) {
    lpuart_write(data, len);
}

void app_main(void) {
    lpuart_init();
    lpuart_set_rx_handler(echo_handler);
    while (1) {

    }
}