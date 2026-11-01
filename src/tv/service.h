#pragma once

#include "autopilot.h"
#include "debug_http.h"
#include "detector.h"
#include "recorder.h"
#include "remote_listen.h"
#include "settings.h"
#include "unicapture.h"

#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <pthread.h>

#define KEYQ_LEN 64

typedef struct {
    settings_t settings;
    pthread_mutex_t lock; // guards everything below that the capture/listener threads touch

    detector_t det;
    autopilot_t ap;

    // Capture
    unicapture_state_t unicapture;
    capture_backend_t video_backend;
    bool backend_initialized;
    bool capture_running;
    bool power_paused; // TV screen is off
    uint64_t preview_until_ms; // keep capturing for the debug page / app preview
    uint64_t frames;

    // Latest frame (RGB24) for the debug page.
    uint8_t* last_frame;
    size_t last_frame_cap;
    int last_w, last_h;
    uint64_t last_frame_ms;

    // Virtual remote + key queue
    int vremote_fd;
    pthread_t key_thread;
    bool key_thread_running;
    pthread_mutex_t kq_lock;
    pthread_cond_t kq_cond;
    int kq[KEYQ_LEN];
    int kq_head, kq_tail;

    remote_listen_t listener;
    debug_http_t http;
    recorder_t rec;

    char fg_app[128];

    LSHandle* handle;
    GMainLoop* loop;
} service_t;

int service_init(service_t* svc, GMainLoop* loop);
int service_register(service_t* svc);
void service_destroy(service_t* svc);
