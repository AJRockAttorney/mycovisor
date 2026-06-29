//
// Created by helios on 6/27/26.
//

#include "ring_buff.h"
#include "string.h"

void rb_init(ring_buf_t *rb, uint8_t *buf, size_t size) {
     rb->buf = buf;
     rb->size = size;
     rb->head = 0;
     rb->tail = 0;
}

bool rb_write(ring_buf_t *rb, const uint8_t *data, const size_t len) {
     size_t head = rb->head;
     size_t tail = rb->tail;
     size_t size = rb->size;
     size_t current_count = 0;
     size_t capacity = 0;
     size_t to_end = size - head;

     /*Check current count of bytes to send*/
     if (head>=tail) {
          current_count = head-tail;
     }
     else {// Handle data wrapping condition
          current_count = (size-tail) + head;
     }
     /*Calculate current buffer capacity*/
     capacity = (size - 1) - current_count;

     /*If there is not enough capacity return false*/
     if (capacity<len) {
          return false;
     }

     if (len <= to_end) {
          memcpy(&rb->buf[head], data, len);
     }
     else {
          memcpy(&rb->buf[head], data, to_end);
          memcpy(&rb->buf[0], data+to_end, len-to_end);
     }
     rb->head = (head+len)%size;
     return true;
}

rb_block_t rb_get_linear_block(ring_buf_t *rb) {
     size_t head = rb->head;
     size_t tail = rb->tail;
     size_t size = rb->size;
     rb_block_t block = {0};

     block.ptr = &rb->buf[tail];
     if (head>=tail) { //Not wrapped so whole block is available in one contiguous block
          block.len = head-tail;
     }
     else {
          block.len = size-tail;
     }
     return block;
}

void rb_skip(ring_buf_t *rb, const size_t len) {
     rb->tail = (rb->tail + len) % rb->size;
}