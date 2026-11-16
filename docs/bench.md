# Benchmarking WiVRn

This page is the measurement protocol, not a feature guide. The tools live in
`tools/bench/` and wrap the existing single-session runner `tools/stream-test.sh`
without duplicating it.

```
preflight.sh  ->  stream-test.sh  ->  collect.py  ->  check.sh
 clean env         one session        CSV + summary     pass/fail gate
```

## Tools

### `tools/bench/preflight.sh`

Checks that a run can start cleanly, and stops before anything is measured if
it cannot. It never starts or kills processes.

- Probes `$XDG_RUNTIME_DIR/wivrn/comp_ipc` by **connecting** to it. A socket file
  can outlive a crashed server, and `pgrep -f wivrn-server` also matches the
  shell that contains the pattern, so neither is a reliable test.
- Fails if a running `wivrn-server`'s `/proc/<pid>/exe` ends in ` (deleted)`:
  the process is still executing the old binary after a rebuild.
- Fails unless `adb devices` lists `HEADSET` in the `device` state.
- Reads the headset config with
  `adb shell run-as org.meumeu.wivrn.local cat files/client.json` and compares
  `resolution_scale` and `bitrate_bps` with the expected values.

Every failure prints `PROBLEM:` and `FIX:` lines. Exit code 1 means do not run.

```sh
tools/bench/preflight.sh
EXPECTED_RESOLUTION_SCALE=1.0 EXPECTED_BITRATE_BPS=35000000 tools/bench/preflight.sh
```

Environment: `HEADSET`, `WIVRN_PKG`, `SERVER_BIN`,
`EXPECTED_RESOLUTION_SCALE`, `EXPECTED_BITRATE_BPS`, `XDG_RUNTIME_DIR`.

### `tools/bench/collect.py`

Parses a `wivrn-server` log into a run directory:

| File | Contents |
|---|---|
| `pipeline.csv` | one row per `stream N pipeline:` line: read-time wall clock, stream, encode, first-packet, transfer, decode, blit, display, total (ms) |
| `events.csv` | one row per recovery/loss event: IDR needed/dropped, local drop, sender busy, stale decoder, network exception, partial write, encode error |
| `summary.json` | sample count, min/median/p95/max per column (overall and per stream), event counts |

The server log has no timestamps, so the timestamp is the time the line was
read. That is enough for ordering and for correlating with a wall clock, not for
sub-millisecond attribution.

The server prints one pipeline line per eye. A two-eye session therefore writes
**two rows per 90-frame batch**. Do not pair or deduplicate them; each line is
already an independent 90-frame average. A three-stream session writes three.

```sh
# one-shot over an existing log; output defaults to <log-dir>/<log-stem>.bench/
tools/bench/collect.py .bench/stream-final.log

# live: incremental, keeps summary.json current, Ctrl-C to stop
tools/bench/collect.py --follow .bench/stream-final.log

# explicit run directory for check.sh
tools/bench/collect.py --follow --out .bench/run-a .bench/stream-final.log

# skip startup noise / continue after a previous collect
tools/bench/collect.py --since-bytes 4096 --out .bench/run-a .bench/stream-final.log
```

`--follow` remembers its byte offset in `<out>/collect-state.json` and resumes
there on restart. If the log shrinks or is replaced, it starts from zero and
counts a truncation in `summary.json`; rows already written are kept, so use a
fresh run directory if a truncation happens between sessions. Use a fresh
`--out` for a fresh run: appending a re-read of the same log to an existing
directory would duplicate rows, so `collect.py` refuses that unless
`--since-bytes` is given.

`--duration SEC` stops a `--follow` run after a fixed time. Without `--follow`
the log is read once and the summary is written immediately.

It reads only the local log. It never calls `adb`, so it cannot stall the stream.

### `tools/bench/check.sh`

Post-run gate. It fails when:

- the server log contains a fatal event pattern (the eight events above);
- there are fewer than `--min-samples` pipeline rows (default 30);
- the median `total_ms` exceeds the threshold.

```sh
tools/bench/check.sh .bench/run-a 32
tools/bench/check.sh --min-samples 60 --max-median-ms 32 --allow idr_needed .bench/run-a
```

`--allow` downgrades named events to warnings. Keep the default strict: a clean
measurement has no loss or recovery events after the first few seconds. If
`idr_needed` appears only at session start, pass `--allow idr_needed` and say so
in the notes; if it appears mid-run, the run is not clean.

