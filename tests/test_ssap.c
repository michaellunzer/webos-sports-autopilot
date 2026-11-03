// Drives src/tv/ssap.c against tests/fake_ssap.py (see test_ssap.sh).
#include "ssap.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    ssap_t s;
    ssap_init(&s, argv[1]);
    printf("key before: '%s'\n", s.client_key);
    const char* keys[] = { "RIGHT", "DOWN", "ENTER", "BACK" };
    for (int i = 0; i < 4; i++) {
        if (ssap_button(&s, keys[i]) != 0) {
            printf("button %s failed\n", keys[i]);
            return 1;
        }
    }
    printf("key after: '%s'\n", s.client_key);
    ssap_close(&s);
    return strcmp(s.client_key, "fakekey123") == 0 ? 0 : 1;
}
