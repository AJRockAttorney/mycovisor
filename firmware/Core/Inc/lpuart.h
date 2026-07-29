//
// Created by helios on 6/20/26.
//

#ifndef UNOQBLINKY_LPUART_DRIVER_H
#define UNOQBLINKY_LPUART_DRIVER_H
#include <main.h>
#include <stdbool.h>


typedef void (*lpuart_rx_handler_t)(const uint8_t *data, size_t len);

/* Monotonic count of RX staging events (ISR context). Compare successive
   reads to tell whether new bytes arrived since you last checked. */
extern volatile uint32_t rx_staged_events;

void lpuart_rx_check(void);
void lpuart_rx_error_check(void);
void lpuart_rx_dma_error_check(void);
void lpuart_rx_init(void);
void lpuart_set_rx_handler(lpuart_rx_handler_t handler);
void lpuart_tx_raw(const uint8_t *data, size_t len);
void lpuart_tx_init(void);
void lpuart_tx_complete(void);
bool lpuart_write(const uint8_t *data, size_t len);
void lpuart_init(void);
size_t lpuart_read(uint8_t *dst, size_t max);

#endif //UNOQBLINKY_LPUART_DRIVER_H
