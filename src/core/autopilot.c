#include "autopilot.h"

#include <stdio.h>
#include <string.h>

const char* ap_key_name(ap_key_t k)
{
    static const char* names[AP_KEY_COUNT] = { "LEFT", "RIGHT", "UP", "DOWN", "ENTER", "BACK" };
    return (k >= 0 && k < AP_KEY_COUNT) ? names[k] : "?";
}

const char* ap_mode_name(ap_mode_t m)
{
    return m == AP_MODE_FULLSCREEN ? "fullscreen" : "multiview";
}

const char* ap_phase_name(ap_phase_t p)
{
    switch (p) {
    case AP_PHASE_MV_WATCH:
        return "multiview";
    case AP_PHASE_MV_SCAN:
        return "scanning";
    case AP_PHASE_FS_WATCH:
        return "fullscreen";
    case AP_PHASE_PAUSED:
        return "paused";
    default:
        return "off";
    }
}

void ap_default_params(ap_params_t* p)
{
    p->grid_cols = 2;
    p->grid_rows = 2;
    p->min_switch_interval_ms = 20000;
    p->manual_pause_ms = 120000;
    p->scan_settle_ms = 6000;
    p->reset_focus_on_start = true;
}

void ap_init(autopilot_t* ap, const ap_params_t* p)
{
    memset(ap, 0, sizeof(*ap));
    ap->p = *p;
    ap->phase = AP_PHASE_OFF;
    ap->focus = -1;
    ap->fs_tile = -1;
    ap->foreground_ok = true;
}

void ap_action_clear(ap_action_t* out)
{
    memset(out, 0, sizeof(*out));
    out->set_layout_tiles = -1;
}

static int grid_tiles(const autopilot_t* ap)
{
    return ap->p.grid_cols * ap->p.grid_rows;
}

int ap_expected_tiles(const autopilot_t* ap)
{
    return ap->phase == AP_PHASE_FS_WATCH ? 1 : grid_tiles(ap);
}

static void push_key(ap_action_t* out, ap_key_t k)
{
    if (out->nkeys < AP_MAX_KEYS)
        out->keys[out->nkeys++] = k;
}

static void set_phase(autopilot_t* ap, ap_phase_t phase, uint64_t now_ms, ap_action_t* out)
{
    int before = ap_expected_tiles(ap);
    ap->phase = phase;
    ap->phase_since_ms = now_ms;
    int after = ap_expected_tiles(ap);
    if (before != after || phase == AP_PHASE_MV_SCAN)
        out->set_layout_tiles = after;
}

// Put focus into the top-left tile by pressing UP/LEFT enough times.
static void reset_focus(autopilot_t* ap, ap_action_t* out)
{
    for (int i = 0; i < ap->p.grid_rows - 1; i++)
        push_key(out, AP_KEY_UP);
    for (int i = 0; i < ap->p.grid_cols - 1; i++)
        push_key(out, AP_KEY_LEFT);
    ap->focus = 0;
}

static void move_focus(autopilot_t* ap, int to, ap_action_t* out)
{
    if (ap->focus < 0)
        reset_focus(ap, out);
    int cols = ap->p.grid_cols;
    int dc = (to % cols) - (ap->focus % cols);
    int dr = (to / cols) - (ap->focus / cols);
    for (; dc > 0; dc--)
        push_key(out, AP_KEY_RIGHT);
    for (; dc < 0; dc++)
        push_key(out, AP_KEY_LEFT);
    for (; dr > 0; dr--)
        push_key(out, AP_KEY_DOWN);
    for (; dr < 0; dr++)
        push_key(out, AP_KEY_UP);
    ap->focus = to;
}

static int distance(const autopilot_t* ap, int a, int b)
{
    if (a < 0 || b < 0)
        return 0;
    int cols = ap->p.grid_cols;
    int dc = (a % cols) - (b % cols);
    int dr = (a / cols) - (b / cols);
    return (dc < 0 ? -dc : dc) + (dr < 0 ? -dr : dr);
}

// Pick the best LIVE tile. Prefers the game that most recently came back
// from a break (it's the least likely to cut away again soon), then the
// closest one to the current focus.
static int choose_live(const autopilot_t* ap, const ap_tile_t* tiles, int ntiles, int exclude)
{
    int best = -1;
    for (int i = 0; i < ntiles; i++) {
        if (i == exclude || tiles[i].state != TILE_LIVE)
            continue;
        if (best < 0 || tiles[i].state_since_ms > tiles[best].state_since_ms
            || (tiles[i].state_since_ms == tiles[best].state_since_ms
                && distance(ap, ap->focus, i) < distance(ap, ap->focus, best))) {
            best = i;
        }
    }
    return best;
}

void ap_set_enabled(autopilot_t* ap, bool enabled, uint64_t now_ms, ap_action_t* out)
{
    if (enabled == ap->enabled)
        return;
    ap->enabled = enabled;
    if (!enabled) {
        set_phase(ap, AP_PHASE_OFF, now_ms, out);
        out->reason = "disabled";
        return;
    }

    // We assume Multiview is on screen when the autopilot is turned on.
    ap->focus = -1;
    ap->fs_tile = -1;
    ap->last_switch_ms = 0;
    if (ap->p.reset_focus_on_start)
        reset_focus(ap, out);
    else
        ap->focus = 0;
    set_phase(ap, ap->mode == AP_MODE_FULLSCREEN ? AP_PHASE_MV_SCAN : AP_PHASE_MV_WATCH, now_ms, out);
    out->set_layout_tiles = grid_tiles(ap);
    out->reason = "enabled";
}