The log is found as `<run-dir>/server.log`, or as the only `*.log` in the run
directory, or at the path recorded in `summary.json`; otherwise pass `--log
FILE`.

## Measurement rules

1. **Never run `adb logcat -d` during a measurement.** A dump can be over a
   megabyte, saturates Wi-Fi, and stalls the stream you are measuring. Read the
   local server log instead. If you need logcat, take it before or after the
   run, never in the sampling window.
2. **Close the in-headset statistics overlay.** It costs several milliseconds of
   render-thread time per frame and inflates every number. The pipeline stats
   come from feedback timestamps and do not need the overlay.
3. **One server, always.** A second `wivrn-server` unlinks the first one's IPC
   socket. Local OpenXR apps then fail with `XR_ERROR_RUNTIME_UNAVAILABLE`, or
   silently talk to the orphaned inode. `preflight.sh` refuses to run in that
   state; `stream-test.sh` kills by exact process name (a pid from `pgrep -x`),
   never with `pkill -f`.
4. **Fix the workload.** Same app, same scene, same head position, same
   resolution scale, codec and bitrate. Use `preflight.sh` to verify the client
   config. A benchmark that changes two variables measures neither.
5. **Interleave A/B.** Run A, B, A, B, ... and compare paired runs. GPU clocks,
   thermal state and Wi-Fi all drift over tens of minutes; a single A then a
   single B is not a comparison.
6. **Discard warm-up.** The first samples of a session are not steady state
   (shader compilation, encoder start, the initial 400-500 ms frame). Start
   `collect.py` after the stream is running, or use `--since-bytes`, and ignore
   the first seconds.
7. **Report per-frame percentiles, not means.** A mean hides the stutters that
   users feel. For frame times, report p50/p95/p99 from a trace; a good median
   with a bad p95 is a latency problem, not a throughput problem. The server's
   `pipeline` line is already a 90-frame average, so percentiles across those
   lines describe drift and multi-second stalls (for example the 498 ms startup
   frame), not per-frame stutter. `summary.json` reports percentiles across
   those averages; use a trace for per-frame numbers. `max` is diagnostic only:
   one startup frame must not be averaged into a 30 ms steady state.
8. **Sample locally.** The only per-frame data needed is already in the server
   log. Do not poll the headset or take screenshots during the window.
9. **Do not screenshot during the window.** A 9 MB `screencap` moves over the
   same Wi-Fi link as the stream. Take it before or after.
10. **Gate every run.** A run that is not gated is an anecdote. `check.sh`
    makes the accept/reject decision before the numbers are compared.

## Traps, and what to do instead

| Trap | Why it ruins the measurement | Do this instead |
|---|---|---|
| `adb logcat -d` | up to 1.25 MB over Wi-Fi; stalls the stream | read the local server log; logcat only outside the window |
| second `wivrn-server` | unlinks `$XDG_RUNTIME_DIR/wivrn/comp_ipc`; local apps break | `preflight.sh`; one server; exact-name kill |
| `pkill -f wivrn-server` | the pattern matches the calling shell; it kills itself | `pgrep -x wivrn-server`, then `kill <pid>` |
| pipeline lines have no timestamp | you cannot align samples with a workload change | `collect.py` stamps rows at read time; note the wall clock when changing the workload |
| 9 MB screenshots | consumes Wi-Fi and CPU during the run | screenshot before/after, never in the window |
| statistics overlay | several ms of render-thread time per frame | close it; use server-side pipeline stats |
| averaging all frames | hides p95 stutter and startup outliers | median + p95; discard warm-up |
| A/B in one pass | thermal/clock drift is attributed to the change | interleave A/B/A/B and compare pairs |
| stale server binary | measures the old build after a rebuild | `preflight.sh` checks `/proc/<pid>/exe` for ` (deleted)` |
| wrong client config | changes resolution/bitrate between runs | `preflight.sh` compares `client.json` with the expected values |
| `summary.json` from another run | gates the wrong data | one run directory per run; `collect.py` refuses ambiguous appends |

## Typical session

```sh
# 1. clean environment
tools/bench/preflight.sh

# 2. one session, local-only sampling (existing runner)
tools/stream-test.sh --app "steam -applaunch 620980" --seconds 120 --label a1

# 3. parse and gate
tools/bench/collect.py --out .bench/a1 .bench/stream-a1.log
tools/bench/check.sh .bench/a1 32

# 4. repeat for B, then interleave again
```

If `check.sh` fails, do not compare the numbers. Fix the cause (loss event,
too few samples, median over threshold) and rerun.
