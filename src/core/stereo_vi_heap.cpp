// [Stereo] ViMain (I2): eye 2's views in the pixel shaders' bindless table (stereo_vi.h, eye2_texture).
//
// The game's pixel shaders read every texture through one unbounded SRV table (t0 space2, root parameter 11 of its root
// signatures) by an index; the main pass takes its screen images' indices from global_textures (11 the light buffer,
// 13 the shadow collector, 15 the scene depth). A converted shader reads view 1's at the table's slot
// kEye2SlotBase + slot instead. The game binds its tables from more than one shader-visible heap (the 2026-10-13
// session), so each heap's table base is learnt from the game's own SetGraphicsRootDescriptorTable on a list that set
// that heap, and eye 2's views are written into each heap there, with the game's own view description of the image
// they copy, once each slot is checked to lie inside the heap and past what the game's descriptor partition has used.

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
#include "core/replay_ops.h"

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
struct GameView {
    SIZE_T cpu = 0;
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
};
std::unordered_map<ID3D12Resource*, std::vector<GameView>> g_views;  // each resource's views (where, as what; a few)
// a depth-stencil format's stencil-only views (the depth's are plane 0)
bool stencil_view(const D3D12_SHADER_RESOURCE_VIEW_DESC& d) {
    return d.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || d.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
}
struct HeapInfo {
    UINT64 gpu = 0;
    SIZE_T cpu = 0;
    UINT inc = 0, count = 0;
    int64_t base = -1;  // its SRV table's first descriptor
    uint64_t binds = 0, moves = 0;
    std::unordered_map<int64_t, uint64_t> counts;  // binds of parameter 11 per base
};
std::unordered_map<ID3D12DescriptorHeap*, HeapInfo> g_heaps;
thread_local std::unordered_map<ID3D12GraphicsCommandList*, ID3D12DescriptorHeap*> t_list_heap;
struct Pending {
    int slot = 0;
    ID3D12Resource* e2 = nullptr;
    ID3D12Resource* game = nullptr;
    std::unordered_map<ID3D12DescriptorHeap*, int64_t> written;  // heap -> the table base it was written for
};
std::vector<Pending> g_pending;
std::atomic<uint64_t> g_written{0}, g_refused{0}, g_table_binds{0}, g_outside{0};
char g_why[300] = "no table seen yet";
int g_logged_bases = 0;

void STDMETHODCALLTYPE hk_CreateSrv(ID3D12Device* dev, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateSrv(dev, r, d, h);
    if (r && d) {
        std::lock_guard lock(g_mutex);
        g_srv_desc[r] = *d;
        auto& v = g_views[r];
        bool have = false;
        for (GameView& g : v)
            if (g.cpu == h.ptr) g.d = *d, have = true;
        if (!have && v.size() < 8) v.push_back(GameView{h.ptr, *d});
    }
}

// Whether `index` may take eye 2's view: inside the heap and, when one of the game's descriptor partitions (test
// channel's "descheap": the main heap's) holds it, past what that partition has used.
bool slot_free(int64_t index, const HeapInfo& h, char* why, size_t len) {
    if (index < 0 || index >= static_cast<int64_t>(h.count)) {
        std::snprintf(why, len, "past the heap's %u descriptors", h.count);
        return false;
    }
    char* dev = *reinterpret_cast<char**>(anchors::addr(anchors::Id::D3dDeviceSingleton));
    for (int ty = 0; dev && ty < 8; ++ty) {
        const char* p = dev + ty * 0x40;
        const int64_t start = *reinterpret_cast<const int*>(p + 0x1f0);
        const int64_t cap = static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(p + 0x208));
        const int64_t used = static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(p + 0x218));
        if (cap > 0 && cap < (1ll << 24) && index >= start && index < start + cap) {
            std::snprintf(why, len, "partition t%d start %lld cap %lld used %lld", ty, static_cast<long long>(start), static_cast<long long>(cap),
                          static_cast<long long>(used));
            return index >= start + used;
        }
    }
    std::snprintf(why, len, "in no partition, inside the heap");
    return true;
}

