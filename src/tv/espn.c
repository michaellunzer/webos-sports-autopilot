#include "espn.h"
#include "log.h"

#include <pbnjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SIZE 25
#define PLAYS_URL "https://sports.core.api.espn.com/v2/sports/football/leagues/nfl/events/%s/competitions/%s/plays?limit=%d&page=%d"

int64_t realtime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void csv_item(const char* list, int idx, char* out, size_t cap)
{
    out[0] = 0;
    const char* p = list;
    for (int i = 0; i < idx && p; i++) {
        p = strchr(p, ',');
        if (p)
            p++;
    }
    if (!p)
        return;
    const char* e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    while (n && *p == ' ') {
        p++;
        n--;
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

// Runs curl and returns the body (caller frees), or NULL.
static char* http_get(const char* url)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -s -f --max-time 8 '%s' 2>/dev/null", url);
    FILE* f = popen(cmd, "r");
    if (!f)
        return NULL;
    size_t cap = 65536, len = 0;
    char* buf = malloc(cap);
    while (buf) {
        if (len + 4096 > cap) {
            char* nb = realloc(buf, cap *= 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        if (n == 0)
            break;
        len += n;
    }
    int status = pclose(f);
    if (!buf || status != 0 || len == 0) {
        free(buf);
        return NULL;
    }
    buf[len] = 0;
    return buf;
}

static int64_t parse_iso8601_ms(const char* s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 5)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (int64_t)timegm(&tm) * 1000;
}

static bool get_string(jvalue_ref obj, const char* key, char* out, size_t cap)
{
    jvalue_ref v = jobject_get(obj, j_cstr_to_buffer(key));
    if (!jis_valid(v) || !jis_string(v))
        return false;
    raw_buffer b = jstring_get(v);
    snprintf(out, cap, "%s", b.m_str ? b.m_str : "");
    jstring_free_buffer(b);
    return true;
}

typedef struct {
    char id[24];
    char type[40];
    int64_t t;
} fetched_play_t;

// Fetches one page; fills plays (up to max) and the total play count.
static int fetch_page(const char* event, int page, int limit, fetched_play_t* plays, int max, int* nplays, int* count)
{
    char url[256];
    snprintf(url, sizeof(url), PLAYS_URL, event, event, limit, page);
    char* body = http_get(url);
    if (!body)
        return -1;
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jvalue_ref root = jdom_parse(j_cstr_to_buffer(body), DOMOPT_NOOPT, &schema);
    free(body);
    if (jis_null(root) || !jis_object(root)) {
        j_release(&root);
        return -2;
    }
    jvalue_ref c = jobject_get(root, j_cstr_to_buffer("count"));
    if (jis_valid(c) && jis_number(c))
        jnumber_get_i32(c, count);
    jvalue_ref items = jobject_get(root, j_cstr_to_buffer("items"));
    int n = 0;
    if (jis_valid(items) && jis_array(items)) {
        ssize_t len = jarray_size(items);
        for (ssize_t i = 0; i < len && n < max; i++) {
            jvalue_ref it = jarray_get(items, i);
            char wall[40] = { 0 };
            fetched_play_t* p = &plays[n];
            if (!get_string(it, "id", p->id, sizeof(p->id)) || !get_string(it, "wallclock", wall, sizeof(wall)))
                continue;
            jvalue_ref type = jobject_get(it, j_cstr_to_buffer("type"));
            if (!jis_valid(type) || !get_string(type, "text", p->type, sizeof(p->type)))
                continue;
            p->t = parse_iso8601_ms(wall);
            if (p->t > 0)
                n++;
        }
    }
    *nplays = n;
    j_release(&root);
    return 0;
}

static int poll_game(espn_poller_t* e, int idx, const char* event)
{
    fetched_play_t plays[2 * PAGE_SIZE];
    int n = 0, count = e->counts[idx];
    if (count <= 0) {
        int dummy;
        if (fetch_page(event, 1, 1, plays, 1, &dummy, &count) != 0)
            return -1;
        if (count <= 0)
            return 0; // game hasn't started
    }
    int page = (count + PAGE_SIZE - 1) / PAGE_SIZE;
    int got = 0;
    int newcount = count;
    if (fetch_page(event, page, PAGE_SIZE, plays, PAGE_SIZE, &got, &newcount) != 0)
        return -1;
    n = got;
    // Page boundary: make sure we also see the last few plays of the previous page.
    if (got < 8 && page > 1) {
        int got2 = 0, c2 = newcount;
        if (fetch_page(event, page - 1, PAGE_SIZE, plays + n, PAGE_SIZE, &got2, &c2) == 0)
            n += got2;
    }
    // If the count grew past this page, the next poll fetches the new last page.

    pthread_mutex_lock(e->lock);
    pred_game_t* g = &e->games[idx];
    if (strcmp(g->event_id, event) == 0) {
        for (int i = 0; i < n; i++)
            pred_game_add_play(g, plays[i].id, plays[i].type, plays[i].t);
        g->last_fetch_ms = realtime_ms();
        e->counts[idx] = newcount;
    }
    pthread_mutex_unlock(e->lock);
    return 0;
}

static void* poll_thread(void* arg)
{
    espn_poller_t* e = arg;
    while (e->running) {
        char events[ESPN_MAX_GAMES][16];
        int interval;
        pthread_mutex_lock(e->lock);
        interval = e->settings->poll_interval_s;
        for (int i = 0; i < ESPN_MAX_GAMES; i++) {
            char label[sizeof(e->games[0].label)];
            csv_item(e->settings->tile_events, i, events[i], sizeof(events[i]));
            csv_item(e->settings->tile_labels, i, label, sizeof(label));
            if (strcmp(e->games[i].event_id, events[i]) != 0) {
                pred_game_init(&e->games[i], events[i], label);
                e->counts[i] = 0;
            } else if (strcmp(e->games[i].label, label) != 0) {
                snprintf(e->games[i].label, sizeof(e->games[i].label), "%s", label);
            }
        }
        pthread_mutex_unlock(e->lock);

        for (int i = 0; i < ESPN_MAX_GAMES && e->running; i++) {
            if (!events[i][0])
                continue;
            bool dup = false; // same game in two tiles: fetched once below
            for (int k = 0; k < i; k++)
                dup |= strcmp(events[k], events[i]) == 0;
            if (dup) {
                pthread_mutex_lock(e->lock);
                for (int k = 0; k < i; k++) {
                    if (strcmp(events[k], events[i]) == 0) {
                        e->games[i] = e->games[k];
                        break;
                    }
                }
                pthread_mutex_unlock(e->lock);
                continue;
            }
            if (poll_game(e, i, events[i]) == 0) {
                e->last_ok_ms = realtime_ms();
                e->last_error[0] = 0;
            } else {
                snprintf(e->last_error, sizeof(e->last_error), "fetch failed for event %s", events[i]);
                WARN("%s", e->last_error);
            }
        }
        for (int t = 0; t < interval * 10 && e->running; t++)
            usleep(100 * 1000);
    }
    return NULL;
}

int espn_start(espn_poller_t* e)
{
    if (e->running)
        return 0;
    e->running = true;
    if (pthread_create(&e->thread, NULL, poll_thread, e) != 0) {
        e->running = false;
        return -1;
    }
    return 0;
}

void espn_stop(espn_poller_t* e)
{
    if (!e->running)
        return;
    e->running = false;
    pthread_join(e->thread, NULL);
}
