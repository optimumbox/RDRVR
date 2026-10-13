// [Stereo] ViMain (I2): eye 2's views in the pixel shaders' bindless table (stereo_vi.h, eye2_texture).
//
// The game's pixel shaders read every texture through one unbounded SRV table (t0 space2, root parameter 11 of its root
// signatures) by an index; the main pass takes its screen images' indices from global_textures (11 the light buffer,
// 13 the shadow collector, 15 the scene depth). A converted shader reads view 1's at the table's slot
// kEye2SlotBase + slot instead. The table's base is learnt from the game's own SetGraphicsRootDescriptorTable, and eye
// 2's views are written there with the game's own view description of the image they copy, once each slot is checked
// to lie past what the game's descriptor partition has used.

#include "core/stereo_vi.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::vi {
namespace {

using CreateSrv_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using SetHeaps_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetTable_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
CreateSrv_t o_CreateSrv = nullptr;
SetHeaps_t o_SetHeaps = nullptr;
SetTable_t o_SetTable = nullptr;
constexpr UINT kSrvTableParam = 11;  // the game's root signatures: t0 space2, unbounded (dxa -dumprs of a dumped one)

std::mutex g_mutex;  // all below
std::unordered_map<ID3D12Resource*, D3D12_SHADER_RESOURCE_VIEW_DESC> g_srv_desc;  // the game's views (the last per resource)
ID3D12DescriptorHeap* g_heap = nullptr;  // the shader-visible CBV/SRV/UAV heap the game binds
UINT64 g_heap_gpu = 0;
SIZE_T g_heap_cpu = 0;
UINT g_inc = 0;
int64_t g_table_base = -1;  // the SRV table's first descriptor (heap index)
struct Pending {
    int slot = 0;
    ID3D12Resource* e2 = nullptr;
    ID3D12Resource* game = nullptr;
    bool written = false;
};
std::vector<Pending> g_pending;
std::atomic<uint64_t> g_written{0}, g_refused{0}, g_table_binds{0};
char g_why[240] = "no table seen yet";

void STDMETHODCALLTYPE hk_CreateSrv(ID3D12Device* dev, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateSrv(dev, r, d, h);
    if (r && d) {
        std::lock_guard lock(g_mutex);
        g_srv_desc[r] = *d;
    }
}

// The game's descriptor partitions (test_channel's "descheap"): the one holding `index`, and whether `index` lies past
// what it has used.
bool slot_free(int64_t index, char* why, size_t len) {
    char* dev = *reinterpret_cast<char**>(anchors::addr(anchors::Id::D3dDeviceSingleton));
    if (!dev) {
        std::snprintf(why, len, "no device");
        return false;
    }
    for (int ty = 0; ty < 8; ++ty) {
        const char* p = dev + ty * 0x40;
        const int64_t start = *reinterpret_cast<const int*>(p + 0x1f0);
        const int64_t cap = static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(p + 0x208));
        const int64_t used = static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(p + 0x218));
        if (cap > 0 && index >= start && index < start + cap) {
            std::snprintf(why, len, "partition t%d start %lld cap %lld used %lld", ty, static_cast<long long>(start), static_cast<long long>(cap),
                          static_cast<long long>(used));
            return index >= start + used;
        }
    }
    std::snprintf(why, len, "in no partition");
    return false;
}

void write_pending() {  // under g_mutex
    if (g_table_base < 0 || !g_heap) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(g_heap->GetDevice(IID_PPV_ARGS(&dev)))) return;
    for (Pending& p : g_pending) {
        if (p.written) continue;
        p.written = true;
        const int64_t index = g_table_base + kEye2SlotBase + p.slot;
        char why[120];
        if (!slot_free(index, why, sizeof(why))) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            std::snprintf(g_why, sizeof(g_why), "slot %d at heap index %lld NOT free (%s)", p.slot, static_cast<long long>(index), why);
            log::warn("[vi] eye 2's view: %s", g_why);
            continue;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        auto it = g_srv_desc.find(p.game);
        const bool own = it != g_srv_desc.end();
        if (own) {
            d = it->second;
        } else {  // (not seen: a plain 2D view of the copy's format)
            d.Format = p.e2->GetDesc().Format;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE h{g_heap_cpu + static_cast<SIZE_T>(index) * g_inc};
        if (o_CreateSrv) o_CreateSrv(dev, p.e2, &d, h);
        else dev->CreateShaderResourceView(p.e2, &d, h);
        g_written.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(g_why, sizeof(g_why), "table base %lld, eye 2's views from %lld (%s)", static_cast<long long>(g_table_base),
                      static_cast<long long>(g_table_base + kEye2SlotBase), why);
        log::info("[vi] eye 2's view of slot %d at heap index %lld (format %d, %s)", p.slot, static_cast<long long>(index),
                  static_cast<int>(d.Format), own ? "the game's own view" : "a plain view");
    }
    dev->Release();
}

