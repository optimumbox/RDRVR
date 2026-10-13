# Single-pass stereo for RDRVR: plan

Goal: KF2VR's single-pass stereo in Red Dead Redemption. The game walks and records the scene
once a frame, and both eyes are drawn from that one recording. The render thread's second walk
of the scene goes away, which is where a CPU-bound headset gains frame time.

This is a fork branch (`single-pass-stereo`). It lives beside the upstream dual pass, which stays
the reference and the fallback.

## Where RDRVR is today (0.8.8)

`camera_lever.cpp` `hk_SceneRender` calls the game's `SceneRender` twice a frame on the same
viewport: pass 1 is the first eye, then that eye's post chain, then pass 2 is the second eye.
That is same-frame stereo, not alternate-eye. Some work is already shared:

| Work | Today |
|---|---|
| Visibility (`VisibilityBuild`, main thread) | Once a frame, for a head frustum covering both eyes (`[XR] HeadCover`) |
| Sun, point and spot shadows | Once a frame, fitted to the two-eye union (`dual_pass` splits `shadows` and `shadowunion`) |
| Scene walk and draw recording (`SceneRender`, render thread) | **Twice** |
| Playback into D3D12 (playback thread) | Twice (batched since 0.8.0) |
| Post chain, TAA or DLSS | Once per eye, with its own DLSS history (`dlss.cpp`) |
| GPU | Twice (unavoidable without view instancing) |

`dual_pass.cpp` needs about a dozen fixes to make the second `SceneRender` see the frame the way
the first did: grass batch counts, gust, light phases, forest timers, rain, the particle map,
ped damage, cloud scroll, sun visibility, god-ray history and the draw-mask stack bug. Each one is
per-frame CPU state that a second walk consumes or advances a second time.

The upstream author has started a single-pass study (`research\sps`, not published). It added the
frame probe, the batched playback, the shared shadows, the pre-pass texture arrays
(`prepass_arrays.cpp`, milestone 2) and the prediction of eye 2's view constants
(`[Stereo] PrepassViews`, milestone 3). That study is heading toward view instancing in the
game's shaders.

## What KF2VR taught us

From `KF2VR-src/docs/SINGLE_PASS.md` and `Adapter.cpp`:

1. **Side-by-side targets cost the most bugs.** Most KF2 fixes came from two eyes sharing one
   double-width target:
   - full-screen quads that assume x = 0;
   - HBAO+ and SSR reading the whole buffer as one view;
   - eye clears through a clear-quad path that skipped the depth guard (DLSS motion vectors
     broke);
   - HiZ built over both eyes.
2. **Anything with one-view state breaks.** Examples: occlusion queries overwriting each other
   (the right eye ran frustum-only), whole-scene shadows fitted to one view, lens-flare
   occlusion read through a per-view cache.
3. **Stale view constants.** The left eye's queries ran with the right eye's constants still in
   the GPU buffer. Write each eye's constants before its first use.
4. **Every fix gets its own switch, and the sequential path stays available.** Diagnostics:
   `-kf2vr-sp-split`, `-sp-split-lighting`, `-sp-separate-state`, `-sp-*-occlusion`.
5. **Test each eye against that eye rendered alone.** `-kf2vr-eye-capture` and the stereo
   reflections test compare single-pass output with a sequential reference.
6. **DLSS per eye, from that eye's image, with its own history and real world depth.**

## Chosen design: record once, replay per eye

KF2's single pass is UE3 drawing two views of one view family: visibility and setup happen once,
then each view's draws are issued in turn. The RAGE equivalent is to walk and record once, then
replay that recording once per eye:

```
render thread:   SceneRender (eye 1 camera)  -> packets P[a..b)      (one walk)
                 post chain eye 1
                 replay request: P[a..b) with eye-2 fix-ups          (no walk)
                 post chain eye 2 (RenderFrame's own)
playback thread: P[a..b) eye 1, post 1, P[a..b) eye 2, post 2
```

- **Same targets, eyes in sequence on the GPU, as today.** This avoids lesson 1 entirely: no
  double-width targets, no quad offsets, no per-rectangle clears, no whole-buffer screen-space
  passes. The post chain, TAA, DLSS per eye, the eye shape and `eye_grab` all keep working
  unchanged.
- **The `dual_pass` CPU-state splits become unnecessary.** With one walk there is no second
  consumption of the grass counts, rain step and so on, which removes that whole class of bugs.
  They stay in the code for the fallback.
