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

## M0 measurement (2026-10-12, Armadillo free roam, Quest via SteamVR at 90 Hz, 0.8.8 release build)

Frame probe over 7-10 s windows. In "one pass", `double off` drew one scene pass a frame:

| per frame (ms) | one pass | two passes (today) | second eye's cost |
|---|---|---|---|
| render thread busy | 2.71 | 5.52 (pass 1 2.85, pass 2 1.80) | +2.8 |
| **playback thread busy** | 3.77 | **6.72** | +2.95 |
| main thread busy | 1.74 | 1.97 | +0.2 |
| GPU | 3.17 | 5.60 | +2.4 |
| D3D12 draws | 4440 | 10983 | +6500 |
| xrWaitFrame (slack) | 6.51 | 3.40 | |

- This PC holds 90 Hz in stereo with about 3.4 ms to spare.
- **The busiest thread in stereo is the playback thread** (6.7 ms), the one that turns packets
  into D3D12 calls, at about 0.6 µs a draw.
- Record-once-and-replay removes the render thread's second walk (about 1.8 to 2.8 ms) but
  replays every packet per eye. It leaves the playback thread, and so the stereo frame's
  ceiling, where it is.
- KF2's single pass paid off because UE3's per-view CPU work was its bottleneck. Here that work
  (visibility and shadows) is already shared.
- Lowering the playback thread's cost needs fewer D3D12 calls: one instanced draw for both eyes
  (view instancing). That also gives the single walk.

## Direction after M0: view-instanced stereo (chosen 2026-10-12)

One walk and one draw for both eyes, with D3D12 view instancing. The GPU supports tier 3 and
SM 6.7 (`[d3d] view instancing tier 3` in the log). The replay design above is set aside.

Proven offline so far:

- **The game's shaders are DXIL with full reflection.** Cbuffer struct names and entry-point
  names survive; only the resource names are stripped. A `pso dump` in Armadillo gave 145
  vertex shaders.
- **Which rows the vertex shaders read** (`tools/sp/dxil/cbrows.py`):
  - `rage_matrices`: constant rows only, never a dynamic index. Nearly all shaders read rows
    0-3 (gWorldSingle, camera-relative, per draw), 4-7 (gWorldView, the per-pass rotation-only
    view-projection) and 12-15 (gViewInverse).
  - Only `VS_TransformUnlit` (3 shaders) reads gWorldViewProj, rows 8-11.
  - Eye 2 therefore needs one extra set of per-pass rows. That is the layout
    `[Stereo] PrepassViews=2` already writes: rows 17-20 and 25-28, and rage_prev_proj rows
    4-7.
- **The shader patch** (`tools/sp/dxil/viewid_patch.py`, `stereo_vs.py`):
  1. Read `SV_ViewID`; in view 1, add 13 to the row of those reads (4 for rage_prev_proj).
  2. Raise the shader model to 6.1 and set the ViewID flag.
  3. Assemble with `dxa`, recompute the ViewID dependency tables with `dxopt -viewid-state`,
     then run the retail `dxv` validator.
  - Result: **133 of 145 validate.** The other 12 read no view block (full-screen and UI).
  - The toolchain is DXC v1.9.2609 built from source in `research\dxc-build` (needs VS's ATL
    component).

Next, in the game:

1. **I1, the pre-pass.** The depth pre-pass and its five companion targets are drawn once,
   view-instanced, into upstream's 2-slice arrays (`prepass_arrays.cpp`, milestone 2), then each
   slice is copied to its eye.
   - The core loads the converted shaders by hash and makes a twin PSO per game PSO
     (`ID3D12Device2::CreatePipelineState` with a 2-view `VIEW_INSTANCING` subobject, view i ->
     render-target array slice i).
   - In the region it sets the twin PSO and `SetViewInstanceMask(3)`; pass 2's pre-pass draws
     are skipped.
   - Check: eye 2's pre-pass slice against the real pass 2's targets (the dual pass is the
     reference).
