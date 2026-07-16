//
// Created by helios on 6/29/26.
//

#include "../Inc/rpc.h"
#include "mpack.h"
#include "lpuart.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define RPC_MAX_PENDING              10
#define RPC_TX_BUF                   128
#define RPC_NOTIFY_SCRATCH_BUF       64    /* discard buffer for a notification handler's
                                               would-be response -- never sent, just needs
                                               to be big enough that mpack doesn't overflow
                                               mid-write */
#define RPC_CALL_TIMEOUT_MS          3000u
#define RPC_TX_RETRY_MS              20u    /* bound on spin-wait for tx_rb space; several times the
                                                worst-case drain time of a full 256B tx_rb at LPUART's
                                                baud rate, so it only ever blocks the main loop briefly */

static mpack_tree_t rx_tree;

static inline uint32_t rpc_now_ms(void) {return HAL_GetTick();}

static bool rpc_send(const uint8_t *buf, size_t len) {
    uint32_t start = rpc_now_ms();
    while (!lpuart_write(buf, len)) {
        if ((uint32_t)(rpc_now_ms() - start) >= RPC_TX_RETRY_MS) {
            return false;
        }
    }
    return true;
}

static inline void reg_led_on(void)  { LL_GPIO_ResetOutputPin(GPIOH, LL_GPIO_PIN_10); }
static inline void reg_led_off(void) { LL_GPIO_SetOutputPin(GPIOH, LL_GPIO_PIN_10); }

static size_t mpack_rx_fill(mpack_tree_t *tree, char *buffer, size_t count) {
    (void)tree;
    return lpuart_read((uint8_t *)buffer, count);
}
static mpack_tree_t nil_tree;
static mpack_node_data_t nil_pool[4];
static const char nil_data[1] = { (char)0xc0};
static bool nil_ready = false;
static void init_nil(void) {
    mpack_tree_init_pool(&nil_tree, nil_data,sizeof(nil_data), nil_pool, 4);
    mpack_tree_parse(&nil_tree);
}
static mpack_node_t nil_node(void) {return mpack_tree_root(&nil_tree);}

static void handle_ping(mpack_node_t params, mpack_writer_t *w) {
    (void)params;
    mpack_write_nil(w);
    mpack_write_nil(w);
}

void rpc_init(void) {
    mpack_tree_init_stream(&rx_tree, mpack_rx_fill, NULL,
                           512,
                           32);
    if (!nil_ready){init_nil(); nil_ready=true;}

    static bool builtins_registered = false;
    if (!builtins_registered) {
        if (rpc_register("ping", handle_ping) != RPC_REG_OK) rpc_fault();
        builtins_registered = true;
    }
}
typedef struct {
    uint32_t      msgid;
    uint32_t      deadline_ms;
    bool          in_use;
    rpc_result_fn cb;
    void          *ctx;
} rpc_pending_t;

static rpc_pending_t pending[RPC_MAX_PENDING];
static uint32_t next_msgid = 1;

static size_t encode_outgoing(uint8_t *buf, size_t cap, uint32_t type, uint32_t msgid, bool has_msgid,
                              const char *method, rpc_params_fn write_params, void *params_ctx)       {
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf,cap);
    mpack_start_array(&w, has_msgid ? 4 : 3);
    mpack_write_uint(&w, type);
    if (has_msgid) mpack_write_uint(&w, msgid);
    mpack_write_cstr(&w, method);
    if (write_params) {
        write_params(&w, params_ctx);
    }
    else {
        mpack_start_array(&w,0);
        mpack_finish_array(&w);
    }
    mpack_finish_array(&w);
    size_t used = mpack_writer_buffer_used(&w);
    return (mpack_writer_destroy(&w) == mpack_ok) ? used : 0;
}

