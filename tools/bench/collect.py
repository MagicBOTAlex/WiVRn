#!/usr/bin/env python3
"""collect.py - parse a wivrn-server log into pipeline.csv, events.csv and summary.json.

The server log has no timestamps of its own, so every row is stamped with the
wall-clock time at which this tool read the line.  Each "stream N pipeline:"
line is one row; the server prints one line per eye, so a two-eye session
produces two rows per 90-frame batch.  Do not "pair" them.

Reads only the local log file.  It never talks to adb, so it cannot disturb the
stream it is measuring.

Examples:
    # one-shot over an existing log
    tools/bench/collect.py .bench/stream-final.log

    # live, while a run is in progress (Ctrl-C to stop and write the summary)
    tools/bench/collect.py --follow .bench/stream-final.log

    # continue after a previous collect (skip the first 4 KiB of startup noise)
    tools/bench/collect.py --since-bytes 4096 .bench/stream-final.log

    # explicit output directory (check.sh expects this layout)
    tools/bench/collect.py --out .bench/run1 .bench/stream-final.log

Output (default: <log-dir>/<log-stem>.bench/):
    pipeline.csv   timestamp_iso,timestamp_epoch,stream,encode_ms,...,total_ms
    events.csv     timestamp_iso,timestamp_epoch,event,stream,detail
    summary.json   samples, columns{count,min,median,p95,max}, per_stream, events

Exit codes: 0 success, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import signal
import sys
import time
from datetime import datetime
from pathlib import Path

# --- log grammar --------------------------------------------------------------
# "stream %u pipeline: encode %.1f first-packet %.1f transfer %.1f decode %.1f
#  blit %.1f display %.1f total %.1f ms"
_NUM = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)"
PIPELINE_RE = re.compile(
    r"stream\s+(?P<stream>\d+)\s+pipeline:\s+"
    rf"encode\s+(?P<encode>{_NUM})\s+"
    rf"first-packet\s+(?P<first_packet>{_NUM})\s+"
    rf"transfer\s+(?P<transfer>{_NUM})\s+"
    rf"decode\s+(?P<decode>{_NUM})\s+"
    rf"blit\s+(?P<blit>{_NUM})\s+"
    rf"display\s+(?P<display>{_NUM})\s+"
    rf"total\s+(?P<total>{_NUM})\s+ms"
)

# (event name, regex with an optional stream capture group)
EVENT_PATTERNS = [
    ("idr_needed", re.compile(r"IDR frame needed on stream (\d+)")),
    ("idr_dropped", re.compile(r"IDR frame dropped, stream (\d+)")),
    ("local_drop", re.compile(r"local drop on stream (\d+)")),
    ("sender_busy", re.compile(r"stream (\d+): sender busy, dropping frame")),
    ("stale_decoder", re.compile(r"stale decoder on stream (\d+)")),
    ("network_exception", re.compile(r"Network exception")),
    ("partial_write", re.compile(r"partial write")),
    ("encode_error", re.compile(r"encode error on stream (\d+)")),
]
EVENT_NAMES = [name for name, _ in EVENT_PATTERNS]

# CSV columns: (summary key, pipeline column suffix)
COLUMNS = [
    ("encode", "encode_ms"),
    ("first_packet", "first_packet_ms"),
    ("transfer", "transfer_ms"),
    ("decode", "decode_ms"),
    ("blit", "blit_ms"),
    ("display", "display_ms"),
    ("total", "total_ms"),
]
PIPELINE_HEADER = [
    "timestamp_iso",
    "timestamp_epoch",
    "stream",
    "encode_ms",
    "first_packet_ms",
    "transfer_ms",
    "decode_ms",
    "blit_ms",
    "display_ms",
    "total_ms",
]
EVENT_HEADER = ["timestamp_iso", "timestamp_epoch", "event", "stream", "detail"]


def now_iso(ts: float) -> str:
    return datetime.fromtimestamp(ts).astimezone().isoformat(timespec="milliseconds")


def percentile(sorted_values: list[float], p: float) -> float:
    """Linear-interpolated percentile on an already sorted list."""
    n = len(sorted_values)
    if n == 0:
        return 0.0
    if n == 1:
        return sorted_values[0]
    k = (n - 1) * p
    lo = int(k)
    hi = min(lo + 1, n - 1)
    frac = k - lo
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * frac


def column_stats(values: list[float]) -> dict:
    if not values:
        return {"count": 0, "min": None, "median": None, "p95": None, "max": None}
    vals = sorted(values)
    return {
        "count": len(vals),
        "min": round(vals[0], 3),
        "median": round(percentile(vals, 0.5), 3),
        "p95": round(percentile(vals, 0.95), 3),
        "max": round(vals[-1], 3),
    }


def parse_pipeline(line: str) -> dict | None:
    m = PIPELINE_RE.search(line)
    if not m:
        return None
    row = {"stream": m.group("stream")}
    for key, _ in COLUMNS:
        row[key] = float(m.group(key))
    return row


def parse_event(line: str) -> tuple[str, str, str] | None:
    for name, pattern in EVENT_PATTERNS:
        m = pattern.search(line)
        if m:
            stream = m.group(1) if m.groups() else ""
            return name, stream or "", line.strip()[:300]
    return None


class Collector:
    def __init__(self, log: Path, out: Path, offset: int, truncations: int = 0):
        self.log = log
        self.out = out
        self.offset = offset
        self.truncations = truncations
        self.inode = self._inode()
        self.partial = b""
        self.rows: list[dict] = []  # in-memory stats, seeded from pipeline.csv
        self.event_counts = {name: 0 for name in EVENT_NAMES}
        self.started = time.time()
        self.out.mkdir(parents=True, exist_ok=True)
        self.pipeline_path = out / "pipeline.csv"
        self.events_path = out / "events.csv"
        self.summary_path = out / "summary.json"
        self.state_path = out / "collect-state.json"
        self._seed()
        self._open_csvs()

    # -- setup ----------------------------------------------------------------
    def _inode(self) -> int:
        try:
            return os.stat(self.log).st_ino
        except FileNotFoundError:
            return 0

    def _seed(self) -> None:
        """Load rows already in the output dir so a resumed summary is complete."""
        if self.pipeline_path.is_file() and self.pipeline_path.stat().st_size > 0:
            with self.pipeline_path.open(newline="") as f:
                for row in csv.DictReader(f):
                    try:
                        parsed = {"stream": str(row["stream"])}
                        for key, _ in COLUMNS:
                            parsed[key] = float(row[f"{key}_ms"])
                        self.rows.append(parsed)
                    except (KeyError, TypeError, ValueError):
                        continue
        if self.events_path.is_file() and self.events_path.stat().st_size > 0:
            with self.events_path.open(newline="") as f:
                for row in csv.DictReader(f):
                    name = row.get("event", "")
                    if name in self.event_counts:
                        self.event_counts[name] += 1

    def _open_csvs(self) -> None:
        self.pipe_new = not (self.pipeline_path.is_file() and self.pipeline_path.stat().st_size > 0)
        self.events_new = not (self.events_path.is_file() and self.events_path.stat().st_size > 0)
        self.pipe_f = self.pipeline_path.open("a", newline="")
        self.events_f = self.events_path.open("a", newline="")
        self.pipe_w = csv.writer(self.pipe_f)
        self.events_w = csv.writer(self.events_f)
        if self.pipe_new:
            self.pipe_w.writerow(PIPELINE_HEADER)
        if self.events_new:
            self.events_w.writerow(EVENT_HEADER)

    # -- reading --------------------------------------------------------------
    def _process_line(self, raw: bytes) -> None:
        line = raw.decode("utf-8", "replace").rstrip("\r")
        ts = time.time()
        iso = now_iso(ts)

        row = parse_pipeline(line)
        if row is not None:
            self.rows.append(row)
            self.pipe_w.writerow(
                [
                    iso,
                    f"{ts:.3f}",
                    row["stream"],
                    f"{row['encode']:.1f}",
                    f"{row['first_packet']:.1f}",
                    f"{row['transfer']:.1f}",
                    f"{row['decode']:.1f}",
                    f"{row['blit']:.1f}",
                    f"{row['display']:.1f}",
                    f"{row['total']:.1f}",
                ]
            )
            return

        event = parse_event(line)
        if event is not None:
            name, stream, detail = event
            self.event_counts[name] += 1
            self.events_w.writerow([iso, f"{ts:.3f}", name, stream, detail])

    def read_new(self) -> int:
        """Read bytes appended since the last call. Returns the number of lines."""
        try:
            st = os.stat(self.log)
        except FileNotFoundError:
            return 0

        # Truncation or rotation: start over rather than seek past EOF forever.
        if st.st_size < self.offset or (self.inode and st.st_ino != self.inode):
            self.offset = 0
            self.partial = b""
            self.truncations += 1
        self.inode = st.st_ino

        with self.log.open("rb") as f:
            f.seek(self.offset)
            data = f.read()
        self.offset += len(data)
        self._write_state()

        if not data:
            return 0

        buf = self.partial + data
        chunks = buf.split(b"\n")
        self.partial = chunks.pop()  # may be an incomplete final line
        for chunk in chunks:
            if chunk:
                self._process_line(chunk)
        self.pipe_f.flush()
        self.events_f.flush()
        return len(chunks)

    def flush_partial(self) -> None:
        if self.partial:
            self._process_line(self.partial)
            self.partial = b""
            self.pipe_f.flush()
            self.events_f.flush()

    # -- output ---------------------------------------------------------------
    def _write_state(self) -> None:
        state = {
            "log": str(self.log),
            "offset": self.offset,
            "inode": self.inode,
            "truncations": self.truncations,
        }
        tmp = self.state_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(state) + "\n")
        os.replace(tmp, self.state_path)

    def summary(self) -> dict:
        overall = {name: column_stats([r[key] for r in self.rows]) for key, name in COLUMNS}
        streams = sorted({r["stream"] for r in self.rows}, key=lambda s: int(s))
        per_stream = {}
        for stream in streams:
            subset = [r for r in self.rows if r["stream"] == stream]
            per_stream[stream] = {
                "samples": len(subset),
                "columns": {
                    name: column_stats([r[key] for r in subset]) for key, name in COLUMNS
                },
            }
        return {
            "generated_at": now_iso(time.time()),
            "log": str(self.log),
            "samples": len(self.rows),
            "streams": streams,
            "columns": overall,
            "per_stream": per_stream,
            "events": dict(self.event_counts),
            "bytes_read": self.offset,
            "truncations": self.truncations,
            "elapsed_s": round(time.time() - self.started, 1),
        }

    def write_summary(self) -> dict:
        summary = self.summary()
        tmp = self.summary_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(summary, indent=2) + "\n")
        os.replace(tmp, self.summary_path)
        return summary

    def close(self) -> None:
        self.pipe_f.close()
        self.events_f.close()


def print_summary(summary: dict, out: Path) -> None:
    total = summary["columns"]["total_ms"]
    med = "n/a" if total["median"] is None else f"{total['median']:.1f}"
    p95 = "n/a" if total["p95"] is None else f"{total['p95']:.1f}"
    mx = "n/a" if total["max"] is None else f"{total['max']:.1f}"
    events = ", ".join(f"{k}={v}" for k, v in summary["events"].items() if v) or "none"
    streams = ",".join(summary["streams"]) or "none"
    print(f"samples: {summary['samples']} (streams {streams})")
    print(f"total_ms: median {med}  p95 {p95}  max {mx}")
    print(f"events: {events}")
    if summary["truncations"]:
        print(f"warning: log was truncated/rotated {summary['truncations']} time(s)")
    print(f"wrote: {out}/pipeline.csv {out}/events.csv {out}/summary.json")


def default_out(log: Path) -> Path:
    return log.parent / (log.stem + ".bench")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="collect.py",
        description="Parse a wivrn-server log into pipeline.csv, events.csv and summary.json.",
        epilog=(
            "The log has no timestamps, so rows are stamped at read time. "
            "Each 'stream N pipeline:' line becomes one row; a two-eye session "
            "writes two rows per batch. Use a fresh --out for a fresh run."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("log", type=Path, help="wivrn-server log to read")
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="output directory (default: <log-dir>/<log-stem>.bench)",
    )
    parser.add_argument(
        "--follow",
        action="store_true",
        help="keep reading appended data until interrupted (Ctrl-C)",
    )
    parser.add_argument(
        "--since-bytes",
        type=int,
        default=None,
        metavar="N",
        help="start reading at byte offset N instead of resuming from state",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.5,
        metavar="SEC",
        help="poll interval for --follow (default: 0.5)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        metavar="SEC",
        help="with --follow, stop after SEC seconds (0 = until interrupted)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="do not print the final summary",
    )
    args = parser.parse_args(argv)

    log: Path = args.log
    out: Path = args.out or default_out(log)

    if not log.is_file() and not args.follow:
        print(f"collect.py: no such log: {log}", file=sys.stderr)
        return 2
    if args.since_bytes is not None and args.since_bytes < 0:
        print("collect.py: --since-bytes must be >= 0", file=sys.stderr)
        return 2
    if args.interval <= 0:
        print("collect.py: --interval must be > 0", file=sys.stderr)
        return 2

    out.mkdir(parents=True, exist_ok=True)
    state_path = out / "collect-state.json"
    pipeline_path = out / "pipeline.csv"

    offset = 0
    truncations = 0
    if args.since_bytes is not None:
        offset = args.since_bytes
    elif state_path.is_file():
        try:
            state = json.loads(state_path.read_text())
            same_log = not state.get("log") or os.path.realpath(
                state["log"]
            ) == os.path.realpath(log)
            if same_log and state.get("inode") in (0, os.stat(log).st_ino):
                offset = int(state.get("offset", 0))
                truncations = int(state.get("truncations", 0))
        except (OSError, ValueError, TypeError, KeyError):
            offset = 0

    has_rows = pipeline_path.is_file() and pipeline_path.stat().st_size > 0
    resuming = offset > 0
    if has_rows and not resuming:
        print(
            f"collect.py: {pipeline_path} already has samples, but there is no "
            "resume state for this log.\n"
            f"  Use a fresh --out directory, or pass --since-bytes N to append.",
            file=sys.stderr,
        )
        return 2

    size = 0
    try:
        size = os.stat(log).st_size
    except FileNotFoundError:
        if not args.follow:
            print(f"collect.py: no such log: {log}", file=sys.stderr)
            return 2
    if offset > size:
        print(
            f"collect.py: warning: --since-bytes {offset} is past EOF ({size}); "
            f"starting at {size}",
            file=sys.stderr,
        )
        offset = size

    collector = Collector(log, out, offset, truncations)

    stop = False

    def on_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    if args.follow:
        print(
            f"following {log} at byte {offset} -> {out} "
            f"(Ctrl-C to stop)",
            file=sys.stderr,
        )
        deadline = time.monotonic() + args.duration if args.duration > 0 else None
        last_size = -1
        while not stop:
            collector.read_new()
            if deadline is not None and time.monotonic() >= deadline:
                break
            # Only rewrite the summary when something changed; keep the file
            # readable by check.sh while the run is still in progress.
            try:
                current = os.stat(log).st_size
            except FileNotFoundError:
                current = last_size
            if current != last_size:
                collector.write_summary()
                last_size = current
            time.sleep(args.interval)
        collector.flush_partial()
    else:
        collector.read_new()
        collector.flush_partial()

    summary = collector.write_summary()
    collector.close()

    if not args.quiet:
        print_summary(summary, out)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BrokenPipeError:
        sys.exit(0)