void ap_set_mode(autopilot_t* ap, ap_mode_t mode, uint64_t now_ms, ap_action_t* out)
{
    if (mode == ap->mode)
        return;
    ap->mode = mode;
    if (!ap->enabled)
        return;

    if (mode == AP_MODE_FULLSCREEN && ap->phase == AP_PHASE_MV_WATCH) {
        // Tiles are already warm: choose right away.
        set_phase(ap, AP_PHASE_MV_SCAN, now_ms, out);
        out->set_layout_tiles = -1; // keep the warm detector state
        ap->phase_since_ms = now_ms - (uint64_t)ap->p.scan_settle_ms;
        ap->fs_tile = -1;
    } else if (mode == AP_MODE_MULTIVIEW && ap->phase == AP_PHASE_FS_WATCH) {
        push_key(out, AP_KEY_BACK);
        ap->focus = ap->fs_tile;
        ap->fs_tile = -1;
        set_phase(ap, AP_PHASE_MV_WATCH, now_ms, out);
    } else if (mode == AP_MODE_MULTIVIEW && ap->phase == AP_PHASE_MV_SCAN) {
        set_phase(ap, AP_PHASE_MV_WATCH, now_ms, out);
        out->set_layout_tiles = -1;
    }
    out->reason = "mode";
}

void ap_set_foreground(autopilot_t* ap, bool ok)
{
    ap->foreground_ok = ok;
}

void ap_manual_input(autopilot_t* ap, uint64_t now_ms)
{
    if (!ap->enabled)
        return;
    if (ap->phase != AP_PHASE_PAUSED) {
        ap->resume_phase = ap->phase;
        ap->phase = AP_PHASE_PAUSED;
        ap->phase_since_ms = now_ms;
    }
    ap->paused_until_ms = now_ms + (uint64_t)ap->p.manual_pause_ms;
    ap->focus = -1; // the user may have moved it
}

static void resume(autopilot_t* ap, uint64_t now_ms, ap_action_t* out)
{
    // We don't know what the user did. Make a best guess and re-sync focus.
    ap->phase = ap->resume_phase; // so set_phase computes the layout delta
    if (ap->mode == AP_MODE_FULLSCREEN && ap->resume_phase == AP_PHASE_FS_WATCH) {
        // Probably still watching a game full screen; which one is unknown.
        ap->fs_tile = -1;
        set_phase(ap, AP_PHASE_FS_WATCH, now_ms, out);
        out->set_layout_tiles = 1;
    } else {
        reset_focus(ap, out);
        set_phase(ap, ap->mode == AP_MODE_FULLSCREEN ? AP_PHASE_MV_SCAN : AP_PHASE_MV_WATCH, now_ms, out);
        out->set_layout_tiles = grid_tiles(ap);
    }
    out->reason = "resumed";
}

void ap_tick(autopilot_t* ap, const ap_tile_t* tiles, int ntiles, uint64_t now_ms, ap_action_t* out)
{
    if (!ap->enabled || ap->phase == AP_PHASE_OFF)
        return;

    if (ap->phase == AP_PHASE_PAUSED) {
        if (now_ms >= ap->paused_until_ms)
            resume(ap, now_ms, out);
        return;
    }

    if (!ap->foreground_ok || ntiles != ap_expected_tiles(ap))
        return;

    bool cooled_down = ap->last_switch_ms == 0 || now_ms - ap->last_switch_ms >= (uint64_t)ap->p.min_switch_interval_ms;

    switch (ap->phase) {
    case AP_PHASE_MV_WATCH: {
        if (ap->focus < 0 || ap->focus >= ntiles) {
            reset_focus(ap, out);
            return;
        }
        if (tiles[ap->focus].state != TILE_AD || !cooled_down)
            return;
        int target = choose_live(ap, tiles, ntiles, ap->focus);
        if (target < 0)
            return; // everything is at commercial; stay put
        int from = ap->focus;
        move_focus(ap, target, out);
        ap->last_switch_ms = now_ms;
        ap->switches++;
        out->reason = "switch";
        snprintf(out->message, sizeof(out->message), "Commercial on game %d -> game %d", from + 1, target + 1);
        return;
    }

    case AP_PHASE_MV_SCAN: {
        if (now_ms - ap->phase_since_ms < (uint64_t)ap->p.scan_settle_ms)
            return;
        int left = ap->fs_tile;
        int target = choose_live(ap, tiles, ntiles, left);
        if (target < 0 && left >= 0 && left < ntiles && tiles[left].state == TILE_LIVE)
            target = left; // the game we left came back first
        if (target < 0)
            return; // keep waiting in Multiview
        move_focus(ap, target, out);
        push_key(out, AP_KEY_ENTER);
        ap->fs_tile = target;
        ap->last_switch_ms = now_ms;
        ap->switches++;
        set_phase(ap, AP_PHASE_FS_WATCH, now_ms, out);
        out->reason = "open";
        snprintf(out->message, sizeof(out->message), "Full screen: game %d", target + 1);
        return;
    }

    case AP_PHASE_FS_WATCH: {
        if (tiles[0].state != TILE_AD || !cooled_down)
            return;
        push_key(out, AP_KEY_BACK);
        ap->focus = ap->fs_tile; // Multiview normally keeps focus on the game we left
        set_phase(ap, AP_PHASE_MV_SCAN, now_ms, out);
        ap->last_switch_ms = now_ms;
        out->reason = "exit";
        if (ap->fs_tile >= 0)
            snprintf(out->message, sizeof(out->message), "Commercial on game %d, finding another", ap->fs_tile + 1);
        else
            snprintf(out->message, sizeof(out->message), "Commercial, finding another game");
        return;
    }

    default:
        return;
    }
}