void write_pending(ID3D12DescriptorHeap* heap) {  // under g_mutex
    auto hi = g_heaps.find(heap);
    if (hi == g_heaps.end() || hi->second.base < 0) return;
    const HeapInfo& h = hi->second;
    ID3D12Device* dev = nullptr;
    if (FAILED(heap->GetDevice(IID_PPV_ARGS(&dev)))) return;
    for (Pending& p : g_pending) {
        auto w = p.written.find(heap);
        if (w != p.written.end() && w->second == h.base) continue;
        p.written[heap] = h.base;
        const int64_t index = h.base + kEye2SlotBase + p.slot;
        char why[120];
        if (!slot_free(index, h, why, sizeof(why))) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            std::snprintf(g_why, sizeof(g_why), "heap %p: slot %d at index %lld NOT free (%s)", static_cast<void*>(heap), p.slot,
                          static_cast<long long>(index), why);
            log::limited("vi.refused", 10, "[vi] eye 2's view: %s", g_why);
            continue;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        auto it = g_srv_desc.find(p.game);
        bool own = it != g_srv_desc.end();
        if (own) d = it->second;
        auto gv = g_views.find(p.game);  // (a depth image: its depth view, not its stencil's)
        if (gv != g_views.end())
            for (const GameView& g : gv->second)
                if (!stencil_view(g.d)) {
                    d = g.d;
                    own = true;
                    break;
                }
        if (!own) {  // (not seen: a plain 2D view of the copy's format)
            d.Format = p.e2->GetDesc().Format;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE c{h.cpu + static_cast<SIZE_T>(index) * h.inc};
        if (o_CreateSrv) o_CreateSrv(dev, p.e2, &d, c);
        else dev->CreateShaderResourceView(p.e2, &d, c);
        g_written.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(g_why, sizeof(g_why), "heap %p table base %lld, eye 2's views from %lld (%s)", static_cast<void*>(heap),
                      static_cast<long long>(h.base), static_cast<long long>(h.base + kEye2SlotBase), why);
        log::limited("vi.written", 20, "[vi] eye 2's view of slot %d in heap %p at index %lld (format %d, %s)", p.slot, static_cast<void*>(heap),
                     static_cast<long long>(index), static_cast<int>(d.Format), own ? "the game's own view" : "a plain view");
    }
    dev->Release();
}

void STDMETHODCALLTYPE hk_SetHeaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* heaps) {
    o_SetHeaps(cl, n, heaps);
    replay_ops::note(replay_ops::kHeaps);
    for (UINT i = 0; i < n && heaps; ++i) {
        const D3D12_DESCRIPTOR_HEAP_DESC d = heaps[i]->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) continue;
        t_list_heap[cl] = heaps[i];
        std::lock_guard lock(g_mutex);
        if (g_heaps.count(heaps[i])) continue;
        ID3D12Device* dev = nullptr;
        if (FAILED(heaps[i]->GetDevice(IID_PPV_ARGS(&dev)))) continue;
        HeapInfo h;
        h.gpu = heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr;
        h.cpu = heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr;
        h.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        h.count = d.NumDescriptors;
        dev->Release();
        g_heaps[heaps[i]] = h;
        log::info("[vi] a shader-visible heap %p (%u descriptors, GPU %#llx)", static_cast<void*>(heaps[i]), d.NumDescriptors,
                  static_cast<unsigned long long>(h.gpu));
    }
}

