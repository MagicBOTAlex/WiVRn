# Performance handoff — NVIDIA streaming path

Status: work in progress, branch `perf/optimize` (HEAD `ee5922e9`).
Audience: whoever continues this. Read this before changing anything; several
"obvious" optimizations were tried and reverted because they traded quality or
dropped frames.

---

## 1. Objective and scope

Make the **PC → headset stream** faster: lower motion-to-photon latency, fewer
missed frames, more headroom.

In scope:

- NVENC encode and the GPU→encoder handoff (`server/encoder/*`)
- compositor frame submission (`server/compositor/*`)
- transport (`common/wivrn_sockets.*`, `server/driver/wivrn_connection.cpp`)
- client decode and present (`client/decoder/*`, `client/scenes/stream*.cpp`)

**Not** in scope:

- camera passthrough (`server/camera/*`, `client/android/uvc.cpp`,
  `client/android/camera.cpp`, `client/java/UsbCamera.java`) — leave it alone.
- Any change that reduces submitted resolution, bitrate, bit depth, refresh rate
  or frame rate. That is not an optimization here.

Environment: RTX 4080 SUPER (driver 615), NixOS, Quest 3 at 90 Hz over Wi-Fi 6,
`resolution_scale=1.2`, `stream_scale=0.5`, 50 Mbit/s, NVENC AV1 10-bit.

---

## 2. Current state

### What is in `perf/optimize`

| Commit | Content |
|---|---|
| `b44c08e8` | actions synced after `xrWaitFrame`; shard-path allocation/lock removal; MediaCodec low-latency + `try_pop`; camera decode worker (out of scope but harmless); `-O3`; socket buffer; sRGB polynomial |
| `236b189e` | **mostly reverted** — blit-target cap (resolution cut) and encoder frame-dropping |
| `ee5922e9` | reverts the two above; adds compositor double-buffering and the pipeline breakdown |

### What is deliberately reverted

- **Blit-target cap** (`client/scenes/stream.cpp`, `client/scenes/stream_defoveator.cpp`).
  It reduced submitted resolution from 2476×2594 to 2064×2162 (no supersampling).
- **Encoder frame drop when the sender is busy** (`server/encoder/video_encoder.cpp`).
  Frame loss; could also stall the IDR state machine for ~1.1 s.
- **3 encoder slots** (`server/encoder/video_encoder.h`). The compositor owns only
  2 images, so the third slot was dead weight.
- **MediaCodec partial frames** (`client/decoder/android/android_decoder.cpp`).
  At ~34 KB/frame the 32 KB threshold split every frame; no measured gain.
- **Camera `can_write()`** (`server/camera/camera_v4l2.cpp`). v4l2loopback returns
  `POLLOUT` unconditionally on an output node; it was a no-op syscall.

### What is kept and should not be undone

- `poll_actions()` after `xrWaitFrame()` (`client/application.cpp`): removes one
  frame of input staleness (~11 ms at 90 Hz).
- `shard_accumulator` O(1) completeness + reused payload buffer
  (`client/decoder/shard_accumulator.*`): ~450 allocations/frame removed.
- MediaCodec `low-latency`, `max-input-size`, non-blocking input buffer with
  return-on-failure (`client/decoder/android/android_decoder.cpp`).
- `-O3` server/APK (`flake.nix`, `build.gradle`): the flake no longer wraps the
  package in `enableDebugging` (which forced `-Og`).
- sRGB polynomial (`client/shaders/reprojection.glsl`): max error 0.019/255.
- Compositor two-frames-in-flight (`server/compositor/compositor.*`): the app no
  longer waits for the GPU inside `xrEndFrame`.

---

## 3. Where the time goes

Measured with `WIVRN_PIPELINE_STATS=1` (90-frame averages, wayvr streaming):

```
stream 0 pipeline: encode 2.1  first-packet 1.7  transfer 0.0  decode 4.5  blit 8.4  display 37.0  total 53.7 ms
stream 1 pipeline: encode 1.6  first-packet 1.7  transfer 0.0  decode 4.2  blit 7.1  display 37.0  total 51.6 ms
```

