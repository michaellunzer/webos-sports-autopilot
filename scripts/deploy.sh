#!/usr/bin/env bash
# Copy the .ipk to a rooted TV over SSH and install it.
#   scripts/deploy.sh 192.168.1.50 [dist/org.nflautopilot.app_0.1.0_all.ipk]
set -euo pipefail
TV="${1:?usage: deploy.sh <tv-ip> [ipk]}"
IPK="${2:-$(ls -t dist/*.ipk | head -1)}"
scp "$IPK" "root@$TV:/tmp/nfl-autopilot.ipk"
ssh "root@$TV" "luna-send-pub -w 20000 -i 'luna://com.webos.appInstallService/dev/install' \
  '{\"id\":\"com.ares.defaultName\",\"ipkUrl\":\"/tmp/nfl-autopilot.ipk\",\"subscribe\":true}' | head -40; \
  rm -f /tmp/nfl-autopilot.ipk; pkill -x nfl-autopilot; true"
echo "Installed. Open 'NFL Autopilot' from the TV launcher (first run: press 'Grant root')."
