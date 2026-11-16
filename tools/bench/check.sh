#!/usr/bin/env bash
# check.sh - post-run gate for a WiVRn benchmark directory.
#
# Fails a run if:
#   - the server log contains a fatal event (network exception, partial write,
#     encode error, sender busy, local drop, stale decoder, IDR needed/dropped);
#   - the pipeline sample count is below a minimum;
#   - the median pipeline total is above a threshold.
#
# Usage: tools/bench/check.sh [options] <run-dir> [max-median-ms]
#
#   <run-dir>          directory written by collect.py (pipeline.csv,
#                      events.csv, summary.json) and the server log
#   [max-median-ms]    maximum acceptable median total, in milliseconds
#
# Options:
#   --log FILE           server log (default: <run-dir>/server.log, else the
#                        only *.log in <run-dir>)
#   --min-samples N      minimum pipeline rows (default: $MIN_SAMPLES or 30)
#   --max-median-ms X    same as the positional threshold
#   --allow LIST         comma-separated event names to downgrade to warnings
#   -h, --help
#
# Exit codes: 0 pass, 1 gate failed, 2 usage or missing input.

set -uo pipefail

min_samples="${MIN_SAMPLES:-30}"
max_median=""
log=""
allow=""
run_dir=""

usage() {
	cat <<'EOF'
check.sh - gate a finished WiVRn benchmark run.

Usage: tools/bench/check.sh [options] <run-dir> [max-median-ms]

Fails if the server log has a fatal event, if there are too few pipeline
samples, or if the median pipeline total exceeds the threshold.

Options:
  --log FILE           server log (default: <run-dir>/server.log, else the
                       only *.log in <run-dir>)
  --min-samples N      minimum pipeline rows (default: MIN_SAMPLES or 30)
  --max-median-ms X    maximum acceptable median total (ms)
  --allow LIST         comma-separated events to downgrade to warnings
                       (idr_needed,idr_dropped,local_drop,sender_busy,
                        stale_decoder,network_exception,partial_write,
                        encode_error)
  -h, --help

Exit codes: 0 pass, 1 gate failed, 2 usage or missing input.
EOF
}

die() {
	printf 'check.sh: %s\n' "$*" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help) usage; exit 0 ;;
		--log) [ $# -ge 2 ] || die "--log needs a file"; log="$2"; shift 2 ;;
		--min-samples) [ $# -ge 2 ] || die "--min-samples needs a number"; min_samples="$2"; shift 2 ;;
		--max-median-ms) [ $# -ge 2 ] || die "--max-median-ms needs a number"; max_median="$2"; shift 2 ;;
		--allow) [ $# -ge 2 ] || die "--allow needs a list"; allow="$2"; shift 2 ;;
		--) shift; break ;;
		-*) die "unknown option: $1 (try --help)" ;;
		*)
			if [ -z "$run_dir" ]; then
				run_dir="$1"
			elif [ -z "$max_median" ]; then
				max_median="$1"
			else
				die "unexpected argument: $1"
			fi
			shift
			;;
	esac
done

# Positionals after --.
while [ $# -gt 0 ]; do
	if [ -z "$run_dir" ]; then
		run_dir="$1"
	elif [ -z "$max_median" ]; then
		max_median="$1"
	else
		die "unexpected argument: $1"
	fi
	shift
done

[ -n "$run_dir" ] || { usage >&2; exit 2; }
[ -d "$run_dir" ] || die "not a directory: $run_dir"
[ -n "$max_median" ] || die "missing max-median-ms threshold (positional or --max-median-ms)"

case "$min_samples" in
	''|*[!0-9]*) die "--min-samples must be a non-negative integer" ;;
esac
case "$max_median" in
	''|*[!0-9.]*) die "--max-median-ms must be a number" ;;
esac
if ! awk -v b="$max_median" 'BEGIN { exit !(b ~ /^[0-9]+(\.[0-9]+)?$/) }'; then
	die "--max-median-ms must be a number"
fi

