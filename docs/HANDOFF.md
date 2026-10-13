# RDRVR single-pass stereo: handoff (2026-10-13, 15:25 EDT)

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

## State (commit 0a5d659 + uncommitted: session.ps1 sentinel wait, docs/HANDOFF.md)

Installed for VR: the latest build, shaders `research\stereo_all11`, `ViSkipRecord=0`.
The user's F7 menu render scale is at 200% (they tried it because 100% was blocked); 100% gives 4778x2688 (eyes
2716x2688) once not blocked. Tell them to put it back to 100%.

Fixed and confirmed by the user in the headset (2026-10-13 15:15): plants (no more smearing), the resolution cause.

## Open problems (the user's screenshots: `research\grab\vr3\`, eye captures; `tools\sp\eyesbs.py` views them)

1. **Glass in the right eye is hazy/grey**: through a window the left eye sees the room clearly, the right eye a grey
   haze with the room barely visible (vr3 capture 151514, bottom row of `research\grab\vr3\a.png`). The refraction
   copy is per eye now (PS_Blit has no twin; checked: slices differ). The scene colour before the glass already
   differed at the station window (eye 2's interior unlit; the depth-resolve index remap, row 27 .w, improved its light
   buffer). Next: which glass pixel shader draws these windows (the trace: "prepass trace 1" logs each draw's vs/ps
   hash; find the draws bound with the scene colour after the refraction blit), and what it reads per view (a $Globals
   index: tools\sp\dxil\read_samplers.py; a view-dependent term such as the camera position or a Fresnel/reflection).
2. **Shadows misaligned between the eyes** (the user's report, captures 151448/151457: the rider's shadow). Compare
   both eyes at full resolution; suspects: the shadow/light pass reading eye 1's depth (index not remapped), the
   cascade selection by eye 1's camera.
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
