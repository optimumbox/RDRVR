# RDRVR single-pass stereo: handoff (2026-10-13, updated 2026-10-14 02:50 EDT)

A session resuming this work starts here. Branch `single-pass-stereo`, repo `C:\Games\RDRVR-src`, game at
`G:\Games\Red Dead Redemption`. Background: `docs/SINGLE_PASS_PLAN.md` (I2: the view-instanced main pass).

## The user's rules for test sessions (also in Claude's memory)

- Test in VR yourself: SteamVR is running with the headset connected (it lies on the desk; nobody wears it).
  `tools\sp\session.ps1 -Vr` installs the build and starts the game; `tools\sp\loadin.ps1` presses Enter until in
  the world (it skips cutscenes). A new game is fine.
- `tools\sp\session.ps1 -Stop` closes the game; it now waits for the render-size boot sentinel to clear first. NEVER
  end a VR start within two minutes any other way: it blocks 4778x2688 for the user's next start (they then play at
  2560x1440: "insanely low res"; happened three times on 2026-10-13). The block lives in
  `%LOCALAPPDATA%\RDRVR\RDRVR.state.ini` ([Boot rt9cf420de] Blocked=...); edit it with the sandbox disabled.
- Quit the game when done. When asking the user a question, stop and wait for the answer.
- When the work is done, leave the build installed for VR: `tools\sp\session.ps1 -Install` (not -Stop).

## State

Installed for VR: the latest build, shaders `research\stereo_all14` (session.ps1's default now), `ViSkipRecord=1`
(session.ps1's default now: the recorder skip on).
The user runs DLSS Quality (RDRVR.user.ini DlssQuality=5, RenderScale=100: render 1809x1790 per eye), not DLAA.
The user's F7 menu render scale is at 200% (they tried it because 100% was blocked); 100% gives 4778x2688 (eyes
2716x2688) once not blocked. Tell them to put it back to 100%.

Fixed and confirmed by the user in the headset (2026-10-13 15:15): plants (no more smearing), the resolution cause.

## Fixed 17:00-17:55 (VR-verified on the monitor captures; commit eca6b31)

- Hazy right-eye glass: slot 12 now reads eye 2's copy of FullScreenCopy (taken before the glass), not eye 2's
  refraction copy (made after the glass in eye 2's region). VR capture: the interior shows through the station window
  in both eyes, as in two-pass.
- Objects missing at the right eye's edge (a chest, a barrel): [Stereo] ViUnionCull: eye 1's viewport culls with
  head-centred planes over both eyes. Likely also the user's shadow misalignment (the cascades fit the cull frustum):
  the sun-shadow stripes now match two-pass in the right eye (residual within the left-eye time-noise control).

## Fixed 18:00-19:05 (VR-measured)

- **Eye 2's sun shadows (and the light passes) an eye distance off** (the user: "the shadows sat in a different place
  slightly for each eye"). Every converted shader that reads an inverse view-projection adds gViewInverse row 3 (eye
  2's camera in view 1) to its result, but view 1's inverse also had T(camera2 - camera1) in it: positions relative to
  eye 1's camera, plus eye 2's camera. Removed (viewid_patch.py patch_inverse; set `research\stereo_all12`). Measured
  on the porch wall's sun stripes (`research\grab\vr23`, scratch scripts disp2.py/eyeshift.py: patch disparity by
  NCC): two-pass stripes -591.3 px = the wall's window -591.3; old I2 stripes -597.4 (eye 2's moved 6 px, eye 1's
  0.1); stereo_all12 -591.4. Two-pass and the shared shadow maps were never wrong (shared vs per-eye maps: no shift).
- **GPU copies**: the depth's copy out deferred to its first possible reader (`prepass gpu lazy on|off`; the shaders
  that cannot read it: `tools\sp\dxil\depth_readers.py` -> `<set>\no_depth.txt`), only the depth planes
  written (`dsexact`), colour copies boxed to the drawn viewports (`box`; only helps under DLSS Quality etc., the user runs
  DLAA: DlssQuality=0). Headset, starting spot: the lazy depth copy took a 45 Hz lock to 65-72 Hz; copies 2.9 -> 2.1 ms.

