#!/usr/bin/env bash
# Cross-compile the service and package the .ipk.
#   TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake scripts/build-ipk.sh
# Needs: the openlgtv buildroot-nc4 toolchain and ares-cli (npm i -g @webosose/ares-cli).
set -euo pipefail
cd "$(dirname "$0")/.."

: "${TOOLCHAIN_FILE:?set TOOLCHAIN_FILE to the buildroot-nc4 toolchainfile.cmake}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S . -B build-tv -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DHOST_BUILD=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-tv --target sports-autopilot dile_vt_backend vtcapture_backend -j"$(nproc)"

rm -rf dist/pkg && mkdir -p dist/pkg/app dist/pkg/service
cp -r frontend/. dist/pkg/app/
if [ -n "${VERSION:-}" ]; then
  # Release builds: stamp the tag version into the packaged appinfo.json.
  python3 - "$VERSION" <<'PY'
import json, sys
p = "dist/pkg/app/appinfo.json"
info = json.load(open(p))
info["version"] = sys.argv[1]
json.dump(info, open(p, "w"), indent=2)
PY
fi
cp build-tv/sports-autopilot build-tv/libdile_vt_backend.so build-tv/libvtcapture_backend.so dist/pkg/service/
cp servicefiles/* dist/pkg/service/
chmod +x dist/pkg/service/sports-autopilot dist/pkg/service/sportsautopilot-autostart

ares-package dist/pkg/app dist/pkg/service -o dist
ls -l dist/*.ipk
