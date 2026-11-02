// sportsap (Sports Autopilot): on-TV service plus a few command-line diagnostics.
//
//   sportsap                     run as luna service (normal mode)
//   sportsap --capture-test [s]  capture for s seconds, save frames to /tmp, report black/non-black
//   sportsap --listen-keys       print every remote key event (find color-button codes)
//   sportsap --send-key KEY      inject one key (LEFT/RIGHT/UP/DOWN/ENTER/BACK or a numeric code)
#include "frame_io.h"
#include "keys.h"
#include "log.h"
#include "remote_listen.h"
#include "service.h"
#include "settings.h"
#include "unicapture.h"
#include "utils.h"

#include <glib.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static GMainLoop* loop = NULL;
static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig __attribute__((unused)))
{
    stop_requested = 1;
    if (loop)
        g_main_loop_quit(loop);
}

// --- --capture-test -------------------------------------------------------

typedef struct {
    int empty;
    int frames;
    int saved;
    double luma_sum;
    int nonblack;
    uint64_t last_save_ms;
} captest_t;

static int captest_frame(void* data, int w, int h, uint8_t* rgb)
{
    captest_t* t = data;
    if (!rgb || w <= 0 || h <= 0) {
        // unicapture calls back even when no frame was captured.
        t->empty++;
        return 0;
    }
    uint64_t now = getticks_us() / 1000;
    double sum = 0;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++)
        sum += (77 * rgb[i * 3] + 150 * rgb[i * 3 + 1] + 29 * rgb[i * 3 + 2]) >> 8;
    double mean = sum / n;
    t->frames++;
    t->luma_sum += mean;
    if (mean > 8)
        t->nonblack++;
    if (t->saved < 5 && now - t->last_save_ms >= 1000) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/sports-autopilot-capture-%d.ppm", t->saved);
        if (ppm_write(path, rgb, w, h, w * 3) == 0)
            printf("saved %s (%dx%d, mean luma %.1f)\n", path, w, h, mean);
        t->saved++;
        t->last_save_ms = now;
    }
    return 0;
}

static int capture_test(int seconds)
{
    settings_t s;
    settings_init(&s);
    settings_load_file(&s, SETTINGS_PATH);

    cap_backend_config_t config;
    memset(&config, 0, sizeof(config));
    config.resolution_width = s.capture_width;
    config.resolution_height = s.capture_height;
    config.fps = s.fps;
    config.quirks = s.quirks;
    config.no_gui = true;

    capture_backend_t backend;
    memset(&backend, 0, sizeof(backend));
    const char* const candidates[] = { "libvtcapture_backend.so", "libdile_vt_backend.so", NULL };
    int ret;
    if (strcmp(s.video_backend, "auto") == 0) {
        ret = unicapture_try_backends(&config, &backend, candidates);
    } else {
        char name[64];
        snprintf(name, sizeof(name), "%s_backend.so", s.video_backend);
        ret = unicapture_init_backend(&config, &backend, name);
    }
    if (ret != 0) {
        printf("RESULT: no capture backend could be initialized (run as root?)\n");
        fflush(stdout);
        // Skip library destructors: LG's libdile_vt asserts in its exit
        // handler when its context was never created ("osal_MutexLock ... Aborted").
        _exit(1);
    }
    printf("backend: %s; capturing for %d s - play YouTube TV Multiview now\n", backend.name, seconds);

    captest_t t;
    memset(&t, 0, sizeof(t));
    unicapture_state_t u;
    unicapture_init(&u);
    u.video_capture = &backend;
    u.vsync = false;
    u.fps = s.fps;
    u.target_format = PIXFMT_RGB;
    u.callback = captest_frame;
    u.callback_data = &t;
    unicapture_start(&u);
    for (int i = 0; i < seconds * 10 && !stop_requested; i++)
        usleep(100 * 1000);
    unicapture_stop(&u);
    backend.cleanup(backend.state);

    printf("frames: %d, non-black: %d, average luma: %.1f (empty callbacks: %d)\n", t.frames, t.nonblack,
        t.frames ? t.luma_sum / t.frames : 0, t.empty);
    if (t.frames == 0)
        printf("RESULT: capture never started (no frames). Reboot the TV and retry; if it persists try \"fps\": 30 in the config.\n");
    else if (t.nonblack == 0)
        printf("RESULT: frames are all black. Capture is probably blocked for this content (DRM) - try another backend/quirk.\n");
    else
        printf("RESULT: capture works. Copy /tmp/sports-autopilot-capture-*.ppm off the TV to check the picture.\n");
    fflush(stdout);
    _exit(0); // skip LG library destructors (they assert on exit)
}

