#include "settings.h"

#include <errno.h>
#include <linux/input.h>
#include <pbnjson.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef KEY_RED
#define KEY_RED 0x18e
#define KEY_GREEN 0x18f
#define KEY_YELLOW 0x190
#define KEY_BLUE 0x191
#endif

void settings_init(settings_t* s)
{
    memset(s, 0, sizeof(*s));
    s->autostart = false; // opt-in: nothing of ours runs at boot until enabled
    s->debug_http_port = 8088;

    strcpy(s->video_backend, "auto");
    s->quirks = 0;
    s->capture_width = 320;
    s->capture_height = 180;
    s->fps = 3;

    strcpy(s->foreground_apps, "ytv");

    strcpy(s->detection, "gamedata");
    s->tile_events[0] = 0;
    s->tile_labels[0] = 0;
    s->stream_delay_s = 45;
    s->idle_break_s = 75;
    s->trigger_idle_s = 40;
    s->resume_lead_s = 20;
    s->poll_interval_s = 5;

    s->mode = 0;
    s->grid_cols = 2;
    s->grid_rows = 2;
    s->min_switch_interval_ms = 20000;
    s->manual_pause_ms = 120000;
    s->scan_settle_ms = 6000;
    s->reset_focus_on_start = true;

    s->mv_inset_left = 0.0;
    s->mv_inset_top = 0.0;
    s->mv_inset_right = 0.0;
    s->mv_inset_bottom = 0.0;
    s->mv_gutter = 0.0;

    s->live_score_on = 0.025;
    s->live_score_off = 0.010;
    s->live_hold_ms = 2000;
    s->ad_hold_ms = 4000;
    s->warmup_ms = 3000;
    s->band_top = 0.22;
    s->band_bottom = 0.25;
    s->edge_thresh = 24.0;
    s->act_thresh = 3.0;
    s->persist_tau_s = 2.0;
    s->persist_on = 0.6;

    s->key_enable = KEY_GREEN;
    s->key_disable = KEY_RED;
    s->key_mode = KEY_YELLOW;
    s->key_status = KEY_BLUE;
    s->key_left = KEY_LEFT;
    s->key_right = KEY_RIGHT;
    s->key_up = KEY_UP;
    s->key_down = KEY_DOWN;
    s->key_enter = KEY_ENTER;
    s->key_back = 412; // LG Magic Remote "Back" (KEY_PREVIOUS)
    s->key_delay_ms = 400;
    s->use_uinput = false;

    snprintf(s->record_dir, sizeof(s->record_dir), "%s/rec", DATA_DIR);
    s->record_interval_ms = 1000;
    s->record_max_frames = 1800;
}

typedef enum { T_BOOL, T_INT, T_DBL, T_STR } ftype_t;
typedef struct {
    const char* name;
    ftype_t type;
    size_t off;
    size_t size;
} field_t;

#define FB(n) { #n, T_BOOL, offsetof(settings_t, n), 0 }
#define FI(n) { #n, T_INT, offsetof(settings_t, n), 0 }
#define FD(n) { #n, T_DBL, offsetof(settings_t, n), 0 }
#define FS(n) { #n, T_STR, offsetof(settings_t, n), sizeof(((settings_t*)0)->n) }

static const field_t fields[] = {
    FB(autostart), FI(debug_http_port),
    FS(video_backend), FI(quirks), FI(capture_width), FI(capture_height), FI(fps),
    FS(foreground_apps),
    FS(detection), FS(tile_events), FS(tile_labels), FI(stream_delay_s), FI(idle_break_s),
    FI(trigger_idle_s), FI(resume_lead_s), FI(poll_interval_s),
    FI(mode), FI(grid_cols), FI(grid_rows), FI(min_switch_interval_ms), FI(manual_pause_ms),
    FI(scan_settle_ms), FB(reset_focus_on_start),
    FD(mv_inset_left), FD(mv_inset_top), FD(mv_inset_right), FD(mv_inset_bottom), FD(mv_gutter),
    FD(live_score_on), FD(live_score_off), FI(live_hold_ms), FI(ad_hold_ms), FI(warmup_ms),
    FD(band_top), FD(band_bottom), FD(edge_thresh), FD(act_thresh), FD(persist_tau_s), FD(persist_on),
    FI(key_enable), FI(key_disable), FI(key_mode), FI(key_status),
    FI(key_left), FI(key_right), FI(key_up), FI(key_down), FI(key_enter), FI(key_back),
    FI(key_delay_ms), FB(use_uinput),
    FS(record_dir), FI(record_interval_ms), FI(record_max_frames),
};
#define NFIELDS (sizeof(fields) / sizeof(fields[0]))

