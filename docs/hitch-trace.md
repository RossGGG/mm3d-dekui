# Hitch trace

Dekui logs every emulated frame slower than 50 ms as a `Hitch:` warning
(src/common/hitch_trace.*). The trace is always on and costs well under 1% of
frame time, so a headset log from normal play explains its own stutter.
`tools/mm3d/hitch_summary.py azahar_log.txt` totals a whole log.

A slow frame logs up to three lines:

```
Hitch: 53.3 ms frame after 4795 smooth frames | cpu 52.0 ms (297, max 16.4) | aot 51.8 ms (298, max 16.4) | aot svc 17.5 ms (765, max 8.3) | draws 16.5 ms (220) | ... | gpu 5.4 ms (2, max 5.4) | at scene 0x43 for 1 s
Hitch calls: svc SendSyncRequest 16.9 ms (124) | gsp::Gpu TriggerCmdReqQueue 16.8 ms (110) | hook 0xfffff2 0.3 ms (6)
Hitch guest: aot 51.8 ms = code 34.4 + svc 17.5; 70 samples in 31 functions | fn 002e8940 25% ~8.6 ms (at 002e8c48 8%, 002e8af8 8%) | ...
```

## The Hitch line

Each part reads `name total ms (count, max longest)`. `max` appears when a part
ran more than once and its longest run took at least 1 ms, so one long stall
can be told from many short ones. Parts nest, so they do not add up to the
frame time.

Emulation thread:

| part | what it measures | inside |
|---|---|---|
| cpu | guest CPU slices: interpreter, AOT, and SVCs called from either | |
| aot | AOT-translated guest code, including the SVCs it calls | cpu |
| aot svc | SVCs called from AOT code: kernel, HLE services, MM3D hooks | aot |
| draws | PICA draw calls (vertex loading, CPU vertex shading, recording) | usually aot svc (GSP commands run inside SendSyncRequest) |
| texture uploads, custom textures | decoding/uploading guest textures, replacement textures | draws or aot svc |
| inline pipeline builds | cache-only pipeline lookups on the draw thread | draws |
| file reads | FS service reads | aot svc |
| gpu wait | waiting on a GPU fence: Scheduler::Wait / Finish, depth or texture readbacks | anywhere, often aot svc or present |
| worker wait | waiting for the Vulkan worker thread to record queued commands | anywhere |
| present | the whole swap (RendererVulkan::SwapBuffers) | |
| present slot | waiting for the presentation thread to free a frame | present |
| present fence | waiting for that frame's previous presentation to finish on the GPU | present |
| present finish | `scheduler.Finish()` when nothing new was presented | present |

Presentation thread (asynchronous; counted in the frame during which it finished):

| part | what it measures |
|---|---|
| pacing | midpoint-interpolation pacing sleep |
| present submit | queue lock + submit of the composition pass |
| present gpu | (visionOS stereo) waiting for the GPU to finish the frame before Metal reads it |
| deliver | (visionOS stereo) `ExternalPresent::Deliver`: copy into the RealityKit ring |
| deliver blit | the part of `deliver` waiting for that Metal copy to complete |
| swap acquire / swap present | (swapchain path: Mac, mono visionOS) `vkAcquireNextImageKHR` and `vkQueuePresentKHR` |

Other threads:

| part | what it measures |
|---|---|
| pipeline builds | pipeline compiles on worker threads (can exceed the frame time) |
| gpu | GPU time of command buffers that completed this frame (timestamp queries, top to bottom of pipe); `max` is the slowest command buffer. Results arrive a frame or two late. |

`at scene 0x43 for 12 s` is the current scene (play state +0x148, the ids the
shader tour logs) and how long ago the game entered it. Outside gameplay it is
`game state N` (6 = file select).

## Hitch calls

The HLE calls that took longest in the frame (at least 0.1 ms, top 10), with
total time and count:

