#include "service.h"
#include "frame_io.h"
#include "keys.h"
#include "log.h"
#include "luna_util.h"
#include "utils.h"

#include <errno.h>
#include <pbnjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SPORTSAP_VERSION
#define SPORTSAP_VERSION "dev"
#endif

#define AUTOSTART_DIR "/var/lib/webosbrew/init.d"
#define AUTOSTART_LINK AUTOSTART_DIR "/sportsautopilot"
#define AUTOSTART_SCRIPT "/media/developer/apps/usr/palm/services/" SERVICE_NAME "/sportsautopilot-autostart"

// Same symbol trick as hyperion-webos: needed for public service
// registration on older webOS versions.
extern bool LSRegisterPubPriv(const char* name, LSHandle** sh, bool public_bus, LSError* lserror) __attribute__((weak));

static uint64_t now_ms(void)
{
    return getticks_us() / 1000;
}

// ---------------------------------------------------------------------------
// Settings -> core parameters
// ---------------------------------------------------------------------------

static void detector_params_from_settings(const settings_t* s, detector_params_t* p)
{
    detector_default_params(p);
    p->live_score_on = (float)s->live_score_on;
    p->live_score_off = (float)s->live_score_off;
    p->live_hold_ms = s->live_hold_ms;
    p->ad_hold_ms = s->ad_hold_ms;
    p->warmup_ms = s->warmup_ms;
    p->band_top = (float)s->band_top;
    p->band_bottom = (float)s->band_bottom;
    p->edge_thresh = (float)s->edge_thresh;
    p->act_thresh = (float)s->act_thresh;
    p->persist_tau_s = (float)s->persist_tau_s;
    p->persist_on = (float)s->persist_on;
}

static bool video_mode(const settings_t* s)
{
    return strcmp(s->detection, "video") == 0;
}

static void pred_params_from_settings(const settings_t* s, pred_params_t* p)
{
    pred_default_params(p);
    p->stream_delay_s = s->stream_delay_s;
    p->idle_break_s = s->idle_break_s;
    p->trigger_idle_s = s->trigger_idle_s;
    p->resume_lead_s = s->resume_lead_s;
}

static void ap_params_from_settings(const settings_t* s, ap_params_t* p)
{
    ap_default_params(p);
    p->grid_cols = s->grid_cols;
    p->grid_rows = s->grid_rows;
    p->min_switch_interval_ms = s->min_switch_interval_ms;
    p->manual_pause_ms = s->manual_pause_ms;
    p->scan_settle_ms = s->scan_settle_ms;
    p->reset_focus_on_start = s->reset_focus_on_start;
    if (!video_mode(s))
        p->scan_settle_ms = 1500; // game data needs no picture warm-up
}

// Lock held.
static void set_layout(service_t* svc, int ntiles)
{
    const settings_t* s = &svc->settings;
    rectf_t rects[DET_MAX_TILES];
    if (ntiles <= 1) {
        detector_layout_full(&rects[0]);
        detector_set_layout(&svc->det, 1, rects);
        return;
    }
    detector_layout_grid(rects, s->grid_cols, s->grid_rows, (float)s->mv_gutter);
    float x0 = (float)s->mv_inset_left, y0 = (float)s->mv_inset_top;
    float w = 1.0f - x0 - (float)s->mv_inset_right, h = 1.0f - y0 - (float)s->mv_inset_bottom;
    int n = s->grid_cols * s->grid_rows;
    for (int i = 0; i < n; i++) {
        rects[i].x = x0 + rects[i].x * w;
        rects[i].y = y0 + rects[i].y * h;
        rects[i].w *= w;
        rects[i].h *= h;
    }
    detector_set_layout(&svc->det, n, rects);
}

// Lock held.
static int rebuild_detector(service_t* svc)
{
    detector_params_t dp;
    detector_params_from_settings(&svc->settings, &dp);
    detector_free(&svc->det);
    if (detector_init(&svc->det, &dp) != 0)
        return -1;
    set_layout(svc, ap_expected_tiles(&svc->ap));
    return 0;
}

// ---------------------------------------------------------------------------
// Virtual remote / key queue
// ---------------------------------------------------------------------------

static int key_code(const settings_t* s, ap_key_t k)
{
    switch (k) {
    case AP_KEY_LEFT:
        return s->key_left;
    case AP_KEY_RIGHT:
        return s->key_right;
    case AP_KEY_UP:
        return s->key_up;
    case AP_KEY_DOWN:
        return s->key_down;
    case AP_KEY_ENTER:
        return s->key_enter;
    case AP_KEY_BACK:
        return s->key_back;
    default:
        return 0;
    }
}

static void open_vremote(service_t* svc)
{
    if (svc->vremote_fd >= 0) {
        vremote_close(svc->vremote_fd);
    ssap_close(&svc->ssap);
        svc->vremote_fd = -1;
    }
    if (!svc->settings.use_uinput)
        return;
    int codes[AP_KEY_COUNT];
    for (int k = 0; k < AP_KEY_COUNT; k++)
        codes[k] = key_code(&svc->settings, (ap_key_t)k);
    int fd = vremote_open(codes, AP_KEY_COUNT);
    if (fd < 0) {
        ERR("virtual remote unavailable: %s (is the service elevated/root?)", strerror(-fd));
        return;
    }
    svc->vremote_fd = fd;
    INFO("virtual remote ready");
}

