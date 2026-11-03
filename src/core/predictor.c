#include "predictor.h"

#include <stdio.h>
#include <string.h>

static const char* const MARKERS[] = {
    "Official Timeout", "Two-minute warning", "Two-Minute Warning", "End Period", "End of Half",
    "End of Regulation", "Halftime", NULL
};
static const char* const TRIGGERS[] = {
    "Touchdown", "Field Goal Good", "Extra Point", "Two-Point", "Timeout", "Punt", "Interception",
    "Fumble Recovery (Opponent)", "Safety", "Kickoff", NULL
};

play_kind_t pred_classify(const char* t)
{
    if (!t)
        return PLAY_NORMAL;
    for (int i = 0; MARKERS[i]; i++) {
        if (strcmp(t, MARKERS[i]) == 0)
            return PLAY_MARKER;
    }
    if (strcmp(t, "End of Game") == 0)
        return PLAY_END_GAME;
    for (int i = 0; TRIGGERS[i]; i++) {
        if (strstr(t, TRIGGERS[i]))
            return PLAY_TRIGGER;
    }
    return PLAY_NORMAL;
}

void pred_default_params(pred_params_t* p)
{
    // Tuned on 13 games from 2026-11-01 (~93% of break time detected).
    p->stream_delay_s = 45;
    p->idle_break_s = 75;
    p->trigger_idle_s = 40;
    p->resume_lead_s = 20;
    p->max_break_s = 600;
    p->halftime_max_s = 1500;
    p->stale_s = 90;
}

void pred_game_init(pred_game_t* g, const char* event_id, const char* label)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->event_id, sizeof(g->event_id), "%s", event_id ? event_id : "");
    snprintf(g->label, sizeof(g->label), "%s", label ? label : "");
}

void pred_game_add_play(pred_game_t* g, const char* id, const char* type, int64_t wallclock_ms)
{
    for (int i = 0; i < g->nplays; i++) {
        if (strcmp(g->plays[i].id, id) == 0) {
            // ESPN sometimes corrects a play; keep the newest details.
            snprintf(g->plays[i].type, sizeof(g->plays[i].type), "%s", type);
            g->plays[i].kind = pred_classify(type);
            return;
        }
    }
    if (g->nplays == PRED_MAX_PLAYS) {
        if (wallclock_ms < g->plays[0].wallclock_ms)
            return; // older than everything we keep
        memmove(&g->plays[0], &g->plays[1], sizeof(pred_play_t) * (PRED_MAX_PLAYS - 1));
        g->nplays--;
    }
    int pos = g->nplays;
    while (pos > 0 && g->plays[pos - 1].wallclock_ms > wallclock_ms)
        pos--;
    memmove(&g->plays[pos + 1], &g->plays[pos], sizeof(pred_play_t) * (g->nplays - pos));
    pred_play_t* p = &g->plays[pos];
    snprintf(p->id, sizeof(p->id), "%s", id);
    snprintf(p->type, sizeof(p->type), "%s", type);
    p->wallclock_ms = wallclock_ms;
    p->kind = pred_classify(type);
    g->nplays++;
}

static int idle_threshold_s(const pred_params_t* p, const pred_play_t* play)
{
    switch (play->kind) {
    case PLAY_MARKER:
        return 0;
    case PLAY_TRIGGER:
        return p->trigger_idle_s;
    default:
        return p->idle_break_s;
    }
}

pred_result_t pred_evaluate(const pred_game_t* g, const pred_params_t* p, int64_t now_ms)
{
    pred_result_t r = { TILE_UNKNOWN, now_ms, 0, NULL };
    const int64_t delay = (int64_t)p->stream_delay_s * 1000;
    const int64_t view = now_ms - delay; // what the viewer is seeing, in game time

    if (g->nplays == 0)
        return r;
    // We must have fresh data covering the viewer's moment.
    if (now_ms - g->last_fetch_ms > (int64_t)p->stale_s * 1000)
        return r;

    int j = -1;
    for (int i = g->nplays - 1; i >= 0; i--) {
        if (g->plays[i].wallclock_ms <= view) {
            j = i;
            break;
        }
    }
    if (j < 0)
        return r; // viewer hasn't reached the first play we know about
    const pred_play_t* last = &g->plays[j];
    r.last = last;
    r.gap_s = (int)((view - last->wallclock_ms) / 1000);

    if (last->kind == PLAY_END_GAME)
        return r; // game over

    // The next snap is imminent: the broadcast is back from break.
    const int64_t lead = (int64_t)p->resume_lead_s * 1000;
    if (j + 1 < g->nplays && g->plays[j + 1].wallclock_ms - view <= lead) {
        r.state = TILE_LIVE;
        r.since_ms = g->plays[j + 1].wallclock_ms - lead + delay;
        return r;
    }

    const int64_t idle = (int64_t)idle_threshold_s(p, last) * 1000;
    const bool halftime = strcmp(last->type, "End of Half") == 0 || strcmp(last->type, "Halftime") == 0;
    const int64_t cap = (int64_t)(halftime ? p->halftime_max_s : p->max_break_s) * 1000;
    const int64_t gap = view - last->wallclock_ms;

    if (gap >= cap)
        return r; // unusually long stoppage: don't guess
    if (gap >= idle) {
        r.state = TILE_AD;
        // Breaks flagged only by a marker were usually already running
        // since the previous play; that's when the viewer saw them start.
        int64_t start = last->wallclock_ms + idle;
        if (last->kind == PLAY_MARKER && j > 0) {
            const pred_play_t* prev = &g->plays[j - 1];
            int64_t prev_start = prev->wallclock_ms + (int64_t)idle_threshold_s(p, prev) * 1000;
            if (prev_start < start)
                start = prev_start;
        }
        r.since_ms = start + delay;
        return r;
    }

    // Live: find where the current live stretch began (after the last long gap).
    int s = j;
    while (s > 0) {
        const pred_play_t* a = &g->plays[s - 1];
        int64_t step = g->plays[s].wallclock_ms - a->wallclock_ms;
        if (a->kind == PLAY_MARKER || step >= (int64_t)idle_threshold_s(p, a) * 1000)
            break;
        s--;
    }
    r.state = TILE_LIVE;
    r.since_ms = g->plays[s].wallclock_ms - lead + delay;
    return r;
}
