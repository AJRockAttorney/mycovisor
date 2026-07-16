/* Native host-side unit tests for ring_buff.c.
 *
 * ring_buff.c has zero STM32/HAL dependencies (just stdbool/stddef/stdint
 * and memcpy), so it builds and runs directly on the host -- no toolchain,
 * no target hardware. See `just test` / README for how to run this.
 */

#include "ring_buff.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void fill_seq(uint8_t *buf, size_t len, uint8_t start) {
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(start + i);
}

static void test_init(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));
    CHECK(rb.head == 0);
    CHECK(rb.tail == 0);
    CHECK(rb.buf == backing);
    CHECK(rb.size == sizeof(backing));
}

static void test_basic_round_trip(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[4] = {1, 2, 3, 4};
    CHECK(rb_write(&rb, in, sizeof(in)) == true);

    uint8_t out[4] = {0};
    CHECK(rb_read(&rb, out, sizeof(out)) == 4);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
}

static void test_empty_read_returns_zero(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t out[4];
    CHECK(rb_read(&rb, out, sizeof(out)) == 0);
}

static void test_zero_length_write(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    CHECK(rb_write(&rb, NULL, 0) == true);
    CHECK(rb.head == 0);
}

static void test_partial_read_returns_only_available(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[3] = {10, 20, 30};
    CHECK(rb_write(&rb, in, sizeof(in)) == true);

    uint8_t out[8] = {0};
    size_t got = rb_read(&rb, out, sizeof(out));
    CHECK(got == 3);
    CHECK(memcmp(in, out, 3) == 0);
}

/* Usable capacity is size-1 (the head==tail slot is reserved to disambiguate
   full vs. empty), so a size-N buffer must accept exactly N-1 bytes and
   reject N. */
static void test_capacity_boundary(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[7];
    fill_seq(in, sizeof(in), 0);
    CHECK(rb_write(&rb, in, 7) == true);   /* exactly usable capacity */

    uint8_t out[7] = {0};
    CHECK(rb_read(&rb, out, 7) == 7);
    CHECK(memcmp(in, out, 7) == 0);
}

static void test_overfull_write_rejected_and_state_unchanged(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[8];
    fill_seq(in, sizeof(in), 0);
    /* size 8 buffer: usable capacity is 7, so writing 8 must fail outright,
       not partially succeed. */
    CHECK(rb_write(&rb, in, 8) == false);
    CHECK(rb.head == 0);
    CHECK(rb.tail == 0);

    /* Buffer must still be fully usable afterward -- a rejected write must
       not have corrupted head/tail bookkeeping. */
    uint8_t in2[4] = {1, 2, 3, 4};
    CHECK(rb_write(&rb, in2, sizeof(in2)) == true);
    uint8_t out[4] = {0};
    CHECK(rb_read(&rb, out, sizeof(out)) == 4);
    CHECK(memcmp(in2, out, sizeof(in2)) == 0);
}

static void test_write_rejected_when_partially_full(void) {
    uint8_t backing[8]; /* usable capacity 7 */
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[5];
    fill_seq(in, sizeof(in), 0);
    CHECK(rb_write(&rb, in, 5) == true); /* 2 bytes of capacity left */

    uint8_t too_big[3] = {0, 0, 0};
    CHECK(rb_write(&rb, too_big, 3) == false); /* would need 3, only 2 left */

    /* the earlier 5 bytes must still be intact and readable */
    uint8_t out[5] = {0};
    CHECK(rb_read(&rb, out, 5) == 5);
    CHECK(memcmp(in, out, 5) == 0);
}

/* Drives head/tail all the way around the buffer at least twice via
   small write/read cycles, so every wraparound branch in both rb_write
   and rb_read (len <= to_end and len > to_end) gets exercised. */
static void test_wraparound_write_and_read(void) {
    uint8_t backing[8]; /* usable capacity 7 */
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t next_val = 0;
    for (int cycle = 0; cycle < 20; cycle++) {
        uint8_t in[5];
        fill_seq(in, sizeof(in), next_val);
        CHECK(rb_write(&rb, in, sizeof(in)) == true);

        uint8_t out[5] = {0};
        CHECK(rb_read(&rb, out, sizeof(out)) == 5);
        CHECK(memcmp(in, out, sizeof(in)) == 0);

        next_val = (uint8_t)(next_val + 5);
    }
}

