#include "detector.h"
#include "synth.h"
#include "test_main.h"

#define W 320
#define H 180
#define FPS 3

static uint8_t frame[W * H * 3];

// Run the detector with the given per-tile content for `seconds`.
static void run(detector_t* d, const syn_content_t* c, int n, double seconds, uint64_t* now_ms, int* fno)
{
    int frames = (int)(seconds * FPS);
    for (int i = 0; i < frames; i++) {
        syn_frame(frame, W, H, c, n, (*fno)++);
        detector_process_rgb(d, frame, W, H, W * 3, *now_ms);
        *now_ms += 1000 / FPS;
    }
}

static void print_tiles(const char* label, const detector_t* d)
{
    printf("%-28s", label);
    for (int i = 0; i < d->ntiles; i++)
        printf(" [%d %-7s score=%+.3f band=%.3f body=%.3f]", i, tile_state_name(d->tiles[i].state),
            d->tiles[i].score, d->tiles[i].band_frac, d->tiles[i].body_frac);
    printf("\n");
}

int main(void)
{
    detector_params_t p;
    detector_default_params(&p);
    detector_t d;
    CHECK(detector_init(&d, &p) == 0, "init");

    uint64_t now = 1000000;
    int fno = 0;

    // 1) Steady state: games 0,1,3 live; game 2 at commercial.
    syn_content_t c1[4] = { SYN_LIVE, SYN_LIVE, SYN_AD, SYN_LIVE };
    run(&d, c1, 4, 2.0, &now, &fno);
    print_tiles("warm-up (2s)", &d);
    for (int i = 0; i < 4; i++)
        CHECK(d.tiles[i].state == TILE_UNKNOWN, "tile %d decided during warm-up", i);

    run(&d, c1, 4, 13.0, &now, &fno);
    print_tiles("steady (15s)", &d);
    CHECK(d.tiles[0].state == TILE_LIVE, "tile 0 should be live");
    CHECK(d.tiles[1].state == TILE_LIVE, "tile 1 should be live");
    CHECK(d.tiles[2].state == TILE_AD, "tile 2 should be ad");
    CHECK(d.tiles[3].state == TILE_LIVE, "tile 3 should be live");

    // 2) Game 0 cuts to commercial; game 2 comes back.
    syn_content_t c2[4] = { SYN_AD, SYN_LIVE, SYN_LIVE, SYN_LIVE };
    run(&d, c2, 4, 2.0, &now, &fno);
    CHECK(d.tiles[0].state == TILE_LIVE, "tile 0 flipped to ad too fast (hysteresis)");
    run(&d, c2, 4, 6.0, &now, &fno);
    print_tiles("after break swap (8s)", &d);
    CHECK(d.tiles[0].state == TILE_AD, "tile 0 should now be ad");
    CHECK(d.tiles[2].state == TILE_LIVE, "tile 2 should be live again");

    // 3) Black transition then commercial on game 1.
    syn_content_t c3a[4] = { SYN_AD, SYN_BLACK, SYN_LIVE, SYN_LIVE };
    syn_content_t c3b[4] = { SYN_AD, SYN_AD, SYN_LIVE, SYN_LIVE };
    run(&d, c3a, 4, 1.0, &now, &fno);
    run(&d, c3b, 4, 5.0, &now, &fno);
    print_tiles("black + ad on 1 (6s)", &d);
    CHECK(d.tiles[1].state == TILE_AD, "tile 1 should be ad after black + ad");

    // 4) Frozen picture and ads with a static logo in the middle must not
    //    look live.
    syn_content_t c4[4] = { SYN_FROZEN, SYN_AD_LOGO, SYN_LIVE, SYN_LIVE };
    run(&d, c4, 4, 15.0, &now, &fno);
    print_tiles("frozen / logo ad (15s)", &d);
    CHECK(d.tiles[0].state != TILE_LIVE, "frozen picture detected as live");
    CHECK(d.tiles[1].state != TILE_LIVE, "ad with centered logo detected as live");

    // 5) Full-screen layout.
    rectf_t full;
    detector_layout_full(&full);
    detector_set_layout(&d, 1, &full);
    syn_content_t f1[1] = { SYN_LIVE };
    run(&d, f1, 1, 8.0, &now, &fno);
    print_tiles("fullscreen live (8s)", &d);
    CHECK(d.tiles[0].state == TILE_LIVE, "fullscreen live");
    syn_content_t f2[1] = { SYN_AD };
    run(&d, f2, 1, 7.0, &now, &fno);
    print_tiles("fullscreen ad (7s)", &d);
    CHECK(d.tiles[0].state == TILE_AD, "fullscreen ad");

    detector_free(&d);
    TEST_DONE();
}
