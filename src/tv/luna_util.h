#pragma once
#include <luna-service2/lunaservice.h>
#include <stdbool.h>

// Remember the service handle used for outgoing calls.
void luna_util_set_handle(LSHandle* handle);
// Fire-and-forget luna call. Safe to call from any thread: the call is
// marshalled onto the GLib main loop.
void luna_post(const char* uri, const char* payload);
// Show a toast notification on the TV. Safe from any thread.
void luna_toast(const char* message);
