#!/usr/bin/env python3
"""Minimal fake of the TV's SSAP websocket server, for testing src/tv/ssap.c.
Usage: fake_ssap.py PORT LOGFILE  - logs pointer-socket messages to LOGFILE."""
import base64, hashlib, json, socket, struct, sys, threading

PORT, LOG = int(sys.argv[1]), sys.argv[2]
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def recv_exact(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d:
            raise EOFError
        b += d
    return b

def handshake(c):
    req = b""
    while b"\r\n\r\n" not in req:
        req += c.recv(1)
    lines = req.decode().split("\r\n")
    path = lines[0].split()[1]
    key = [l.split(": ", 1)[1] for l in lines if l.lower().startswith("sec-websocket-key")][0]
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    return path

def read_frame(c):
    h = recv_exact(c, 2)
    op, ln = h[0] & 15, h[1] & 127
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(c, 2))[0]
    assert h[1] & 0x80, "client frames must be masked"
    m = recv_exact(c, 4)
    data = bytes(b ^ m[i % 4] for i, b in enumerate(recv_exact(c, ln)))
    return op, data.decode()

def send(c, op, text):
    d = text.encode()
    hdr = bytes([0x80 | op, len(d)]) if len(d) < 126 else bytes([0x80 | op, 126]) + struct.pack(">H", len(d))
    c.sendall(hdr + d)

def client(c):
    try:
        path = handshake(c)
        if path.endswith("netinput.pointer.sock"):
            while True:
                op, t = read_frame(c)
                if op == 1:
                    with open(LOG, "a") as f:
                        f.write(t.replace("\n", "|") + "\n")
            return
        while True:
            op, t = read_frame(c)
            if op == 0xA:
                with open(LOG, "a") as f:
                    f.write("pong\n")
                continue
            m = json.loads(t)
            if m["type"] == "register":
                if "client-key" not in m["payload"]:
                    send(c, 1, json.dumps({"type": "response", "id": m["id"], "payload": {"pairingType": "PROMPT", "returnValue": True}}))
                send(c, 1, json.dumps({"type": "registered", "id": m["id"], "payload": {"client-key": "fakekey123"}}))
                send(c, 9, "ping")
            elif m["type"] == "request":
                send(c, 1, json.dumps({"type": "response", "id": m["id"], "payload": {
                    "socketPath": "ws://127.0.0.1:%d/resources/abc/netinput.pointer.sock" % PORT, "returnValue": True}}))
    except (EOFError, ConnectionError, OSError):
        pass

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT))
s.listen(8)
print("ready", flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=client, args=(c,), daemon=True).start()
