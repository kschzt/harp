#!/bin/bash
# latefr-eth-test — §8.2 host-paced late-frame discard, over the §8.7 loopback.
#
# Launch harp-deviced (TCP); harp-eth-latefr-test does audio.start in host-paced mode
# with a host-paced TCP audio port (key 7), accepts the device's connect-back, sends
# pacing frames in SSI order plus ONE rewound frame, and asserts the device discards +
# counts it (audio_late_frames 0 -> 1). POSIX-only (the tool isn't built on Windows).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
LATEFR="${LATEFR:-./build/harp-eth-latefr-test}"
PORT="${PORT:-47994}"
DEVDIR=latefr-eth-state   # workspace-RELATIVE (see eth-tests.sh header)
DEVLOG=/tmp/latefr-eth-dev.log
fail() { echo "LATEFR-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"
echo "── start device on $PORT over the §8.7 loopback"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

"$LATEFR" "127.0.0.1:$PORT" || { cat "$DEVLOG"; fail "host-paced late-frame discard not observed"; }