uint32_t rpc_call(const char *method,
                  rpc_params_fn write_params,
                  void *params_ctx,
                  rpc_result_fn on_result,
                  void *result_ctx) {
    rpc_pending_t *slot = NULL;
    for (size_t i = 0; i < RPC_MAX_PENDING; i++) {
        if (!pending[i].in_use) {slot =&pending[i]; break; }
    }
    if (slot == NULL) { return 0;}

    if (next_msgid == 0) next_msgid = 1;
    uint32_t msgid = next_msgid++;

    uint8_t buf[RPC_TX_BUF];
    size_t used = encode_outgoing(buf, sizeof(buf), 0, msgid, true, method, write_params, params_ctx);
    if (used == 0) return 0;

    if (!rpc_send(buf, used)) {
        /* Never went out: don't reserve a slot for a call the peer will
           never see, and don't leave the caller believing it was sent. */
        return 0;
    }

    slot->msgid       = msgid;
    slot->deadline_ms = rpc_now_ms()+RPC_CALL_TIMEOUT_MS;
    slot->cb          = on_result;
    slot->ctx         = result_ctx;
    slot->in_use      = true;

    return msgid;
}

void rpc_notify(const char *method, rpc_params_fn write_params, void *params_ctx) {
    uint8_t buf[RPC_TX_BUF];
    size_t used = encode_outgoing(buf, sizeof(buf), 2,0, false,
                                  method, write_params, params_ctx);
    if (used) {
        rpc_send(buf, used);
    }
}

static void reap_timeouts(uint32_t now) {
    for (size_t i = 0; i < RPC_MAX_PENDING; i++) {
        if (pending[i].in_use && (int32_t)(now-pending[i].deadline_ms) >= 0) {
            rpc_result_fn cb = pending[i].cb;
            void *ctx        = pending[i].ctx;
            pending[i].in_use = false;
            if (cb) cb(RPC_TIMEOUT, nil_node(), nil_node(), ctx);
        }
    }
}

/*SERVER SIDE*/
#define RPC_MAX_METHODS      8
#define RPC_METHOD_NAME_MAX  16

typedef struct {
    char           name[RPC_METHOD_NAME_MAX];
    size_t         name_len;
    rpc_handler_fn handler;
} rpc_method_t;

static rpc_method_t method_table[RPC_MAX_METHODS];
static size_t        method_count = 0;

rpc_reg_status_t rpc_register(const char *name, rpc_handler_fn handler) {
    size_t len = strlen(name);
    if (len == 0 || len >= RPC_METHOD_NAME_MAX) return RPC_REG_TOO_LONG;
    if (method_count >= RPC_MAX_METHODS)        return RPC_REG_FULL;

    rpc_method_t *m = &method_table[method_count];
    memcpy(m->name, name, len);
    m->name_len = len;
    m->handler  = handler;
    method_count++;
    return RPC_REG_OK;
}

static const rpc_method_t *find_method(mpack_node_t method) {
    const char *p = mpack_node_str(method);
    size_t n = mpack_node_strlen(method);
    for (size_t i = 0; i < method_count; i++) {
        if (method_table[i].name_len == n &&
        memcmp(method_table[i].name,p,n) == 0) {
        return &method_table[i];
        }
    }
    return NULL;
}
static void send_response(uint32_t msgid, rpc_handler_fn handler, mpack_node_t params) {
    uint8_t buf[RPC_TX_BUF];
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, sizeof(buf));

    mpack_start_array(&w, 4);
    mpack_write_uint(&w, 1);
    mpack_write_uint(&w, msgid);
    handler(params, &w);
    mpack_finish_array(&w);

    if (mpack_writer_destroy(&w) == mpack_ok) {
        rpc_send(buf, mpack_writer_buffer_used(&w));
    }
}

static void dispatch_no_reply(rpc_handler_fn handler, mpack_node_t params) {
    uint8_t scratch[RPC_NOTIFY_SCRATCH_BUF];
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)scratch, sizeof scratch);
    handler(params, &w);        // writes error+result into the void
    mpack_writer_destroy(&w);   // deliberately NOT lpuart_write()'d
}

/*Registration*/
static size_t reg_idx = 0;
static bool reg_inflight = false;

static void write_register_params(mpack_writer_t *w, void *ctx) {
    const rpc_method_t *m = (const rpc_method_t *)ctx;
    mpack_start_array(w,1);
    mpack_write_str(w, m->name, (uint32_t)m->name_len);
    mpack_finish_array(w);
}

