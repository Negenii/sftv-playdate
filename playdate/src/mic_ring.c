#include "mic_ring.h"

#define MASK (MIC_RING_CAP - 1)

void mic_ring_init(mic_ring* r) {
    r->head = 0;
    r->tail = 0;
}

int mic_ring_write(mic_ring* r, const int16_t* s, int n) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    uint32_t free_slots = (tail - head - 1) & MASK;
    if((uint32_t)n > free_slots) n = (int)free_slots;
    for(int i = 0; i < n; i++) {
        r->buf[(head + (uint32_t)i) & MASK] = s[i];
    }
    r->head = (head + (uint32_t)n) & MASK;
    return n;
}

int mic_ring_read(mic_ring* r, int16_t* out, int cap) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    uint32_t avail = (head - tail) & MASK;
    if((uint32_t)cap > avail) cap = (int)avail;
    for(int i = 0; i < cap; i++) {
        out[i] = r->buf[(tail + (uint32_t)i) & MASK];
    }
    r->tail = (tail + (uint32_t)cap) & MASK;
    return cap;
}