- `encode`: NVENC AV1 10-bit, 1280×1344.
- `first-packet`: encode end → first shard on the headset.
- `decode`: MediaCodec, whole frame.
- `blit`: decode done → defoveate submitted (includes waiting for the render
  thread and for both eyes).
- `display`: submitted → photons. Mostly **slack** (the frame is ready ~37 ms
  before its display slot).
- `total`: encode start → photons.

Active work is ~12–16 ms; the rest is waiting. **The encoder is ~2 ms of ~50 ms.**
That is why encoder micro-optimizations do not move the number.

Reproduce:

```bash
LD_LIBRARY_PATH=/run/opengl-driver/lib XRT_LOG=info WIVRN_PIPELINE_STATS=1 \
  ./build-server-opt/server/wivrn-server
VRBENCH_APP=wayvr tools/bench.sh baseline 20
grep 'pipeline: encode' .build/server-bench.log | tail
```

---

## 4. Plan (do in this order)

### T1 — Client blit pipelining (biggest controllable client term: 4–8 ms)

**Where:** `client/scenes/stream.cpp:901` (`waitForFences` at the top of
`render()`), one command buffer/fence/query pool in `client/scenes/stream.h`,
`client/scenes/stream.cpp:927` (`common_frame`), `:950` (`ensure_swapchain`).

**Now:** one frame in flight. The CPU cannot record frame N until frame N-1's
blit has finished on the GPU, so frame time is CPU + GPU instead of max(CPU, GPU).

**Do first:** measure. Add a 1 Hz log of the `waitForFences` duration at
`stream.cpp:901` and the swapchain wait inside `xr::swapchain::acquire`. If the
fence wait is < 0.2 ms, skip T1 and go to T2.

**Change:** two command buffers, fences and query pools indexed by frame parity;
wait on the fence from two frames ago. Keep the `current_blit_handles` and the
defoveator descriptor sets per slot so the GPU can still read them.

**Expected:** 0.5–2 ms. **Risk:** medium — the runtime may not hand out a second
swapchain image while one is in flight; verify `xrWaitSwapchainImage` does not
block.

**Accept:** pipeline `blit` median −≥0.5 ms over 3 interleaved A/B runs, p95 not
worse, `frames not sent` unchanged.

---

### T2 — Encode both eyes concurrently (~1.4–2 ms)

**Where:** `server/compositor/compositor.cpp:655` (`encoder_work`), the serial
loop at `:670` calling `encoder->encode(...)`; `server/encoder/video_encoder.cpp:251`.

**Now:** one encoder thread encodes stream 0, then stream 1, then alpha. Each
`encode()` blocks in `nvEncEncodePicture` + `nvEncLockBitstream` (~1.4 ms).
Two NVENC AV1 sessions run ~90 % in parallel on this GPU, so ~1.4 ms is lost.

**Change:** one worker per stream (or a 2-worker pool). `encode_request` becomes
per-stream; `layer_commit` signals all workers after `present_image`. Everything
an encoder touches is per-instance (`shard`, `timing_info`, `clock`, `state`);
only `sender` is shared (already mutex-protected).

**Expected:** ~1.4 ms. **Risk:** medium-low — the NVENC shared state/CUDA context
may serialize internally. Measure; do not assume.

**Verify:** Perfetto `encoder` track (requires `-DWIVRN_USE_PERFETTO=ON`, see
§7); the two `encode` spans must overlap. The `encode_begin→encode_end` per-stream
values should stay the same; the gap between the two streams' `encode_end` shrinks.

---

### T3 — Remove the tiled→linear detile copy (0.5–1 ms)

**Where:** `server/encoder/video_encoder_nvenc.cpp:437` (`vkCmdCopyImageToBuffer`),
`:550` (`nvEncMapInputResource` every frame, never unmapped), `:342` (the linear
buffer + CUDA import).

