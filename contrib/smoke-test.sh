#!/usr/bin/env bash
# Smoke test for the lgmd proxy source: start Sunshine, confirm the daemon is
# reachable and that only H.264 is advertised, then stop it.
#
# Usage: SUNSHINE_LGMD_SOCKET=/run/user/1000/lgmd.sock contrib/smoke-test.sh [path-to-sunshine]

set -euo pipefail

BIN="${1:-$(dirname "$0")/../build/sunshine}"
: "${SUNSHINE_LGMD_SOCKET:?set SUNSHINE_LGMD_SOCKET to the capture daemon socket}"

[[ -x "$BIN" ]] || { echo "sunshine binary not found: $BIN" >&2; exit 1; }
[[ -S "$SUNSHINE_LGMD_SOCKET" ]] || { echo "socket not found: $SUNSHINE_LGMD_SOCKET" >&2; exit 1; }

log=$(mktemp)
cleanup() {
    [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true
    wait "${pid:-}" 2>/dev/null || true
}
trap cleanup EXIT

"$BIN" >"$log" 2>&1 &
pid=$!

for _ in $(seq 1 40); do
    if curl -fsS --max-time 1 http://127.0.0.1:47989/serverinfo -o /tmp/serverinfo.xml 2>/dev/null; then
        break
    fi
    sleep 0.5
done

if [[ ! -s /tmp/serverinfo.xml ]]; then
    echo "serverinfo did not answer; log:" >&2
    tail -20 "$log" >&2
    exit 1
fi

codec=$(sed -n 's:.*<ServerCodecModeSupport>\([0-9]*\)</ServerCodecModeSupport>.*:\1:p' /tmp/serverinfo.xml | head -1)
echo "ServerCodecModeSupport=$codec (1 = H.264 only)"

if [[ "$codec" != "1" ]]; then
    echo "expected H.264 only while proxying" >&2
    exit 1
fi

grep -q "Streaming video from" "$log" || echo "note: proxy source not used yet (no session started)"

echo "ok"