int settings_load_json(settings_t* s, void* obj)
{
    jvalue_ref source = (jvalue_ref)obj;
    if (!jis_object(source))
        return -1;
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t* f = &fields[i];
        jvalue_ref v = jobject_get(source, j_cstr_to_buffer(f->name));
        if (!jis_valid(v) || jis_null(v))
            continue;
        char* p = (char*)s + f->off;
        switch (f->type) {
        case T_BOOL:
            if (jis_boolean(v))
                jboolean_get(v, (bool*)p);
            break;
        case T_INT:
            if (jis_number(v))
                jnumber_get_i32(v, (int32_t*)p);
            break;
        case T_DBL:
            if (jis_number(v))
                jnumber_get_f64(v, (double*)p);
            break;
        case T_STR:
            if (jis_string(v)) {
                raw_buffer str = jstring_get(v);
                snprintf(p, f->size, "%s", str.m_str ? str.m_str : "");
                jstring_free_buffer(str);
            }
            break;
        }
    }
    // Sanity limits.
    if (s->fps < 1)
        s->fps = 1;
    if (s->fps > 30)
        s->fps = 30;
    if (s->grid_cols < 1 || s->grid_cols > 2)
        s->grid_cols = 2;
    if (s->grid_rows < 1 || s->grid_rows > 2)
        s->grid_rows = 2;
    if (s->mode != 0 && s->mode != 1)
        s->mode = 0;
    if (s->poll_interval_s < 2)
        s->poll_interval_s = 2;
    if (s->stream_delay_s < 0)
        s->stream_delay_s = 0;
    return 0;
}

int settings_save_json(const settings_t* s, void* obj)
{
    jvalue_ref target = (jvalue_ref)obj;
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t* f = &fields[i];
        const char* p = (const char*)s + f->off;
        jvalue_ref v;
        switch (f->type) {
        case T_BOOL:
            v = jboolean_create(*(const bool*)p);
            break;
        case T_INT:
            v = jnumber_create_i32(*(const int*)p);
            break;
        case T_DBL:
            v = jnumber_create_f64(*(const double*)p);
            break;
        default:
            v = jstring_create(p);
            break;
        }
        jobject_set(target, j_cstr_to_buffer(f->name), v);
    }
    return 0;
}

int settings_load_file(settings_t* s, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1 << 20) {
        fclose(f);
        return -2;
    }
    char* json = calloc(size + 1, 1);
    size_t got = json ? fread(json, 1, size, f) : 0;
    fclose(f);
    if (got != (size_t)size) {
        free(json);
        return -2;
    }

    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jvalue_ref parsed = jdom_parse(j_cstr_to_buffer(json), DOMOPT_NOOPT, &schema);
    free(json);
    if (jis_null(parsed)) {
        j_release(&parsed);
        return -3;
    }
    int ret = settings_load_json(s, parsed);
    j_release(&parsed);
    return ret;
}

int settings_save_file(const settings_t* s, const char* path)
{
    mkdir(DATA_DIR, 0755);
    jvalue_ref obj = jobject_create();
    settings_save_json(s, obj);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        j_release(&obj);
        return -1;
    }
    fputs(jvalue_tostring_simple(obj), f);
    int ok = fclose(f) == 0;
    j_release(&obj);
    if (!ok || rename(tmp, path) != 0)
        return -1;
    return 0;
}
