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
void install_device_hooks(ID3D12Device* dev);           // CreateShaderResourceView (the game's views, for eye 2's)
void install_list_hooks(ID3D12GraphicsCommandList* cl);  // SetDescriptorHeaps, SetGraphicsRootDescriptorTable (the table)
bool main_pass();  // [Stereo] ViMain
std::string heap_status();  // "vi heap": eye 2's views
bool heap_ready();          // every screen image's eye-2 view written (view 1 reads them unconditionally)
bool wanted();      // this frame's pre-pass may draw both eyes (configured, switched on, twins made)
bool configured();  // [Stereo] ViPrepass on and its prerequisites met
std::string command(const std::vector<std::string>& t);

}  // namespace rdrvr::vi
