# Performance notes

This page records the performance work done on the camera-passthrough fork and how
to reproduce the measurements. It is a work in progress: the low-risk fixes are
in, the architectural ones are listed at the end.

## Benchmark

`tools/bench.sh <label> [seconds]` runs a fixed VR workload and prints:

- server CPU (% of one core, worker process)
- headset CPU (% of one core) and the five busiest threads
- camera frames/s for `/dev/video10` and `/dev/video11`
- dropped/incomplete video frames from logcat
- a screenshot of the WiVRn statistics page

The workload is `hello_xr -g Vulkan -s Local` by default; `VRBENCH_APP=wayvr` gives
a real stream with a running app. The server is started if none is running
(`SERVER_BIN` overrides the binary).

```bash
# terminal 1: a server built with the features you want to test
./build-server-opt/server/wivrn-server

# terminal 2
VRBENCH_APP=wayvr tools/bench.sh baseline 20
```

Reading the numbers:

- the headset CPU is dominated by the render thread (`Thread-3`) and the decoder
  (`HwBinder`/`NDK`/`ImageReader` threads);
- `frames not sent` / `late shards` must stay at 0 on a healthy link;
- the statistics page shows CPU time, GPU time (defoveate), network rate and
  "estimated motion to photons latency".

## Baseline (before this branch)

Measured on the same machine (RTX 4080 SUPER, Quest 3, Wi-Fi 6 at 1200 Mbit/s,
RSSI -37 dBm):

| Metric | Value |
|---|---|
| Encoder | NVENC AV1 10-bit, 1280x1344/eye, 24.6 Mbit/s |
| Client config | `resolution_scale=1.2`, `stream_scale=0.5`, 90 Hz, 10-bit |
| Camera A (`/dev/video10`, EPOS, bulk) | 30.0 fps, p95 40 ms |
| Camera B (`/dev/video11`, iso) | 7.8 fps, p95 131 ms — device-limited, **0 frames dropped** |
| Camera uplink (TCP control socket) | 6.0 Mbit/s |
| Control-socket RTT | 27.5 ms SRTT / 2.3 ms min |
| Server CPU (lobby) | 6.5 % |
| Headset CPU (lobby) | 58 % |
| Motion to photons | ~70 ms |
| GPU defoveate | 2-7 ms |
| Wi-Fi retries | 1.1 % |

Camera B's 7.8 fps is not a passthrough bug: the UVC counters show 0 dropped
frames and the frame intervals are a rock-steady 128 ms. The camera itself is
pacing (it is a cheap UVC module in a dark room, likely exposure-limited). Camera
A does 30 fps on the same bus.

## What changed on this branch

| Change | Why |
|---|---|
| `poll_actions()` moved after `xrWaitFrame()` | input was one full frame stale (~11 ms at 90 Hz) |
| `shard_accumulator`: O(1) completeness (`contiguous`), reused payload vector, no per-shard `shared_lock` | the receive path allocated a vector **per shard** (~225-450/frame) and rescanned the whole frame per shard |
| MediaCodec `low-latency`, QTI low-latency key, `max-input-size` | the default decoder keeps extra internal buffering (~1 frame) |
| Partial-frame input (`AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME`, 32 KB threshold) with automatic fallback | decode started only after the whole frame had arrived (4-12 ms at 100-200 Mbit/s) |
| Non-blocking codec input (`try_pop`) and buffer return on queue failure | a full input pool blocked the receive thread forever |
| Camera decode moved to a per-camera worker thread, `AVPacket`/`AVFrame` reused | MJPEG decode (1.5-6 ms/frame) ran on the session thread and delayed tracking |
| sRGB `pow()` → polynomial in the reprojection shader | ~3 transcendentals per pixel over a full eye |
| Server built at `-O3` (flake no longer forces `-Og`); APK at `-O3` | `enableDebugging` injected `-Og` into every server TU (2.4x on the camera conversion loop) |
| UDP send buffer 5 MB → 512 KB, DSCP EF | bounded queueing delay under congestion; Wi-Fi WMM video AC |
| Render thread priority -4 (Android) | keep the render thread ahead of decode/network/camera threads |
| Loss-path logs demoted to debug | the packet-loss path logged several lines per frame on the network thread |

## Results after

Same workload (`wayvr` streaming, 2 cameras, 20 s runs):

| Metric | Before | After |
|---|---|---|
| Camera A fps | 30.0 | 30.0 |
| Camera B fps | 7.8 | 7.8 (device-limited) |
| Dropped frames | 0 | 0 |
| Server CPU (streaming) | 13.2 % (-Og) | 13.0 % (-O3) |
| Headset CPU (streaming) | ~104 % | ~104 % |
| Motion to photons | ~70 ms | 67-74 ms |

The CPU counters did not move much: the stream is GPU/NVENC-bound and the
remaining CPU is spread across decode and compositing. The wins on this branch are
latency and jitter (input, decode start, session-thread stalls), which these
counters do not show. Measuring them needs the feedback timestamps
(`encode_begin` … `displayed`) or a high-speed camera on the panel.

## Where the remaining time goes

From the reviews (ranked by expected gain):

1. **Server `layer_commit` waits for the GPU every frame** (`compositor.cpp`) to
   read timestamp queries that only feed debug graphs. Removing it needs
   double-buffered command buffers/descriptor sets/squash target; worth ~1-2 ms
   and real CPU/GPU overlap.
2. **Encoder and sender are serialised** (`video_encoder::sender::wait_idle`,
   one bitstream buffer, synchronous NVENC). Per-slot bitstream buffers and
   non-blocking `present_image` would remove 1-3 ms under load.
3. **The client blit target is 1.2x the panel** (`resolution_scale`). The extra
   detail is discarded by the panel; capping the client swapchain at 1.0 would cut
   ~30 % of the defoveate fill (2-7 ms GPU).
4. **Camera frames share the TCP control socket** with feedback. A camera frame
   can delay feedback and, on a stalled socket, the session. Moving camera frames
   to the UDP stream path (they are all keyframes) removes that coupling.
5. **Adaptive bitrate**: `feedback` has no loss/queue signal, so the bitrate is a
   static slider. A simple delay-based controller would improve quality per bit
   and avoid loss→IDR amplification.
6. **Input action reads**: ~120 `xrGetActionState*` per frame; filtering to the
   bound interaction profile would cut ~80 % of them (0.2-0.5 ms/frame).

## Reproducing the A/B

```bash
# optimised server
cmake -B build-server-opt . -GNinja -DWIVRN_BUILD_CLIENT=OFF \
  -DWIVRN_BUILD_DASHBOARD=OFF -DCMAKE_BUILD_TYPE=Release -DWIVRN_USE_NVENC=ON
cmake --build build-server-opt -j$(nproc)

# old-style debug build for comparison
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_DASHBOARD=OFF
cmake --build build-server -j$(nproc)
```

Verify the flags actually differ:

```bash
llvm-dwarfdump --debug-info build-server/server/wivrn-server | grep -m1 DW_AT_producer
# must end with -O2/-O3, not -Og
```
