#pragma once
// Presses remote buttons through the TV's own remote-control server
// ("second screen" SSAP websocket on 127.0.0.1:3000), exactly like LG's phone
// app and Home Assistant do. Unlike a uinput device, YouTube TV accepts these.
//
// The first connection shows an "Allow" prompt on the TV; the resulting
// client key is saved and reused afterwards.
#include <stdbool.h>

typedef struct {
    int main_fd;
    int ptr_fd;
    char key_path[160];
    char client_key[80];
} ssap_t;

void ssap_init(ssap_t* s, const char* key_path);
// Sends one button press ("LEFT", "RIGHT", "UP", "DOWN", "ENTER", "BACK", ...).
// Connects / registers on demand. Returns 0 on success.
int ssap_button(ssap_t* s, const char* name);
bool ssap_has_key(const ssap_t* s);
void ssap_close(ssap_t* s);
