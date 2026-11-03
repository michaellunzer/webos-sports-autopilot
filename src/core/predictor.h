#pragma once
// Commercial-break predictor driven by live play-by-play data.
//
// YouTube TV's video and audio can't be captured on the TV (DRM), so instead
// we follow each game's play-by-play feed (ESPN) and infer breaks:
//  - "marker" plays (Official Timeout, Two-minute warning, End Period,
//    End of Half) mean the broadcast is in a break right now;
//  - otherwise, a long gap with no new play means a break (normal pace is
//    25-45 s between plays; breaks are 150 s+). After scores, timeouts,
//    punts, turnovers and kickoffs a shorter gap is enough;
//  - a game is live again shortly before the next snap.
// Everything is evaluated at "viewer time" = now - stream delay, because
// YouTube TV runs behind the real game.
//
// No webOS dependencies: unit-tested on a PC.

#include "detector.h" // tile_state_t
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRED_MAX_PLAYS 96

typedef enum {
    PLAY_NORMAL = 0,
    PLAY_TRIGGER = 1, // score / timeout / punt / turnover / kickoff: break often follows
    PLAY_MARKER = 2, // official timeout, two-minute warning, end of quarter/half
    PLAY_END_GAME = 3,
} play_kind_t;

play_kind_t pred_classify(const char* type_text);

typedef struct {
    char id[24];
    char type[40];
    int64_t wallclock_ms; // real (stadium) time of the play, epoch ms
    play_kind_t kind;
} pred_play_t;

typedef struct {
    char event_id[16];
    char label[24]; // e.g. "BAL @ BUF"
    pred_play_t plays[PRED_MAX_PLAYS]; // sorted by wallclock, oldest dropped
    int nplays;
    int64_t last_fetch_ms; // real time of the last successful data refresh
} pred_game_t;

typedef struct {
    int stream_delay_s; // how far YouTube TV runs behind the real game
    int idle_break_s; // no new play for this long => break
    int trigger_idle_s; // same, after a trigger play
    int resume_lead_s; // switch back to "live" this long before the next snap
    int max_break_s; // longer stoppages => unknown (weather, injury, ...)
    int halftime_max_s;
    int stale_s; // data older than this => unknown
} pred_params_t;

void pred_default_params(pred_params_t* p);

void pred_game_init(pred_game_t* g, const char* event_id, const char* label);
// Adds a play (ignored if its id is already known). Keeps plays sorted.
void pred_game_add_play(pred_game_t* g, const char* id, const char* type, int64_t wallclock_ms);

typedef struct {
    tile_state_t state;
    int64_t since_ms; // real time at which the viewer saw this state begin (approx.)
    int gap_s; // viewer-time seconds since the last play
    const pred_play_t* last; // last play the viewer has seen, or NULL
} pred_result_t;

pred_result_t pred_evaluate(const pred_game_t* g, const pred_params_t* p, int64_t now_ms);

#ifdef __cplusplus
}
#endif
