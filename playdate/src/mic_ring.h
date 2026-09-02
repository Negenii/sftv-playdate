#pragma once
#include <stdint.h>

/* Single-producer (audio callback) / single-consumer (update loop) ring of int16 samples.
 * Capacity is a power of two; one slot is kept free to distinguish full from empty. */
#define MIC_RING_CAP 16384

typedef struct {
    int16_t buf[MIC_RING_CAP];
    volatile uint32_t head; /* write index */
    volatile uint32_t tail; /* read index */
} mic_ring;

void mic_ring_init(mic_ring* r);
int mic_ring_write(mic_ring* r, const int16_t* s, int n);
int mic_ring_read(mic_ring* r, int16_t* out, int cap);
