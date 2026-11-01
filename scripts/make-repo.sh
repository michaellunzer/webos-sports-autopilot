#!/usr/bin/env bash
# Generate a Homebrew Channel repository (apps.json) and app manifest for a
# released ipk.
#   scripts/make-repo.sh <ipk> <version> <ipk-download-url> <site-base-url> <out-dir>
set -euo pipefail
IPK="$1"; VERSION="$2"; IPK_URL="$3"; SITE="${4%/}"; OUT="$5"
mkdir -p "$OUT"
cp frontend/assets/icon_130.png "$OUT/icon.png"
python3 - "$IPK" "$VERSION" "$IPK_URL" "$SITE" "$OUT" <<'PY'
import hashlib, json, os, sys
ipk, version, ipk_url, site, out = sys.argv[1:]
app = json.load(open("frontend/appinfo.json"))
data = open(ipk, "rb").read()
desc = "Watches YouTube TV Multiview and moves to another game when the current one goes to commercial. Requires root."
manifest = {
    "id": app["id"],
    "version": version,
    "type": "web",
    "title": app["title"],
    "appDescription": desc,
    "iconUri": f"{site}/icon.png",
    "sourceUrl": "https://github.com/michaellunzer/webos-sports-autopilot",
    "rootRequired": True,
    "ipkUrl": ipk_url,
    "ipkHash": {"sha256": hashlib.sha256(data).hexdigest()},
    "ipkSize": len(data),
}
repo = {
    "paging": {"page": 1, "count": 1, "maxPage": 1, "itemsTotal": 1, "prevUrl": None, "nextUrl": None},
    "packages": [{
        "id": app["id"],
        "title": app["title"],
        "iconUri": f"{site}/icon.png",
        "manifestUrl": f"{site}/manifest.json",
        "manifest": manifest,
        "pool": "main",
        "requirements": {"webosRelease": ">=4.0"},
        "shortDescription": desc,
    }],
}
json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
json.dump(repo, open(os.path.join(out, "apps.json"), "w"), indent=2)
print(json.dumps(manifest, indent=2))
PY
