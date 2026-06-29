//
// Created by helios on 6/27/26.
//

#ifndef UNOQBLINKY_RING_BUFF_H
#define UNOQBLINKY_RING_BUFF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t size;
    volatile size_t head;
    volatile size_t tail;
} ring_buf_t;

typedef struct {
    uint8_t *ptr;//Start of contiguous readable block
    size_t len;  //How many contiguous bytes
} rb_block_t;

rb_block_t rb_get_linear_block(ring_buf_t *rb);
void rb_skip(ring_buf_t *rb, size_t len);
void rb_init(ring_buf_t *rb, uint8_t *buf, size_t size);
bool rb_write(ring_buf_t *rb, const uint8_t *data, size_t len);
#endif //UNOQBLINKY_RING_BUFF_H