static void enqueue_key(service_t* svc, ap_key_t key)
{
    pthread_mutex_lock(&svc->kq_lock);
    int next = (svc->kq_tail + 1) % KEYQ_LEN;
    if (next != svc->kq_head) {
        svc->kq[svc->kq_tail] = (int)key;
        svc->kq_tail = next;
        pthread_cond_signal(&svc->kq_cond);
    }
    pthread_mutex_unlock(&svc->kq_lock);
}

static void* key_thread(void* arg)
{
    service_t* svc = arg;
    pthread_mutex_lock(&svc->kq_lock);
    while (svc->key_thread_running) {
        if (svc->kq_head == svc->kq_tail) {
            pthread_cond_wait(&svc->kq_cond, &svc->kq_lock);
            continue;
        }
        ap_key_t key = (ap_key_t)svc->kq[svc->kq_head];
        svc->kq_head = (svc->kq_head + 1) % KEYQ_LEN;
        int fd = svc->vremote_fd;
        int delay = svc->settings.key_delay_ms;
        bool uinput = svc->settings.use_uinput;
        int code = key_code(&svc->settings, key);
        svc->last_injected_ms = now_ms();
        pthread_mutex_unlock(&svc->kq_lock);

        int r;
        if (uinput) {
            r = fd >= 0 ? vremote_press(fd, code) : -1;
        } else {
            if (!ssap_has_key(&svc->ssap))
                luna_toast("Sports Autopilot wants to control the TV - select Allow on the prompt");
            r = ssap_button(&svc->ssap, ap_key_name(key));
        }
        INFO("press %s via %s: %s", ap_key_name(key), uinput ? "uinput" : "ssap", r == 0 ? "ok" : "FAILED");
        usleep((useconds_t)delay * 1000);

        pthread_mutex_lock(&svc->kq_lock);
        svc->last_injected_ms = now_ms();
    }
    pthread_mutex_unlock(&svc->kq_lock);
    return NULL;
}

