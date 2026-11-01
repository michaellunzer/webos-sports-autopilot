#include "autopilot.h"
#include "test_main.h"
#include <string.h>

static void keys_str(const ap_action_t* a, char* buf, size_t n)
{
    buf[0] = 0;
    for (int i = 0; i < a->nkeys; i++) {
        strncat(buf, ap_key_name(a->keys[i]), n - strlen(buf) - 1);
        if (i + 1 < a->nkeys)
            strncat(buf, ",", n - strlen(buf) - 1);
    }
}

static void tiles4(ap_tile_t* t, tile_state_t a, tile_state_t b, tile_state_t c, tile_state_t d)
{
    tile_state_t s[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        t[i].state = s[i];
        t[i].state_since_ms = 1000;
    }
}

#define EXPECT_KEYS(act, want)                                     \
    do {                                                           \
        char _b[128];                                              \
        keys_str(&(act), _b, sizeof(_b));                          \
        CHECK(strcmp(_b, want) == 0, "keys '%s' != '%s'", _b, want); \
    } while (0)

int main(void)
{
    ap_params_t p;
    ap_default_params(&p);
    autopilot_t ap;
    ap_action_t a;
    ap_tile_t t[4];
    uint64_t now = 100000;

    // --- Multiview mode ---
    ap_init(&ap, &p);
    ap_action_clear(&a);
    ap_set_enabled(&ap, true, now, &a);
    EXPECT_KEYS(a, "UP,LEFT"); // focus reset to top-left
    CHECK(ap.focus == 0, "focus %d", ap.focus);
    CHECK(a.set_layout_tiles == 4, "layout %d", a.set_layout_tiles);

    // Focused game live: nothing happens.
    tiles4(t, TILE_LIVE, TILE_LIVE, TILE_AD, TILE_LIVE);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 1000, &a);
    EXPECT_KEYS(a, "");

    // Focused game goes to commercial; game 4 (index 3) just came back.
    tiles4(t, TILE_AD, TILE_LIVE, TILE_AD, TILE_LIVE);
    t[3].state_since_ms = 50000;
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 1000, &a);
    EXPECT_KEYS(a, "RIGHT,DOWN");
    CHECK(ap.focus == 3, "focus %d", ap.focus);

    // Cool-down: new focus goes to ad immediately; no switch within 20s.
    tiles4(t, TILE_LIVE, TILE_LIVE, TILE_AD, TILE_AD);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 5000, &a);
    EXPECT_KEYS(a, "");
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 16000, &a);
    EXPECT_KEYS(a, "UP"); // to tile 1 (closest of the equally fresh live games)
    CHECK(ap.focus == 1, "focus %d", ap.focus);

    // Everything at commercial: stay put.
    tiles4(t, TILE_AD, TILE_AD, TILE_AD, TILE_AD);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 30000, &a);
    EXPECT_KEYS(a, "");

    // Manual input pauses, then resumes with a focus reset.
    ap_manual_input(&ap, now);
    CHECK(ap.phase == AP_PHASE_PAUSED, "phase %s", ap_phase_name(ap.phase));
    tiles4(t, TILE_AD, TILE_LIVE, TILE_LIVE, TILE_LIVE);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 60000, &a);
    EXPECT_KEYS(a, "");
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 61000, &a);
    EXPECT_KEYS(a, "UP,LEFT");
    CHECK(ap.phase == AP_PHASE_MV_WATCH, "phase %s", ap_phase_name(ap.phase));

    // Not in YouTube TV: hands off.
    ap_set_foreground(&ap, false);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 30000, &a);
    EXPECT_KEYS(a, "");
    ap_set_foreground(&ap, true);

    // Disable.
    ap_action_clear(&a);
    ap_set_enabled(&ap, false, now, &a);
    CHECK(ap.phase == AP_PHASE_OFF, "phase %s", ap_phase_name(ap.phase));
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 30000, &a);
    EXPECT_KEYS(a, "");

    // --- Full-screen mode ---
    ap_init(&ap, &p);
    ap.mode = AP_MODE_FULLSCREEN;
    ap_action_clear(&a);
    ap_set_enabled(&ap, true, now, &a);
    EXPECT_KEYS(a, "UP,LEFT");
    CHECK(ap.phase == AP_PHASE_MV_SCAN, "phase %s", ap_phase_name(ap.phase));

    tiles4(t, TILE_AD, TILE_UNKNOWN, TILE_LIVE, TILE_AD);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 1000, &a); // still settling
    EXPECT_KEYS(a, "");
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 6000, &a);
    EXPECT_KEYS(a, "DOWN,ENTER");
    CHECK(ap.phase == AP_PHASE_FS_WATCH && ap.fs_tile == 2, "phase %s tile %d", ap_phase_name(ap.phase), ap.fs_tile);
    CHECK(a.set_layout_tiles == 1, "layout %d", a.set_layout_tiles);

    // Full-screen game goes to commercial -> BACK to Multiview.
    ap_tile_t one[1] = { { TILE_AD, 0 } };
    ap_action_clear(&a);
    ap_tick(&ap, one, 1, now += 25000, &a);
    EXPECT_KEYS(a, "BACK");
    CHECK(ap.phase == AP_PHASE_MV_SCAN, "phase %s", ap_phase_name(ap.phase));
    CHECK(a.set_layout_tiles == 4, "layout %d", a.set_layout_tiles);

    // Scan: game 2 (index 1) live -> open it.
    tiles4(t, TILE_AD, TILE_LIVE, TILE_AD, TILE_AD);
    ap_action_clear(&a);
    ap_tick(&ap, t, 4, now += 7000, &a);
    EXPECT_KEYS(a, "RIGHT,UP,ENTER");
    CHECK(ap.fs_tile == 1, "fs_tile %d", ap.fs_tile);

    // Switching mode back to Multiview from full screen presses BACK.
    ap_action_clear(&a);
    ap_set_mode(&ap, AP_MODE_MULTIVIEW, now, &a);
    EXPECT_KEYS(a, "BACK");
    CHECK(ap.phase == AP_PHASE_MV_WATCH && ap.focus == 1, "phase %s focus %d", ap_phase_name(ap.phase), ap.focus);

    TEST_DONE();
}
