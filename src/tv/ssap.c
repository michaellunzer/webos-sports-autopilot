#include "ssap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef SSAP_HOST
#define SSAP_HOST "127.0.0.1"
#endif
#ifndef SSAP_PORT
#define SSAP_PORT 3000
#endif

#define REGISTER_TIMEOUT_MS 60000 // time to answer the "Allow" prompt
#define REPLY_TIMEOUT_MS 5000

static const char* const PERMISSIONS =
    "\"APP_TO_APP\",\"CLOSE\",\"CONTROL_AUDIO\",\"CONTROL_DISPLAY\",\"CONTROL_INPUT_JOYSTICK\","
    "\"CONTROL_INPUT_MEDIA_PLAYBACK\",\"CONTROL_INPUT_MEDIA_RECORDING\",\"CONTROL_INPUT_TEXT\","
    "\"CONTROL_INPUT_TV\",\"CONTROL_MOUSE_AND_KEYBOARD\",\"CONTROL_POWER\",\"CONTROL_TV_SCREEN\","
    "\"LAUNCH\",\"LAUNCH_WEBAPP\",\"READ_APP_STATUS\",\"READ_COUNTRY_INFO\",\"READ_CURRENT_CHANNEL\","
    "\"READ_INPUT_DEVICE_LIST\",\"READ_INSTALLED_APPS\",\"READ_LGE_SDX\",\"READ_LGE_TV_INPUT_EVENTS\","
    "\"READ_NETWORK_STATE\",\"READ_NOTIFICATIONS\",\"READ_POWER_STATE\",\"READ_RUNNING_APPS\","
    "\"READ_SETTINGS\",\"READ_TV_CHANNEL_LIST\",\"READ_TV_CURRENT_TIME\",\"READ_UPDATE_INFO\","
    "\"SEARCH\",\"TEST_OPEN\",\"TEST_PROTECTED\",\"TEST_SECURE\",\"UPDATE_FROM_REMOTE_APP\","
    "\"WRITE_NOTIFICATION_ALERT\",\"WRITE_NOTIFICATION_TOAST\",\"WRITE_SETTINGS\"";

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- minimal websocket client ----------------------------------------------

static int write_all(int fd, const void* buf, size_t len)
{
    const uint8_t* p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int read_exact(int fd, void* buf, size_t len, int64_t deadline)
{
    uint8_t* p = buf;
    while (len) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return -2;
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -2;
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int ws_send(int fd, int opcode, const char* data, size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (len >> 8) & 0xff;
        hdr[h++] = len & 0xff;
    } else {
        return -1;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (uint8_t)(rand() & 0xff);
    memcpy(hdr + h, mask, 4);
    h += 4;
    uint8_t* buf = malloc(h + len);
    if (!buf)
        return -1;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; i++)
        buf[h + i] = (uint8_t)data[i] ^ mask[i % 4];
    int r = write_all(fd, buf, h + len);
    free(buf);
    return r;
}

// Receives the next text frame into buf (NUL-terminated). Answers pings.
static int ws_recv_text(int fd, char* buf, size_t cap, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        uint8_t h[2];
        int r = read_exact(fd, h, 2, deadline);
        if (r)
            return r;
        int op = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if ((r = read_exact(fd, e, 2, deadline)))
                return r;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if ((r = read_exact(fd, e, 8, deadline)))
                return r;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | e[i];
        }
        if (h[1] & 0x80) { // server frames are never masked; tolerate anyway
            uint8_t m[4];
            if ((r = read_exact(fd, m, 4, deadline)))
                return r;
        }
        if (len > 1 << 20)
            return -1;
        char* payload = malloc(len + 1);
        if (!payload)
            return -1;
        if ((r = read_exact(fd, payload, len, deadline))) {
            free(payload);
            return r;
        }
        payload[len] = 0;
        if (op == 0x9) { // ping
            ws_send(fd, 0xA, payload, len);
        } else if (op == 0x8) { // close
            free(payload);
            return -1;
        } else if (op == 0x1) {
            snprintf(buf, cap, "%s", payload);
            free(payload);
            return 0;
        }
        free(payload);
    }
}