**Now:** the foveation pass writes a tiled (`eOptimal`) two-plane YCbCr image;
every frame it is detiled into a linear buffer for NVENC (5.16 MB/eye) on the
graphics queue, and the input resource is mapped again.

**Do first:** measure the `nvenc_copy` GPU slice (Perfetto build). If < 0.15 ms/eye,
skip.

**Change, cheap:** map each input resource once at init, unmap at destroy; move
the copy to a transfer queue (needs an acquire barrier on the target queue —
`compositor.cpp:422` already emits the release side when `need_transfer`).
**Change, big:** have the foveation shader store NV12/P010 directly into the
NVENC buffer, deleting the copy and the staging buffers.

**Expected:** cheap 0.2–0.3 ms (overlap), big 0.5–1 ms. **Risk:** big is
medium-high (shader store pattern, barriers). Verify the decoded frame is
bit-identical (`WIVRN_DUMP_VIDEO`, SHA-256 of 200 frames with a static pose).

---

### T4 — NVENC low-latency rate control (1–2 ms, quality trade — ask first)

**Where:** `server/encoder/video_encoder_nvenc.cpp:144` (`multiPass =
TWO_PASS_QUARTER_RESOLUTION`), `:204` (`PRESET_P4`), `:140` (`vbvBufferSize =
bitrate/fps*2`).

Measured on this GPU (AV1 10-bit, 1280×1344): P4+ULL+qres **1.42 ms**, multipass
off **1.06 ms**, P1 **0.83 ms**, P7 **2.42 ms**.

`multiPass` off is a small quality-per-bit loss; P1/P2 is larger. Do not do this
without explicit approval. Measure BD-rate or at least compare a static frame at
the same bitrate.

---

### T5 — Action-read filtering (0.2–0.5 ms client CPU)

**Where:** `client/scenes/stream_actions.cpp:155-227`, list built in
`client/scenes/stream.cpp:466-474` (union of every interaction profile, ~120
entries).

Query only the actions bound for the current interaction profile (already known
in `stream::interaction_profiles`). **Risk:** missing inputs on exotic profiles;
test Touch, Touch Pro, and hand interaction.

---

### T6 — Compositor hygiene (0.05–0.2 ms + fill for narrow-FOV apps)

- `server/compositor/foveation.cpp:492-493`: the same 64 KB UBO is copied twice
  (`vmaCopyMemoryToAllocation` then `memcpy`). Delete one.
- `server/compositor/layer_squasher.cpp:570`: `fovs[view] = all_layers_fov` is
  assigned *before* `w2/h2` are computed from it, so the viewport shrink ratio is
  always 1.0 and the FOV-driven fill reduction never happens. Compute the ratio
  first.

---

### T7 — Transport (p99 only)

- `server/driver/wivrn_connection.cpp:267`: `set_send_buffer_size(512*1024)`;
  the kernel doubles it to 1 MB ≈ 165 ms of video at 48 Mbit/s. The socket is
  **blocking**, so a full buffer blocks the sender rather than dropping. If you
  lower it, measure `frames not sent` and p99 together.
- `:269`: `set_tos(0xB8)` uses `IP_TOS` on an `AF_INET6` socket; it only works
  for v4-mapped peers. Add `IPV6_TCLASS` and do not throw on failure.

Do not bother with encryption (6.7 µs/frame) or `sendmmsg` batching (2.4 µs/frame).

---

## 5. Measurement protocol

- Workload: `VRBENCH_APP=wayvr tools/bench.sh <label> <seconds>`.
  `hello_xr` is the default but renders the lobby in the client; `wayvr` gives a
  real stream.
- Close the in-headset **statistics page** for CPU measurements; it costs several
  ms of render-thread time while open.
- 3 interleaved runs (A/B/A/B/A/B), 20 s each, discard the first 5 s.
- Report median and p95 of the pipeline columns, plus `frames not sent`.
- Headset CPU% swings 30–50 % between identical runs (thermal/Wi-Fi). Use
  per-frame CPU time from the overlay, not process-wide CPU%.
