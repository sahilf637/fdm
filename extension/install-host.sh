#!/usr/bin/env bash
# Registers the FDM native-messaging host with locally installed browsers so the
# extension can talk to it.
#
#   ./install-host.sh [--host-path <abs path to fdm-native-host>] \
#                     [--chrome-id <unpacked extension id>]
#
# Firefox uses a fixed extension id (from manifest.json) so it is always
# registered. Chrome/Chromium identify the extension by its id, which is only
# known once you "Load unpacked" -- pass it with --chrome-id (re-run any time).
set -euo pipefail

HOST_NAME="com.fdm.native_host"
GECKO_ID="suspended035@gmail.com"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEMPLATE="$SCRIPT_DIR/com.fdm.native_host.json.in"

HOST_PATH="$REPO_ROOT/build/host/fdm-native-host"
CHROME_ID=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-path) HOST_PATH="$2"; shift 2 ;;
    --chrome-id) CHROME_ID="$2"; shift 2 ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

HOST_PATH="$(readlink -f "$HOST_PATH")"
if [[ ! -x "$HOST_PATH" ]]; then
  echo "error: host binary not found / not executable: $HOST_PATH" >&2
  echo "       build it first:  cmake --build build --target fdm-native-host" >&2
  exit 1
fi

write_chrome_manifest() {
  local dir="$1"
  if [[ -z "$CHROME_ID" ]]; then
    return 1
  fi
  mkdir -p "$dir"
  sed -e "s|__HOST_PATH__|$HOST_PATH|" -e "s|__CHROME_ID__|$CHROME_ID|" \
    "$TEMPLATE" > "$dir/$HOST_NAME.json"
  echo "  installed: $dir/$HOST_NAME.json"
}

write_firefox_manifest() {
  local dir="$1"
  mkdir -p "$dir"
  cat > "$dir/$HOST_NAME.json" <<EOF
{
  "name": "$HOST_NAME",
  "description": "FDM native messaging host",
  "path": "$HOST_PATH",
  "type": "stdio",
  "allowed_extensions": ["$GECKO_ID"]
}
EOF
  echo "  installed: $dir/$HOST_NAME.json"
}

echo "host binary: $HOST_PATH"

echo "Firefox:"
write_firefox_manifest "$HOME/.mozilla/native-messaging-hosts"
# Snap and Flatpak Firefox are sandboxed: each runs with $HOME inside its own
# data tree, so it ignores ~/.mozilla and reads native-host manifests from a
# private location. Install there too when those packages are present.
if [[ -d "$HOME/snap/firefox" ]]; then
  write_firefox_manifest "$HOME/snap/firefox/common/.mozilla/native-messaging-hosts"
fi
if [[ -d "$HOME/.var/app/org.mozilla.firefox" ]]; then
  write_firefox_manifest "$HOME/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts"
fi

echo "Chrome / Chromium:"
if [[ -n "$CHROME_ID" ]]; then
  write_chrome_manifest "$HOME/.config/google-chrome/NativeMessagingHosts" || true
  write_chrome_manifest "$HOME/.config/chromium/NativeMessagingHosts" || true
else
  echo "  skipped (no --chrome-id). After 'Load unpacked' in chrome://extensions,"
  echo "  re-run:  $0 --chrome-id <the extension id shown there>"
fi

echo "done."
