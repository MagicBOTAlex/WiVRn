# Performance handoff — NVIDIA streaming path

Status: T8 fixed in `c8af13eb` (needs a long soak to confirm), T2 done, T1/T3
closed as ~0. Branch `perf/optimize`.
Audience: whoever continues this. Read this before changing anything; several
"obvious" optimizations were tried and reverted because they traded quality or
dropped frames.

> **T8 (green screen) is fixed in `c8af13eb`.** The section below keeps the
> original analysis plus the corrections that came out of the deep re-audit —
> read the corrections, several original claims were wrong.

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
| `62d41508`, `53a3d7b9` | docs: green-screen analysis and perf-hunt follow-ups |
| `c8af13eb` | T8 correctness fixes (bounded GOP, honest feedback, codec flush), T2 parallel encode, non-blocking UDP, bounded `wait_idle`, NVENC unmap, foveation/decoder hygiene, `fps_divider` clamp |

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

**Order:** **T8 first (critical correctness).** Perf work (T1–T7) should not
start until the green-screen root cause in T8 is fixed and verified over
>10 min without a decode error. T1–T7 are unchanged below.

### T8 — Green screen every ~10 min — FIXED in `c8af13eb`

**Status: fixed and verified to build/run; long soak pending.** What landed:

| Change | File |
|---|---|
| GOP bounded to 360 frames (4 s) instead of `NVENC_INFINITE_GOPLENGTH`; intra refresh removed (it is a no-op for AV1 and cannot coexist with a finite GOP) | `server/encoder/video_encoder_nvenc.cpp` |
| `push_data` returns whether the frame was accepted; the shard accumulator only sets `sent_to_decoder` when it was | `client/decoder/decoder.h`, `android_decoder.cpp`, `shard_accumulator.cpp` |
| A dropped frame is dropped whole (remaining shards discarded), never queued with a hole | `client/decoder/android/android_decoder.cpp` |
| `on_media_error` sends the real `frame_index` and flushes the codec | `client/decoder/android/android_decoder.cpp` |
| `non_ref_frames{512, -1}` (a 2-element vector) fixed | `server/encoder/idr_handler.h` |

**Verified from the wire** (`WIVRN_DUMP_VIDEO` + `ffprobe`): before the fix,
`dump-0.av1` had **1 KEY frame in 2510 frames**, zero `INTRA_ONLY`/`SWITCH`
frames, `primary_ref_frame = 0` on every inter frame. Removing one frame makes
libdav1d fail and never recover. After the fix, 8-12 KEY frames per ~3000
frames, ~340 apart.

**Corrections to the original analysis below (the deep re-audit overturned
these):**

- Cause #3 ("Quest AV1 SWITCH-frame handling") is **wrong**. NVENC emits no
  refresh frames for AV1 at all; there is nothing for the decoder to
  mishandle. Intra refresh is configured (100/50), the caps check passes, and
  the driver silently ignores it.
- "The server sees `sent_to_decoder != 0`, advances `last_ack`" is **wrong**:
  `last_ack` advances only on `received_from_decoder`
  (`server/encoder/idr_handler.cpp:57-58`). The real hole is that a concealing
  decoder still reports `received_from_decoder`, so the `last_ack + 60` stale
  check never fires. The server can detect *no output*, never *wrong output*.
- A **single** dropped reference frame does **not** poison the stream forever:
  AV1 rotates 8 DPB slots, so it heals in well under a second. Permanence
  required *recurring* corruption — which is what the branch's `try_pop` path
  produced, because it usually submitted a **head-truncated** frame (valid
  timestamp, bitstream starting mid-frame) rather than skipping it cleanly.
- `nvEncMapInputResource` without unmap and `frameIdx = 0` are real/non-issues
  respectively, but **neither** is the green-screen cause. The map leak is
  fixed anyway (unmap after the bitstream lock).
- The ~600 s Wi-Fi `wpa_group_rekey` trigger is **unproven**: no code timer
  exists, modern CCMP hostapd defaults to 86400 s, and this PC is wired
  (`enp6s0` up, `wlp7s0` down) so a server-side EAPOL capture sees nothing.
  Use Quest `wpa_supplicant` logs or an AP-side capture to confirm.
- The stall detector (`client/scenes/stream.cpp:981`, drops to the lobby after
  1 s without a blit) proves the decoder is **alive** during the green screen:
  the corruption is in content/chroma, never "the codec stopped".

**Original analysis (kept for context):**

**Symptom:** decoding error every ~10 min; the screen goes everything green
with only moving things updating (moving luma updates, static areas stay
green). No code timer fires at 600 s — the period comes from the transport
trigger (see below), not an app timer.

