#!/usr/bin/env bash
# Assemble a Firefox-loadable copy of the extension. Chrome and Firefox need
# different "background" manifest keys (service_worker vs scripts) and Chrome
# rejects the Firefox form, so we keep manifest.json = Chrome and build a
# Firefox folder here.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SCRIPT_DIR/dist-firefox"

rm -rf "$OUT"
mkdir -p "$OUT/icons"
cp "$SCRIPT_DIR/background.js" "$SCRIPT_DIR/popup.html" "$SCRIPT_DIR/popup.js" "$OUT/"
cp "$SCRIPT_DIR/icons/"*.png "$OUT/icons/"
cp "$SCRIPT_DIR/manifest.firefox.json" "$OUT/manifest.json"

echo "Firefox extension ready at: $OUT"
echo "Load via about:debugging -> This Firefox -> Load Temporary Add-on -> $OUT/manifest.json"
