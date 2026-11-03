#pragma once
#include <stdbool.h>

#define APP_ID "org.sportsautopilot.app"
#define SERVICE_NAME "org.sportsautopilot.app.service"
#define DATA_DIR "/media/developer/sports-autopilot"
#define SETTINGS_PATH DATA_DIR "/config.json"

// All user-tunable settings. Persisted as JSON in SETTINGS_PATH and exposed
// through the getSettings / setSettings luna methods.
typedef struct {
    // Service
    bool autostart; // start the service at boot (autopilot itself stays off)
    int debug_http_port; // 0 = disabled

    // Capture
    char video_backend[32]; // "auto", "libvtcapture", "libdile_vt"
    int quirks;
    int capture_width;
    int capture_height;
    int fps;

    // Which foreground app ids count as YouTube TV (comma-separated substrings).
    char foreground_apps[128];

    // Commercial detection: "gamedata" (ESPN play-by-play, works with DRM) or
    // "video" (screen capture; blocked by YouTube TV's DRM on webOS 4.x).
    char detection[16];
    // ESPN event ids of the games in Multiview tiles 1..4 (comma-separated,
    // empty entries allowed) and their display labels.
    char tile_events[96];
    char tile_labels[160];
    int stream_delay_s; // how far YouTube TV runs behind live
    int idle_break_s;
    int trigger_idle_s;
    int resume_lead_s;
    int poll_interval_s;

    // Autopilot
    int mode; // 0 = multiview, 1 = fullscreen
    int grid_cols;
    int grid_rows;
    int min_switch_interval_ms;
    int manual_pause_ms;
    int scan_settle_ms;
    bool reset_focus_on_start;

    // Multiview geometry (fractions of the frame).
    double mv_inset_left;
    double mv_inset_top;
    double mv_inset_right;
    double mv_inset_bottom;
    double mv_gutter;

    // Detector
    double live_score_on;
    double live_score_off;
    int live_hold_ms;
    int ad_hold_ms;
    int warmup_ms;
    double band_top;
    double band_bottom;
    double edge_thresh;
    double act_thresh;
    double persist_tau_s;
    double persist_on;

    // Remote: Linux input key codes.
    int key_enable; // default KEY_GREEN
    int key_disable; // default KEY_RED
    int key_mode; // default KEY_YELLOW
    int key_status; // default KEY_BLUE
    int key_left;
    int key_right;
    int key_up;
    int key_down;
    int key_enter;
    int key_back;
    int key_delay_ms; // pause between injected key presses
    bool use_uinput; // false = press keys via the TV's SSAP server (works with YouTube TV)

    // Recording (for tuning)
    char record_dir[128];
    int record_interval_ms;
    int record_max_frames;
} settings_t;

void settings_init(settings_t* s);


// Returns 0 on success. Unknown keys are ignored.
int settings_load_json(settings_t* s, void* jobj /* jvalue_ref */);
int settings_save_json(const settings_t* s, void* jobj /* jvalue_ref */);
int settings_load_file(settings_t* s, const char* path);
int settings_save_file(const settings_t* s, const char* path);
