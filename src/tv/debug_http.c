#include "debug_http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char INDEX_HTML[] = "<!doctype html><html><head><meta charset=utf-8>"
                                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                                 "<title>NFL Autopilot</title><style>"
                                 "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:16px}"
                                 "img{width:100%;max-width:960px;image-rendering:pixelated;border:1px solid #444}"
                                 "pre{background:#222;padding:8px;overflow:auto;max-width:960px;font-size:12px}"
                                 ".live{color:#4c4}.ad{color:#e44}.unknown{color:#999}"
                                 "</style></head><body><h2>NFL Autopilot</h2>"
                                 "<img id=f src=/frame.bmp><div id=t></div><pre id=s></pre><script>"
                                 "async function tick(){try{"
                                 "document.getElementById('f').src='/frame.bmp?'+Date.now();"
                                 "const s=await (await fetch('/status.json')).json();"
                                 "document.getElementById('s').textContent=JSON.stringify(s,null,1);"
                                 "document.getElementById('t').innerHTML='<p>'+(s.enabled?'ON':'OFF')+' &middot; '+s.mode+' &middot; '+s.phase+'</p>'+"
                                 "(s.tiles||[]).map((x,i)=>`<span class=${x.state}>Game ${i+1}: ${x.state} (${x.score.toFixed(3)})</span>`).join(' &nbsp; ');"
                                 "}catch(e){}setTimeout(tick,1000)}tick();</script></body></html>";

static void send_all(int fd, const void* buf, size_t len)
{
    const char* p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return;
        p += n;
        len -= n;
    }
}

static void respond(int fd, const char* status, const char* type, const void* body, size_t len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, type, len);
    send_all(fd, hdr, n);
    send_all(fd, body, len);
}

static void handle_client(debug_http_t* h, int fd)
{
    char req[1024];
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 2000) <= 0)
        return;
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = 0;
    char method[8] = { 0 }, path[256] = { 0 };
    if (sscanf(req, "%7s %255s", method, path) != 2 || strcmp(method, "GET") != 0) {
        respond(fd, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }
    char* q = strchr(path, '?');
    if (q)
        *q = 0;

    if (strcmp(path, "/") == 0) {
        respond(fd, "200 OK", "text/html; charset=utf-8", INDEX_HTML, sizeof(INDEX_HTML) - 1);
    } else if (strcmp(path, "/status.json") == 0) {
        char* s = h->status_json(h->data);
        respond(fd, "200 OK", "application/json", s ? s : "{}", s ? strlen(s) : 2);
        free(s);
    } else if (strcmp(path, "/frame.bmp") == 0) {
        size_t len = 0;
        uint8_t* bmp = h->frame_bmp(h->data, &len);
        if (bmp)
            respond(fd, "200 OK", "image/bmp", bmp, len);
        else
            respond(fd, "503 Service Unavailable", "text/plain", "no frame yet", 12);
        free(bmp);
    } else {
        respond(fd, "404 Not Found", "text/plain", "not found", 9);
    }
}

static void* http_thread(void* arg)
{
    debug_http_t* h = arg;
    while (h->running) {
        struct pollfd p = { h->listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0)
            continue;
        int fd = accept(h->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        handle_client(h, fd);
        close(fd);
    }
    return NULL;
}

int debug_http_start(debug_http_t* h, int port)
{
    if (h->running || port <= 0)
        return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        int e = errno;
        close(fd);
        WARN("debug http: cannot listen on %d: %s", port, strerror(e));
        return -e;
    }
    h->listen_fd = fd;
    h->port = port;
    h->running = true;
    if (pthread_create(&h->thread, NULL, http_thread, h) != 0) {
        h->running = false;
        close(fd);
        return -1;
    }
    INFO("debug page on http://<tv-ip>:%d/", port);
    return 0;
}

void debug_http_stop(debug_http_t* h)
{
    if (!h->running)
        return;
    h->running = false;
    pthread_join(h->thread, NULL);
    close(h->listen_fd);
}
