#!/usr/bin/env bash
# stream-test.sh - run one clean streaming session and sample the pipeline.
#
# Why this exists: testing by hand repeatedly broke itself. `pkill -f <pattern>`
# matches the calling shell and kills it; a second wivrn-server unlinks the
# first one's IPC socket; `adb logcat -d` dumps megabytes over Wi-Fi and stalls
# the stream it is supposed to measure. This script avoids all three: it kills
# by exact process name, guarantees a single server, and only reads the server
# log (local) while sampling.
#
# Usage: tools/stream-test.sh [--app "steam -applaunch 620980"] [--seconds 120]
#                             [--label name] [--keep-running]
#
# Output: .bench/stream-<label>.txt (samples) and .bench/stream-<label>.log
# (server log). Exit code is non-zero if the stream never produced frames.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/.bench"
mkdir -p "$out"

label=run
secs=120
app=""
keep=0
pkg=org.meumeu.wivrn.local
headset="${HEADSET:-192.168.50.211:5555}"
server_bin="${SERVER_BIN:-$root/build-server-opt/server/wivrn-server}"

while [ $# -gt 0 ]; do
	case "$1" in
		--app) app="$2"; shift 2 ;;
		--seconds) secs="$2"; shift 2 ;;
		--label) label="$2"; shift 2 ;;
		--keep-running) keep=1; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

log="$out/stream-$label.log"
samples="$out/stream-$label.txt"
: > "$log"
: > "$samples"

adb() { command adb -s "$headset" "$@"; }
say() { printf '%s\n' "$*"; }

# --- one server, always --------------------------------------------------------
# Kill by exact name (never -f: the pattern would match this script's own
# command line and take the shell down with it).
for p in $(pgrep -x wivrn-server 2>/dev/null); do
	kill -9 "$p" 2>/dev/null || true
done
sleep 1
# A stale socket file makes every local OpenXR app fail with
# XR_ERROR_RUNTIME_UNAVAILABLE while the old inode stays bound.
rm -rf "/run/user/$(id -u)/wivrn"

say "== starting server"
setsid nohup env LD_LIBRARY_PATH=/run/opengl-driver/lib XRT_LOG=info \
	WIVRN_PIPELINE_STATS=1 "$server_bin" > "$log" 2>&1 < /dev/null &
sleep 5

if [ -z "$(pgrep -x wivrn-server 2>/dev/null)" ]; then
	say "server failed to start"; tail -5 "$log"; exit 1
fi
say "server pid(s): $(pgrep -x wivrn-server | tr '\n' ' ')"

# --- connect the headset -------------------------------------------------------
say "== connecting headset"
# Do NOT force-stop a running client: Android revokes USB permission grants when
# an app is force-stopped, so every run popped the camera permission dialog again.
if [ -z "$(adb shell pidof "$pkg" 2>/dev/null | tr -d '\r')" ]; then
	adb shell am start -a android.intent.action.VIEW -d "wivrn://192.168.50.58:9757" \
		-n "$pkg/org.meumeu.wivrn.MainActivity" >/dev/null 2>&1
else
	say "client already running, reusing it"
	adb shell am start -a android.intent.action.VIEW -d "wivrn://192.168.50.58:9757" \
		-n "$pkg/org.meumeu.wivrn.MainActivity" >/dev/null 2>&1
fi

connected() {
	# The server logs the session in several ways depending on the path; the
	# camera description only appears once the headset has completed the
	# handshake, so accept any of them.
	grep -qiE "client connected|Streaming [0-9]+ camera|Started native session|Session state changed: XR_SESSION_STATE_FOCUSED" "$log"
}
for _ in $(seq 1 30); do
	sleep 1
	if connected; then break; fi
done
if ! connected; then
	say "headset did not connect"; tail -5 "$log"; exit 1
fi
sleep 4

# --- start the workload --------------------------------------------------------
if [ -n "$app" ]; then
	say "== workload: $app"
	setsid nohup $app > "$out/stream-$label-app.log" 2>&1 < /dev/null &
	sleep 30
fi

# --- sample (local reads only) -------------------------------------------------
say "== sampling ${secs}s"
start=$(date +%s)
frames0=$(grep -cE "pipeline: encode" "$log")
while [ $(( $(date +%s) - start )) -lt "$secs" ]; do
	sleep 15
	{
		echo "--- t=$(( $(date +%s) - start ))s $(date +%H:%M:%S)"
		grep -E "pipeline: encode" "$log" | tail -2
		echo "frames=$(grep -cE 'pipeline: encode' "$log") idr=$(grep -cE 'IDR frame needed' "$log") idr_dropped=$(grep -cE 'IDR frame dropped' "$log") netexc=$(grep -cE 'Network exception' "$log") busy=$(grep -cE 'sender busy' "$log") stale=$(grep -cE 'stale decoder' "$log")"
		echo "gpu=$(nvidia-smi --query-gpu=utilization.gpu,power.draw --format=csv,noheader | head -1)"
	} >> "$samples"
done

frames1=$(grep -cE "pipeline: encode" "$log")
say "pipeline samples: $frames0 -> $frames1"

if [ "$keep" -eq 0 ]; then
	say "== stopping"
	if [ -n "$app" ]; then
		case "$app" in
			*"Beat Saber"*|*620980*) pkill -x "Beat Saber.exe" 2>/dev/null || true ;;
		esac
	fi
	for p in $(pgrep -x wivrn-server 2>/dev/null); do
		kill "$p" 2>/dev/null || true
	done
fi

if [ "$frames1" -le "$frames0" ]; then
	say "no frames produced (see $samples and $log)"
	exit 1
fi

say "samples: $samples"
say "server log: $log"
