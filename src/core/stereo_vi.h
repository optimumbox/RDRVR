#pragma once
// View-instanced stereo (docs/SINGLE_PASS_PLAN.md, step I1: the depth pre-pass). One draw for both eyes: the game's
// pipelines get view-instanced twins (2 views, view i to render-target array slice i) made from vertex and pixel shaders
// converted offline (tools/sp/dxil: SV_ViewID picks eye 2's rows of rage_matrices and rage_prev_proj, which
// prepass_arrays.cpp's [Stereo] PrepassViews=2 writes past the blocks' declared ends).
//
// In a view-instanced frame (prepass::vi_region): slice 0's pre-pass region binds both slices of upstream's 2-slice
// arrays and each draw whose pipeline has a twin is made with the twin (SetViewInstanceMask 3); slice 1's region (the
// second eye's pass) skips those draws and its clears, and draws the rest itself. A draw whose targets are not all the
// six arrays draws as the game's in both regions.
//
// [Stereo] ViPrepass=1 (needs [Stereo] PrepassArrays=1 and PrepassViews=2), ViShaders=<folder>: vs_<hash>.dxil and
// ps_<hash>.dxil (converted shaders, by the fnv hash of the game's bytecode) and ps_safe.txt (pixel shaders that read
// nothing view-dependent, one hash a line). A pipeline gets a twin only if its vertex shader is converted and its pixel
// shader is converted, listed safe, or absent. Test channel: "vi on|off|load [dir]|dump <dir>|status".

#include <d3d12.h>

#include <string>
#include <vector>

namespace rdrvr::vi {

void init();  // config; the converted shaders loaded (their twins made as the game's pipelines come)
// pso.cpp's CreateGraphicsPipelineState hook, after the game's pipeline was made: kept for its twin.
void on_pipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* pso);
// The playback thread's list calls: the game's SetPipelineState (returns the pipeline to bind), its Reset, and before
// each draw (true: skip it).
ID3D12PipelineState* on_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);
void on_reset(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);
bool before_draw(ID3D12GraphicsCommandList* cl);
bool skip_clear(ID3D12GraphicsCommandList* cl);
bool drop_predication(ID3D12GraphicsCommandList* cl);  // "vi pred off": pass 2's main pass of a view-instanced frame
// The query probe (I2's occlusion study): kind 0 BeginQuery, 1 EndQuery, 2 ResolveQueryData (index = start, n), 3
// SetPredication (res and off: the predicate's buffer; type: the op). Counted per region of a view-instanced frame.
void on_query(ID3D12GraphicsCommandList* cl, int kind, D3D12_QUERY_TYPE type, UINT index, ID3D12Resource* res, UINT64 off, UINT n);
// [Stereo] ViMain: eye 2's copy of a texture the main pass reads through global_textures slot `slot` (prepass_arrays.cpp
// made it, like `game`): its view is written into the pixel shaders' bindless table at eye 2's slot (the table's base,
// learnt from the game's own binds, + kEye2SlotBase + slot), as the game's own view of `game`.
constexpr unsigned kEye2SlotBase = 14960;  // = tools/sp/dxil/viewid_patch.py EYE2_SLOT_BASE
void eye2_texture(int slot, ID3D12Resource* e2, ID3D12Resource* game);
// The scene depth itself (not its Depth Resolve copy): shaders read it by an index of their own, not a global_textures
// slot; view 1 compares each index with the game's depth view's (rage_matrices row 30 .w, written by prepass_arrays.cpp)
// and reads eye 2's copy at kEye2SlotBase + kDepthBufferSlot when equal.
constexpr int kDepthBufferSlot = 16;  // = tools/sp/dxil/viewid_patch.py DEPTH_BUFFER_SLOT
// The bindless table index of the game's own (depth, not stencil) view of the image registered for `slot`, -1 unknown.
int64_t game_view_index(int slot);
void install_device_hooks(ID3D12Device* dev);           // CreateShaderResourceView (the game's views, for eye 2's)
// A resource just created (d3d_hooks): views recorded for an earlier resource at its address are dropped
void forget_resource(ID3D12Resource* r);
void install_list_hooks(ID3D12GraphicsCommandList* cl);  // SetDescriptorHeaps, SetGraphicsRootDescriptorTable (the table)
void install_rs_hooks(ID3D12GraphicsCommandList* cl);   // RSSetViewports, RSSetScissorRects (view 1's own, index 1)
bool main_pass();  // [Stereo] ViMain
std::string heap_status();  // "vi heap": eye 2's views
bool heap_ready();          // every screen image's eye-2 view written (view 1 reads them unconditionally)
bool untwinned_recent();  // a draw without a twin (not on purpose) in slice 0's region lately: eye 2's scene recorded
uint64_t untwinned_count();
bool wanted();      // this frame's pre-pass may draw both eyes (configured, switched on, twins made)
bool configured();  // [Stereo] ViPrepass on and its prerequisites met
// The depth's planes the list's game pipeline (this thread's lists) can write: bit 0 depth, bit 1 stencil; -1 not
// known (-2: no list state)
int writes_depth(ID3D12GraphicsCommandList* cl);
// The list's game pipeline (this thread's lists) may read the scene depth: not both its shaders in <ViShaders>\no_depth.txt
bool may_read_depth(ID3D12GraphicsCommandList* cl);
// The list's viewport 0 as the game set it since its Reset (this thread's lists): its right and bottom edges, in pixels
bool viewport_extent(ID3D12GraphicsCommandList* cl, uint32_t* right, uint32_t* bottom);
std::string command(const std::vector<std::string>& t);

}  // namespace rdrvr::vi