static int ws_connect(const char* host, int port, const char* path)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: c3BvcnRzYXV0b3BpbG90IQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
        path, host, port);
    if (write_all(fd, req, n)) {
        close(fd);
        return -1;
    }
    // Read the HTTP response header byte by byte (it's short).
    char resp[1024];
    size_t got = 0;
    int64_t deadline = now_ms() + REPLY_TIMEOUT_MS;
    while (got < sizeof(resp) - 1) {
        if (read_exact(fd, resp + got, 1, deadline)) {
            close(fd);
            return -1;
        }
        got++;
        if (got >= 4 && memcmp(resp + got - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    resp[got] = 0;
    if (!strstr(resp, " 101 ")) {
        close(fd);
        return -1;
    }
    return fd;
}

// --- SSAP ------------------------------------------------------------------

// Extracts the string value of "key":"..." from a JSON text (no escapes).
static int json_str(const char* json, const char* key, char* out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"')
        return -1;
    p++;
    const char* e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= cap)
        return -1;
    memcpy(out, p, e - p);
    out[e - p] = 0;
    return 0;
}

void ssap_init(ssap_t* s, const char* key_path)
{
    memset(s, 0, sizeof(*s));
    s->main_fd = s->ptr_fd = -1;
    snprintf(s->key_path, sizeof(s->key_path), "%s", key_path);
    FILE* f = fopen(key_path, "r");
    if (f) {
        if (fgets(s->client_key, sizeof(s->client_key), f))
            s->client_key[strcspn(s->client_key, "\r\n \t")] = 0;
        fclose(f);
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

bool ssap_has_key(const ssap_t* s)
{
    return s->client_key[0] != 0;
}

void ssap_close(ssap_t* s)
{
    if (s->ptr_fd >= 0)
        close(s->ptr_fd);
    if (s->main_fd >= 0)
        close(s->main_fd);
    s->ptr_fd = s->main_fd = -1;
}

static int ssap_connect(ssap_t* s)
{
    ssap_close(s);
    s->main_fd = ws_connect(SSAP_HOST, SSAP_PORT, "/");
    if (s->main_fd < 0)
        return -1;

    char msg[4096];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"register\",\"id\":\"reg0\",\"payload\":{\"forcePairing\":false,\"pairingType\":\"PROMPT\",%s%s%s"
        "\"manifest\":{\"appVersion\":\"1.1\",\"manifestVersion\":1,\"permissions\":[%s]}}}",
        s->client_key[0] ? "\"client-key\":\"" : "", s->client_key, s->client_key[0] ? "\"," : "", PERMISSIONS);
    if (ws_send(s->main_fd, 0x1, msg, n))
        goto fail;

    char reply[8192];
    int64_t deadline = now_ms() + REGISTER_TIMEOUT_MS;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0 || ws_recv_text(s->main_fd, reply, sizeof(reply), left))
            goto fail;
        if (strstr(reply, "\"error\""))
            goto fail; // e.g. the prompt was declined
        if (strstr(reply, "\"registered\"")) {
            char key[80];
            if (json_str(reply, "client-key", key, sizeof(key)) == 0 && strcmp(key, s->client_key) != 0) {
                snprintf(s->client_key, sizeof(s->client_key), "%s", key);
                FILE* f = fopen(s->key_path, "w");
                if (f) {
                    fprintf(f, "%s\n", key);
                    fclose(f);
                }
            }
            break;
        }
    }

    const char* req = "{\"type\":\"request\",\"id\":\"ptr\",\"uri\":\"ssap://com.webos.service.networkinput/getPointerInputSocket\"}";
    if (ws_send(s->main_fd, 0x1, req, strlen(req)))
        goto fail;
    char path[256] = { 0 };
    deadline = now_ms() + REPLY_TIMEOUT_MS;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0 || ws_recv_text(s->main_fd, reply, sizeof(reply), left))
            goto fail;
        if (strstr(reply, "\"ptr\"") && json_str(reply, "socketPath", path, sizeof(path)) == 0)
            break;
    }
    // ws://127.0.0.1:3000/resources/<id>/netinput.pointer.sock
    const char* p = strstr(path, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    if (!p)
        goto fail;
    s->ptr_fd = ws_connect(SSAP_HOST, SSAP_PORT, p);
    if (s->ptr_fd < 0)
        goto fail;
    return 0;

fail:
    ssap_close(s);
    return -1;
}

int ssap_button(ssap_t* s, const char* name)
{
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "type:button\nname:%s\n\n", name);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s->ptr_fd < 0 && ssap_connect(s) != 0)
            continue;
        // Drain anything the server sent meanwhile (answers pings, detects close).
        struct pollfd pf = { s->main_fd, POLLIN, 0 };
        while (poll(&pf, 1, 0) > 0) {
            char junk[2048];
            if (ws_recv_text(s->main_fd, junk, sizeof(junk), 200) == -1) {
                ssap_close(s);
                break;
            }
        }
        if (s->ptr_fd >= 0 && ws_send(s->ptr_fd, 0x1, msg, n) == 0)
            return 0;
        ssap_close(s);
    }
    return -1;
}
