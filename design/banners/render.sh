#!/usr/bin/env bash
# Render the README / social imagery from design/banners/art.html (needs Google Chrome).
set -euo pipefail
cd "$(dirname "$0")/../.."
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p docs/art
shot() { # <name> <query> <w> <h>
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --user-data-dir="$TMP/p" \
    --allow-file-access-from-files \
    --window-size="$3,$4" --force-device-scale-factor=2 --virtual-time-budget=2000 \
    --screenshot="$PWD/docs/art/$1.png" "file://$PWD/design/banners/art.html?$2" >/dev/null 2>&1
}
for t in dark light; do
  shot "banner-$t" "art=hero&theme=$t" 1280 440
  for c in tune human realtime private; do shot "card-$c-$t" "art=$c&theme=$t" 640 400; done
done
shot social-preview "art=social&theme=dark" 1280 640
echo "rendered → docs/art"
