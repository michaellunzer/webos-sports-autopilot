#pragma once
// Minimal HTTP server for the debug page: live captured frame with tile
// labels plus the status JSON. Open http://<tv-ip>:8088/ on a phone/PC.
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Write status JSON into a newly allocated string (caller frees).
    char* (*status_json)(void* data);
    // Encode the current annotated frame as BMP (caller frees), or NULL.
    uint8_t* (*frame_bmp)(void* data, size_t* len);
    void* data;

    int port;
    int listen_fd;
    bool running;
    pthread_t thread;
} debug_http_t;

int debug_http_start(debug_http_t* h, int port);
void debug_http_stop(debug_http_t* h);
