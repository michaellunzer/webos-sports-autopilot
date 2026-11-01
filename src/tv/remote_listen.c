#include "remote_listen.h"
#include "keys.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void close_all(remote_listen_t* rl)
{
    for (int i = 0; i < rl->nfds; i++)
        close(rl->fds[i]);
    rl->nfds = 0;
}

static void scan_devices(remote_listen_t* rl)
{
    close_all(rl);
    DIR* d = opendir("/dev/input");
    if (!d) {
        WARN("opendir /dev/input: %s", strerror(errno));
        return;
    }
    struct dirent* de;
    while ((de = readdir(d)) != NULL && rl->nfds < RL_MAX_DEVS) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        unsigned long evbits = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits);
        if (strcmp(name, VREMOTE_NAME) == 0 || !(evbits & (1UL << EV_KEY))) {
            close(fd);
            continue;
        }
        rl->fds[rl->nfds] = fd;
        snprintf(rl->names[rl->nfds], sizeof(rl->names[0]), "%s", name);
        DBG("listening on %s (%s)", path, name);
        rl->nfds++;
    }
    closedir(d);
}

static void* listen_thread(void* arg)
{
    remote_listen_t* rl = arg;
    time_t last_scan = 0;
    while (rl->running) {
        time_t now = time(NULL);
        if (now - last_scan >= 10) { // remotes can (re)connect at any time
            scan_devices(rl);
            last_scan = now;
        }
        struct pollfd pfd[RL_MAX_DEVS];
        for (int i = 0; i < rl->nfds; i++) {
            pfd[i].fd = rl->fds[i];
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        int r = poll(pfd, rl->nfds, 500);
        if (r <= 0) {
            if (rl->nfds == 0)
                usleep(500 * 1000);
            continue;
        }
        for (int i = 0; i < rl->nfds; i++) {
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                last_scan = 0; // device went away; rescan
                continue;
            }
            if (!(pfd[i].revents & POLLIN))
                continue;
            struct input_event ev[16];
            ssize_t n = read(rl->fds[i], ev, sizeof(ev));
            if (n <= 0)
                continue;
            for (size_t k = 0; k < n / sizeof(struct input_event); k++) {
                if (ev[k].type == EV_KEY)
                    rl->cb(rl->data, ev[k].code, ev[k].value, rl->names[i]);
            }
        }
    }
    close_all(rl);
    return NULL;
}

int remote_listen_start(remote_listen_t* rl, remote_key_cb_t cb, void* data)
{
    if (rl->running)
        return 0;
    rl->cb = cb;
    rl->data = data;
    rl->nfds = 0;
    rl->running = true;
    if (pthread_create(&rl->thread, NULL, listen_thread, rl) != 0) {
        rl->running = false;
        return -1;
    }
    return 0;
}

void remote_listen_stop(remote_listen_t* rl)
{
    if (!rl->running)
        return;
    rl->running = false;
    pthread_join(rl->thread, NULL);
}
