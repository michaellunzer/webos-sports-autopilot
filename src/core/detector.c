#include "detector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char* tile_state_name(tile_state_t s)
{
    switch (s) {
    case TILE_LIVE:
        return "live";
    case TILE_AD:
        return "ad";
    default:
        return "unknown";
    }
}

void detector_default_params(detector_params_t* p)
{
    p->work_w = 96;
    p->work_h = 54;
    p->act_thresh = 3.0f;
    p->edge_thresh = 24.0f;
    p->act_tau_s = 0.6f;
    p->persist_tau_s = 2.0f;
    p->persist_on = 0.6f;
    p->band_top = 0.22f;
    p->band_bottom = 0.25f;
    p->live_score_on = 0.025f;
    p->live_score_off = 0.010f;
    p->live_hold_ms = 2000;
    p->ad_hold_ms = 4000;
    p->warmup_ms = 3000;
    p->black_luma = 20.0f;
    p->black_std = 6.0f;
}

static void tile_free(det_tile_t* t)
{
    free(t->prev);
    free(t->act);
    free(t->persist);
    free(t->cur);
    memset(t, 0, sizeof(*t));
}

static int tile_alloc(det_tile_t* t, int n)
{
    memset(t, 0, sizeof(*t));
    t->prev = calloc(n, sizeof(float));
    t->act = calloc(n, sizeof(float));
    t->persist = calloc(n, sizeof(float));
    t->cur = calloc(n, sizeof(float));
    if (!t->prev || !t->act || !t->persist || !t->cur) {
        tile_free(t);
        return -1;
    }
    return 0;
}

static void tile_reset(det_tile_t* t, int n, uint64_t now_ms)
{
    memset(t->prev, 0, n * sizeof(float));
    memset(t->act, 0, n * sizeof(float));
    memset(t->persist, 0, n * sizeof(float));
    t->has_prev = false;
    t->state = TILE_UNKNOWN;
    t->pending = TILE_UNKNOWN;
    t->pending_since_ms = now_ms;
    t->state_since_ms = now_ms;
    t->score = t->band_frac = t->body_frac = t->motion = t->mean_luma = 0;
    t->black = false;
}

void detector_layout_grid(rectf_t* out, int cols, int rows, float gutter)
{
    float w = (1.0f - gutter * (cols - 1)) / cols;
    float h = (1.0f - gutter * (rows - 1)) / rows;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            rectf_t* o = &out[r * cols + c];
            o->x = c * (w + gutter);
            o->y = r * (h + gutter);
            o->w = w;
            o->h = h;
        }
    }
}

void detector_layout_full(rectf_t* out)
{
    out->x = 0;
    out->y = 0;
    out->w = 1;
    out->h = 1;
}

int detector_init(detector_t* d, const detector_params_t* p)
{
    memset(d, 0, sizeof(*d));
    d->p = *p;
    int n = p->work_w * p->work_h;
    for (int i = 0; i < DET_MAX_TILES; i++) {
        if (tile_alloc(&d->tiles[i], n) != 0) {
            detector_free(d);
            return -1;
        }
    }
    rectf_t grid[4];
    detector_layout_grid(grid, 2, 2, 0.0f);
    detector_set_layout(d, 4, grid);
    return 0;
}

void detector_free(detector_t* d)
{
    for (int i = 0; i < DET_MAX_TILES; i++)
        tile_free(&d->tiles[i]);
}

void detector_reset(detector_t* d, uint64_t now_ms)
{
    int n = d->p.work_w * d->p.work_h;
    for (int i = 0; i < DET_MAX_TILES; i++)
        tile_reset(&d->tiles[i], n, now_ms);
    d->layout_since_ms = now_ms;
    d->last_ms = now_ms;
    d->started = true;
}

void detector_set_layout(detector_t* d, int ntiles, const rectf_t* rects)
{
    if (ntiles < 1)
        ntiles = 1;
    if (ntiles > DET_MAX_TILES)
        ntiles = DET_MAX_TILES;
    d->ntiles = ntiles;
    memcpy(d->rects, rects, ntiles * sizeof(rectf_t));
    // Timestamps are re-anchored on the next frame.
    d->started = false;
}