**Ranked causes (five hunt agents converged):**

1. **Infinite GOP + purely feedback-driven IDR + lying feedback (highest).**
   - NVENC uses infinite GOP: `config.gopLength = NVENC_INFINITE_GOPLENGTH`
     (`server/encoder/video_encoder_nvenc.cpp:224`) and `idrPeriod =
     NVENC_INFINITE_GOPLENGTH` for H.264/HEVC/AV1
     (`server/encoder/video_encoder_nvenc.cpp:278`, `:295`, `:311`).
   - Server IDR is purely feedback-driven: `server/encoder/idr_handler.cpp:49`
     (`running` only flips to `need_idr` when `!f.sent_to_decoder`) and
     `:118-126` (`running` only forces an I-frame when `frame_index >
     last_ack + 60` — a large stale window). One dropped reference frame is
     therefore never replaced except by client feedback.
   - Client feedback can lie. `client/decoder/shard_accumulator.cpp:193-215`
     sets `sent_to_decoder` for the completed frame and forwards feedback
     even when `decoder_->push_data` silently dropped it:
     `client/decoder/android/android_decoder.cpp:224-226` (`try_pop` empty
     → `return`) and `:238-242` (oversize > buffer → `return`). The server
     sees `sent_to_decoder != 0`, advances `last_ack`
     (`server/encoder/idr_handler.cpp:57-58`), and never requests a new IDR.
     One dropped reference poisons the DPB forever (infinite GOP means no
     periodic recovery).
   - The error path cannot recover either: `on_media_error` sends feedback
     with `frame_index` unset (0) (`client/decoder/android/android_decoder.cpp:528-541`),
     which matches neither `idr_id` nor `last_ack`, so `idr_handler.cpp:28-62`
     ignores it.
2. **2-frame shard window drops late IDR.** The accumulator keeps only
   `current`/`next` (`client/decoder/shard_accumulator.cpp:121-125`,
   `:144-152`; `frame_diff > 2` resets both). An IDR on TCP
   (`server/encoder/video_encoder_nvenc.cpp:600`) racing P-shards on UDP
   (`server/encoder/video_encoder.cpp:316`) can land outside the window and be
   discarded as "old"/reset, while feedback still claims success for the
   preceding (reference-less) frame.
3. **Intra refresh interacts with Quest AV1 SWITCH frames.** `intra_refresh_period
   = 100`, `intra_refresh_cnt = 50`
   (`server/encoder/video_encoder_nvenc.cpp:249-264`, applied to all codecs at
   `:278-312`). With infinite GOP the only intra refresh is this rolling
   refresh; the Quest AV1 decoder's SWITCH-frame handling can mis-handle
   partially refreshed references, leaving a corrupt reference that decodes
   luma for moving regions but leaves static regions green until a full IDR.
4. **Transport trigger (~600 s) + blocking send.** No 600 s timer exists in
   the code. The ~10 min period matches the Wi‑Fi `wpa_group_rekey=600` rekey/
   scan visible on the wire (EAPOL). During that window, the blocking UDP send
   (`common/wivrn_sockets.cpp:355-383`, `server/driver/wivrn_connection.cpp:267`
   — 512 KB buffer, blocking socket) can stall the sender, and
   `video_encoder::sender::wait_idle` blocks the encode path
   (`server/encoder/video_encoder.cpp:84-89`, called from `:212` and `:272`),
   widening the loss window so cause #1 fires. Trigger, not root cause.
5. **Encoder hygiene (contributing, not primary):** `nvEncMapInputResource`
   every frame, never unmapped (`server/encoder/video_encoder_nvenc.cpp:550`);
   `frameIdx = 0` hardcoded on every frame (`:559`); serial `encoder_work`
   try/catch slot desync (`server/compositor/compositor.cpp:655`,
   `:670-683` — one encoder exception skips the sibling stream).

**Mechanism — why moving luma updates + static green:** the decoder DPB holds
a broken/corrupt reference. AV1 P-frames predict from that reference; blocks
that move carry enough new luma residual to re-decoder visible changes, while
static (zero-residual) blocks are copied from the green-corrupt reference and
stay green. With infinite GOP there is no periodic IDR to clean the DPB, and
(or) feedback says the corrupt frames were "sent to decoder", so the server
keeps emitting P-frames indefinitely. Only a hunted IDR (or a codec flush)
clears it.

**Why periodic ~10 min:** the Wi‑Fi group rekey/scan at ~600 s
(`wpa_group_rekey=600`) is the likely trigger that creates the loss/stall
window in which the lying-feedback poison in cause #1 occurs. It is a
trigger, not an app timer; the bug is that any single dropped reference is
fatal under infinite GOP + feedback-only IDR.

