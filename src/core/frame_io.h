#pragma once
// Tiny helpers for saving/loading RGB24 frames (binary PPM) and encoding
// a BMP for the debug web page. No webOS dependencies.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ppm_write(const char* path, const uint8_t* rgb, int width, int height, int stride);
// Allocates *rgb (caller frees). Returns 0 on success.
int ppm_read(const char* path, uint8_t** rgb, int* width, int* height);

// Encode a 24-bit bottom-up BMP into a newly allocated buffer.
// Returns the buffer (caller frees) and its size in *out_len, or NULL.
uint8_t* bmp_encode(const uint8_t* rgb, int width, int height, int stride, size_t* out_len);

// Draw a rectangle outline (2px) into an RGB24 buffer.
void rgb_draw_rect(uint8_t* rgb, int width, int height, int stride,
    int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
