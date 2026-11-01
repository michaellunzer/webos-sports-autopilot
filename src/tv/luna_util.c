#include "luna_util.h"
#include "log.h"
#include "settings.h"

#include <glib.h>
#include <pbnjson.h>
#include <stdlib.h>
#include <string.h>

static LSHandle* g_handle = NULL;

typedef struct {
    char* uri;
    char* payload;
} pending_call_t;

void luna_util_set_handle(LSHandle* handle)
{
    g_handle = handle;
}

static gboolean do_call(gpointer data)
{
    pending_call_t* c = data;
    if (g_handle) {
        LSError err;
        LSErrorInit(&err);
        if (!LSCallOneReply(g_handle, c->uri, c->payload, NULL, NULL, NULL, &err)) {
            WARN("luna call %s failed: %s", c->uri, err.message);
            LSErrorFree(&err);
        }
    }
    free(c->uri);
    free(c->payload);
    free(c);
    return G_SOURCE_REMOVE;
}

void luna_post(const char* uri, const char* payload)
{
    pending_call_t* c = calloc(1, sizeof(*c));
    if (!c)
        return;
    c->uri = strdup(uri);
    c->payload = strdup(payload);
    g_idle_add(do_call, c);
}

void luna_toast(const char* message)
{
    INFO("toast: %s", message);
    jvalue_ref obj = jobject_create();
    jobject_set(obj, j_cstr_to_buffer("sourceId"), jstring_create(APP_ID));
    jobject_set(obj, j_cstr_to_buffer("message"), jstring_create(message));
    luna_post("luna://com.webos.notification/createToast", jvalue_tostring_simple(obj));
    j_release(&obj);
}
