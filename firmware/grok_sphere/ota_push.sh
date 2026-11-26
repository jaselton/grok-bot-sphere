#!/bin/sh
# Over-the-air update: upload the last build to the relay. The sphere picks it up the next time it's idle.
#   ./ota_push.sh [path/to/grok_sphere.ino.bin]        (default ../../build/grok_sphere.ino.bin)
# Needs: OTA_TOKEN in the Keychain (service charlie-relay-ota) and set on the relay (wrangler secret put OTA_TOKEN).
# The version is FW_VERSION from config.h: bump it before building, or the sphere won't see the image as newer.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$D/../../build/grok_sphere.ino.bin}
VER=$(sed -n 's/^#define FW_VERSION "\([^"]*\)".*/\1/p' "$D/config.h")
RELAY=${RELAY:-$(sed -n 's/^#define RELAY_URL *"wss:\/\/\([^/"]*\).*/\1/p' "$D/secrets.h")}
[ -n "$VER" ] && [ -n "$RELAY" ] && [ -f "$BIN" ] || { echo "need FW_VERSION, RELAY_URL (secrets.h or RELAY=host) and $BIN" >&2; exit 1; }
TOK=$(security find-generic-password -s charlie-relay-ota -w) || { echo "no OTA token in Keychain" >&2; exit 1; }
echo "uploading $(basename "$BIN") as $VER ($(wc -c < "$BIN" | tr -d ' ') bytes)"
printf 'header = "Authorization: Bearer %s"\n' "$TOK" | curl -sS --fail-with-body -K - -X POST \
  -H "x-fw-version: $VER" -H "content-type: application/octet-stream" --data-binary "@$BIN" "https://$RELAY/fw"
echo
