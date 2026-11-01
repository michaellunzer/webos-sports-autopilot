// Host tool: run the detector over a recorded frame sequence and print a CSV
// timeline. Use it to tune thresholds on real recordings made on the TV
// (see "Recording frames" in the README).
//
//   replay [--full] [--param name=value ...] frame_dir
//
// Frames must be binary PPM files named "<timestamp_ms>.ppm" (the format the
// service records); they are processed in timestamp order.
#include "detector.h"
#include "frame_io.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned long long ts;
    char name[256];
} entry_t;

static int cmp_entry(const void* a, const void* b)
{
    const entry_t* x = a;
    const entry_t* y = b;
    return x->ts < y->ts ? -1 : x->ts > y->ts;
}

static int set_param(detector_params_t* p, const char* kv)
{
    char key[64];
    const char* eq = strchr(kv, '=');
    if (!eq || eq - kv >= (int)sizeof(key))
        return -1;
    memcpy(key, kv, eq - kv);
    key[eq - kv] = 0;
    double v = atof(eq + 1);
#define F(name)                     \
    if (strcmp(key, #name) == 0) {  \
        p->name = (__typeof__(p->name))v; \
        return 0;                   \
    }
    F(work_w) F(work_h) F(act_thresh) F(edge_thresh) F(act_tau_s) F(persist_tau_s) F(persist_on)
    F(band_top) F(band_bottom) F(live_score_on) F(live_score_off) F(live_hold_ms) F(ad_hold_ms)
    F(warmup_ms) F(black_luma) F(black_std)
#undef F
    return -1;
}

int main(int argc, char** argv)
{
    detector_params_t p;
    detector_default_params(&p);
    int full = 0;
    const char* dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) {
            full = 1;
        } else if (strcmp(argv[i], "--param") == 0 && i + 1 < argc) {
            if (set_param(&p, argv[++i]) != 0) {
                fprintf(stderr, "unknown param: %s\n", argv[i]);
                return 2;
            }
        } else {
            dir = argv[i];
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s [--full] [--param name=value ...] frame_dir\n", argv[0]);
        return 2;
    }

    DIR* dp = opendir(dir);
    if (!dp) {
        perror(dir);
        return 1;
    }
    size_t cap = 1024, n = 0;
    entry_t* ents = malloc(cap * sizeof(entry_t));
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".ppm") != 0 || len >= sizeof(ents[0].name))
            continue;
        if (n == cap)
            ents = realloc(ents, (cap *= 2) * sizeof(entry_t));
        ents[n].ts = strtoull(de->d_name, NULL, 10);
        strcpy(ents[n].name, de->d_name);
        n++;
    }
    closedir(dp);
    qsort(ents, n, sizeof(entry_t), cmp_entry);

    detector_t d;
    if (detector_init(&d, &p) != 0)
        return 1;
    if (full) {
        rectf_t r;
        detector_layout_full(&r);
        detector_set_layout(&d, 1, &r);
    }

    printf("t_s,file");
    for (int i = 0; i < d.ntiles; i++)
        printf(",tile%d_state,tile%d_score,tile%d_band,tile%d_body,tile%d_motion", i, i, i, i, i);
    printf("\n");

    unsigned long long t0 = n ? ents[0].ts : 0;
    for (size_t k = 0; k < n; k++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ents[k].name);
        uint8_t* rgb;
        int w, h;
        if (ppm_read(path, &rgb, &w, &h) != 0) {
            fprintf(stderr, "skip unreadable %s\n", path);
            continue;
        }
        detector_process_rgb(&d, rgb, w, h, w * 3, ents[k].ts);
        free(rgb);
        printf("%.2f,%s", (ents[k].ts - t0) / 1000.0, ents[k].name);
        for (int i = 0; i < d.ntiles; i++) {
            const det_tile_t* t = &d.tiles[i];
            printf(",%s,%.4f,%.4f,%.4f,%.2f", tile_state_name(t->state), t->score, t->band_frac, t->body_frac, t->motion);
        }
        printf("\n");
    }
    detector_free(&d);
    free(ents);
    return 0;
}
