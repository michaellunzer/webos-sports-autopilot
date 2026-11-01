#pragma once
// Autopilot state machine: decides which remote keys to press based on the
// per-tile live/ad states produced by the detector.
//
// Pure logic, no I/O, so it can be unit-tested on a PC. The caller feeds it
// time, tile states and events (enable/disable, manual input, ...) and
// executes the returned key presses.

#include "detector.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_KEY_LEFT = 0,
    AP_KEY_RIGHT,
    AP_KEY_UP,
    AP_KEY_DOWN,
    AP_KEY_ENTER,
    AP_KEY_BACK,
    AP_KEY_COUNT
} ap_key_t;

const char* ap_key_name(ap_key_t k);

typedef enum {
    AP_MODE_MULTIVIEW = 0, // stay in Multiview, move audio focus between tiles
    AP_MODE_FULLSCREEN = 1, // watch one game full screen, hop on commercials
} ap_mode_t;

const char* ap_mode_name(ap_mode_t m);

typedef enum {
    AP_PHASE_OFF = 0,
    AP_PHASE_MV_WATCH, // Multiview on screen, focus on `focus`
    AP_PHASE_MV_SCAN, // Multiview on screen, looking for a live game to open full screen
    AP_PHASE_FS_WATCH, // `fs_tile` is full screen
    AP_PHASE_PAUSED, // user is using the remote; hands off
} ap_phase_t;

const char* ap_phase_name(ap_phase_t p);

typedef struct {
    int grid_cols; // Multiview grid, normally 2x2
    int grid_rows;
    int min_switch_interval_ms; // never switch more often than this
    int manual_pause_ms; // hands-off time after the user presses a key
    int scan_settle_ms; // after returning to Multiview, wait before choosing
    bool reset_focus_on_start; // press UP/LEFT to put focus in the top-left tile
} ap_params_t;

#define AP_MAX_KEYS 16

typedef struct {
    int nkeys;
    ap_key_t keys[AP_MAX_KEYS];
    // If >= 0, the detector should switch to this many tiles
    // (1 = full screen game, grid_cols*grid_rows = Multiview) before the next frame.
    int set_layout_tiles;
    const char* reason; // short human-readable reason (static string), or NULL
    // Free-form detail for toasts/logs, e.g. "Commercial on game 2 -> game 3".
    char message[96];
} ap_action_t;

// Minimal per-tile view the autopilot needs.
typedef struct {
    tile_state_t state;
    uint64_t state_since_ms;
} ap_tile_t;

typedef struct {
    ap_params_t p;
    bool enabled;
    ap_mode_t mode;
    ap_phase_t phase;
    ap_phase_t resume_phase;
    uint64_t phase_since_ms;
    uint64_t last_switch_ms;
    uint64_t paused_until_ms;
    bool foreground_ok; // YouTube TV is the foreground app
    int focus; // focused Multiview tile (or -1 if unknown)
    int fs_tile; // tile shown full screen (or -1)
    int switches; // number of automatic switches (stats)
} autopilot_t;

void ap_default_params(ap_params_t* p);
void ap_init(autopilot_t* ap, const ap_params_t* p);

// All functions below may append key presses / layout changes to `out`.
// `out` must be cleared by ap_action_clear() before use.
void ap_action_clear(ap_action_t* out);

void ap_set_enabled(autopilot_t* ap, bool enabled, uint64_t now_ms, ap_action_t* out);
void ap_set_mode(autopilot_t* ap, ap_mode_t mode, uint64_t now_ms, ap_action_t* out);
void ap_set_foreground(autopilot_t* ap, bool ok);
// The user pressed a navigation key on the real remote.
void ap_manual_input(autopilot_t* ap, uint64_t now_ms);
// Periodic update with fresh detector results.
void ap_tick(autopilot_t* ap, const ap_tile_t* tiles, int ntiles, uint64_t now_ms, ap_action_t* out);

// Number of detector tiles the current phase expects.
int ap_expected_tiles(const autopilot_t* ap);

#ifdef __cplusplus
}
#endif