**Confirmation steps (do these first, no fixes yet):**
- `WIVRN_DUMP_VIDEO` (checked at `server/encoder/video_encoder.cpp:164`):
  dump server-side encoded frames around the failure and decode them
  offline; a corrupt/green decoded frame that is present on the wire means a
  missing reference, not a transport loss.
- Log correlation: server `frame_idx` / `last_ack` (`idr_handler.cpp:52-54`),
  "IDR frame needed" / "stale decoder" / "inconsistent IDR state"
  (`idr_handler.cpp:51`, `:121`, `:129`), client "Drop shard for old frame"
  (`shard_accumulator.cpp:122`), "data to decode is larger than decoder
  buffer" (`android_decoder.cpp:240`), "Mediacodec error"
  (`android_decoder.cpp:530`). Expect: a drop/error, then `sent_to_decoder`
  feedback with no IDR for a long run, then the green screen.
- `tcpdump` for EAPOL (group rekey) on the Wi‑Fi interface, timestamped
  against the decode error; check whether errors align with rekeys (~600 s).
  No EAPOL at error time weakens the transport-trigger half of the theory and
  focuses on cause #1/2.

**Fix tasks (after confirmation):**
- Finite `idrPeriod` — bound the GOP (e.g. a real period instead of
  `NVENC_INFINITE_GOPLENGTH` at `video_encoder_nvenc.cpp:224`, `:278`, `:295`,
  `:311`) so one bad reference self-heals even if feedback lies.
- Honest `push_data` feedback — propagate drop/oversize from
  `android_decoder.cpp:224-242` through `shard_accumulator.cpp:193-215` so
  `sent_to_decoder` is only set when the frame actually reached the decoder
  input buffer; set a distinct "dropped" feedback that `idr_handler.cpp:49`
  treats as `need_idr`.
- Send a real `frame_index` in `on_media_error`
  (`android_decoder.cpp:528-541`) — the current `frame_index=0` payload never
  matches `idr_id`/`last_ack` and is ignored; also handle the non-`MALFORMED`
  path.
- `AMediaCodec_flush` on error — flush the decoder DPB on `MALFORMED` (and
  wire it to an IDR request), rather than relying on the next feedback loop.
- Decode-confirm gate — do not advance `last_ack` (`idr_handler.cpp:57-58`) on
  `sent_to_decoder` alone; require decoder confirmation (or bound the
  `last_ack + 60` stale window at `:118-126`).
- Counter logging — log drop/oversize/`try_pop` counters with `frame_index`
  on both sides so the poison frame (`frame_index` dropped but "sent")
  is directly visible in the correlated logs.
- `nvEncMapInputResource` map-once (`video_encoder_nvenc.cpp:550` — map at
  init, unmap at destroy) and stop hardcoding `frameIdx = 0` (`:559`).
- Per-encoder try/catch in `encoder_work` (`compositor.cpp:670-683`) so one
  stream's exception cannot skip/desync the other.
- Bounded `wait_idle` (`video_encoder.cpp:84-89`) — it currently waits
  indefinitely in 100 ms slices; bound it and drop/skip rather than stalling
  the encode path.
- Non-blocking UDP send (`wivrn_sockets.cpp:355-383`,
  `wivrn_connection.cpp:267`) — make oversized/full-buffer sends drop
  (mark frame lost so feedback triggers IDR) instead of blocking.
  (Also fix `set_tos` IPV6_TCLASS, `:269` — see T7.)

**Accept:** no green screen / decode error over ≥3× the ~10 min period on the
same Wi‑Fi setup, correlated logs show an IDR following the first observed
drop, and decoded `WIVRN_DUMP_VIDEO` output has no corrupt reference.
Then (and only then) resume T1–T7.

### T1 — Client blit pipelining — CLOSED, expected gain ~0

**Why closed:** `blit` is not GPU work. It is `blitted - received_from_decoder`,
where `blitted` is stamped when the render thread picks the handle up
(`client/scenes/stream.cpp:968,981`) and `received_from_decoder` is stamped on
the ImageReader thread (`stream.cpp:667`). It is the phase wait for the render
tick (0–11.1 ms sawtooth), not a fence stall: the defoveate pass is submitted a
full display period before its fence is waited on. Double-buffering removes a
wait that is already ~0. Reopen only if a 1 Hz timer on `waitForFences`
(`stream.cpp:901`) shows >0.2 ms.

**Original plan (kept for context):**

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

### T2 — Encode both eyes concurrently — DONE in `c8af13eb`

Implemented as one worker per encoder with an epoch-ordered job queue
(`server/compositor/compositor.cpp`, `compositor.h`). This also fixes the
single `try/catch` that let one stream's exception starve the others (which
hung `xrEndFrame` after two frames) and the `images[i].busy` leak on exception.

