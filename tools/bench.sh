#!/usr/bin/env bash
# bench.sh <label> [seconds]
#
# Runs a fixed VR benchmark on the Quest and prints a summary:
#   - server CPU (% of one core)
#   - headset CPU (% of one core, and the busiest threads)
#   - camera frames/s for /dev/video10 and /dev/video11
#   - dropped/incomplete video frames from logcat
#   - a screenshot of the WiVRn statistics page
#
# The VR workload is hello_xr (OpenXR sample, Vulkan, 90 Hz) unless VRBENCH_APP
# is set, e.g. VRBENCH_APP=wayvr.
#
# Env:
#   SERVER_BIN   server binary (default: build-server/server/wivrn-server)
#   VRBENCH_APP  command to run on the PC as the VR workload
#   CAMERAS      space separated video devices (default: "10 11")
#
set -uo pipefail

label="${1:-run}"
secs="${2:-30}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
wivrn="$root"
out="$root/.bench"
mkdir -p "$out"
pkg="org.meumeu.wivrn.local"
headset="${HEADSET:-192.168.50.211:5555}"
server_bin="${SERVER_BIN:-$wivrn/build-server/server/wivrn-server}"
app="${VRBENCH_APP:-hello_xr -g Vulkan -s Local}"
cameras="${CAMERAS:-10 11}"

say() { printf '%s\n' "$*"; }
adb() { command adb -s "$headset" "$@"; }

# --- server -------------------------------------------------------------------
# WiVRn forks a worker child; pick the process that actually burns CPU.
pick_server() {
	for p in $(pgrep -f 'wivrn-server' 2>/dev/null); do
		[ -r "/proc/$p/stat" ] || continue
		awk '{print $14+$15, $1}' "/proc/$p/stat" 2>/dev/null
	done | sort -rn | head -1 | awk '{print $2}'
}
server_pid="$(pick_server)"
if [ -z "$server_pid" ]; then
	say "== starting server"
	(cd "$wivrn" && nohup env LD_LIBRARY_PATH=/run/opengl-driver/lib XRT_LOG=info \
		"$server_bin" > "$root/.bench/server.log" 2>&1 &)
	sleep 8
	server_pid="$(pick_server)"
fi
[ -n "$server_pid" ] || { say "no server"; exit 1; }

# --- headset ------------------------------------------------------------------
say "== connecting headset"
adb shell am force-stop "$pkg"
sleep 2
adb logcat -c
adb shell am start -a android.intent.action.VIEW -d "wivrn://192.168.50.58:9757" \
	-n "$pkg/org.meumeu.wivrn.MainActivity" >/dev/null 2>&1
sleep 12

pid="$(adb shell pidof "$pkg" 2>/dev/null | tr -d '\r')"
[ -n "$pid" ] || { say "app not running"; exit 1; }

# --- VR workload --------------------------------------------------------------
say "== workload: $app"
if [ -n "$app" ]; then
	# shellcheck disable=SC2086
	nohup $app > "$out/$label-app.log" 2>&1 &
	app_pid=$!
fi
sleep 10

"$wivrn/build-server/server/wivrnctl" tab stats >/dev/null 2>&1 || true

# --- sample -------------------------------------------------------------------
ticks="$(getconf CLK_TCK)"
cpu_ticks() { awk '{print $14+$15}' "$1" 2>/dev/null || echo 0; }

# Re-pick now that the client is connected (the worker child only exists then).
server_pid="$(pick_server)"
[ -n "$server_pid" ] || { say "no server worker"; exit 1; }
srv0="$(cpu_ticks /proc/$server_pid/stat)"
hs0="$(adb shell "awk '{print \$14+\$15}' /proc/$pid/stat" 2>/dev/null | tr -d '\r')"
thr_comm0="$(adb shell "cat /proc/$pid/task/*/comm" 2>/dev/null | tr -d '\r')"
thr0="$(adb shell "awk '{print \$14+\$15}' /proc/$pid/task/*/stat" 2>/dev/null | tr -d '\r')"
say "== sampling ${secs}s"
sleep "$secs"
srv1="$(cpu_ticks /proc/$server_pid/stat)"
hs1="$(adb shell "awk '{print \$14+\$15}' /proc/$pid/stat" 2>/dev/null | tr -d '\r')"
thr1="$(adb shell "awk '{print \$14+\$15}' /proc/$pid/task/*/stat" 2>/dev/null | tr -d '\r')"

srv_cpu="$(awk -v a="$srv0" -v b="$srv1" -v t="$ticks" -v s="$secs" 'BEGIN{printf "%.1f", (b-a)/t/s*100}')"
hs_cpu="$(awk -v a="$hs0" -v b="$hs1" -v t="$ticks" -v s="$secs" 'BEGIN{printf "%.1f", (b-a)/t/s*100}')"

# busiest headset threads
threads="$(paste <(printf '%s\n' "$thr_comm0") <(printf '%s\n' "$thr0") <(printf '%s\n' "$thr1") |
	awk -v t="$ticks" -v s="$secs" '{name=$1; c[name]+=$3-$2} END{for(n in c) printf "%6.1f %s\n", c[n]/t/s*100, n}' |
	sort -rn | head -5)"

# camera throughput
cam_res=""
for d in $cameras; do
	[ -e "/dev/video$d" ] || continue
	fps="$(timeout "$secs" ffmpeg -hide_banner -loglevel info -f v4l2 -use_wallclock_as_timestamps 1 \
		-i "/dev/video$d" -vf showinfo -f null - 2>&1 |
		grep -o 'pts_time:[0-9.]*' | sed 's/pts_time://' |
		awk 'NR>1{t=$1-p;s+=t;n++}{p=$1} END{if(n>0) printf "%.2f", 1/(s/n); else printf "0"}')"
	cam_res+="video$d=${fps}fps "
done

loss="$(adb logcat -d 2>/dev/null | grep -c 'was not sent' || true)"
late="$(adb logcat -d 2>/dev/null | grep -c 'Drop shard for old frame' || true)"

# stats screenshot
adb exec-out screencap -p > "$out/$label.png" 2>/dev/null || true

[ -n "${app_pid:-}" ] && kill "$app_pid" 2>/dev/null || true

# --- report -------------------------------------------------------------------
{
	echo "label:            $label"
	echo "duration:         ${secs}s"
	echo "server cpu:       ${srv_cpu} %"
	echo "headset cpu:      ${hs_cpu} %"
	echo "headset threads:"
	printf '%s\n' "$threads" | sed 's/^/  /'
	echo "cameras:          ${cam_res:-none}"
	echo "frames not sent:  $loss"
	echo "late shards:      $late"
	echo "screenshot:       $out/$label.png"
} | tee "$out/$label.txt"
