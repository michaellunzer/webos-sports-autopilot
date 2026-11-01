#pragma once
// Commercial detector.
//
// Splits each captured frame into tiles (4 in Multiview, 1 when a game is
// full screen) and decides, per tile, whether the game is LIVE or at an AD
// break.
//
// Primary signal: during live NFL play the broadcaster's score bug is on
// screen almost all the time; during commercials it is not. A score bug is a
// set of high-contrast pixels that stay perfectly still while the rest of the
// picture moves. We track, per pixel, how long it has been a "static edge" and
// measure how many such pixels sit in the top/bottom bands of the tile (where
// score bugs live) compared to the rest of the tile.
//
// This file has no webOS dependencies so it can be unit-tested on a PC.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DET_MAX_TILES 4

typedef enum {
    TILE_UNKNOWN = 0,
    TILE_LIVE = 1,
    TILE_AD = 2,
} tile_state_t;

const char* tile_state_name(tile_state_t s);

// Rectangle in normalized frame coordinates (0..1).
typedef struct {
    float x, y, w, h;
} rectf_t;

typedef struct {
    // Analysis resolution per tile. The tile region is box-filtered to this.
    int work_w;
    int work_h;

    // Per-pixel thresholds (luma units, 0..255).
    float act_thresh; // max smoothed |frame-to-frame diff| to count as "still"
    float edge_thresh; // min gradient magnitude to count as "edge"

    // Time constants (seconds).
    float act_tau_s; // smoothing for the activity EMA
    float persist_tau_s; // smoothing for the "static edge" persistence EMA

    float persist_on; // persistence above which a pixel counts as overlay

    // Bands (fraction of tile height) searched for the score bug.
    float band_top;
    float band_bottom;

    // score = band_static_fraction - body_static_fraction
    float live_score_on; // score >= this -> evidence of LIVE
    float live_score_off; // score <= this -> evidence of AD

    // Hysteresis: how long evidence must persist before switching.
    int live_hold_ms;
    int ad_hold_ms;
    // Ignore evidence for this long after a (re)start / layout change.
    int warmup_ms;

    // Black transition frames.
    float black_luma; // mean luma below this...
    float black_std; // ...and std below this => black frame
} detector_params_t;

typedef struct {
    tile_state_t state;
    tile_state_t pending;
    uint64_t pending_since_ms;
    uint64_t state_since_ms;
    uint64_t last_live_ms; // last time the tile was observed LIVE (state)

    // Latest metrics (for debugging / UI).
    float score;
    float band_frac;
    float body_frac;
    float motion;
    float mean_luma;
    bool black;

    // Internal buffers (work_w * work_h).
    float* prev;
    float* act;
    float* persist;
    float* cur;
    bool has_prev;
} det_tile_t;

typedef struct {
    detector_params_t p;
    int ntiles;
    rectf_t rects[DET_MAX_TILES];
    det_tile_t tiles[DET_MAX_TILES];
    uint64_t last_ms;
    uint64_t layout_since_ms;
    bool started;
} detector_t;

void detector_default_params(detector_params_t* p);

int detector_init(detector_t* d, const detector_params_t* p);
void detector_free(detector_t* d);

// Replace the tile layout. Resets all tile state (re-enters warm-up).
void detector_set_layout(detector_t* d, int ntiles, const rectf_t* rects);

// Convenience layouts.
void detector_layout_grid(rectf_t* out, int cols, int rows, float gutter);
void detector_layout_full(rectf_t* out);

// Reset all temporal state without changing the layout.
void detector_reset(detector_t* d, uint64_t now_ms);

// Feed one packed RGB24 frame.
void detector_process_rgb(detector_t* d, const uint8_t* rgb, int width, int height, int stride, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