Measured after: per-stream `encode` rises (the three sessions now contend for
one NVENC engine) while the client-visible `total` dropped 32.4 → 30.8 ms.
Still to measure: whether two sessions really overlap on this GPU (Perfetto is
unavailable, `perfetto.h` missing from the flake).

**Original plan (kept for context):**

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

### T3 — Remove the tiled→linear detile copy — CLOSED, expected gain ~0

**Why closed:** the copy moves 5.16 MB/eye (10.3 MB/frame read+write) on a
4080 SUPER: 15–60 µs even pessimistically, and it is already inside the
measured `encode` value because `encode()` waits on the copy fence. The
"0.5–1 ms" estimate was an order of magnitude off on bandwidth. The
`nvEncMapInputResource` leak in this path was fixed separately (unmap after the
bitstream lock).

**Original plan (kept for context):**

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

### T6 — Compositor hygiene — DONE in `c8af13eb`

- The duplicate 64 KB UBO copy is gone; `update_ubo` now snapshots under the
  mutex and computes outside it (the network thread needs that mutex to deliver
  eye-tracked angles), and `fill_param_2d` reuses two thread-local vectors
  instead of allocating eight per frame.
- `layer_squasher.cpp`: the reference FOV is measured before `fovs[view]` is
  overwritten, so the crop ratio is no longer 1 by construction. Note this path
  is dead for single-projection apps (they take the fast path in
  `compositor.cpp:415-436`), so it only matters for multi-layer apps, and the
  `min_size` clamp still limits how far the viewport can shrink.

---

### T7 — Transport (p99 only)

- `server/driver/wivrn_connection.cpp:267`: `set_send_buffer_size(512*1024)`;
  the kernel doubles it to 1 MB ≈ 165 ms of video at 48 Mbit/s. The socket is
  **blocking**, so a full buffer blocks the sender rather than dropping. If you
  lower it, measure `frames not sent` and p99 together.
- `:269`: `set_tos(0xB8)` uses `IP_TOS` on an `AF_INET6` socket; it only works
  for v4-mapped peers. Add `IPV6_TCLASS` and do not throw on failure.

Do not bother with encryption (6.7 µs/frame) or `sendmmsg` batching (2.4 µs/frame).

**Partly done in `c8af13eb`:** UDP is now non-blocking (a full buffer drops
datagrams instead of blocking the sender), the send buffer is 64 KB instead of
512 KB, `wait_idle` is bounded at 500 ms and drops the frame, the sender pops
its queue unconditionally, and `set_tos` also sets `IPV6_TCLASS`.

---

### T9 — Measure the pacer, not the stages (do this before any further tuning)

`total` is not a sum of work: it is `displayed - encode_begin`, and `displayed`
is a vsync slot chosen by `pacer::predict` (`server/compositor/pacer.cpp:96-126`).
The schedule moves in whole display periods (11.1 ms at 90 Hz), and
`safe_present_to_decoded_ns` is the **p99.5 over the last 5000 frames (~55 s)**
(`pacer.cpp:47-71`). So a 300 ms network stall pins the schedule one frame
later for the next minute — a sustained +11.1 ms.

Consequence: making a stage faster in the median moves nothing. Only tail
reduction (or not dropping frames) can step the schedule earlier.

**Do:** log at 1 Hz from `pacer::predict`: `predicted_client_render - now`,
`safe_present_to_decoded_ns`, `mean_wake_up_to_present_ns`,
`mean_render_to_display_ns`. Those four numbers explain the whole latency
figure and make every other measurement interpretable.

Also add counters for the silent drops: `compositor.cpp:313` (encoder busy /
disconnected / no clock), `:320` (`acquire_image() < 0`), and the sender's
frame skip. The pipeline stats exclude dropped frames, so the current numbers
are survivorship-biased.

### T10 — Remaining measured items (small, low risk)

- `client/scenes/stream.cpp:910-915`: `getResults(..., eWait)` is redundant
  (the fence at `:901` already covers the command buffer) and the timestamp
  block runs even with the stats overlay closed. Gate it on overlay visibility.
  10–40 µs.
- `server/compositor/shaders/foveation.comp:56-63`: `pow()` per pixel
  (~10 M/frame). Measured cost ~11 µs, so this is not worth a quality risk —
  skip unless the shader is being touched anyway.
- `client/scenes/stream.cpp:1269-1273`: the client sends foveation angles every
  frame, so the server's `update_ubo` never early-outs. Quantising the angles
  would make the fast path hit; the server-side cost is now ~40 µs (computed
  outside the mutex, no duplicate copy).

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
