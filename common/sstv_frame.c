#include "sstv_frame.h"
#include "crc8.h"
#include "rs.h"
#include <string.h>

void sstv_frame_pack(uint8_t line_no, const uint8_t* pixels, uint8_t* out) {
    out[0] = line_no;
    memcpy(out + 1, pixels, SSTV_LINE_BYTES);
    out[SSTV_DATA_BYTES - 1] = crc8_compute(out, 1 + SSTV_LINE_BYTES);
    rs_encode(out, SSTV_DATA_BYTES, out + SSTV_DATA_BYTES);
}

bool sstv_frame_unpack_ex(const uint8_t* in, uint8_t* line_no, uint8_t* pixels, int* corrected) {
    uint8_t buf[SSTV_FRAME_BYTES];
    memcpy(buf, in, SSTV_FRAME_BYTES);
    int c = rs_decode(buf, SSTV_FRAME_BYTES);
    if(corrected) *corrected = c;
    const uint8_t* src = c >= 0 ? buf : in;
    *line_no = src[0];
    memcpy(pixels, src + 1, SSTV_LINE_BYTES);
    return c >= 0 && crc8_compute(src, 1 + SSTV_LINE_BYTES) == src[SSTV_DATA_BYTES - 1];
}

bool sstv_frame_unpack(const uint8_t* in, uint8_t* line_no, uint8_t* pixels) {
    return sstv_frame_unpack_ex(in, line_no, pixels, NULL);
}
