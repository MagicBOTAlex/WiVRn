# WiVRn — camera passthrough fork

A fork of [WiVRn](https://github.com/WiVRn/WiVRn) with USB camera passthrough from
the headset to the PC, plus a long pass over streaming correctness and latency.

**Camera passthrough is kept and works**: up to three USB cameras on the headset
are streamed to the PC as `/dev/videoN` devices, with stable identity → device
mapping and no root required.

## Attempted

- Capped the client blit target at the panel resolution — reverted, it was a resolution cut, not an optimisation.
- Dropped encoder frames when the sender was busy — reverted, it lost frames and could stall the IDR state machine for ~1.1 s.
- Used three encoder slots — reverted, the compositor only owns two images.
- Sent MediaCodec partial frames — reverted, the threshold split every frame and abandoned partials could corrupt the next one.
- Gated camera writes on `POLLOUT` — reverted, v4l2loopback always reports writable, so it was a no-op syscall.
- De-ratcheted the pacer's schedule bump — reverted, measured 8 ms worse (50 ms vs 42 ms).
- Skipped `start_frame` for scenes without a world — reverted, the GUI draws on an empty registry.
- Hid apps from unmounted Steam libraries — reverted, it hid installed games; now keyed on the appmanifest instead.
- Shrank the ImageReader pool — reverted, it risked the input starvation that was just fixed.
- Replaced the server's sRGB `pow()` with a polynomial — skipped, measured at ~11 µs per frame.
- Double-buffered the client blit — skipped, `blit` is a phase wait for the vsync, not GPU work.
- Removed the tiled→linear detile copy — skipped, 10 MB per frame is 15–60 µs and already inside `encode`.
- Turned NVENC multipass off / used the P1 preset — not done, it is a quality-per-bit trade.
- Streamed at 4–8K per eye — tested and rejected, `resolution_scale` also scales the client's blit target, so it collapses.

## Kept

- Bounded the GOP to 4 s instead of infinite, so a lost reference heals by itself.
- Made `push_data` report whether the frame reached the decoder.
- Dropped frames whole instead of queueing a bitstream with a hole.
- Sent the real frame index, and flushed the codec, on MediaCodec errors.
- Backed off 30 frames after a lost keyframe, so a keyframe cannot become a storm.
- Applied 20 ms of backpressure instead of instantly dropping when the decoder is saturated.
- Encoded the streams on one worker each instead of serially.
- Released `images[i].busy` with a scope guard, so one exception cannot kill the stream.
- Made the UDP send non-blocking with a 192 KB buffer.
- Bounded `wait_idle` and popped the sender queue unconditionally.
- Moved client control traffic to its own writer thread.
- Chunked control keyframes at 64 KB instead of one ~1 MB write.
- Dropped camera frames when the control socket backs up.
- Unmapped the NVENC input after the bitstream lock.
- Computed foveation outside the mutex and removed the duplicate 64 KB copy.
- Checked the hardware-buffer cache before the per-frame properties query.
- Queried only active input actions.
- Added TCP keepalive, EINTR handling and partial-write poisoning.
- Took a single-instance lock so two servers cannot fight over the IPC socket.
- Kept the OpenXR runtime registered for the server's lifetime.
- Let a client that fell back to the app list re-enter the stream.
- Raised the stall threshold from 3 s to 30 s so a loading screen cannot eject you.
- Fixed the launcher's app names and icon cache.
- Clamped the client-supplied `fps_divider`.
- Declared the AV1 colour range as BT.709 full, matching the client.
- Built with `-O3` (the nixpkgs package used `-Og`).
- Added benchmark tooling and opt-in instrumentation.

## Details

### Camera passthrough

The headset's UVC cameras are captured as MJPEG, sent to the PC, decoded once
with libavcodec and converted to I420 into v4l2loopback nodes. Identity is the
USB serial, so the mapping survives replugs and enumeration order changes. No
root is needed: the app holds the USB permission and hands the usbfs fd to
libusb. Up to three cameras are supported; a camera the link cannot carry is
dropped at frame granularity, never degraded.

### The green screen

The original bug was a stream that turned green after roughly ten minutes and
never recovered. Verified from the encoded bitstream: the AV1 stream had one
KEY frame in 2510 frames and NVENC emitted no intra-refresh frames at all, so
nothing could ever repair a lost reference. The client made it permanent by
reporting every complete frame as delivered even when the decoder had silently
dropped or truncated it.

The fix is a bounded GOP plus honest feedback: `push_data` returns whether the
frame was accepted, dropped frames are dropped whole, and MediaCodec errors
carry the real frame index and flush the codec. A lost frame now costs a short
glitch instead of a restart.

### The keyframe storm

At high resolution the stream could enter a loop where every keyframe was lost,
each loss requested another, and none arrived. Fixed with decoder backpressure
(a saturated decoder throttles the receive thread instead of dropping) and a
30-frame backoff after a lost keyframe. At 3136×3264 the count went from 11
dropped keyframes to 0.

### Latency work

Measured against a stock build on the same workload: server CPU 8.5 % vs 8.0 %,
headset CPU 41.6 % vs 41.0 %, motion-to-photon 42 ms vs 43 ms. Throughput is a
wash because the pipeline is display-scheduling-bound: of ~27 ms, ~15.6 ms is
the frame waiting for its vsync slot and only ~11 ms is work (encode 2,
network 1.8, decode 3.8, blit 3.4). The wins are in failure modes, not speed.

### Instrumentation

- `WIVRN_PIPELINE_STATS=1` — per-stream encode/network/decode/blit/display/total.
- `WIVRN_PACER_STATS=1` — the pacer's controller inputs.
- `WIVRN_FEEDBACK_CSV=path` — one line per frame with every timestamp.
- `WIVRN_STALL_STATS=1` / `adb shell setprop debug.wivrn.stall_stats 1` — logs any render-thread call over 20 ms.
- `tools/stream-test.sh` — one clean session with local-only sampling.
- `tools/bench/collect.py` — log to CSV plus percentiles.

### Building

```bash
# server
nix build .            # NVENC enabled, VAAPI disabled
# headset
nix-shell ../.build/shell.nix --run './gradlew --no-daemon assembleDebug'
```

The release APK uses package `org.meumeu.wivrn.camera`, so it coexists with
upstream WiVRn.