// Box-filter the tile region of the RGB frame into t->cur (luma).
static void sample_tile(const detector_t* d, const rectf_t* r, det_tile_t* t,
    const uint8_t* rgb, int width, int height, int stride)
{
    const int ww = d->p.work_w, wh = d->p.work_h;
    float x0f = r->x * width, y0f = r->y * height;
    float sx = r->w * width / ww, sy = r->h * height / wh;

    for (int j = 0; j < wh; j++) {
        int ya = (int)(y0f + j * sy);
        int yb = (int)(y0f + (j + 1) * sy);
        if (yb <= ya)
            yb = ya + 1;
        if (ya < 0)
            ya = 0;
        if (yb > height)
            yb = height;
        for (int i = 0; i < ww; i++) {
            int xa = (int)(x0f + i * sx);
            int xb = (int)(x0f + (i + 1) * sx);
            if (xb <= xa)
                xb = xa + 1;
            if (xa < 0)
                xa = 0;
            if (xb > width)
                xb = width;
            uint32_t acc = 0, cnt = 0;
            for (int y = ya; y < yb; y++) {
                const uint8_t* p = rgb + (size_t)y * stride + (size_t)xa * 3;
                for (int x = xa; x < xb; x++, p += 3) {
                    acc += (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                    cnt++;
                }
            }
            t->cur[j * ww + i] = cnt ? (float)acc / cnt : 0.0f;
        }
    }
}

static void update_state(const detector_params_t* p, det_tile_t* t, tile_state_t raw, uint64_t now_ms)
{
    if (t->state == TILE_LIVE)
        t->last_live_ms = now_ms;

    if (raw == TILE_UNKNOWN || raw == t->state) {
        t->pending = t->state;
        t->pending_since_ms = now_ms;
        return;
    }
    if (raw != t->pending) {
        t->pending = raw;
        t->pending_since_ms = now_ms;
    }
    int hold = raw == TILE_LIVE ? p->live_hold_ms : p->ad_hold_ms;
    if (now_ms - t->pending_since_ms >= (uint64_t)hold) {
        t->state = raw;
        t->state_since_ms = now_ms;
        if (raw == TILE_LIVE)
            t->last_live_ms = now_ms;
    }
}

static void process_tile(detector_t* d, det_tile_t* t, float dt_s, uint64_t now_ms)
{
    const detector_params_t* p = &d->p;
    const int ww = p->work_w, wh = p->work_h, n = ww * wh;

    // Global stats for black-frame detection.
    double sum = 0, sum2 = 0;
    for (int k = 0; k < n; k++) {
        sum += t->cur[k];
        sum2 += (double)t->cur[k] * t->cur[k];
    }
    double mean = sum / n;
    double var = sum2 / n - mean * mean;
    t->mean_luma = (float)mean;
    t->black = mean < p->black_luma && sqrt(var > 0 ? var : 0) < p->black_std;

    if (t->black) {
        // Ad-break boundaries are often black. Forget everything learned so
        // far so a stale score bug cannot keep the tile "live".
        memset(t->persist, 0, n * sizeof(float));
        memset(t->act, 0, n * sizeof(float));
        memcpy(t->prev, t->cur, n * sizeof(float));
        t->has_prev = true;
        t->score = t->band_frac = t->body_frac = 0;
        bool warm = now_ms - d->layout_since_ms >= (uint64_t)p->warmup_ms;
        update_state(p, t, warm ? TILE_AD : TILE_UNKNOWN, now_ms);
        return;
    }

    if (!t->has_prev) {
        memcpy(t->prev, t->cur, n * sizeof(float));
        t->has_prev = true;
        return;
    }

    const float a_act = 1.0f - expf(-dt_s / p->act_tau_s);
    const float a_per = 1.0f - expf(-dt_s / p->persist_tau_s);
    const int top_rows = (int)(p->band_top * wh + 0.5f);
    const int bot_start = wh - (int)(p->band_bottom * wh + 0.5f);

    int band_n = 0, band_on = 0, body_n = 0, body_on = 0;
    double motion = 0;

    for (int j = 0; j < wh; j++) {
        bool in_band = j < top_rows || j >= bot_start;
        for (int i = 0; i < ww; i++) {
            int k = j * ww + i;
            float diff = fabsf(t->cur[k] - t->prev[k]);
            t->act[k] += a_act * (diff - t->act[k]);

            float gx = 0, gy = 0;
            if (i > 0 && i < ww - 1)
                gx = fabsf(t->cur[k + 1] - t->cur[k - 1]);
            if (j > 0 && j < wh - 1)
                gy = fabsf(t->cur[k + ww] - t->cur[k - ww]);
            float edge = gx + gy;

            float s = (t->act[k] < p->act_thresh && edge > p->edge_thresh) ? 1.0f : 0.0f;
            t->persist[k] += a_per * (s - t->persist[k]);
            bool on = t->persist[k] > p->persist_on;

            if (in_band) {
                band_n++;
                band_on += on;
            } else {
                body_n++;
                body_on += on;
                motion += t->act[k];
            }
        }
    }
    memcpy(t->prev, t->cur, n * sizeof(float));

    t->band_frac = band_n ? (float)band_on / band_n : 0;
    t->body_frac = body_n ? (float)body_on / body_n : 0;
    t->motion = body_n ? (float)(motion / body_n) : 0;
    t->score = t->band_frac - t->body_frac;

    tile_state_t raw = TILE_UNKNOWN;
    if (now_ms - d->layout_since_ms >= (uint64_t)p->warmup_ms) {
        if (t->score >= p->live_score_on)
            raw = TILE_LIVE;
        else if (t->score <= p->live_score_off)
            raw = TILE_AD;
    }
    update_state(p, t, raw, now_ms);
}

void detector_process_rgb(detector_t* d, const uint8_t* rgb, int width, int height, int stride, uint64_t now_ms)
{
    if (!d->started)
        detector_reset(d, now_ms);

    float dt_s = (now_ms - d->last_ms) / 1000.0f;
    if (dt_s <= 0)
        dt_s = 0.001f;
    if (dt_s > 2.0f)
        dt_s = 2.0f; // capture stalled; don't over-weight one frame
    d->last_ms = now_ms;

    for (int i = 0; i < d->ntiles; i++) {
        det_tile_t* t = &d->tiles[i];
        sample_tile(d, &d->rects[i], t, rgb, width, height, stride);
        process_tile(d, t, dt_s, now_ms);
    }
}
