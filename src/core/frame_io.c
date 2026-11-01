#include "frame_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ppm_write(const char* path, const uint8_t* rgb, int width, int height, int stride)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        if (fwrite(rgb + (size_t)y * stride, 3, width, f) != (size_t)width) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int skip_ws_comments(FILE* f)
{
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            break;
        }
    }
    if (c == EOF)
        return -1;
    ungetc(c, f);
    return 0;
}

int ppm_read(const char* path, uint8_t** rgb, int* width, int* height)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    char magic[3] = { 0 };
    int w, h, maxv;
    if (fread(magic, 1, 2, f) != 2 || strcmp(magic, "P6") != 0
        || skip_ws_comments(f) || fscanf(f, "%d", &w) != 1
        || skip_ws_comments(f) || fscanf(f, "%d", &h) != 1
        || skip_ws_comments(f) || fscanf(f, "%d", &maxv) != 1
        || maxv != 255 || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        fclose(f);
        return -1;
    }
    fgetc(f); // single whitespace after header
    size_t n = (size_t)w * h * 3;
    uint8_t* buf = malloc(n);
    if (!buf || fread(buf, 1, n, f) != n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *rgb = buf;
    *width = w;
    *height = h;
    return 0;
}

static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

uint8_t* bmp_encode(const uint8_t* rgb, int width, int height, int stride, size_t* out_len)
{
    size_t row = ((size_t)width * 3 + 3) & ~(size_t)3;
    size_t data = row * height;
    size_t total = 54 + data;
    uint8_t* out = calloc(1, total);
    if (!out)
        return NULL;
    out[0] = 'B';
    out[1] = 'M';
    put_le32(out + 2, (uint32_t)total);
    put_le32(out + 10, 54);
    put_le32(out + 14, 40);
    put_le32(out + 18, (uint32_t)width);
    put_le32(out + 22, (uint32_t)height);
    out[26] = 1; // planes
    out[28] = 24; // bpp
    put_le32(out + 34, (uint32_t)data);
    for (int y = 0; y < height; y++) {
        const uint8_t* src = rgb + (size_t)(height - 1 - y) * stride;
        uint8_t* dst = out + 54 + row * y;
        for (int x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    *out_len = total;
    return out;
}

static void put_px(uint8_t* rgb, int width, int height, int stride, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    uint8_t* p = rgb + (size_t)y * stride + (size_t)x * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

void rgb_draw_rect(uint8_t* rgb, int width, int height, int stride,
    int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    for (int t = 0; t < 2; t++) {
        for (int x = x0; x <= x1; x++) {
            put_px(rgb, width, height, stride, x, y0 + t, r, g, b);
            put_px(rgb, width, height, stride, x, y1 - t, r, g, b);
        }
        for (int y = y0; y <= y1; y++) {
            put_px(rgb, width, height, stride, x0 + t, y, r, g, b);
            put_px(rgb, width, height, stride, x1 - t, y, r, g, b);
        }
    }
}