static void on_register_result(rpc_status_t status, mpack_node_t error, mpack_node_t result, void *ctx) {
    (void)error;
    (void)result;
    (void)ctx;
    reg_inflight = false;
    reg_led_off();
    if (status == RPC_TIMEOUT) {
        return;
    }
    reg_idx++;
}

static uint32_t last_reg_attempt_ms = 0;
#define REG_RETRY_MS 500u
static void registration_tick(uint32_t now) {
    if (reg_inflight) return;
    if (reg_idx >= method_count) {
        return;
    }
    if ((int32_t)(now-last_reg_attempt_ms)<(int32_t)REG_RETRY_MS) return;
    const rpc_method_t *m = &method_table[reg_idx];
    last_reg_attempt_ms = now;
    if (rpc_call("$/register", write_register_params, (void *)m, on_register_result, NULL)) {
        reg_inflight = true;
        reg_led_on();
    }
}

/*Inbound demux*/
static void handle_request(mpack_node_t msgid_n, mpack_node_t method_n, mpack_node_t params_n) {
    if (mpack_node_type(method_n) != mpack_type_str) return;
    uint32_t msgid = mpack_node_u32(msgid_n);
    if (mpack_tree_error(&rx_tree) != mpack_ok) return;

    const rpc_method_t *m = find_method(method_n);
    if (m == NULL) return;
    send_response(msgid, m->handler, params_n);
}

static void handle_notification(mpack_node_t method_n, mpack_node_t params_n) {
    if (mpack_node_type(method_n) != mpack_type_str) return;
    if (mpack_tree_error(&rx_tree) != mpack_ok) return;

    const rpc_method_t *m = find_method(method_n);
    if (m == NULL) return;
    dispatch_no_reply(m->handler, params_n);
}

static void handle_response(mpack_node_t msgid_n, mpack_node_t error_n, mpack_node_t result_n) {
    uint32_t msgid = mpack_node_u32(msgid_n);
    if (mpack_tree_error(&rx_tree) != mpack_ok) return;

    for (size_t i = 0; i < RPC_MAX_PENDING; i++) {
        if (pending[i].in_use && pending[i].msgid == msgid) {
            rpc_result_fn cb = pending[i].cb;
            void *ctx = pending[i].ctx;
            pending[i].in_use = false;

            if (cb) cb(RPC_OK, error_n, result_n, ctx);
            return;
        }
    }
}

static void dispatch(mpack_node_t root) {
    // Frameless stream: never trust shape. A stray byte is valid msgpack.
    if (mpack_node_type(root) != mpack_type_array) return;
    size_t len = mpack_node_array_length(root);
    if (len < 3) return;
    if (mpack_tree_error(&rx_tree) != mpack_ok) return;

    uint32_t type = mpack_node_u32(mpack_node_array_at(root, 0));

    if (mpack_tree_error(&rx_tree) != mpack_ok) return;
    switch (type) {
        case 0:
            if (len != 4) return;
            handle_request(mpack_node_array_at(root,1),
                           mpack_node_array_at(root,2),
                           mpack_node_array_at(root, 3));
            break;
        case 1:
            if (len != 4) return;
            handle_response(mpack_node_array_at(root,1),
                           mpack_node_array_at(root,2),
                           mpack_node_array_at(root, 3));
            break;
        case 2:
            if (len != 3) return;
            handle_notification(mpack_node_array_at(root,1),
                                mpack_node_array_at(root, 2));
            break;
        default:
            return;

    }
}
/*Only retry mpack parson on new data arriving*/
static uint32_t last_staged_seen = 0;

void rpc_poll(void) {
    uint32_t staged_now = rx_staged_events;
    if (staged_now != last_staged_seen) {
        last_staged_seen = staged_now;
        if (mpack_tree_try_parse(&rx_tree)) {
            dispatch(mpack_tree_root(&rx_tree));
        }
    }

    mpack_error_t err = mpack_tree_error(&rx_tree);

    if (err == mpack_error_memory) {
        rpc_fault();
        return;
    }
    if (err != mpack_ok) {
        mpack_tree_destroy(&rx_tree);
        rpc_init();
    }

    uint32_t now = rpc_now_ms();
    reap_timeouts(now);
    registration_tick(now);
}