- `svc <name>`: kernel SVCs (SendSyncRequest, WaitSynchronizationN, ControlMemory, ...).
- `<service> <command>`: HLE service commands, e.g. `gsp::Gpu TriggerCmdReqQueue`
  (PICA command lists: draws, display transfers, memory fills) or `fs:USER OpenFile`.
  They run inside `svc SendSyncRequest`. A bare command with a leading space
  (` Read`, ` Write`) is an FS file or directory session: those sessions have
  no service name.
- `hook 0x...`: MM3D hook SVCs (src/core/arm/mm3d_guest_hooks.cpp; e.g. the
  visibility-depth read, Classic Saving, screen shrink).

## Hitch guest

`aot X ms = code Y + svc Z` splits AOT time into translated guest code and the
SVCs it called. The samples come from a sampler thread that ticks every 0.5 ms;
each tick is charged to the AOT code running at that moment. Ticks that land
inside an SVC are dropped, because SVC time is measured exactly.

- Translated blocks chain into each other (direct branches and calls), so the
  sampler knows the **entry** of the current chain of blocks: the target of the
  last indirect branch or return. Time in a function called with BL is charged to
  its caller until the callee returns.
- `fn 002e8940` is a guess at the function start: the nearest preceding
  `push {..., lr}` before the sampled PC. Leaf functions without one merge into
  the function before them. `at` lists the sampled entry PCs inside it.
- `~8.6 ms` is the function's share of samples times `code` ms. Resolution
  is one tick (0.5 ms; about 1 ms on macOS, where sleeps overshoot), so with
  fewer than ~50 samples treat shares as rough.
- Addresses are in the Restoration-patched image (the one AOT was generated
  from), not vanilla MM3D. There is no symbol map yet. To name a function,
  disassemble it in the patched code.bin
  (~/Library/Developer/Dekui/aot/work/code.bin, loaded at 0x100000), or match
  it against the MM3D decomp by byte pattern.

## Reading a stutter

- **aot dominant, svc small**: guest code itself is slow. Look at the top `fn`s
  in `Hitch guest` across the burst (`hitch_summary.py` totals them).
- **aot dominant, svc large**: look at `Hitch calls`. `gsp::Gpu TriggerCmdReqQueue`
  plus `draws` means PICA work. `gpu wait` inside means a GPU readback stalled the
  guest (for example the visibility-depth hook or a texture download).
- **present dominant**: `present slot` / `present fence` mean the emulation thread
  waited for the presentation thread. Then look at that thread's parts in
  the same and following frames: `present gpu` (the GPU was still rendering:
  compare with `gpu`), `deliver blit` (the RealityKit copy queue was busy), or
  `pacing`. `present finish` with `gpu wait` means a full GPU drain on the
  emulation thread.

## Frame summary

Every 10 s, one `Frame summary:` line covers all frames, slow or not, so a
change that makes ordinary frames cheaper shows up even without hitches:

```
Frame summary: 599 frames in 10.0 s | frame avg 10.9 p90 22.1 max 47.9 ms | gpu avg 7.9 p90 16.1 ms | present avg 0.0 | present slot avg 0.0 | gpu wait avg 0.0 | present gpu avg 0.0 | deliver blit avg 0.0 | at scene 0x10
```

Frames are emulated frames (60 per second). `gpu` is GPU time per emulated
frame (as on the Hitch line), and the waits are averages per frame. Compare
runs scene by scene (`at`), since scenes differ widely.

## Switches

- `DEKUI_HITCH_PROFILE=0`: no guest sampling (no sampler thread).
- `DEKUI_HITCH_GPU_TIMING=0`: no GPU timestamp queries.

Renderer A/B switches (for comparing against the older behaviour):

- `DEKUI_VERTEX_DEPTH=0`: fragment shaders write the PICA depth again
  (gl_FragDepth), which turns off early depth tests and hidden surface removal.
- `DEKUI_RGBA8_STORAGE=1`: every RGBA8 surface gets storage usage from the
  start, which costs it Metal's lossless compression. Without it, storage is
  turned on (and all surfaces recreated) at the first shadow draw.
