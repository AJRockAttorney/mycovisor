#include "main.h"
#include "lpuart.h"
#include <stdio.h>

#include "mpack.h"
#include "rpc.h"

#ifdef DEBUG
/* TEMP -- soak-test support method. Only registered in Debug builds so it
   never ships as reachable RPC surface in a production image. */
volatile uint32_t rpc_test_notif_count = 0;

static void handle_getcount(mpack_node_t params, mpack_writer_t *w) {
    rpc_test_notif_count++;
    (void)params;
    mpack_write_nil(w);
    mpack_write_uint(w, rpc_test_notif_count);
}
#endif

void app_main(void) {
    lpuart_init();
    rpc_init();
#ifdef DEBUG
    if (rpc_register("getcount", handle_getcount) != RPC_REG_OK) rpc_fault();
#endif
    LL_GPIO_SetOutputPin(GPIOH, LL_GPIO_PIN_10);//active-low
    while (1) {
        rpc_poll();

    }
}