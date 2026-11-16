#!/usr/bin/env bash
# preflight.sh - refuse to start a benchmark run unless the environment is clean.
#
# Checks, in order:
#   1. no wivrn-server is already listening on the IPC socket;
#   2. no running server is a deleted/replaced binary;
#   3. adb sees the headset in the "device" state;
#   4. the headset client config matches the expected resolution scale and bitrate.
#
# Each failure prints what is wrong and the exact command to fix it.  This
# script never starts or kills anything: in particular it uses `pgrep -x
# wivrn-server` (exact name), never `pkill -f`, which would match the caller.
#
# Usage: tools/bench/preflight.sh [--help]
#
# Environment:
#   HEADSET                    adb serial (default: 192.168.50.211:5555)
#   WIVRN_PKG                  client package (default: org.meumeu.wivrn.local)
#   SERVER_BIN                 server binary to expect (default: build-server-opt/server/wivrn-server)
#   EXPECTED_RESOLUTION_SCALE  expected client resolution_scale (default: 1.2)
#   EXPECTED_BITRATE_BPS       expected client bitrate_bps (default: 50000000)
#   XDG_RUNTIME_DIR            runtime dir holding wivrn/comp_ipc (default: /run/user/$(id -u))
#
# Exit codes: 0 clean, 1 at least one problem, 2 usage error.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

headset="${HEADSET:-192.168.50.211:5555}"
pkg="${WIVRN_PKG:-org.meumeu.wivrn.local}"
server_bin="${SERVER_BIN:-$root/build-server-opt/server/wivrn-server}"
expected_scale="${EXPECTED_RESOLUTION_SCALE:-1.2}"
expected_bitrate="${EXPECTED_BITRATE_BPS:-50000000}"
runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
sock="$runtime_dir/wivrn/comp_ipc"

usage() {
	cat <<'EOF'
preflight.sh - check that a WiVRn benchmark run can start cleanly.

Usage: tools/bench/preflight.sh [--help]

Checks:
  - no wivrn-server is listening on $XDG_RUNTIME_DIR/wivrn/comp_ipc
  - no running server binary was deleted or replaced on disk
  - adb sees HEADSET in the "device" state
  - the headset client.json has the expected resolution_scale and bitrate_bps

Environment:
  HEADSET                    adb serial (default: 192.168.50.211:5555)
  WIVRN_PKG                  client package (default: org.meumeu.wivrn.local)
  SERVER_BIN                 server binary to expect
  EXPECTED_RESOLUTION_SCALE  expected resolution_scale (default: 1.2)
  EXPECTED_BITRATE_BPS       expected bitrate_bps (default: 50000000)
  XDG_RUNTIME_DIR            directory holding wivrn/comp_ipc

Exit codes: 0 clean, 1 problem found, 2 usage error.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help) usage; exit 0 ;;
		*) printf 'preflight.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
	esac
done

problems=0
problem() {
	problems=$((problems + 1))
	printf 'PROBLEM: %s\n' "$*" >&2
}
fix() {
	printf '  FIX:   %s\n' "$*" >&2
}
note() {
	printf 'note: %s\n' "$*"
}

# --- 1. an existing server ----------------------------------------------------
# Connecting is the only reliable test: a stale socket file exists after a crash,
# and `pgrep -f wivrn-server` would also match this script's command line.
probe_socket() {
	python3 - "$sock" <<'PY'
import socket
import sys

path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(1.0)
try:
    s.connect(path)
except FileNotFoundError:
    print("absent")
except ConnectionRefusedError:
    print("refused")
except PermissionError:
    print("denied")
except OSError as e:
    print(f"error:{e.errno}:{e.strerror}")
else:
    print("listening")
finally:
    s.close()
PY
}

socket_state="$(probe_socket 2>/dev/null)"
case "$socket_state" in
	listening)
		pids="$(pgrep -x wivrn-server 2>/dev/null | tr '\n' ' ')"
		problem "a wivrn-server is already running (IPC socket $sock accepts connections)."
		if [ -n "$pids" ]; then
			fix "stop it first: kill ${pids% }   (exact pid; never pkill -f)"
		else
			fix "stop the process that owns $sock before starting a new server."
		fi
		fix "a second server unlinks the first one's socket and breaks every local OpenXR app."
		;;
	refused)
		note "stale IPC socket at $sock (nothing is listening); the server removes it on startup."
		;;
	denied)
		problem "cannot connect to $sock: permission denied."
		fix "check that XDG_RUNTIME_DIR ($runtime_dir) belongs to uid $(id -u) and is mode 0700."
		;;
	absent)
		;;
	*)
		problem "could not probe the IPC socket $sock (${socket_state:-no answer})."
		fix "check that python3 is installed and XDG_RUNTIME_DIR is set correctly."
		;;
