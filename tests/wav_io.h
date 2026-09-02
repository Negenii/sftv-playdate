#pragma once
#include <stdint.h>

int wav_write_mono16(const char* path, const int16_t* samples, int n, int sample_rate);
/* Returns malloc'd samples (left channel if stereo) or NULL. */
int16_t* wav_read_mono16(const char* path, int* n, int* sample_rate);
