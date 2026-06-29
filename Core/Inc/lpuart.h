//
// Created by helios on 6/20/26.
//

#ifndef UNOQBLINKY_LPUART_DRIVER_H
#define UNOQBLINKY_LPUART_DRIVER_H
#include <main.h>
#include <stdbool.h>


typedef void (*lpuart_rx_handler_t)(const uint8_t *data, size_t len);

void lpuart_rx_check(void);
void lpuart_rx_init(void);
void lpuart_set_rx_handler(lpuart_rx_handler_t handler);
void lpuart_tx_raw(const uint8_t *data, size_t len);
void lpuart_tx_init(void);
void lpuart_tx_complete(void);
bool lpuart_write(const uint8_t *data, size_t len);
void lpuart_tx_complete(void);
void lpuart_init(void);
#endif //UNOQBLINKY_LPUART_DRIVER_H
