#include "wav_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32(FILE* f, uint32_t v) {
    uint8_t b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF};
    fwrite(b, 1, 4, f);
}
static void put_u16(FILE* f, uint16_t v) {
    uint8_t b[2] = {v & 0xFF, (v >> 8) & 0xFF};
    fwrite(b, 1, 2, f);
}
static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

int wav_write_mono16(const char* path, const int16_t* samples, int n, int sample_rate) {
    FILE* f = fopen(path, "wb");
    if(!f) return -1;
    uint32_t data_bytes = (uint32_t)n * 2;
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1); /* PCM */
    put_u16(f, 1); /* mono */
    put_u32(f, (uint32_t)sample_rate);
    put_u32(f, (uint32_t)sample_rate * 2);
    put_u16(f, 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, data_bytes);
    for(int i = 0; i < n; i++) put_u16(f, (uint16_t)samples[i]);
    fclose(f);
    return 0;
}

int16_t* wav_read_mono16(const char* path, int* n, int* sample_rate) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size < 12) {
        fclose(f);
        return NULL;
    }
    uint8_t* buf = malloc((size_t)size);
    if(fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if(memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return NULL;
    }

    int channels = 0, bits = 0, rate = 0;
    const uint8_t* data = NULL;
    uint32_t data_len = 0;
    size_t p = 12;
    while(p + 8 <= (size_t)size) {
        uint32_t clen = get_u32(buf + p + 4);
        const uint8_t* body = buf + p + 8;
        if(memcmp(buf + p, "fmt ", 4) == 0 && clen >= 16) {
            uint16_t fmt = get_u16(body);
            if(fmt != 1 && fmt != 0xFFFE) {
                free(buf);
                return NULL;
            }
            channels = get_u16(body + 2);
            rate = (int)get_u32(body + 4);
            bits = get_u16(body + 14);
        } else if(memcmp(buf + p, "data", 4) == 0) {
            data = body;
            data_len = clen;
            /* Streaming recorders often leave the length at 0 or 0xFFFFFFFF: take the rest. */
            if(data_len == 0 || p + 8 + data_len > (size_t)size) data_len = (uint32_t)((size_t)size - p - 8);
        }
        p += 8 + clen + (clen & 1);
    }
    if(!data || channels < 1 || bits != 16 || rate <= 0) {
        free(buf);
        return NULL;
    }
    int frames = (int)(data_len / (uint32_t)(2 * channels));
    int16_t* out = malloc(sizeof(int16_t) * (size_t)frames);
    for(int i = 0; i < frames; i++) out[i] = (int16_t)get_u16(data + (size_t)i * 2 * (size_t)channels);
    free(buf);
    *n = frames;
    *sample_rate = rate;
    return out;
}