void STDMETHODCALLTYPE hk_SetTable(ID3D12GraphicsCommandList* cl, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE gh) {
    o_SetTable(cl, param, gh);
    replay_ops::note(replay_ops::kTable);
    if (param != kSrvTableParam) return;
    g_table_binds.fetch_add(1, std::memory_order_relaxed);
    auto lh = t_list_heap.find(cl);
    if (lh == t_list_heap.end()) return;
    std::lock_guard lock(g_mutex);
    auto hi = g_heaps.find(lh->second);
    if (hi == g_heaps.end()) return;
    HeapInfo& h = hi->second;
    ++h.binds;
    if (gh.ptr < h.gpu || gh.ptr >= h.gpu + static_cast<UINT64>(h.count) * h.inc) {
        g_outside.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Parameter 11 is the bindless SRV table in the game's main root signatures only (other root signatures bind other
    // tables there: the 2026-10-13 session saw bases 32 and 20401 alternate). The table's base is the one bound most
    // often, taken once 2000 binds are counted and kept unless another base overtakes it by far.
    const int64_t base = static_cast<int64_t>((gh.ptr - h.gpu) / h.inc);
    const uint64_t c = ++h.counts[base];
    if (h.binds < 2000 || base == h.base) return;
    if (h.base >= 0 && c < 2 * h.counts[h.base]) return;
    if (h.base >= 0) ++h.moves;
    if (g_logged_bases < 12) {
        ++g_logged_bases;
        log::info("[vi] heap %p: the SRV table at %lld (bound %llu times; was %lld)", static_cast<void*>(lh->second), static_cast<long long>(base),
                  static_cast<unsigned long long>(c), static_cast<long long>(h.base));
    }
    h.base = base;
    write_pending(lh->second);
}

}  // namespace

void eye2_texture(int slot, ID3D12Resource* e2, ID3D12Resource* game) {
    std::lock_guard lock(g_mutex);
    bool found = false;
    for (Pending& p : g_pending)
        if (p.slot == slot) {
            p = Pending{slot, e2, game, {}};
            found = true;
        }
    if (!found) g_pending.push_back(Pending{slot, e2, game, {}});
    for (auto& [heap, h] : g_heaps)
        if (h.base >= 0) write_pending(heap);
}

int64_t game_view_index(int slot) {
    std::lock_guard lock(g_mutex);
    ID3D12Resource* game = nullptr;
    for (const Pending& p : g_pending)
        if (p.slot == slot) game = p.game;
    auto gv = game ? g_views.find(game) : g_views.end();
    if (gv == g_views.end()) return -1;
    const HeapInfo* best = nullptr;
    for (auto& [heap, h] : g_heaps)
        if (h.base >= 0 && (!best || h.binds > best->binds)) best = &h;
    if (!best) return -1;
    for (const GameView& g : gv->second) {
        if (stencil_view(g.d) || g.cpu < best->cpu || g.cpu >= best->cpu + static_cast<SIZE_T>(best->count) * best->inc) continue;
        return static_cast<int64_t>((g.cpu - best->cpu) / best->inc) - best->base;
    }
    return -1;
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
    if (g_pending.size() < 3 || g_refused.load() != 0) return false;  // the three screen images: light buffer, collector, depth
    bool any = false;  // every heap whose table has been bound holds all three, at its current base
    for (auto& [heap, h] : g_heaps) {
        if (h.base < 0) continue;
        any = true;
        for (const Pending& p : g_pending) {
            auto w = p.written.find(heap);
            if (w == p.written.end() || w->second != h.base) return false;
        }
    }
    return any;
}

std::string heap_status() {
    std::lock_guard lock(g_mutex);
    char b[300];
    std::snprintf(b, sizeof(b), "eye 2's views: %llu written, %llu refused, %zu images | table binds %llu (outside their heap %llu) | %s",
                  static_cast<unsigned long long>(g_written.load()), static_cast<unsigned long long>(g_refused.load()), g_pending.size(),
                  static_cast<unsigned long long>(g_table_binds.load()), static_cast<unsigned long long>(g_outside.load()), g_why);
    std::string out = b;
    for (auto& [heap, h] : g_heaps) {
        std::snprintf(b, sizeof(b), " | heap %p: %u descriptors, table base %lld, binds %llu, moves %llu", static_cast<void*>(heap), h.count,
                      static_cast<long long>(h.base), static_cast<unsigned long long>(h.binds), static_cast<unsigned long long>(h.moves));
        out += b;
    }
    return out;
}

}  // namespace rdrvr::vi