// Lock held.
static void apply_action(service_t* svc, const ap_action_t* a)
{
    if (a->set_layout_tiles >= 0)
        set_layout(svc, a->set_layout_tiles);
    for (int i = 0; i < a->nkeys; i++)
        enqueue_key(svc, a->keys[i]);
    if (a->reason)
        INFO("autopilot: %s%s%s (keys: %d)", a->reason, a->message[0] ? " - " : "", a->message, a->nkeys);
    if (a->message[0])
        luna_toast(a->message);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

static const char* state_short(tile_state_t s)
{
    return s == TILE_LIVE ? "LIVE" : s == TILE_AD ? "AD" : "?";
}

// Lock held.
static void status_text(service_t* svc, char* buf, size_t n)
{
    int len = snprintf(buf, n, "Autopilot %s - %s", svc->ap.enabled ? "ON" : "OFF",
        svc->ap.mode == AP_MODE_FULLSCREEN ? "Full screen" : "Multiview");
    if (svc->ap.phase == AP_PHASE_PAUSED)
        len += snprintf(buf + len, n - len, " (paused)");
    if (!video_mode(&svc->settings)) {
        for (int i = 0; i < ESPN_MAX_GAMES && len < (int)n; i++) {
            if (!svc->games[i].event_id[0])
                continue;
            len += snprintf(buf + len, n - len, " | G%d %s %s", i + 1,
                svc->games[i].label[0] ? svc->games[i].label : "?", state_short(svc->game_results[i].state));
        }
        if (svc->settings.tile_events[0] == 0)
            snprintf(buf + len, n - len, " - no games assigned (open the app)");
        return;
    }
    if (!svc->capture_running) {
        snprintf(buf + len, n - len, " - capture off");
        return;
    }
    for (int i = 0; i < svc->det.ntiles && len < (int)n; i++)
        len += snprintf(buf + len, n - len, " | G%d %s", i + 1, state_short(svc->det.tiles[i].state));
}

// Lock held.
static jvalue_ref status_obj(service_t* svc)
{
    uint64_t now = now_ms();
    jvalue_ref o = jobject_create();
    jobject_set(o, j_cstr_to_buffer("returnValue"), jboolean_create(true));
    jobject_set(o, j_cstr_to_buffer("version"), jstring_create(SPORTSAP_VERSION));
    jobject_set(o, j_cstr_to_buffer("elevated"), jboolean_create(getuid() == 0));
    jobject_set(o, j_cstr_to_buffer("enabled"), jboolean_create(svc->ap.enabled));
    jobject_set(o, j_cstr_to_buffer("mode"), jstring_create(ap_mode_name(svc->ap.mode)));
    jobject_set(o, j_cstr_to_buffer("phase"), jstring_create(ap_phase_name(svc->ap.phase)));
    jobject_set(o, j_cstr_to_buffer("focus"), jnumber_create_i32(svc->ap.focus));
    jobject_set(o, j_cstr_to_buffer("fullscreenTile"), jnumber_create_i32(svc->ap.fs_tile));
    jobject_set(o, j_cstr_to_buffer("switches"), jnumber_create_i32(svc->ap.switches));
    jobject_set(o, j_cstr_to_buffer("pausedForMs"),
        jnumber_create_i32(svc->ap.phase == AP_PHASE_PAUSED && svc->ap.paused_until_ms > now ? (int)(svc->ap.paused_until_ms - now) : 0));
    jobject_set(o, j_cstr_to_buffer("foregroundApp"), jstring_create(svc->fg_app));
    jobject_set(o, j_cstr_to_buffer("foregroundOk"), jboolean_create(svc->ap.foreground_ok));
    jobject_set(o, j_cstr_to_buffer("virtualRemote"), jboolean_create(svc->vremote_fd >= 0));
    jobject_set(o, j_cstr_to_buffer("captureRunning"), jboolean_create(svc->capture_running));
    jobject_set(o, j_cstr_to_buffer("powerPaused"), jboolean_create(svc->power_paused));
    jobject_set(o, j_cstr_to_buffer("backend"), svc->video_backend.name ? jstring_create(svc->video_backend.name) : jnull());
    jobject_set(o, j_cstr_to_buffer("framerate"), jnumber_create_f64(svc->unicapture.metrics.framerate));
    jobject_set(o, j_cstr_to_buffer("frames"), jnumber_create_f64((double)svc->frames));
    jobject_set(o, j_cstr_to_buffer("frameAgeMs"),
        jnumber_create_i32(svc->last_frame_ms ? (int)(now - svc->last_frame_ms) : -1));
    jobject_set(o, j_cstr_to_buffer("recording"), jboolean_create(svc->rec.active));
    jobject_set(o, j_cstr_to_buffer("recordedFrames"), jnumber_create_i32(svc->rec.count));
    jobject_set(o, j_cstr_to_buffer("recordDir"), jstring_create(svc->rec.dir));

    jvalue_ref tiles = jarray_create(NULL);
    double luma = 0;
    for (int i = 0; i < svc->det.ntiles; i++) {
        const det_tile_t* t = &svc->det.tiles[i];
        jvalue_ref to = jobject_create();
        jobject_set(to, j_cstr_to_buffer("state"), jstring_create(tile_state_name(t->state)));
        jobject_set(to, j_cstr_to_buffer("score"), jnumber_create_f64(t->score));
        jobject_set(to, j_cstr_to_buffer("band"), jnumber_create_f64(t->band_frac));
        jobject_set(to, j_cstr_to_buffer("body"), jnumber_create_f64(t->body_frac));
        jobject_set(to, j_cstr_to_buffer("motion"), jnumber_create_f64(t->motion));
        jobject_set(to, j_cstr_to_buffer("luma"), jnumber_create_f64(t->mean_luma));
        jobject_set(to, j_cstr_to_buffer("black"), jboolean_create(t->black));
        jobject_set(to, j_cstr_to_buffer("stateForMs"),
            jnumber_create_i32(t->state_since_ms && now > t->state_since_ms ? (int)(now - t->state_since_ms) : 0));
        jarray_append(tiles, to);
        luma += t->mean_luma;
    }
    jobject_set(o, j_cstr_to_buffer("tiles"), tiles);

    jobject_set(o, j_cstr_to_buffer("detection"), jstring_create(svc->settings.detection));
    jobject_set(o, j_cstr_to_buffer("keyMethod"), jstring_create(svc->settings.use_uinput ? "uinput" : "ssap"));
    jobject_set(o, j_cstr_to_buffer("remotePaired"), jboolean_create(ssap_has_key(&svc->ssap)));
    jobject_set(o, j_cstr_to_buffer("streamDelay"), jnumber_create_i32(svc->settings.stream_delay_s));
    jobject_set(o, j_cstr_to_buffer("dataError"), jstring_create(svc->espn.last_error));
    jobject_set(o, j_cstr_to_buffer("dataAgeS"),
        jnumber_create_i32(svc->espn.last_ok_ms ? (int)((realtime_ms() - svc->espn.last_ok_ms) / 1000) : -1));
    jvalue_ref games = jarray_create(NULL);
    for (int i = 0; i < ESPN_MAX_GAMES; i++) {
        const pred_game_t* g = &svc->games[i];
        const pred_result_t* r = &svc->game_results[i];
        jvalue_ref go = jobject_create();
        jobject_set(go, j_cstr_to_buffer("event"), jstring_create(g->event_id));
        jobject_set(go, j_cstr_to_buffer("label"), jstring_create(g->label));
        jobject_set(go, j_cstr_to_buffer("state"), jstring_create(g->event_id[0] ? tile_state_name(r->state) : "none"));
        jobject_set(go, j_cstr_to_buffer("gapS"), jnumber_create_i32(r->gap_s));
        jobject_set(go, j_cstr_to_buffer("lastPlay"), jstring_create(r->last ? r->last->type : ""));
        jobject_set(go, j_cstr_to_buffer("plays"), jnumber_create_i32(g->nplays));
        jarray_append(games, go);
    }
    jobject_set(o, j_cstr_to_buffer("games"), games);
    // ~0 for a long time while video plays means capture returns black (DRM).
    jobject_set(o, j_cstr_to_buffer("frameMeanLuma"), jnumber_create_f64(svc->det.ntiles ? luma / svc->det.ntiles : 0));
    return o;
}

static char* status_json_cb(void* data)
{
    service_t* svc = data;
    pthread_mutex_lock(&svc->lock);
    // Looking at the debug page keeps capture alive for a minute.
    svc->preview_until_ms = now_ms() + 60000;
    jvalue_ref o = status_obj(svc);
    char* s = strdup(jvalue_tostring_simple(o));
    j_release(&o);
    pthread_mutex_unlock(&svc->lock);
    return s;
}

static uint8_t* frame_bmp_cb(void* data, size_t* len)
{
    service_t* svc = data;
    uint8_t* out = NULL;
    pthread_mutex_lock(&svc->lock);
    svc->preview_until_ms = now_ms() + 60000;
    if (svc->last_frame && svc->last_w > 0) {
        int w = svc->last_w, h = svc->last_h;
        size_t sz = (size_t)w * h * 3;
        uint8_t* copy = malloc(sz);
        if (copy) {
            memcpy(copy, svc->last_frame, sz);
            for (int i = 0; i < svc->det.ntiles; i++) {
                const rectf_t* r = &svc->det.rects[i];
                tile_state_t st = svc->det.tiles[i].state;
                uint8_t cr = st == TILE_AD ? 255 : st == TILE_LIVE ? 0 : 128;
                uint8_t cg = st == TILE_LIVE ? 220 : st == TILE_AD ? 40 : 128;
                bool focused = (svc->ap.phase == AP_PHASE_MV_WATCH || svc->ap.phase == AP_PHASE_MV_SCAN) && svc->ap.focus == i;
                int x0 = (int)(r->x * w) + 1, y0 = (int)(r->y * h) + 1;
                int x1 = (int)((r->x + r->w) * w) - 2, y1 = (int)((r->y + r->h) * h) - 2;
                rgb_draw_rect(copy, w, h, w * 3, x0, y0, x1, y1, cr, cg, 40);
                if (focused)
                    rgb_draw_rect(copy, w, h, w * 3, x0 + 3, y0 + 3, x1 - 3, y1 - 3, 255, 255, 255);
                // Score-bug search bands.
                int bt = y0 + (int)(svc->det.p.band_top * r->h * h);
                int bb = y1 - (int)(svc->det.p.band_bottom * r->h * h);
                for (int x = x0 + 2; x < x1 - 2; x += 4) {
                    uint8_t* p1 = copy + (size_t)bt * w * 3 + (size_t)x * 3;
                    uint8_t* p2 = copy + (size_t)bb * w * 3 + (size_t)x * 3;
                    p1[0] = p1[1] = p1[2] = 255;
                    p2[0] = p2[1] = p2[2] = 255;
                }
            }
            out = bmp_encode(copy, w, h, w * 3, len);
            free(copy);
        }
    }
    pthread_mutex_unlock(&svc->lock);
    return out;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

static int on_frame(void* data, int width, int height, uint8_t* rgb)
{
    service_t* svc = data;
    if (!rgb || width <= 0 || height <= 0)
        return 0;
    uint64_t now = now_ms();
    size_t sz = (size_t)width * height * 3;

    pthread_mutex_lock(&svc->lock);
    if (svc->last_frame_cap < sz) {
        uint8_t* nb = realloc(svc->last_frame, sz);
        if (nb) {
            svc->last_frame = nb;
            svc->last_frame_cap = sz;
        }
    }
    if (svc->last_frame_cap >= sz) {
        memcpy(svc->last_frame, rgb, sz);
        svc->last_w = width;
        svc->last_h = height;
        svc->last_frame_ms = now;
    }
    svc->frames++;

    detector_process_rgb(&svc->det, rgb, width, height, width * 3, now);

    ap_tile_t tiles[DET_MAX_TILES];
    for (int i = 0; i < svc->det.ntiles; i++) {
        tiles[i].state = svc->det.tiles[i].state;
        tiles[i].state_since_ms = svc->det.tiles[i].state_since_ms;
    }

    if (svc->rec.active) {
        char line[256];
        int len = snprintf(line, sizeof(line), "%s,%s,%d", ap_phase_name(svc->ap.phase), ap_mode_name(svc->ap.mode), svc->ap.focus);
        for (int i = 0; i < svc->det.ntiles && len < (int)sizeof(line); i++)
            len += snprintf(line + len, sizeof(line) - len, ",%s,%.4f", tile_state_name(tiles[i].state), svc->det.tiles[i].score);
        recorder_frame(&svc->rec, rgb, width, height, now, line);
    }

    if (video_mode(&svc->settings)) {
        ap_action_t a;
        ap_action_clear(&a);
        ap_tick(&svc->ap, tiles, svc->det.ntiles, now, &a);
        apply_action(svc, &a);
    }
    pthread_mutex_unlock(&svc->lock);
    return 0;
}

static int init_backend(service_t* svc)
{
    const settings_t* s = &svc->settings;
    cap_backend_config_t config;
    memset(&config, 0, sizeof(config));
    config.resolution_width = s->capture_width;
    config.resolution_height = s->capture_height;
    config.fps = s->fps;
    config.quirks = s->quirks;
    config.no_gui = true;

    const char* const candidates[] = { "libvtcapture_backend.so", "libdile_vt_backend.so", NULL };
    int ret;
    if (s->video_backend[0] == 0 || strcmp(s->video_backend, "auto") == 0) {
        ret = unicapture_try_backends(&config, &svc->video_backend, candidates);
    } else {
        char name[64];
        snprintf(name, sizeof(name), "%s_backend.so", s->video_backend);
        ret = unicapture_init_backend(&config, &svc->video_backend, name);
    }
    svc->backend_initialized = ret == 0;
    return ret;
}

static void start_capture(service_t* svc)
{
    if (!svc->backend_initialized && init_backend(svc) != 0) {
        ERR("no capture backend available");
        return;
    }
    unicapture_init(&svc->unicapture);
    svc->unicapture.video_capture = &svc->video_backend;
    svc->unicapture.ui_capture = NULL;
    svc->unicapture.vsync = false; // pace with fps instead of every vsync
    svc->unicapture.fps = svc->settings.fps;
    svc->unicapture.target_format = PIXFMT_RGB;
    svc->unicapture.callback = on_frame;
    svc->unicapture.callback_nv12 = NULL;
    svc->unicapture.callback_data = svc;

    pthread_mutex_lock(&svc->lock);
    set_layout(svc, ap_expected_tiles(&svc->ap)); // re-enter warm-up
    pthread_mutex_unlock(&svc->lock);

    if (unicapture_start(&svc->unicapture) == 0) {
        pthread_mutex_lock(&svc->lock);
        svc->capture_running = true;
        pthread_mutex_unlock(&svc->lock);
        INFO("capture started (%s, %dx%d @ %d fps)", svc->video_backend.name,
            svc->settings.capture_width, svc->settings.capture_height, svc->settings.fps);
    }
}

static void stop_capture(service_t* svc)
{
    if (svc->capture_running) {
        unicapture_stop(&svc->unicapture); // joins the capture thread; lock must not be held
        pthread_mutex_lock(&svc->lock);
        svc->capture_running = false;
        pthread_mutex_unlock(&svc->lock);
        INFO("capture stopped");
    }
    if (svc->backend_initialized && svc->video_backend.cleanup) {
        svc->video_backend.cleanup(svc->video_backend.state);
        svc->backend_initialized = false;
        free(svc->video_backend.name);
        svc->video_backend.name = NULL;
    }
}

// Main-loop only.
static void update_capture(service_t* svc)
{
    pthread_mutex_lock(&svc->lock);
    uint64_t now = now_ms();
    bool need = ((svc->ap.enabled && video_mode(&svc->settings)) || svc->rec.active || now < svc->preview_until_ms)
        && !svc->power_paused;
    bool running = svc->capture_running;
    pthread_mutex_unlock(&svc->lock);

    if (need && !running)
        start_capture(svc);
    else if (!need && running)
        stop_capture(svc);
}

static gboolean housekeeping(gpointer data)
{
    update_capture((service_t*)data);
    return G_SOURCE_CONTINUE;
}

// Once a second: evaluate the break predictor for each tile and drive the
// autopilot from it (game-data mode).
static gboolean gamedata_tick(gpointer data)
{
    service_t* svc = data;
    pthread_mutex_lock(&svc->lock);
    pred_params_t pp;
    pred_params_from_settings(&svc->settings, &pp);
    int64_t rt = realtime_ms();
    for (int i = 0; i < ESPN_MAX_GAMES; i++)
        svc->game_results[i] = pred_evaluate(&svc->games[i], &pp, rt);

    if (!video_mode(&svc->settings) && svc->ap.enabled) {
        // Express "since" on the monotonic clock the autopilot uses.
        uint64_t now = now_ms();
        ap_tile_t tiles[ESPN_MAX_GAMES];
        int n = ap_expected_tiles(&svc->ap);
        for (int i = 0; i < n && i < ESPN_MAX_GAMES; i++) {
            int gi = n == 1 ? svc->ap.fs_tile : i;
            if (gi < 0 || gi >= ESPN_MAX_GAMES || !svc->games[gi].event_id[0]) {
                tiles[i].state = TILE_UNKNOWN;
                tiles[i].state_since_ms = 0;
                continue;
            }
            const pred_result_t* r = &svc->game_results[gi];
            tiles[i].state = r->state;
            int64_t ago = rt - r->since_ms;
            tiles[i].state_since_ms = ago > 0 && (uint64_t)ago < now ? now - (uint64_t)ago : now;
        }
        ap_action_t a;
        ap_action_clear(&a);
        ap_tick(&svc->ap, tiles, n, now, &a);
        apply_action(svc, &a);
    }
    pthread_mutex_unlock(&svc->lock);
    return G_SOURCE_CONTINUE;
}

static gboolean update_capture_idle(gpointer data)
{
    update_capture((service_t*)data);
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Remote buttons
// ---------------------------------------------------------------------------

static bool is_nav_key(const settings_t* s, int code)
{
    return code == s->key_left || code == s->key_right || code == s->key_up || code == s->key_down
        || code == s->key_enter || code == s->key_back;
}

// Keys we press through the TV may show up on an input device too; don't
// mistake them for the user taking over.
static bool recently_injected(service_t* svc)
{
    pthread_mutex_lock(&svc->kq_lock);
    bool busy = svc->kq_head != svc->kq_tail || now_ms() - svc->last_injected_ms < 1500;
    pthread_mutex_unlock(&svc->kq_lock);
    return busy;
}

static void on_remote_key(void* data, int code, int value, const char* device)
{
    service_t* svc = data;
    if (value != 1)
        return; // presses only
    DBG("key %d from %s", code, device);

    const settings_t* s = &svc->settings;
    char msg[160] = { 0 };
    bool capture_change = false;
    uint64_t now = now_ms();
    ap_action_t a;
    ap_action_clear(&a);

    pthread_mutex_lock(&svc->lock);
    if (code == s->key_enable) {
        ap_set_enabled(&svc->ap, true, now, &a);
        apply_action(svc, &a);
        snprintf(msg, sizeof(msg), "Autopilot ON - %s", svc->ap.mode == AP_MODE_FULLSCREEN ? "Full screen" : "Multiview");
        capture_change = true;
    } else if (code == s->key_disable) {
        ap_set_enabled(&svc->ap, false, now, &a);
        apply_action(svc, &a);
        snprintf(msg, sizeof(msg), "Autopilot OFF");
        capture_change = true;
    } else if (code == s->key_mode) {
        ap_mode_t m = svc->ap.mode == AP_MODE_MULTIVIEW ? AP_MODE_FULLSCREEN : AP_MODE_MULTIVIEW;
        ap_set_mode(&svc->ap, m, now, &a);
        apply_action(svc, &a);
        svc->settings.mode = (int)m;
        snprintf(msg, sizeof(msg), "Autopilot mode: %s", m == AP_MODE_FULLSCREEN ? "Full screen" : "Multiview");
    } else if (code == s->key_status) {
        status_text(svc, msg, sizeof(msg));
    } else if (is_nav_key(s, code) && svc->ap.enabled && !recently_injected(svc)) {
        bool was_paused = svc->ap.phase == AP_PHASE_PAUSED;
        ap_manual_input(&svc->ap, now);
        if (!was_paused)
            snprintf(msg, sizeof(msg), "Autopilot paused for %d s (remote in use)", s->manual_pause_ms / 1000);
    }
    pthread_mutex_unlock(&svc->lock);

    if (msg[0])
        luna_toast(msg);
    if (capture_change)
        g_idle_add(update_capture_idle, svc);
}

// ---------------------------------------------------------------------------
// Luna methods
// ---------------------------------------------------------------------------

static jvalue_ref parse_payload(LSMessage* msg)
{
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    return jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);
}

static void reply(LSHandle* sh, LSMessage* msg, jvalue_ref obj)
{
    LSError err;
    LSErrorInit(&err);
    if (!LSMessageReply(sh, msg, jvalue_tostring_simple(obj), &err)) {
        WARN("reply failed: %s", err.message);
        LSErrorFree(&err);
    }
}

static void reply_simple(LSHandle* sh, LSMessage* msg, bool ok, const char* error)
{
    jvalue_ref o = jobject_create();
    jobject_set(o, j_cstr_to_buffer("returnValue"), jboolean_create(ok));
    if (error)
        jobject_set(o, j_cstr_to_buffer("errorText"), jstring_create(error));
    reply(sh, msg, o);
    j_release(&o);
}

static bool get_bool(jvalue_ref obj, const char* key, bool* out)
{
    jvalue_ref v = jobject_get(obj, j_cstr_to_buffer(key));
    if (!jis_valid(v) || !jis_boolean(v))
        return false;
    jboolean_get(v, out);
    return true;
}

static bool get_str(jvalue_ref obj, const char* key, char* out, size_t n)
{
    jvalue_ref v = jobject_get(obj, j_cstr_to_buffer(key));
    if (!jis_valid(v) || !jis_string(v))
        return false;
    raw_buffer b = jstring_get(v);
    snprintf(out, n, "%s", b.m_str ? b.m_str : "");
    jstring_free_buffer(b);
    return true;
}

static bool m_status(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    pthread_mutex_lock(&svc->lock);
    jvalue_ref o = status_obj(svc);
    pthread_mutex_unlock(&svc->lock);
    reply(sh, msg, o);
    j_release(&o);
    return true;
}

static bool m_get_settings(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref o = jobject_create();
    jobject_set(o, j_cstr_to_buffer("returnValue"), jboolean_create(true));
    pthread_mutex_lock(&svc->lock);
    settings_save_json(&svc->settings, o);
    pthread_mutex_unlock(&svc->lock);
    reply(sh, msg, o);
    j_release(&o);
    return true;
}

static void update_autostart(const settings_t* s)
{
    if (unlink(AUTOSTART_LINK) != 0 && errno != ENOENT)
        WARN("autostart link removal failed: %s", strerror(errno));
    if (s->autostart) {
        mkdir(AUTOSTART_DIR, 0755);
        if (symlink(AUTOSTART_SCRIPT, AUTOSTART_LINK) != 0)
            WARN("autostart link creation failed: %s", strerror(errno));
    }
}

static bool m_set_settings(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    if (jis_null(parsed)) {
        j_release(&parsed);
        reply_simple(sh, msg, false, "invalid JSON");
        return true;
    }

    pthread_mutex_lock(&svc->lock);
    settings_t old = svc->settings;
    settings_t s = svc->settings;
    settings_load_json(&s, parsed);
    svc->settings = s;
    ap_params_from_settings(&s, &svc->ap.p);
    svc->ap.mode = (ap_mode_t)s.mode;
    int det_ok = rebuild_detector(svc);
    pthread_mutex_unlock(&svc->lock);
    j_release(&parsed);

    bool capture_changed = strcmp(old.video_backend, s.video_backend) != 0 || old.quirks != s.quirks
        || old.capture_width != s.capture_width || old.capture_height != s.capture_height || old.fps != s.fps;
    if (capture_changed)
        stop_capture(svc); // housekeeping restarts it with the new settings

    bool keys_changed = old.key_left != s.key_left || old.key_right != s.key_right || old.key_up != s.key_up
        || old.key_down != s.key_down || old.key_enter != s.key_enter || old.key_back != s.key_back;
    if (keys_changed || old.use_uinput != s.use_uinput) {
        pthread_mutex_lock(&svc->kq_lock);
        open_vremote(svc);
        pthread_mutex_unlock(&svc->kq_lock);
    }
    if (old.debug_http_port != s.debug_http_port) {
        debug_http_stop(&svc->http);
        debug_http_start(&svc->http, s.debug_http_port);
    }
    if (old.autostart != s.autostart)
        update_autostart(&s);

    int saved = settings_save_file(&s, SETTINGS_PATH);
    reply_simple(sh, msg, det_ok == 0 && saved == 0, saved != 0 ? "settings not saved" : NULL);
    return true;
}

static bool m_enable(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    bool enabled = true;
    get_bool(parsed, "enabled", &enabled);
    j_release(&parsed);

    ap_action_t a;
    ap_action_clear(&a);
    pthread_mutex_lock(&svc->lock);
    ap_set_enabled(&svc->ap, enabled, now_ms(), &a);
    apply_action(svc, &a);
    pthread_mutex_unlock(&svc->lock);
    update_capture(svc);
    luna_toast(enabled ? "Autopilot ON" : "Autopilot OFF");
    reply_simple(sh, msg, true, NULL);
    return true;
}

static bool m_set_mode(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    char mode[32] = { 0 };
    bool ok = get_str(parsed, "mode", mode, sizeof(mode));
    j_release(&parsed);
    if (!ok || (strcmp(mode, "multiview") != 0 && strcmp(mode, "fullscreen") != 0)) {
        reply_simple(sh, msg, false, "mode must be \"multiview\" or \"fullscreen\"");
        return true;
    }
    ap_mode_t m = strcmp(mode, "fullscreen") == 0 ? AP_MODE_FULLSCREEN : AP_MODE_MULTIVIEW;
    ap_action_t a;
    ap_action_clear(&a);
    pthread_mutex_lock(&svc->lock);
    ap_set_mode(&svc->ap, m, now_ms(), &a);
    apply_action(svc, &a);
    svc->settings.mode = (int)m;
    settings_t copy = svc->settings;
    pthread_mutex_unlock(&svc->lock);
    settings_save_file(&copy, SETTINGS_PATH);
    reply_simple(sh, msg, true, NULL);
    return true;
}

static bool m_record(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    bool start = true;
    get_bool(parsed, "start", &start);
    j_release(&parsed);

    int ret = 0;
    pthread_mutex_lock(&svc->lock);
    if (start && !svc->rec.active)
        ret = recorder_start(&svc->rec, svc->settings.record_dir, svc->settings.record_interval_ms, svc->settings.record_max_frames);
    else if (!start)
        recorder_stop(&svc->rec);
    pthread_mutex_unlock(&svc->lock);
    update_capture(svc);
    reply_simple(sh, msg, ret == 0, ret == 0 ? NULL : "cannot create record directory");
    return true;
}

static bool m_preview(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    pthread_mutex_lock(&svc->lock);
    svc->preview_until_ms = now_ms() + 60000;
    pthread_mutex_unlock(&svc->lock);
    update_capture(svc);
    reply_simple(sh, msg, true, NULL);
    return true;
}

static bool m_send_key(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    char name[16] = { 0 };
    bool ok = get_str(parsed, "key", name, sizeof(name));
    j_release(&parsed);
    int key = -1;
    for (int k = 0; ok && k < AP_KEY_COUNT; k++) {
        if (strcasecmp(name, ap_key_name((ap_key_t)k)) == 0)
            key = k;
    }
    if (key < 0) {
        reply_simple(sh, msg, false, "key must be LEFT, RIGHT, UP, DOWN, ENTER or BACK");
        return true;
    }
    enqueue_key(svc, (ap_key_t)key);
    reply_simple(sh, msg, true, NULL);
    return true;
}

static LSMethod methods[] = {
    { "status", m_status, LUNA_METHOD_FLAGS_NONE },
    { "getSettings", m_get_settings, LUNA_METHOD_FLAGS_NONE },
    { "setSettings", m_set_settings, LUNA_METHOD_FLAGS_NONE },
    { "enable", m_enable, LUNA_METHOD_FLAGS_NONE },
    { "setMode", m_set_mode, LUNA_METHOD_FLAGS_NONE },
    { "record", m_record, LUNA_METHOD_FLAGS_NONE },
    { "preview", m_preview, LUNA_METHOD_FLAGS_NONE },
    { "sendKey", m_send_key, LUNA_METHOD_FLAGS_NONE },
    { 0, 0, 0 },
};

// ---------------------------------------------------------------------------
// Subscriptions: power state and foreground app
// ---------------------------------------------------------------------------

static bool on_power(LSHandle* sh __attribute__((unused)), LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    char state[64] = { 0 };
    if (get_str(parsed, "state", state, sizeof(state))) {
        INFO("power state: %s", state);
        pthread_mutex_lock(&svc->lock);
        svc->power_paused = strcmp(state, "Active") != 0;
        pthread_mutex_unlock(&svc->lock);
        update_capture(svc);
    }
    j_release(&parsed);
    return true;
}

static bool app_matches(const char* list, const char* app)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", list);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        if (*tok && strstr(app, tok))
            return true;
    }
    return false;
}

