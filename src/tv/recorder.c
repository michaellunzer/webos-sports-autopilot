#include "recorder.h"
#include "frame_io.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void mkdir_p(const char* path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

int recorder_start(recorder_t* r, const char* base_dir, int interval_ms, int max_frames)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(r->dir, sizeof(r->dir), "%s/%s", base_dir, stamp);
    mkdir_p(r->dir);
    struct stat st;
    if (stat(r->dir, &st) != 0) {
        ERR("cannot create %s: %s", r->dir, strerror(errno));
        return -1;
    }
    r->interval_ms = interval_ms;
    r->max_frames = max_frames;
    r->count = 0;
    r->last_ms = 0;
    r->active = true;
    INFO("recording to %s", r->dir);
    return 0;
}

void recorder_stop(recorder_t* r)
{
    if (r->active)
        INFO("recording stopped (%d frames in %s)", r->count, r->dir);
    r->active = false;
}

bool recorder_frame(recorder_t* r, const uint8_t* rgb, int w, int h, uint64_t now_ms, const char* csv_line)
{
    if (!r->active || (r->last_ms && now_ms - r->last_ms < (uint64_t)r->interval_ms))
        return false;
    if (r->count >= r->max_frames) {
        recorder_stop(r);
        return false;
    }
    char path[300];
    snprintf(path, sizeof(path), "%s/%llu.ppm", r->dir, (unsigned long long)now_ms);
    if (ppm_write(path, rgb, w, h, w * 3) != 0) {
        WARN("write %s failed; stopping recording", path);
        recorder_stop(r);
        return false;
    }
    if (csv_line) {
        snprintf(path, sizeof(path), "%s/labels.csv", r->dir);
        FILE* f = fopen(path, "a");
        if (f) {
            fprintf(f, "%llu,%s\n", (unsigned long long)now_ms, csv_line);
            fclose(f);
        }
    }
    r->last_ms = now_ms;
    r->count++;
    return true;
}