2. **I2, the opaque scene.** The same for the main forward pass. Pixel shaders that read the
   camera (fog, specular) get the same patch. Full-screen passes inside the scene run per view
   on the array slices (their textures read as arrays: the patch's second form).
3. **I3:** the remaining passes, with the walk itself once (`SceneRender` once, both eyes'
   post runs after it), then the headset.

## I1 result (2026-10-12, flat double pass in Armadillo, `double on 0.064`)

What works:

- Twin pipelines need **signed** DXIL: the runtime refuses an unsigned container with
  E_INVALIDARG. `dxv -o=` signs. With that, 369 of 484 pipelines got twins and none were
  refused.
- Eye 2's rows: the prediction matches the game's real pass 2 exactly (402 frames, difference
  0).
- **The world matrices are camera-relative** (gWorldView is the same for both eyes; only
  gViewInverse moves), so eye 2's gWorldView must also carry eye 1 - eye 2 as a translation
  (`prepass::set_eye2_offset`).
- 421 view-instanced frames, with no device loss. Per frame, about 3,750 pre-pass draws were
  made once for both eyes, about 60 were drawn as the game's (no twin), and eye 2's copies were
  skipped.

**The blocker:**

- 37 main-pass pipelines (HDR target, format 10) draw the twinned geometry with depth **EQUAL**
  and no depth write.
- Eye 2's pre-pass depth now comes from the twin's math: pass 1's camera-relative world, times
  the translated eye-2 view-projection. Eye 2's main pass is still the game's own pass 2: the
  world relative to eye 2, times the untranslated view-projection. The two differ by rounding,
  so EQUAL fails and the background shows through, worst on John (hat and head).
- With the eyes 0.1 mm apart the image is correct.
- (`tools/sp/psoinfo.py` lists each dumped pipeline's depth test.)

**Conclusion:** the pre-pass cannot be instanced alone. Eye 2's main pass must come from the
same walk with the same math. I2 extends the region from the pre-pass through the opaque main
pass:

- the HDR target and the main depth become 2-slice arrays too;
- the full-screen passes in between (the Shadow Collector) read the arrays per view;
- the second pass skips everything the first drew.

## Correction (2026-10-13): the world matrices are absolute; the warped right eye

**What the matrices really are.** `VSFlattenTerrain`'s position math:

```
world = vertex x gWorldSingle    (rows 0-3, absolute)
rel   = world - gViewInverse[3]  (row 15: the camera's position)
clip  = rel x gWorldView         (rows 4-7: the view-projection without the camera's translation)
```

- Eye 2's rows exactly as upstream predicts them (eye 2's own viewport) are therefore the game's
  pass 2 rows. Redirecting rows 4-7 and 12-15 is the whole camera change.
- The "camera-relative world" reading was wrong, and with it the eye-offset shift added to eye 2's
  gWorldView. The shift moved eye 2's shared geometry by part of the eye separation: at 0.5 m
  apart, captures showed -12 px of disparity against the correct -60. It was the real cause of
  the EQUAL-test holes.
- The pass-2 camera trick built on it ("camfix") never reached the main pass. It stays as an off
  switch.

**The warped right eye in the headset.** The pre-pass's occlusion queries are resolved and
predicate the main pass's draws in the same pass. With eye 2's pre-pass draws skipped, eye 2's
queries counted nothing; its main pass then skipped those objects, and eye 1's pixels stayed in
the shared target. A draw inside an open occlusion query is now drawn in slice 1's region
(KF2VR's lesson: occlusion per eye). That is about 40% of eye 2's pre-pass draws, a cost to win
back later, for example by copying the union's query results.

**Verified with the eye-capture tools** (flat, 0.5 m apart):

- eye 2's disparity is -55 px with view instancing on, against -54 px with it off;
- eye 2's depth differs from the normal pass no more than two normal frames differ from each
  other;
- the pair shows no ghosts and no holes.

**Motion vectors:** D (the eye-1 to eye-2 previous-frame correction) matched the game's own pass 2
to 3.9e-8 relative, but that was measured with the camera trick on. To be re-checked without it,
under DLSS.

**Tools:**

- `eyecap` / `grabeyes`, with `tools/sp/eyecap.py`: disparity, vertical mismatch and unmatched
  patches;
- `grabdepth`, with `tools/sp/depthdiff.py`;
- `vi queries` (the query probe), `vi skip|pred|qdraw|camfix` (A/B switches), `vilog`.

## Risks

- The recorded packets may point into per-pass transient memory that is recycled before the
  replay. M1 finds this out first; the fallback is to copy the range.
- The constant ring's per-frame headroom: eye 2's rewritten blocks need ring space.
  `ring_probe` measures it.
- Upstream moves fast (several releases a day). Keep the fork's changes in new files with thin
  hooks into `camera_lever.cpp` so rebases stay small.

## I2 (the view-instanced main pass): first running build, 2026-10-13

Flat test, Armadillo, `double on 0.5`, [Stereo] ViMain=1, shaders `research/stereo_all4` (271 converted, 180 safe,
0 refused). Every draw of the shared region is made once for both eyes (`vi plain`: 0 pipelines left); the right eye's
geometry, lighting and sun shadows match a two-pass frame (eyecap on6 vs off6; `prepass grab light`: the light
buffers' means 44.7 / 41.0, unlit 68 % / 71 %).

What it took, beyond the commit log:
- the region ends with every scene (a single pass's region never ended and the arrays stood down at load);
- gWorldViewProj (rage_matrices rows 8-11, per draw) built in view 1 from the draw's gWorldSingle, eye 2's camera and
  eye 2's gWorldView; inverse view-projections ($Globals WorldViewProjInverse...) as
  inv(gWorldView2) x T(c2 - c1) x gWorldView1 x X1, inv(gWorldView2) packed in rows 29-31 (rage_matrices grown to 0x200);
- rage_shadowmap_common is shared (pass 1's shadow maps serve both eyes); distance scalars named "Camera" are shared;
- the scene depth itself gets an eye-2 copy (slot 16): the light passes read it by an index of their own; view 1
  compares each texture index with the game's depth view's (row 30 .w);
- **a global_textures slot is the pass's image, not a fixed one**: slot 15 is the scene depth in the main pass but the
  sun's shadow map (DepthTextureSampler0, a comparison sampler) in the light pass. Redirecting by slot number gave the
  right eye no sun shadows; the converter now redirects a slot only when the shader names it as a screen image.

Tools: `vi plain [reset]` (the region's eye-1-only draws), `prepass grab <kind> <path>` + `tools/sp/rda2png.py` (both
slices of a pre-pass array), `vi load <dir> all` (remake every twin from reconverted shaders, no restart).

Performance: not measured properly yet. Playback draws fell 16.5k -> 10.6k a frame, but frame time in the free camera
rose (14.3 -> 16.0 ms, GPU-bound, and the view moved between windows). Suspects: the publishes' full-screen copies (the
depth's two planes at each write->read barrier, now also into eye 2's copies). Next: GPU timestamps around the regions
and the publishes, A/B in a fixed view, then fewer copies (publish once per region end where the game allows).

### I2 GPU time (`prepass gpu`, `vi ab 30`: both modes alternating in one view), 2026-10-13

Flat, 2560x1440 per eye, Armadillo station. "Both regions" = the two scenes' regions (pre-pass to scene end, each eye).

| build | two passes | view-instanced | copies in them (2p / vi) |
|---|---|---|---|
| copy at every write->read barrier | 12.14 ms | 13.76 ms | 5.61 / 8.41 |
| copy only what was written since (b74b7a7+) | 10.39 ms | **9.35 ms** | 3.88 / 4.12 |

The view-instanced region's own work (copies excluded) is ~5.2 ms against ~6.5 ms for two passes; eye 2's region is
now ~1 ms (what is left: its shadow maps' scene, the few draws that are not shared, its copies). The depth is still
copied out 12 times a region (both planes, and into eye 2's copy): the next saving is to read the arrays' slices
directly (an array-slice view per eye for the converted shaders) instead of copying the depth into the game's texture.
Eye 2's shadow-map pass is also still drawn in a view-instanced frame though pass 1's maps serve both eyes.

### I2 CPU: the second scene recorded without draws (2026-10-13)

- Skipping eye 2's draws at the D3D12 calls, or even its packets at playback (`ops`, `[Stereo] ViSkipPackets`: the
  dispatch hooked, the D3D12 calls each opcode makes profiled), saved no playback time: the playback thread consumes
  packets as the render thread records them, so eye 2's region lasted as long as its recording.
- Skipping the whole second SceneRender hung the frame (the main thread waits for a completion that never came; the
  post run also needs PostFx +0xf8, the scene depth target, which the scene sets).
- What works: the second scene runs, but the render thread's recorder methods for draws and draw state return at once
  (`recorder_skip`: 19 RecorderVtbl entries through thunks gated on the render thread's id; the opcode each method
  writes in `research/recorder_ops.txt`). Clears, target binds, copies, queries and markers are still recorded, so the
  region's bookkeeping and the frame's signalling are intact. On by default (`[Stereo] ViSkipRecord=1`; `vi pass2 draw`).

Flat A/B, Armadillo station, `vi ab 30` (both modes alternating in one view):

| per frame | two passes | view-instanced, second scene without draws |
|---|---|---|
| whole frame (playback thread, start to start) | 9.69 ms | **7.80 ms** |
| playback thread cycles | 37.0 M | 31.0 M (eye 2's region 11.2 -> 6.2 M) |
| GPU, both regions | 7.86 ms | 5.84 ms (copies 3.10 / 2.98) |

Both eyes' captures (porch shadows included) match the second scene drawn whole. Next: the headset measurement
against the M0 table, DLSS/TAA per eye with the skip (the second scene's TAA update still runs), and the copies.