/* Explicitly forces a write whose payload straddles the physical end of
   the backing array (the len > to_end branch in rb_write), then reads it
   back the same way (the len > to_end branch in rb_read). */
static void test_write_and_read_straddling_end(void) {
    uint8_t backing[8]; /* usable capacity 7 */
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    /* Advance head/tail to 6 so the next write of 4 bytes must wrap:
       2 bytes land at [6,7], the remaining 2 wrap to [0,1]. */
    uint8_t filler[6];
    fill_seq(filler, sizeof(filler), 0xA0);
    CHECK(rb_write(&rb, filler, sizeof(filler)) == true);
    uint8_t discard[6];
    CHECK(rb_read(&rb, discard, sizeof(discard)) == 6);
    CHECK(rb.head == 6);
    CHECK(rb.tail == 6);

    uint8_t in[4] = {1, 2, 3, 4}; /* only 2 bytes fit before the end (index 6,7) */
    CHECK(rb_write(&rb, in, sizeof(in)) == true);
    CHECK(rb.head == 2); /* (6+4) % 8 */

    uint8_t out[4] = {0};
    CHECK(rb_read(&rb, out, sizeof(out)) == 4);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(rb.tail == 2);
}

static void test_get_linear_block_empty(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    rb_block_t block = rb_get_linear_block(&rb);
    CHECK(block.len == 0);
}

static void test_get_linear_block_not_wrapped(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[4] = {1, 2, 3, 4};
    CHECK(rb_write(&rb, in, sizeof(in)) == true);

    rb_block_t block = rb_get_linear_block(&rb);
    CHECK(block.len == 4);
    CHECK(block.ptr == &backing[0]);
    CHECK(memcmp(block.ptr, in, 4) == 0);
}

/* When tail > head (wrapped), the linear block can only cover up to the
   physical end of the buffer -- the caller is expected to call this twice
   (consuming via rb_skip between calls) to see the rest. */
static void test_get_linear_block_wrapped(void) {
    uint8_t backing[8]; /* usable capacity 7 */
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t filler[6];
    fill_seq(filler, sizeof(filler), 0);
    CHECK(rb_write(&rb, filler, sizeof(filler)) == true);
    uint8_t discard[6];
    CHECK(rb_read(&rb, discard, sizeof(discard)) == 6);
    /* head == tail == 6 here */

    uint8_t in[4] = {9, 9, 9, 9}; /* wraps: 2 bytes at [6,7], 2 bytes at [0,1] */
    CHECK(rb_write(&rb, in, sizeof(in)) == true);
    /* tail=6, head=2 -> wrapped */

    rb_block_t first = rb_get_linear_block(&rb);
    CHECK(first.len == 2); /* size - tail = 8 - 6 */
    CHECK(first.ptr == &backing[6]);

    rb_skip(&rb, first.len);
    CHECK(rb.tail == 0);

    rb_block_t second = rb_get_linear_block(&rb);
    CHECK(second.len == 2); /* head - tail = 2 - 0 */
    CHECK(second.ptr == &backing[0]);
}

static void test_skip_wraps_tail(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    rb.head = 5;
    rb.tail = 5;
    rb_skip(&rb, 6); /* (5+6) % 8 == 3 */
    CHECK(rb.tail == 3);
}

static void test_skip_zero_is_noop(void) {
    uint8_t backing[8];
    ring_buf_t rb;
    rb_init(&rb, backing, sizeof(backing));

    uint8_t in[3] = {1, 2, 3};
    CHECK(rb_write(&rb, in, sizeof(in)) == true);
    size_t tail_before = rb.tail;
    rb_skip(&rb, 0);
    CHECK(rb.tail == tail_before);
}

int main(void) {
    test_init();
    test_basic_round_trip();
    test_empty_read_returns_zero();
    test_zero_length_write();
    test_partial_read_returns_only_available();
    test_capacity_boundary();
    test_overfull_write_rejected_and_state_unchanged();
    test_write_rejected_when_partially_full();
    test_wraparound_write_and_read();
    test_write_and_read_straddling_end();
    test_get_linear_block_empty();
    test_get_linear_block_not_wrapped();
    test_get_linear_block_wrapped();
    test_skip_wraps_tail();
    test_skip_zero_is_noop();

    if (failures == 0) {
        printf("ok: %d checks passed\n", checks);
        return 0;
    }
    fprintf(stderr, "FAILED: %d/%d checks failed\n", failures, checks);
    return 1;
}
