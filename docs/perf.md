# Performance notes (NVIDIA streaming path)

Scope: the PC-side stream (NVENC encode → send) and the client decode/present
path. Camera passthrough is out of scope.

## Where the 50 ms actually goes

`WIVRN_PIPELINE_STATS=1 wivrn-server` prints a per-stream breakdown averaged over
90 frames, using the timestamps the headset already sends back in `feedback`:

```
stream 0 pipeline: encode 2.1  first-packet 1.7  transfer 0.0  decode 4.5  blit 8.4  display 37.0  total 53.7 ms
stream 1 pipeline: encode 1.6  first-packet 1.7  transfer 0.0  decode 4.2  blit 7.1  display 37.0  total 51.6 ms
```

| Stage | Time | What it is |
|---|---|---|
| encode | 1.6-2.1 ms | NVENC AV1 10-bit, 1280x1344 |
| first packet | 1.7 ms | encode end → first shard on the headset |
| decode | 4.2-4.5 ms | MediaCodec, whole frame |
| blit | 7.1-8.4 ms | decode done → defoveate submitted |
| display | ~37 ms | submitted → photons (the frame waits for its display slot) |
| **total** | **~52 ms** | encode start → photons |

The `display` term is mostly *slack*: the frame is ready about 37 ms before the
headset's compositor shows it. The active work is encode 2 + network 2 + decode
4.5 + blit 8 ≈ 17 ms, plus the app's own render time and the compositor's present
latency.

This is why the encoder is not the lever: a perfect NVENC would save ~2 ms out of
52. The controllable client terms are `blit` (8 ms) and `decode` (4.5 ms).

## What this branch changes (kept)

| Change | Effect | Quality |
|---|---|---|
| `layer_commit`: two command buffers/query pools, wait for the previous frame at the *start* instead of the current frame at the end | removes a GPU wait from the synchronous `xrEndFrame` IPC round trip; the GPU has a whole frame to finish, so the start wait is normally zero | neutral |
| `poll_actions()` after `xrWaitFrame()` | input is no longer one frame stale (~11 ms at 90 Hz) | neutral |
| `shard_accumulator`: O(1) completeness, reused payload buffer, no per-shard lock | removes ~450 allocations/frame on the receive thread | neutral |
| MediaCodec: low-latency mode, `max-input-size`, non-blocking input buffer with return-on-failure | decoder no longer buffers an extra frame; receive thread can never stall on a full input pool | neutral |
| Server built `-O3` (flake no longer forces `-Og`), APK `-O3` | 2.4x on per-pixel server loops | neutral |
| UDP send buffer 512 KB (kernel doubles it to 1 MB), DSCP EF | bounds queueing somewhat | neutral |
| sRGB `pow()` → polynomial in the reprojection shader | max error 0.019/255 | neutral |

## Reverted (were wrong)

- **Client blit target capped at the panel.** This reduced submitted resolution
  (2476x2594 → 2064x2162), i.e. it removed supersampling. Not an optimization.
- **Encoder drops a frame when the sender is busy.** Frame loss, and it can stall
  the IDR state machine for ~1.1 s. `wait_idle` is restored.
- **3 encoder slots.** The compositor only owns 2 images, so the third slot did
  nothing but use VRAM.
- **MediaCodec partial frames.** At 34 KB/frame the 32 KB threshold split every
  frame into two buffers; no measured benefit, and abandoned partial frames could
  corrupt the next frame.
- **Camera `can_write()`.** v4l2loopback returns `POLLOUT` unconditionally on an
  output node, so the check was a no-op (one syscall per frame). Removed.

## Next, in order

1. **Client blit (8 ms).** Time `waitForFences` / `swapchain.wait()` first; if
   they are significant, double-buffer the blit command buffer/fence so the CPU
   does not wait for the previous frame's GPU work.
2. **Encode the two eyes concurrently** (`compositor::encoder_work` calls
   `encode()` serially; NVENC AV1 runs ~90% in parallel on two sessions). ~1.4 ms.
3. **Detile copy**: `vkCmdCopyImageToBuffer` converts the tiled YCbCr image to a
   linear buffer for NVENC every frame (0.25-0.5 ms/eye). Measure with Perfetto
   (`nvenc_copy` slice; requires `-DWIVRN_USE_PERFETTO=ON`, currently off) before
   rewriting the foveation shader to store into the NVENC buffer directly.
4. **Display scheduling** (37 ms slack) is the largest term but lives in the
   headset compositor's present path, not in WiVRn.

## Benchmark

`tools/bench.sh <label> [seconds]` runs a fixed VR workload, samples server and
headset CPU per thread, camera fps, frame loss, and saves the statistics
screenshot. Use `VRBENCH_APP=wayvr` for a real stream. Compare CPU time per frame
from the overlay, not process-wide CPU% (which swings 30-50% with thermal state).