Headset numbers now (DLAA, 539 frames): GPU ~9.6 ms (regions 7.4, copies 2.1); render thread scene 8.1 ms (pass 1
4.9, pass 2 2.85: eye 2's scene recorded whole because ViSkipRecord=0); playback region 2 12.2 Mcycles; ~7.5 % late
frames, so SteamVR holds 45 Hz. Next: the recorder skip narrowed so eye 2's needed draws (no-twin passes, the plants'
motion vectors) are still recorded; that is ~2.85 ms render + ~2.5 ms playback.

## 19:05-19:35: the recorder skip narrowed (headset, DLSS Quality)

- The recorder skip (`ViSkipRecord=1`, then `vi pass2 skip|draw`) records no eye-2 draws at all, so eye 2 lost the
  passes without twins. `vi drawn2 on|list` (new) counts slice 1's region's draws that are made, by shader pair: at
  the desert scrub ~9 a frame: footprint colour/collector (3+3), PS_Grass_Fullscreen, PSAlphaFoliageResolveFS_Deferred,
  PS_Blit (the refraction copy: unused by eye 2 since slot 12 reads eye 2's FullScreenCopy).
- The grass and foliage resolves have twins again: eye 2 copies of Grass FS Colour / Light Multiply / Tree FS Colour
  (slots 17-19), their game view indices in rage_matrices rows 25/26 .w (14-bit fields), compared in view 1 by every
  converted shader (viewid_patch.py SCREEN_INDEX_FIELDS). Set `research\stereo_all13`. Headset: skip vs draw, the right
  eye's grass and bushes the same (`research\grab\vr25`, cmp_draw_skip1_R.png).
- Still lost in eye 2 under the skip (no rage_matrices in those shaders, so no index channel): footprints
  (PSFootprintColor/Collector), light glows (PS_lightglow). PSMiragerSingle (heat haze) has rage_matrices and could take
  a field for its RenderMapBilinearSampler image (FullScreenCopy? unverified: it never drew in the tested views).
- Headset, DLSS Quality, the scrub by the station: skip 85.4 Hz (5.0 % late, GPU 9.6 ms, playback busy 9.5, scene 4.6)
  vs draw 82.4 Hz (8.4 % late, GPU 10.1, playback 10.4, scene 5.4). Earlier, a heavier view: 81.5 vs 65 Hz. The motion
  vectors under the skip were fine (ground and far within the headset's jitter). Not on by default: footprints/glows.
- The box copies (`prepass gpu box`) do nothing: the game's targets are render-sized already ("last extents").

## 2026-10-14: ViSlices, and the recorder skip on by default

- `[Stereo] ViSlices` (on with ViMain, commit 95b7354): the game's render-size targets are made with two array slices
  (d3d_hooks.cpp: made inside `set_stereo_targets`' window in eye_shape `shape()`; a desc inherited by GetDesc on the
  window's thread is doubled again; a private-data sentinel forgets a destroyed target's registry entry, else the
  registry filled and a re-make came out mixed). The prepass's native kinds need no publishes/end copies. Same spot:
  two-pass GPU 5.15 ms (regions 3.35) vs single-pass 4.35 (regions 2.71, copies 0.12, was 2.6).
- The user's headset run was CPU-bound (spikes to 18 ms) because the install wrote `ViSkipRecord=0`: eye 2's scene was
  recorded whole (render thread scene 8.5 ms). The skip lost eye 2's light glows and footprints; they now have twins:
  viewid_patch.py GLOBALS_DEPTH_PS (view 1 reads `EYE2_SLOT_BASE + 16`, the depth's slice 1, for their $Globals
  DepthBufferSampler: no rage_matrices to compare the index with). Set `research\stereo_all14`; session.ps1 writes
  `ViSkipRecord=1`. Desk test (DLSS Quality, town edge): skip vs draw, render busy 5.45 vs 6.42 ms, playback 7.32 vs
  8.86, late 0.6 vs 2.3 %; eye 2 draws nothing in slice 1's region (`vi drawn2`); right eye skip vs draw the same
  (`research\grabr27\R_skip_draw.png`; daytime, no footprints or glows in view: unconfirmed visually).
- Still without a twin: PSMiragerSingle (heat haze; has rage_matrices, could take a SCREEN_INDEX_FIELDS field), PS_Blit
  (unused by eye 2).

- 2026-10-14 03:40, the user's "1, 2 and 3" (headset on the desk, DLSS Quality, the town edge):
  1. The mod's hooks (`hookprof`): ~0.7 ms of CPU a frame across threads once the profiler's own ~40 ns a call is taken
     off (descriptor tables 24k calls a frame, draws 8k). The playback thread's "busy" includes its xrWaitFrame and
     xrEndFrame waits (3.5-4.4 ms of ~8): its own work is ~4.5 ms.
  2. Eye 2's skipped scene still walked its draw lists: `[Stereo] ViSkipWalk` (default on, with ViSkipRecord) returns
     from SceneDrawWalk (RDR.exe+0x706050, a vtable method; new anchor) in it (recorder_skip.cpp cuts; test:
     `vi pass2 cut <rva hex> on|off`). Same spot, three rounds: pass 2 1.00 -> 0.46 ms, render busy 4.09 -> 3.54,
     playback 5.24 -> 4.75; a heavier view 1.61 -> 0.78. Both eyes' images unchanged (`research\grabr28`,
     base_cut_LR.png); walking, grass and bushes in both eyes (vr30). Cutting its caller FUN_1405cbfd0 instead gave eye 2
     eye 1's image: not that one. prepass's motion-vector check now reads 1.0 (it compares pass 2's last draw, which
     the cut removes): not an error.
  3. Bigscreen Beyond (2560 an eye, 4552x2560) vs Automatic (SteamVR's recommended 2688, 4778x2688): GPU 8.04 vs 8.30
     ms, late frames 0.64 vs 1.37 %. The GPU's p95 (10.4-11 ms) is now the limit: most remaining late frames are GPU.
- Drive C: was full (55 MB) on 2026-10-14: eye captures are ~200 MB raw (.rde); delete them after converting.

## Open problems (as of 15:25; 1 and 2 addressed above, re-check in the user's next session) (the user's screenshots: `research\grab\vr3\`, eye captures; `tools\sp\eyesbs.py` views them)

1. **Glass in the right eye is hazy/grey**: through a window the left eye sees the room clearly, the right eye a grey
   haze with the room barely visible (vr3 capture 151514, bottom row of `research\grab\vr3\a.png`). The refraction
   copy is per eye now (PS_Blit has no twin; checked: slices differ). The scene colour before the glass already
   differed at the station window (eye 2's interior unlit; the depth-resolve index remap, row 27 .w, improved its light
   buffer). Next: which glass pixel shader draws these windows (the trace: "prepass trace 1" logs each draw's vs/ps
   hash; find the draws bound with the scene colour after the refraction blit), and what it reads per view (a $Globals
   index: tools\sp\dxil\read_samplers.py; a view-dependent term such as the camera position or a Fresnel/reflection).
2. ~~Shadows misaligned between the eyes~~: fixed 19:00 (above); the user to confirm in the headset.
3. **Light glows on the train in the right eye only** (vr10 captures): PS_lightglow now has no twin and they remain;
   find the drawing pass (trace).
4. Later: narrow the recorder skip (ViSkipRecord) to the region so it saves CPU without biasing eye 2's motion
   vectors; per-eye motion vectors in VR (grabburst ... vel, tools\sp\velpair.py).

## Tools

- Test channel: `tools\sp\rdrcmd.ps1 "<command>"` (vi, vi plain, vi heap, prepass grab <kind> <path>, prepass trace 1,
  eyecap now <dir>, grabburst <prefix> 1 <w> <h> vel, stick lx ly rx ry ms, mode, renderres).
- Shaders: dump `vi dump <dir>`, convert `python tools\sp\dxil\stereo_vs.py research\dump_all research\stereo_allN`,
  load live `vi load <dir> all`; NO_TWIN_PS in stereo_vs.py for full-screen passes that read screen images by their
  own index.
- Build: `powershell -File tools\build.ps1`.
