#pragma once
// Synthetic Multiview frames for tests: each tile shows a panning,
// textured "field"; LIVE tiles additionally carry a static score bug in the
// bottom band. Content types let us simulate ads, black frames and freezes.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    SYN_LIVE, // moving field + static score bug
    SYN_AD, // moving content, no bug
    SYN_BLACK, // black frame
    SYN_FROZEN, // static textured picture, no bug (e.g. a still ad card)
    SYN_AD_LOGO, // moving content + static logo in the middle of the picture
} syn_content_t;

static inline uint32_t syn_hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static inline void syn_fill_tile(uint8_t* rgb, int stride, int x0, int y0, int w, int h,
    syn_content_t c, int frame, int seed)
{
    int pan = (c == SYN_FROZEN) ? 0 : frame * 5;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = rgb + (size_t)(y0 + y) * stride + (size_t)(x0 + x) * 3;
            if (c == SYN_BLACK) {
                p[0] = p[1] = p[2] = 4;
                continue;
            }
            uint32_t v = syn_hash((uint32_t)(((x + pan) / 4) * 7919 + (y / 4) * 104729 + seed * 13 + (c == SYN_AD ? 977 : 0)));
            uint8_t lum = 40 + (v & 0x7f);
            p[0] = lum / 3;
            p[1] = lum;
            p[2] = lum / 3;
            if (c == SYN_AD || c == SYN_AD_LOGO) {
                p[0] = lum;
                p[2] = (uint8_t)(255 - lum);
            }
        }
    }
    if (c == SYN_LIVE) {
        // Score bug: dark bar with bright "digits" along the bottom.
        int by0 = y0 + h - h / 7 - 2, by1 = y0 + h - 3;
        int bx0 = x0 + w / 6, bx1 = x0 + w - w / 6;
        for (int y = by0; y < by1; y++) {
            for (int x = bx0; x < bx1; x++) {
                uint8_t* p = rgb + (size_t)y * stride + (size_t)x * 3;
                bool digit = ((x - bx0) / 3) % 3 == 0 && y > by0 + 1 && y < by1 - 1;
                uint8_t l = digit ? 240 : 15;
                p[0] = p[1] = p[2] = l;
            }
        }
    }
    if (c == SYN_AD_LOGO) {
        int lx0 = x0 + w / 2 - w / 8, lx1 = x0 + w / 2 + w / 8;
        int ly0 = y0 + h / 2 - h / 8, ly1 = y0 + h / 2 + h / 8;
        for (int y = ly0; y < ly1; y++) {
            for (int x = lx0; x < lx1; x++) {
                uint8_t* p = rgb + (size_t)y * stride + (size_t)x * 3;
                uint8_t l = (((x - lx0) / 3) + ((y - ly0) / 3)) % 2 ? 250 : 10;
                p[0] = p[1] = p[2] = l;
            }
        }
    }
}

// Render a 2x2 Multiview frame (or a single full-screen tile if n == 1).
static inline void syn_frame(uint8_t* rgb, int width, int height, const syn_content_t* c, int n, int frame)
{
    memset(rgb, 0, (size_t)width * height * 3);
    if (n == 1) {
        syn_fill_tile(rgb, width * 3, 0, 0, width, height, c[0], frame, 1);
        return;
    }
    int tw = width / 2, th = height / 2;
    for (int i = 0; i < 4; i++)
        syn_fill_tile(rgb, width * 3, (i % 2) * tw, (i / 2) * th, tw, th, c[i], frame, i + 1);
}
