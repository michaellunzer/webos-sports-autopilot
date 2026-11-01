#pragma once
// Saves captured frames as PPM files (named <timestamp_ms>.ppm) so the
// detector can be tuned offline with tools/replay.
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    char dir[256];
    int interval_ms;
    int max_frames;
    int count;
    uint64_t last_ms;
} recorder_t;

int recorder_start(recorder_t* r, const char* base_dir, int interval_ms, int max_frames);
void recorder_stop(recorder_t* r);
// Returns true if a frame was written.
bool recorder_frame(recorder_t* r, const uint8_t* rgb, int w, int h, uint64_t now_ms, const char* csv_line);
