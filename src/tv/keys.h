#pragma once
// Virtual remote: injects key presses through /dev/uinput (needs root).
#include <stdbool.h>

#define VREMOTE_NAME "sports-autopilot virtual remote"

// Creates the virtual input device advertising `codes`. Returns fd or -errno.
int vremote_open(const int* codes, int ncodes);
// Press + release `code`. Returns 0 on success.
int vremote_press(int fd, int code);
void vremote_close(int fd);
