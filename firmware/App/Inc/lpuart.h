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

void lpuart_rx_init(void);
void lpuart_set_rx_handler(lpuart_rx_handler_t handler);
void lpuart_tx_raw(const uint8_t *data, size_t len);
void lpuart_tx_init(void);
bool lpuart_write(const uint8_t *data, size_t len);
void lpuart_init(void);
size_t lpuart_read(uint8_t *dst, size_t max);

/* IRQ entry points -- call one of these from the matching handler in
   Core/Src/stm32u5xx_it.c. All flag-check/clear logic lives here, not in
   the CubeMX-owned file, so stm32u5xx_it.c stays a trampoline. */
void lpuart_tx_dma_irq(void);   /* GPDMA1_Channel1_IRQHandler */
void lpuart_rx_dma_irq(void);   /* GPDMA1_Channel3_IRQHandler */
void lpuart_uart_irq(void);     /* LPUART1_IRQHandler */

#endif //UNOQBLINKY_LPUART_DRIVER_H
