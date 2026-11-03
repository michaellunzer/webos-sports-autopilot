#include "predictor.h"
#include "test_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char id[24];
    char type[40];
    int64_t t_ms;
} fplay_t;

static int load_fixture(const char* path, fplay_t* out, int max)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        if (line[0] == '#')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        char* a = strtok(line, ",");
        char* b = strtok(NULL, ",");
        char* c = strtok(NULL, "");
        if (!a || !b || !c)
            continue;
        snprintf(out[n].id, sizeof(out[n].id), "%s", a);
        snprintf(out[n].type, sizeof(out[n].type), "%s", c);
        out[n].t_ms = atoll(b) * 1000;
        n++;
    }
    fclose(f);
    return n;
}

// Approximate ground truth: a break surrounds every marker play.
static int in_true_break(const fplay_t* p, int n, int64_t view_ms)
{
    for (int i = 1; i < n - 1; i++) {
        if (pred_classify(p[i].type) != PLAY_MARKER)
            continue;
        int64_t a = p[i - 1].t_ms + 30000, b = p[i + 1].t_ms - 20000;
        if (strcmp(p[i].type, "End of Half") == 0)
            a = p[i - 1].t_ms + 20000, b = p[i + 1].t_ms - 30000;
        if (view_ms >= a && view_ms < b)
            return 1;
    }
    return 0;
}

static void synthetic_checks(void)
{
    CHECK(pred_classify("Official Timeout") == PLAY_MARKER, "marker");
    CHECK(pred_classify("Passing Touchdown") == PLAY_TRIGGER, "trigger");
    CHECK(pred_classify("Rush") == PLAY_NORMAL, "normal");
    CHECK(pred_classify("End of Game") == PLAY_END_GAME, "end");

    pred_params_t p;
    pred_default_params(&p);
    p.stream_delay_s = 0;
    pred_game_t g;
    pred_game_init(&g, "1", "A @ B");
    int64_t t0 = 1000000000;
    pred_game_add_play(&g, "a", "Rush", t0);
    pred_game_add_play(&g, "b", "Pass Reception", t0 + 30000);
    pred_game_add_play(&g, "b", "Pass Reception", t0 + 30000); // duplicate ignored
    CHECK(g.nplays == 2, "dedupe: %d", g.nplays);
    g.last_fetch_ms = t0 + 40000;
    CHECK(pred_evaluate(&g, &p, t0 + 40000).state == TILE_LIVE, "live mid-drive");
    g.last_fetch_ms = t0 + 120000;
    CHECK(pred_evaluate(&g, &p, t0 + 120000).state == TILE_AD, "long gap = break");
    pred_game_add_play(&g, "c", "Official Timeout", t0 + 125000);
    g.last_fetch_ms = t0 + 126000;
    pred_result_t r = pred_evaluate(&g, &p, t0 + 126000);
    CHECK(r.state == TILE_AD, "marker = break");
    pred_game_add_play(&g, "d", "Rush", t0 + 200000);
    g.last_fetch_ms = t0 + 185000;
    CHECK(pred_evaluate(&g, &p, t0 + 185000).state == TILE_LIVE, "resume lead");
    g.last_fetch_ms = t0 + 1000000;
    CHECK(pred_evaluate(&g, &p, t0 + 900000).state == TILE_UNKNOWN, "stale data");
}

int main(int argc, char** argv)
{
    synthetic_checks();

    const char* path = argc > 1 ? argv[1] : "tests/data/bal-buf-2026-11-01.csv";
    static fplay_t fx[400];
    int n = load_fixture(path, fx, 400);
    CHECK(n > 100, "fixture %s: %d plays", path, n);
    if (n <= 0)
        TEST_DONE();

    pred_params_t p;
    pred_default_params(&p);
    const int64_t latency = 15000; // ESPN publishes plays ~15 s after they happen
    pred_game_t g;
    pred_game_init(&g, "x", "BAL @ BUF");

    int next = 0, tp = 0, fp = 0, fn = 0, tot = 0, flips = 0;
    tile_state_t prev = TILE_UNKNOWN;
    int64_t start = fx[0].t_ms + p.stream_delay_s * 1000LL, end = fx[n - 1].t_ms;
    for (int64_t now = start; now < end; now += 2000) {
        while (next < n && fx[next].t_ms + latency <= now) {
            pred_game_add_play(&g, fx[next].id, fx[next].type, fx[next].t_ms);
            next++;
        }
        g.last_fetch_ms = now;
        pred_result_t r = pred_evaluate(&g, &p, now);
        int truth = in_true_break(fx, n, now - p.stream_delay_s * 1000LL);
        int pred = r.state == TILE_AD;
        tp += truth && pred;
        fp += !truth && pred;
        fn += truth && !pred;
        tot++;
        if (r.state != prev && prev != TILE_UNKNOWN)
            flips++;
        prev = r.state;
    }
    double recall = tp + fn ? (double)tp / (tp + fn) : 0;
    double precision = tp + fp ? (double)tp / (tp + fp) : 0;
    printf("replay %s: recall %.2f precision %.2f false-break share %.3f state changes %d\n",
        path, recall, precision, (double)fp / tot, flips);
    CHECK(recall >= 0.85, "recall %.2f", recall);
    CHECK(precision >= 0.75, "precision %.2f", precision);
    TEST_DONE();
}