static bool on_foreground(LSHandle* sh __attribute__((unused)), LSMessage* msg, void* data)
{
    service_t* svc = data;
    jvalue_ref parsed = parse_payload(msg);
    char app[128] = { 0 };
    if (get_str(parsed, "appId", app, sizeof(app))) {
        pthread_mutex_lock(&svc->lock);
        snprintf(svc->fg_app, sizeof(svc->fg_app), "%s", app);
        bool ok = app_matches(svc->settings.foreground_apps, app);
        ap_set_foreground(&svc->ap, ok);
        pthread_mutex_unlock(&svc->lock);
        INFO("foreground app: %s (%s)", app, ok ? "watching" : "ignored");
    }
    j_release(&parsed);
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int service_init(service_t* svc, GMainLoop* loop)
{
    memset(svc, 0, sizeof(*svc));
    svc->loop = loop;
    svc->vremote_fd = -1;
    pthread_mutex_init(&svc->lock, NULL);
    pthread_mutex_init(&svc->kq_lock, NULL);
    pthread_cond_init(&svc->kq_cond, NULL);

    settings_init(&svc->settings);
    if (settings_load_file(&svc->settings, SETTINGS_PATH) != 0)
        INFO("no saved settings, using defaults");

    ap_params_t app;
    ap_params_from_settings(&svc->settings, &app);
    ap_init(&svc->ap, &app);
    svc->ap.mode = (ap_mode_t)svc->settings.mode;
    if (rebuild_detector(svc) != 0)
        return -1;

    mkdir(DATA_DIR, 0755);
    ssap_init(&svc->ssap, DATA_DIR "/ssap-key");
    open_vremote(svc);
    svc->key_thread_running = true;
    pthread_create(&svc->key_thread, NULL, key_thread, svc);

    remote_listen_start(&svc->listener, on_remote_key, svc);

    svc->http.status_json = status_json_cb;
    svc->http.frame_bmp = frame_bmp_cb;
    svc->http.data = svc;
    debug_http_start(&svc->http, svc->settings.debug_http_port);

    svc->espn.lock = &svc->lock;
    svc->espn.games = svc->games;
    svc->espn.settings = &svc->settings;
    espn_start(&svc->espn);

    update_autostart(&svc->settings);
    g_timeout_add(500, housekeeping, svc);
    g_timeout_add(1000, gamedata_tick, svc);
    return 0;
}

static void subscribe(LSHandle* h, const char* uri, const char* payload, LSFilterFunc cb, void* data)
{
    LSError err;
    LSErrorInit(&err);
    if (!LSCall(h, uri, payload, cb, data, NULL, &err)) {
        WARN("%s failed: %s", uri, err.message);
        LSErrorFree(&err);
    }
}

int service_register(service_t* svc)
{
    LSHandle* handle = NULL;
    LSHandle* legacy = NULL;
    LSError err;
    LSErrorInit(&err);

    bool registered = LSRegister(SERVICE_NAME, &handle, &err);
    bool registered_legacy = false;
    if (&LSRegisterPubPriv != NULL)
        registered_legacy = LSRegisterPubPriv(SERVICE_NAME, &legacy, true, &err);
    if (!registered && !registered_legacy) {
        ERR("unable to register on luna bus: %s", err.message);
        LSErrorFree(&err);
        return -1;
    }

    LSHandle* handles[2] = { registered ? handle : NULL, registered_legacy ? legacy : NULL };
    for (int i = 0; i < 2; i++) {
        LSHandle* h = handles[i];
        if (!h)
            continue;
        LSRegisterCategory(h, "/", methods, NULL, NULL, &err);
        LSCategorySetData(h, "/", svc, &err);
        LSGmainAttach(h, svc->loop, &err);
    }

    LSHandle* main_handle = registered ? handle : legacy;
    svc->handle = main_handle;
    luna_util_set_handle(main_handle);

    subscribe(main_handle, "luna://com.webos.service.tvpower/power/getPowerState", "{\"subscribe\":true}", on_power, svc);
    subscribe(main_handle, "luna://com.webos.applicationManager/getForegroundAppInfo", "{\"subscribe\":true}", on_foreground, svc);

    LSErrorFree(&err);
    return 0;
}

void service_destroy(service_t* svc)
{
    espn_stop(&svc->espn);
    remote_listen_stop(&svc->listener);
    debug_http_stop(&svc->http);
    stop_capture(svc);

    pthread_mutex_lock(&svc->kq_lock);
    svc->key_thread_running = false;
    pthread_cond_signal(&svc->kq_cond);
    pthread_mutex_unlock(&svc->kq_lock);
    pthread_join(svc->key_thread, NULL);
    vremote_close(svc->vremote_fd);

    recorder_stop(&svc->rec);
    detector_free(&svc->det);
    free(svc->last_frame);
}