void STDMETHODCALLTYPE hk_SetHeaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* heaps) {
    o_SetHeaps(cl, n, heaps);
    for (UINT i = 0; i < n && heaps; ++i) {
        const D3D12_DESCRIPTOR_HEAP_DESC d = heaps[i]->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) continue;
        std::lock_guard lock(g_mutex);
        if (g_heap == heaps[i]) continue;
        ID3D12Device* dev = nullptr;
        if (FAILED(heaps[i]->GetDevice(IID_PPV_ARGS(&dev)))) continue;
        g_heap = heaps[i];
        g_heap_gpu = heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr;
        g_heap_cpu = heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr;
        g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dev->Release();
        g_table_base = -1;  // (learnt again for this heap)
        for (Pending& p : g_pending) p.written = false;
        log::info("[vi] the shader-visible heap %p (%u descriptors)", static_cast<void*>(heaps[i]), d.NumDescriptors);
    }
}

void STDMETHODCALLTYPE hk_SetTable(ID3D12GraphicsCommandList* cl, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    o_SetTable(cl, param, h);
    if (param != kSrvTableParam) return;
    g_table_binds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    if (!g_heap || !g_inc || h.ptr < g_heap_gpu) return;
    const int64_t base = static_cast<int64_t>((h.ptr - g_heap_gpu) / g_inc);
    if (base == g_table_base) return;
    if (g_table_base >= 0)
        log::warn("[vi] the SRV table moved: %lld -> %lld (eye 2's views written again)", static_cast<long long>(g_table_base),
                  static_cast<long long>(base));
    g_table_base = base;
    for (Pending& p : g_pending) p.written = false;
    write_pending();
}

}  // namespace

void eye2_texture(int slot, ID3D12Resource* e2, ID3D12Resource* game) {
    std::lock_guard lock(g_mutex);
    bool found = false;
    for (Pending& p : g_pending)
        if (p.slot == slot) {
            p = Pending{slot, e2, game, false};
            found = true;
        }
    if (!found) g_pending.push_back(Pending{slot, e2, game, false});
    write_pending();
}

void install_device_hooks(ID3D12Device* dev) {
    if (config::get_bool("Stereo", "ViMain", false) && !o_CreateSrv)
        hooks::install("ID3D12Device::CreateShaderResourceView (ViMain)", hooks::vtable_entry(dev, 18), hk_CreateSrv, &o_CreateSrv);
}

void install_list_hooks(ID3D12GraphicsCommandList* cl) {
    if (!config::get_bool("Stereo", "ViMain", false) || o_SetHeaps) return;
    hooks::install("CL::SetDescriptorHeaps (ViMain)", hooks::vtable_entry(cl, 28), hk_SetHeaps, &o_SetHeaps);
    hooks::install("CL::SetGraphicsRootDescriptorTable (ViMain)", hooks::vtable_entry(cl, 32), hk_SetTable, &o_SetTable);
}

bool heap_ready() {
    std::lock_guard lock(g_mutex);
    if (g_pending.size() < 3 || g_table_base < 0) return false;  // the three screen images: light buffer, collector, depth
    for (const Pending& p : g_pending)
        if (!p.written) return false;
    return g_refused.load() == 0;
}

std::string heap_status() {
    std::lock_guard lock(g_mutex);
    char b[420];
    std::snprintf(b, sizeof(b), "eye 2's views: %llu written, %llu refused, %zu images | table binds %llu | %s",
                  static_cast<unsigned long long>(g_written.load()), static_cast<unsigned long long>(g_refused.load()), g_pending.size(),
                  static_cast<unsigned long long>(g_table_binds.load()), g_why);
    return b;
}

}  // namespace rdrvr::vi