- Avoid the hourly Wi-Fi re-association (`:06` past the hour).

Acceptance for the whole effort: **pipeline `total` median −≥3 ms with p95 not
worse, no increase in dropped frames, no visual change** (screenshot + static
scene hash).

---

## 6. Known traps (learned the hard way)

1. The server **exits when no client is connected** (`quit_if_no_client`). Start
   it, then connect the headset promptly; `tools/bench.sh` handles this.
2. A **USB permission dialog** blocks app launches ("Launch is blocked because: a
   Reprojected OS dialog is currently showing"). Dismiss it on the headset.
3. `build-server` (the old dev-shell build) is `-Og`. Use `build-server-opt`
   (Release) for anything you measure.
4. **Perfetto is compiled out** (`WIVRN_USE_PERFETTO=OFF`), so GPU slices
   (`nvenc_copy`, `encode`) are empty. Rebuild with `-DWIVRN_USE_PERFETTO=ON` to
   see them.
5. The earlier **AppImage is VAAPI-only** (nixpkgs package has no NVENC). It
   cannot encode on NVIDIA; rebuild with `-DWIVRN_USE_NVENC=ON` if you ship one.
6. `xrEndFrame` is a **synchronous IPC round trip**; anything you add inside
   `layer_commit` is charged to the app's thread.
7. The compositor owns **2 images**; a third encoder slot does nothing.

---

## 7. Build and run

```bash
# server (Release, NVENC)
nix develop --command bash -c \
  'cmake -B build-server-opt . -GNinja -DWIVRN_BUILD_CLIENT=OFF \
   -DWIVRN_BUILD_DASHBOARD=OFF -DCMAKE_BUILD_TYPE=Release \
   -DWIVRN_USE_NVENC=ON -DWIVRN_USE_VAAPI=OFF'
nix develop --command bash -c 'cmake --build build-server-opt -j$(nproc)'

# with GPU tracing
nix develop --command bash -c 'cmake -B build-server-trace ... -DWIVRN_USE_PERFETTO=ON'
WIVRN_TRACING=inprocess WIVRN_TRACING_FILE=/tmp/wivrn-%t.pftrace ./build-server-trace/server/wivrn-server

# APK
nix-shell ../.build/shell.nix --run './gradlew --no-daemon assembleDebug'
adb install -r build/outputs/apk/debug/WiVRn-debug.apk

# connect
adb shell am start -a android.intent.action.VIEW -d "wivrn://192.168.50.58:9757" \
  -n org.meumeu.wivrn.local/org.meumeu.wivrn.MainActivity
```

---

## 8. File map

| Area | Files |
|---|---|
| compositor / frame submission | `server/compositor/compositor.cpp` (`layer_commit` 300-580, `encoder_work` 655+), `compositor.h` |
| squash / foveation | `server/compositor/layer_squasher.cpp`, `foveation.cpp`, `shaders/` |
| encoder base / slots / sender | `server/encoder/video_encoder.cpp` (`present_image` 236, `encode` 251, sender 50-95), `video_encoder.h` |
| NVENC | `server/encoder/video_encoder_nvenc.cpp` (settings 130-160, init 300-360, copy 420-500, encode 570-600) |
| transport | `common/wivrn_sockets.cpp` (UDP send 355, TCP send 518), `server/driver/wivrn_connection.cpp` |
| session / feedback | `server/driver/wivrn_session.cpp` (`feedback` 824, pipeline stats 840-880) |
| client present | `client/scenes/stream.cpp` (render 870-1290, swapchain 1368-1430), `stream.h` |
| client decode | `client/decoder/android/android_decoder.cpp`, `client/decoder/shard_accumulator.cpp` |
| client input | `client/scenes/stream_actions.cpp`, `client/application.cpp` (`poll_actions` ordering) |
| shaders | `client/shaders/reprojection.glsl`, `server/compositor/shaders/foveation.comp` |
| tools / docs | `tools/bench.sh`, `docs/perf.md` |
