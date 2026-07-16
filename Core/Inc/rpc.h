//
// Created by helios on 6/29/26.
//

#ifndef UNOQBLINKY_RPC_H
#define UNOQBLINKY_RPC_H
#include <stdint.h>
#include "mpack.h"
typedef enum {RPC_OK = 0, RPC_TIMEOUT} rpc_status_t;
typedef void (*rpc_params_fn)(mpack_writer_t *w, void *ctx);
typedef void (*rpc_result_fn)(rpc_status_t status, mpack_node_t error, mpack_node_t result, void *ctx);
typedef void (*rpc_handler_fn)(mpack_node_t params, mpack_writer_t *w);

typedef enum {
    RPC_REG_OK = 0,
    RPC_REG_FULL,      /* table has no free slot */
    RPC_REG_TOO_LONG,  /* name exceeds RPC_METHOD_NAME_MAX */
} rpc_reg_status_t;

void rpc_init(void);
void rpc_poll(void);

/* Called on a static/impossible protocol condition (e.g. the fixed method
   table is full, or the mpack tree runs out of memory) that this generic
   transport layer has no way to recover from on its own. rpc.c only calls
   this -- it does not define it, and knows nothing about how the fault is
   handled (log-and-reset, halt-and-blink, whatever fits the target). The
   application/platform layer must supply an implementation; this keeps
   rpc.c/rpc.h free of any MCU- or board-specific dependency, so they can
   be dropped onto a different Cortex-M target's CubeMX-generated LL
   firmware without modification. */
void rpc_fault(void);

uint32_t rpc_call(const char *method, rpc_params_fn write_params, void *params_ctx,
                  rpc_result_fn on_result, void *result_ctx);
void rpc_notify(const char *method, rpc_params_fn write_params, void *params_ctx);

/* Registers an inbound method handler. Main-loop context only (same
   non-reentrancy contract as the rest of rpc.c's mutable state) -- never
   call from an ISR. Safe to call at any point, including after rpc_poll()
   has already started running: registration_tick()'s sweep is
   level-triggered against the current count, not a one-shot latch. */
rpc_reg_status_t rpc_register(const char *name, rpc_handler_fn handler);
#endif //UNOQBLINKY_RPC_H