- **Eye 2's fix-ups during the replay:**
  - **View-dependent constants:** `rage_matrices` (b1: gWorldView, gWorldViewProj, gViewInverse,
    ...), `rage_prev_proj` and the per-pass globals (gViewport, the camera position, the clip
    planes, the TAA jitter). Each block is rewritten from eye 1's block by a fixed per-frame
    transform (eye 1's view-projection inverse times eye 2's, in the shaders' camera-relative
    space), or taken from the engine's own computation for eye 2 (a private eye-2 viewport, the
    way `PrepassViews` builds it).
  - **Queries and predication (lesson 2):** eye 2 issues no occlusion queries. Its predication
    is a switch: off, so eye 2 draws everything that passed visibility (the default), or eye 1's
    results.
  - **One-time work inside the pass is not replayed:** imposter captures, readbacks, GPU timers,
    the particle collision map. Each item is found by census and gets its own switch.
  - **The mod's own draws in the pass:** body, hands, held props and rounds. These are already
    per eye (`round_draw::note_pass`) and are re-issued for eye 2 with eye 2's camera.
- **Validation (lesson 5):** the dual pass is an exact reference. In a test frame, record pass 2
  for real, replay pass 1 with eye-2 fix-ups, and compare the two images (`eye_grab`,
  `align_grab`). Then predict every view-dependent block and compare it with what the game
  computes in a real pass 2 (`PrepassViews` does this for three blocks today).
- **Fallback (lesson 4):** `[Stereo] SinglePass=0|1`. Any fix-up that cannot be classified, any
  replay range that does not close, or any packet it cannot follow is logged, and that frame and
  the rest of the session go back to the dual pass.

### Not chosen now: view instancing (one draw for both eyes)

This would halve the GPU's draw-call overhead too, but it means rewriting every scene shader's
bytecode to choose its rows by instance or view ID, and writing `SV_ViewportArrayIndex`. It is
the upstream study's direction. It can be added on top of the replay once the replay is right,
because both need the same classification of view-dependent constants.

## Milestones

Each milestone is a switch, off by default, until headset play accepts it.

- **M0, baseline and measurement.**
  - The fork builds (done: `tools\build.ps1`, VS 2022 Build Tools, vcpkg at `C:\dev\vcpkg`, the
    RedHook SDK in `extern\redhook-sdk`).
  - Record the frame probe's `kPass1`, `kPass2`, `kMid` and playback times in the headset. This
    is the ceiling of what single pass can save.
- **M1, the recording.**
  - Find the recorder's chunk API and the packet format (the playback loop is already known:
    `playback_head.asm`).
  - Keep pass 1's packet range alive to the frame's end.
  - Identity gate: replay with eye 2 = eye 1 (ipd 0) must equal a real second pass, bit for
    bit (R3's gate, reused).
- **M2, the census.**
  - In real dual-pass frames, diff every CBV and root CBV bound in pass 1 against the
    corresponding binding in pass 2. The blocks that differ are the view-dependent set.
  - Derive each one's eye-2 rule and check the predictions against pass 2 for many frames
    (`PrepassViews`, extended).
  - Also list the queries, copies, readbacks and one-time captures inside the pass.
- **M3, the replay.**
  - Eye 2 is drawn from the replay with the fix-ups.
  - Image comparison against the real pass 2 in the simulator.
  - `[Stereo] SinglePass=1` with its fallback.
- **M4, per-item switches** for queries and predication, imposters, readbacks and the mod's
  own draws. Eye capture: both raw eyes and the atlas saved as PNGs.
- **M5, headset.** Play, comfort, then the CPU frame time against the dual pass and the
  presets.

## Findings so far (static, `research\re\opcodes.py` on RDR.exe sha256 as the anchors)

- `ReplayChunk` 0x140ecdd30 `(chunk, ctx, &offset, terminator)` runs a chunk's packets through a
  jump table at 0x140ece54c: 0x50 opcodes (1..0x50; 0x50 ends the chunk). Each case calls a
  method of `ctx`'s vtable (offsets 0x20..0x378) or a small helper, and returns the packet's
  size, which is fixed per opcode: 0x10, 0x20, 0x30 or 0x70 bytes. **There is no inline
  constant data.** Constants are written by the render thread into the upload ring, and the
  packets carry their addresses.
- **Packets are consumed destructively.** Several helpers release a refcounted object the
  packet holds (`lock xadd [obj+0xc], -1`, destroyed at zero): ops 0xf, 0x11 and others of that
  family. Op 3 frees a heap string. A second replay must take an extra reference for those
  objects first (or skip the release), and must copy the packets before the chunk is reused.
- Op 0x40 (`FUN_140eca0a0`) replays another chunk through the immediate context (a nested
  `ReplayChunk`). The engine already supports calling one recording from another.
- Ops 0x2b, 0x2c and 0x2d share one handler with three draw-like vtable calls (0x188, 0x190,
  0x198), each with 3 or 4 integer arguments. These are the draw candidates; M1 confirms them
  in the game.

## Risks

- The recorded packets may point into per-pass transient memory that is recycled before the
  replay. M1 finds this out first; the fallback is to copy the range.
- The constant ring's per-frame headroom: eye 2's rewritten blocks need ring space.
  `ring_probe` measures it.
- Upstream moves fast (several releases a day). Keep the fork's changes in new files with thin
  hooks into `camera_lever.cpp` so rebases stay small.
