#pragma once
// Polls ESPN's public play-by-play feed for the games assigned to the
// Multiview tiles and feeds the plays into the break predictor.
#include "predictor.h"
#include "settings.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define ESPN_MAX_GAMES 4

typedef struct {
    pthread_mutex_t* lock; // guards games[] and settings (owned by the service)
    pred_game_t* games; // ESPN_MAX_GAMES entries, one per tile
    const settings_t* settings;

    int counts[ESPN_MAX_GAMES]; // last known play count per game
    char last_error[128];
    int64_t last_ok_ms;

    bool running;
    pthread_t thread;
} espn_poller_t;

int64_t realtime_ms(void);
int espn_start(espn_poller_t* e);
void espn_stop(espn_poller_t* e);
// Splits a comma-separated list; returns item `idx` (empty if missing).
void csv_item(const char* list, int idx, char* out, size_t cap);