esac

# --- 2. a stale server binary -------------------------------------------------
if command -v pgrep >/dev/null 2>&1; then
	for pid in $(pgrep -x wivrn-server 2>/dev/null); do
		exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
		case "$exe" in
			*" (deleted)")
				problem "running wivrn-server (pid $pid) is a deleted/replaced binary: $exe"
				fix "restart it so the freshly built binary is loaded: kill $pid, then start the server again."
				;;
		esac
	done
fi

if [ ! -x "$server_bin" ]; then
	problem "server binary is missing or not executable: $server_bin"
	fix "build it, or point SERVER_BIN at the binary you intend to measure."
fi

# --- 3. adb and the headset ---------------------------------------------------
if ! command -v adb >/dev/null 2>&1; then
	problem "adb is not on PATH."
	fix "install android-tools (or add adb to PATH) and rerun."
else
	devices="$(adb devices 2>&1)"
	state="$(printf '%s\n' "$devices" | awk -v d="$headset" '$1 == d { print $2; exit }')"
	case "$state" in
		device)
			;;
		unauthorized)
			problem "headset $headset is unauthorized."
			fix "put the headset on and accept the USB debugging prompt, then rerun."
			;;
		offline)
			problem "headset $headset is offline."
			fix "reconnect it: adb disconnect $headset && adb connect $headset"
			;;
		"")
			problem "headset $headset is not listed by adb devices."
			fix "connect it (adb connect $headset) or set HEADSET=<serial>."
			fix "current adb devices output:"
			printf '%s\n' "$devices" | sed 's/^/         /' >&2
			;;
		*)
			problem "headset $headset is in unexpected adb state: $state"
			fix "run: adb disconnect $headset && adb connect $headset"
			;;
	esac
fi

# --- 4. headset client config -------------------------------------------------
read_client_config() {
	command adb -s "$headset" shell run-as "$pkg" cat files/client.json 2>/dev/null | tr -d '\r'
}

if [ "${state:-}" = "device" ]; then
	cfg="$(read_client_config)"
	if [ -z "$cfg" ]; then
		problem "cannot read the headset client config (run-as $pkg cat files/client.json)."
		fix "install the debuggable local build ($pkg); a release build cannot be read with run-as."
		fix "check that the headset is connected and the app has been launched at least once."
	else
		scale="$(printf '%s' "$cfg" | jq -r '.resolution_scale // empty' 2>/dev/null)"
		bitrate="$(printf '%s' "$cfg" | jq -r '.bitrate_bps // empty' 2>/dev/null)"
		if [ -z "$scale" ] || [ -z "$bitrate" ]; then
			problem "headset client config has no readable resolution_scale/bitrate_bps."
			fix "open the WiVRn client once so it writes files/client.json, then rerun."
		else
			if ! awk -v a="$scale" -v b="$expected_scale" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= 0.001) }'; then
				problem "headset resolution_scale is $scale, expected $expected_scale."
				fix "set render resolution in the headset WiVRn settings to match, or export EXPECTED_RESOLUTION_SCALE=$scale."
			fi
			if ! awk -v a="$bitrate" -v b="$expected_bitrate" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= 1) }'; then
				problem "headset bitrate_bps is $bitrate, expected $expected_bitrate."
				fix "set the bitrate in the headset WiVRn settings to match, or export EXPECTED_BITRATE_BPS=$bitrate."
			fi
			if [ "$problems" -eq 0 ]; then
				note "headset config: resolution_scale=$scale bitrate_bps=$bitrate"
			fi
		fi
	fi
fi

if [ "$problems" -ne 0 ]; then
	printf '\npreflight: %d problem(s); not starting a run.\n' "$problems" >&2
	exit 1
fi

note "preflight: clean (no server, $headset reachable, config matches)."
exit 0