# --- locate the log -----------------------------------------------------------
if [ -z "$log" ]; then
	if [ -f "$run_dir/server.log" ]; then
		log="$run_dir/server.log"
	else
		count=0
		candidate=""
		for f in "$run_dir"/*.log; do
			[ -f "$f" ] || continue
			count=$((count + 1))
			candidate="$f"
		done
		case "$count" in
			1) log="$candidate" ;;
		esac
	fi
fi
# Fall back to the log path recorded by collect.py.
if [ -z "$log" ] && [ -f "$run_dir/summary.json" ]; then
	recorded="$(jq -r '.log // empty' "$run_dir/summary.json" 2>/dev/null)"
	if [ -n "$recorded" ] && [ -f "$recorded" ]; then
		log="$recorded"
	fi
fi
if [ -z "$log" ]; then
	if [ -f "$run_dir/summary.json" ] || [ -f "$run_dir/pipeline.csv" ]; then
		die "cannot find the server log for $run_dir; pass --log FILE"
	fi
	die "no *.log in $run_dir; pass --log FILE"
fi
[ -f "$log" ] || die "log not found: $log"

# --- fatal event patterns -----------------------------------------------------
# These are the recovery/loss events that invalidate a clean measurement.
event_names=(idr_needed idr_dropped local_drop sender_busy stale_decoder network_exception partial_write encode_error)
event_patterns=(
	'IDR frame needed on stream'
	'IDR frame dropped, stream'
	'local drop on stream'
	'sender busy, dropping frame'
	'stale decoder on stream'
	'Network exception'
	'partial write'
	'encode error on stream'
)

allowed() {
	[ -n "$allow" ] || return 1
	local want="$1" item
	local IFS=,
	for item in $allow; do
		[ "$item" = "$want" ] && return 0
	done
	return 1
}

fatal=0
event_report=""
for i in "${!event_names[@]}"; do
	name="${event_names[$i]}"
	pattern="${event_patterns[$i]}"
	n="$(grep -cF -- "$pattern" "$log" 2>/dev/null || true)"
	[ -n "$n" ] || n=0
	if [ "$n" -gt 0 ]; then
		if allowed "$name"; then
			event_report+="  $name=$n (allowed)"$'\n'
		else
			fatal=$((fatal + n))
			event_report+="  $name=$n FATAL"$'\n'
		fi
	fi
done

# --- pipeline sample count and median ----------------------------------------
summary="$run_dir/summary.json"
samples=""
median=""

if [ -f "$summary" ]; then
	samples="$(jq -r '.samples // empty' "$summary" 2>/dev/null)"
	median="$(jq -r '.columns.total_ms.median // empty' "$summary" 2>/dev/null)"
fi

if [ -z "$samples" ] || [ -z "$median" ]; then
	csv="$run_dir/pipeline.csv"
	if [ -f "$csv" ]; then
		# Fallback for a run where summary.json was not written.
		read -r samples median <<EOF
$(python3 - "$csv" <<'PY'
import csv
import sys

values = []
with open(sys.argv[1], newline="") as f:
    for row in csv.DictReader(f):
        try:
            values.append(float(row["total_ms"]))
        except (KeyError, TypeError, ValueError):
            pass
values.sort()
if values:
    n = len(values)
    k = (n - 1) * 0.5
    lo = int(k)
    hi = min(lo + 1, n - 1)
    med = values[lo] + (values[hi] - values[lo]) * (k - lo)
    print(n, f"{med:.3f}")
else:
    print(0, "none")
PY
)
EOF
	else
		die "no summary.json or pipeline.csv in $run_dir; run collect.py --out $run_dir first"
	fi
fi

[ -n "$samples" ] || samples=0
case "$samples" in
	''|*[!0-9]*) samples=0 ;;
esac

# --- report -------------------------------------------------------------------
printf 'run:          %s\n' "$run_dir"
printf 'log:          %s\n' "$log"
printf 'samples:      %s (min %s)\n' "$samples" "$min_samples"
if [ -n "$median" ] && [ "$median" != "none" ]; then
	printf 'median total: %s ms (max %s)\n' "$median" "$max_median"
else
	printf 'median total: n/a (max %s)\n' "$max_median"
fi
if [ -n "$event_report" ]; then
	printf 'events:\n%s' "$event_report"
else
	printf 'events:       none\n'
fi

if [ -f "$summary" ]; then
	truncations="$(jq -r '.truncations // 0' "$summary" 2>/dev/null)"
	if [ "${truncations:-0}" != "0" ]; then
		printf 'warning:      log was truncated/rotated %s time(s)\n' "$truncations"
	fi
fi

failed=0
if [ "$fatal" -gt 0 ]; then
	printf 'FAIL: %s fatal event line(s) in the log; this run is not a clean measurement.\n' "$fatal"
	failed=1
fi
if [ "$samples" -lt "$min_samples" ]; then
	printf 'FAIL: only %s pipeline sample(s), need at least %s.\n' "$samples" "$min_samples"
	failed=1
fi
if [ -z "$median" ] || [ "$median" = "none" ]; then
	printf 'FAIL: could not compute a median pipeline total.\n'
	failed=1
elif ! awk -v a="$median" -v b="$max_median" 'BEGIN { exit !(a <= b) }'; then
	printf 'FAIL: median total %s ms exceeds the %s ms threshold.\n' "$median" "$max_median"
	failed=1
fi

if [ "$failed" -ne 0 ]; then
	exit 1
fi

printf 'PASS\n'
exit 0
