#pragma once
// Passively reads key events from every /dev/input/event* device (except our
// own virtual remote). Devices are not grabbed, so the TV still receives
// every key as usual.
#include <pthread.h>
#include <stdbool.h>

#define RL_MAX_DEVS 16

// value: 1 = press, 2 = autorepeat, 0 = release
typedef void (*remote_key_cb_t)(void* data, int code, int value, const char* device);

typedef struct {
    remote_key_cb_t cb;
    void* data;
    bool running;
    pthread_t thread;
    int fds[RL_MAX_DEVS];
    char names[RL_MAX_DEVS][64];
    int nfds;
} remote_listen_t;

int remote_listen_start(remote_listen_t* rl, remote_key_cb_t cb, void* data);
void remote_listen_stop(remote_listen_t* rl);
