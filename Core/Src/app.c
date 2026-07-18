#include "main.h"
#include "lpuart.h"
#include "fault.h"

#include "mpack.h"
#include "rpc.h"

void rpc_fault(void) {
    fault_record_and_reset(FAULT_REASON_RPC_FAULT);
}

void app_main(void) {
    lpuart_init();
    rpc_init();
    LL_GPIO_SetOutputPin(GPIOH, LL_GPIO_PIN_10);//active-low
    while (1) {
        rpc_poll();
    }
}