// --- --listen-keys --------------------------------------------------------

static void print_key(void* data __attribute__((unused)), int code, int value, const char* device)
{
    if (value == 1)
        printf("key code %3d (0x%03x) from \"%s\"\n", code, code, device);
    fflush(stdout);
}

static int listen_keys(void)
{
    remote_listen_t rl;
    memset(&rl, 0, sizeof(rl));
    printf("Press remote buttons (Ctrl-C to quit). Expected color keys: RED=%d GREEN=%d YELLOW=%d BLUE=%d\n",
        0x18e, 0x18f, 0x190, 0x191);
    if (remote_listen_start(&rl, print_key, NULL) != 0)
        return 1;
    while (!stop_requested)
        usleep(100 * 1000);
    remote_listen_stop(&rl);
    return 0;
}

// --- --send-key -----------------------------------------------------------

static int send_key(const char* name)
{
    settings_t s;
    settings_init(&s);
    settings_load_file(&s, SETTINGS_PATH);
    struct {
        const char* n;
        int code;
    } map[] = {
        { "LEFT", s.key_left }, { "RIGHT", s.key_right }, { "UP", s.key_up },
        { "DOWN", s.key_down }, { "ENTER", s.key_enter }, { "BACK", s.key_back },
    };
    int code = atoi(name);
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(name, map[i].n) == 0)
            code = map[i].code;
    }
    if (code <= 0 || code >= KEY_MAX) {
        fprintf(stderr, "unknown key %s\n", name);
        return 2;
    }
    int fd = vremote_open(&code, 1);
    if (fd < 0) {
        fprintf(stderr, "cannot create virtual remote: %s (run as root)\n", strerror(-fd));
        return 1;
    }
    usleep(500 * 1000); // let the compositor pick up the new device
    int r = vremote_press(fd, code);
    usleep(200 * 1000);
    vremote_close(fd);
    printf("sent key %d: %s\n", code, r == 0 ? "ok" : "failed");
    return r == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    log_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            log_set_level(Debug);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--capture-test") == 0)
            return capture_test(i + 1 < argc && atoi(argv[i + 1]) > 0 ? atoi(argv[i + 1]) : 10);
        if (strcmp(argv[i], "--listen-keys") == 0)
            return listen_keys();
        if (strcmp(argv[i], "--send-key") == 0 && i + 1 < argc)
            return send_key(argv[i + 1]);
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [-v] [--capture-test [seconds] | --listen-keys | --send-key KEY]\n", argv[0]);
            return 0;
        }
    }

    // Kill switch: `touch /media/developer/sports-autopilot/disabled` stops the
    // service from doing anything (handy if something misbehaves at boot).
    if (access(DATA_DIR "/disabled", F_OK) == 0) {
        INFO("%s/disabled exists - exiting without starting", DATA_DIR);
        return 0;
    }

    INFO("starting sports-autopilot service");
    loop = g_main_loop_new(NULL, false);
    service_t* svc = calloc(1, sizeof(service_t));
    if (!svc || service_init(svc, loop) != 0) {
        ERR("service init failed");
        return 1;
    }
    if (service_register(svc) != 0)
        WARN("luna registration failed; color buttons and the debug page still work");

    g_main_loop_run(loop);

    INFO("shutting down");
    service_destroy(svc);
    free(svc);
    g_main_loop_unref(loop);
    return 0;
}
