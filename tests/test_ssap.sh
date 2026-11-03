#!/usr/bin/env bash
# Usage: test_ssap.sh <test_ssap binary> <fake_ssap.py>
set -euo pipefail
BIN="$1"; FAKE="$2"; PORT=39123
TMP="$(mktemp -d)"; trap 'kill $PID 2>/dev/null || true; rm -rf "$TMP"' EXIT
python3 "$FAKE" "$PORT" "$TMP/log" > "$TMP/out" & PID=$!
for i in $(seq 50); do grep -q ready "$TMP/out" 2>/dev/null && break; sleep 0.1; done
"$BIN" "$TMP/key"             # first run: pairs and saves the key
"$BIN" "$TMP/key"             # second run: reuses the saved key
sleep 0.3
cat "$TMP/log"
test "$(cat "$TMP/key")" = "fakekey123"
test "$(grep -c '^type:button|name:' "$TMP/log")" -eq 8
grep -q '^type:button|name:RIGHT||$' "$TMP/log"
grep -q '^pong$' "$TMP/log"
echo "ssap test passed"
