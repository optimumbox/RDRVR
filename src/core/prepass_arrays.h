#pragma once
// [Stereo] PrepassArrays (single-pass stereo, research\sps\stageVI-prepass.md milestone 2; test only, off by default):
// each eye's depth pre-pass drawn into its slice of mod-owned 2-slice arrays instead of the game's six pre-pass targets
// (the Shadow Collector, its depth, the velocity, the grass and tree screen targets), then copied into the game's own
// before the game reads them. No instancing yet: the image must be the game's, bit for bit; the copies' cost measured.
//
// On the playback thread, in the game's list: the game's clear of the Shadow Collector Depth Buffer starts a region (the
// frame's first pre-pass slice 0, its second slice 1); in it the six targets' binds and clears go to the slice; the
// game's first barrier taking that depth out of DEPTH_WRITE ends it: first each touched target's slice is copied into
// the game's target, from the states the region saw (a bind or a clear: RENDER_TARGET or DEPTH_WRITE; a game
// barrier: its StateAfter). Any state not known, any mismatch or a region that does not end: logged, nothing copied
// from then on, and the arrays stood down.
#include <d3d12.h>
#include <string>

namespace rdrvr::prepass {
void init();
void install();              // after anchors::verify(): the walk's end hook (with [Stereo] PrepassArrays=1), then on
bool on();                   // the switch (a region may start)
bool active();               // a region is open (the binds, clears and barriers go through the module)
void set_on(bool want);      // "prepass on|off" (from the next region)
D3D12_CPU_DESCRIPTOR_HANDLE clear_dsv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h);
D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h);
// The game's bind in a region: true with the substituted handles (out_rts[0..n), *out_ds when ds was given).
bool bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
          const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out_rts, D3D12_CPU_DESCRIPTOR_HANDLE* out_ds);
void drawn(ID3D12GraphicsCommandList* cl);  // a draw made (not skipped by view instancing): its targets written
void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);  // before the game's batch
void before_draw(ID3D12GraphicsCommandList* cl);  // a draw in a region: the walk's end copied first
void flush_reads(ID3D12GraphicsCommandList* cl);  // a dispatch or a copy in a region: the depth's deferred copy out made
// Every game bind while on and outside a region: the game binds the pre-pass's targets before it clears them, so the
// region's start binds them again with the slices.
void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
std::string status();
// "prepass grab <kind> <path>": both slices of a kind's array at the next view-instanced frame's publishes (RDA1 file)
std::string grab(const std::string& kind, const std::string& path);
// "prepass gpu [on|off|reset]": GPU time of the regions and their copies, view-instanced frames and two-pass ones apart
std::string gpu_command(const std::string& arg);
void gpu_mark_dlss(ID3D12GraphicsCommandList* cl, bool end);  // the playback thread, around slEvaluateFeature (DLSS)
void set_ab(unsigned frames);  // "vi ab <n>": view-instanced every other n frames (0 off), for "prepass gpu"
// View-instanced stereo (stereo_vi.h): in a region of a view-instanced frame on this list, 1 (slice 0's: the draws go to
// both slices) or 2 (slice 1's: the draws made in slice 0's are skipped), else 0. *all_arrays: every target bound now is
// one of the six (only then can a view-instanced pipeline draw).
int vi_region(const ID3D12GraphicsCommandList* cl, bool* all_arrays);
// The render thread, before pass 1 of a double frame: whether this frame's pre-pass is view-instanced (recorded into the
// stream for the playback thread's regions; false when the marker could not be recorded).
bool latch_vi();
std::string vi_status();
// The playback thread: this frame's last pre-pass region's slice (0, 1), or -1 before any this frame.
int last_slice();
bool vi_frame_now();  // the playback thread: this frame's slice-0 region drew both eyes
// [Stereo] ViMain: the render thread, after each SceneRender of a double frame: the region's end marker
void scene_end();
// [Stereo] ViMain with [Stereo] ViSlices (on by default): the game's render-size targets made with a slice per eye
// (d3d::set_stereo_targets), drawn by the regions in place: no copies out but eye 2's slice into eye 1's at eye 2's
// region's start
bool slices_wanted();
// [Stereo] ViMain: the game's CopyResource in a region (the depth into "Depth Resolve"): true when made from the arrays
bool copy_resource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src);
// [Stereo] PrepassViews (milestone 3, test only; stageVI-prepass.md): eye 2's per-view constants for pass 1.
// Step 1, observe only (nothing written to the game): camera_lever builds a private eye-2 viewport in pass 1 and hands
// its rows here; after each pass's walk the game's own rows are read from the constant buffers' storage, and pass 2's
// (the game's eye 2) are compared with pass 1's prediction ("prepass views" reports the differences).
bool views_wanted();
// the render thread, pass 1: vp2's +0xc0 (rotation-only VP) and +0x140 (view inverse); prev_vp: eye 2's gPrevViewProj
// (the TAA update's +0x4f0) or nullptr (no jitter)
void set_eye2(const void* vp2, const void* prev_vp);
// Before set_eye2: the two eyes' camera positions (world), for the view-instanced pre-pass's eye-2 rows
void set_eye2_offset(const float* eye1, const float* eye2);
// Before set_eye2 (taa.cpp's eye-2 update): eye 1's gPrevViewProj this frame, for eye 2's motion-vector transform
void set_eye1_prev(const void* p1);
// The render thread, pass 1 (taa.cpp): the NDCToPrevNDC the view-instanced velocity pass's view 1 reads (eff: the TAA
// effect; eye2: eye 2's own update's, else eye 1's when no eye-2 update ran), with the depth views' indices
void set_eye2_ndc(const void* eff, const float* ndc, bool eye2);
std::string mv_text();
void set_d_move(bool on);
void set_ndc_bias(float x);  // test ("prepass ndcbias <x>")  // test ("prepass dmove on|off")  // test ("prepass mv"): the motion-vector matrices last written
std::string views_status();
void views_size(bool grown);  // the test's: the two declared sizes grown or as the game's (from the next pass 1)
// A trace of the frames after "prepass on" (or "prepass trace"): every bind, clear and barrier of the six targets
// logged with the list's draws so far ([prepass] trace lines), in a region or not. Test only.
bool tracing();
void trace_start(int frames);
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth);
}  // namespace rdrvr::prepass
