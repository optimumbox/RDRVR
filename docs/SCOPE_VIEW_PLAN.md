# A rendered scope view (KF2-style), as a third view of single-pass stereo

Status: scoped 2026-10-15, not started. The stopgap is the magnified lens (`src/core/scope.cpp`): a patch of an
eye image enlarged onto the lens. It is soft by construction: the log showed a 21 px patch at zoom 4.

## The idea

When the rifle is at the face (scope.cpp's raise test), the view-instanced draws render a **third view**: a narrow
camera along the barrel, into slice 2 of the render-size targets. The scope's frustum lies inside eye 1's, so the
draws already recorded and culled for the eyes cover what it sees, and no extra draw recording is needed. When the
rifle is not raised, `SetViewInstanceMask(3)` leaves view 2 out, so the cost is zero when not scoped.

Cost while scoped:
- **CPU:** near zero (no new draw calls).
- **GPU vertex work:** about one more view of geometry. View instancing repeats the vertex shader per view.
- **GPU pixel work:** only for view 2's viewport, kept small (e.g. 768×768 inside the slice) by giving view 2 its
  own viewport index. That is about 18% of an eye's pixels.

## What it touches (all exist for view 1 today)

| Part | View 1 today | View 2 needs |
| --- | --- | --- |
| Shader conversion (`tools/sp/dxil/viewid_patch.py`) | `ViewID != 0` → eye 2's rows past each block's end, eye 2's bindless slots (`EYE2_SLOT_BASE`) | `ViewID == 2` → a third copy of the rows (scope camera) and a third slot range. Every converted shader re-made; the release's `RDRVR_shaders` re-shipped |
| Twins (`stereo_vi.cpp`) | 2 views, `kLocations {0,0},{1,1}`, mask 3 | 3 views, view 2 at slice 2 and viewport 2; mask 3 or 7 per frame. The twin cache is rebuilt once |
| Constant rows (`prepass_arrays.cpp`) | eye 2's gWorldView, inverse, camera, prev-proj, D matrix | the same for the scope camera: a narrow projection along the barrel, no jitter, its own previous frame |
| Render-size targets (`[Stereo] ViSlices`) | 2 slices each | 3 slices (+50% of those targets' memory; there is room) |
| Screen images the main pass reads (light buffer, shadow collector, depth, water) | eye 2's views in the bindless table | view 2's views; the shadow collector and depth drawn by the view-instanced pre-pass in slice 2 |
| Light buffer | eye 2's **reprojected from eye 1** (the light copy, a stopgap) | cannot be reprojected at 4× zoom: drawn for view 2, or lighting in the scope is soft. **The largest risk** |
| Post (tonemap, TAA/DLSS) | eye 2 gets the game's post chain through the second SceneRender | no third SceneRender (extra SceneRender calls are fragile). A mod pass tonemaps slice 2 with the frame's exposure and gamma into the lens swapchain; no TAA/DLSS at first (some aliasing), FXAA later if needed |
| Lens (`scope.cpp`) | n/a | copies slice 2's tonemapped image instead of the eye patch; the mask and the raise test stay |

## Phases and gates

Effort is in working sessions like 2026-10-15's (building, then a headset test round).

1. **Feasibility (1 session).** 3-view twins and 3-slice targets, mask 7 while scoped, with no shader change: view 2
   reads eye 2's rows (the patch tests `ViewID != 0`), so slice 2 should equal eye 2's image. Show slice 2 on the lens.
   *Gate:* the frame is intact, slice 2 is a clean image, and the GPU cost is measured. If the twins or slices fail at 3
   views, stop here and fall back to route 1 (a third SceneRender at a small size), or keep the lens.
2. **The scope camera (1–2 sessions).** viewid_patch.py gains view 2's rows, and the shaders are re-converted.
   prepass_arrays writes the scope camera. View 2 gets its own viewport (768×768).
   *Gate:* the scope shows a sharp, zoomed, correct image (geometry only; lighting may be wrong).
3. **Screen images for view 2 (1–2 sessions).** Shadow collector, depth and water views for slice 2, and the light
   buffer drawn for view 2. *Gate:* shadows and lights in the scope match the eyes.
4. **Output (1 session).** A tonemap pass from slice 2 into the lens with the game's exposure and gamma. Retire the
   patch copy. *Gate:* the colours match the eyes.
5. **Polish (1–2 sessions).** LOD at zoom (distant LODs look blurry or pop), draws without a twin (those not drawn into
   all six arrays don't appear in view 2: check particles, transparents, sky), scope-only DLSS/TAA if the aliasing
   bothers, release notes.

**Total: about 5–9 sessions.** The first gate decides whether the rest is worth it.

## Risks

- The light buffer (phase 3): eye 2's is a reprojection today, and view 2 cannot use one.
- Untwinned draws are missing from view 2. The eyes don't show this today because pass 2 draws them for eye 2.
  View 2 has no pass of its own.
- Every converted shader changes: a regression risk to single-pass stereo itself. Keep the 2-view build switchable
  until phase 4.
- Load-bearing switches stay on: [Debug] Dred, FrameGraph and LogResources (see the memory note).
