#include "core/prepass_arrays.h"

#include <windows.h>
#include <d3d12.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/log.h"
#include "core/state.h"
#include "core/stereo_vi.h"

namespace rdrvr::prepass {
namespace {

// The pre-pass's six, then (view-instanced main pass, [Stereo] ViMain) the main pass's own targets
enum Kind { kDepth, kCollector, kVelocity, kGrassColour, kGrassLight, kTree, kLight, kFsc, kReact, kRefr, kKinds };
constexpr int kPrepassKinds = kLight;
// The global_textures slot each kind is read through (the main pass's shaders: 11 LightBufferSampler, 13
// ShadowCollectorSampler; the scene depth: its own index, vi::kDepthBufferSlot), -1 none. Eye 2's copy of such a kind
// is made for view 1's reads (stereo_vi.h).
const int kSlot[kKinds] = {vi::kDepthBufferSlot, 13, -1, -1, -1, -1, 11, -1, -1, -1};
constexpr int kDepthSlot = 15;  // the scene depth's slot: the game's "Depth Resolve" copy
const char* const kNames[kKinds] = {"Shadow Collector Depth Buffer", "Shadow Collector",        "Velocity RT",
                                    "Grass FS Colour",               "Grass FS Light Multiply", "Tree FS Colour Diffuse",
                                    "Light Buffer",                  "FullScreenCopy",          "Reactivity RT",
                                    "Refraction RT"};

std::atomic<bool> g_on{false}, g_active{false}, g_failed{false}, g_hooked{false};
// [Stereo] ViMain (I2, test only): the region runs from the pre-pass's start to the scene's end (a marker after each
// SceneRender, scene_end) and takes the main pass's targets too; a target the game turns from written to read in the
// region is copied out of its slices there (publish: the region's slice into the game's target, and in a
// view-instanced slice-0 region slice 1 into eye 2's copy, which view 1 reads through its global_textures slot).
std::atomic<bool> g_i2{false};
std::atomic<uint64_t> g_publishes{0}, g_depth_copies{0};
int kind_count() { return g_i2.load(std::memory_order_relaxed) ? kKinds : kPrepassKinds; }
std::atomic<bool> g_views{false}, g_views_write{false};  // [Stereo] PrepassViews: 1 observe, 2 also write eye 2's rows
std::atomic<bool> g_end_pending{false};  // the walk's end marker played back: the next list call ends the region
std::atomic<uint64_t> g_markers{0}, g_marker_ends{0};
std::atomic<uint64_t> g_trace_until{0};  // the trace runs while state::presents is below it
std::atomic<uint64_t> g_regions{0}, g_copies{0}, g_redirects{0}, g_skipped{0};
char g_fail_reason[160] = "";

struct Arr {
    ID3D12Resource* game = nullptr;  // the game's target it stands in for (no reference held)
    UINT64 w = 0;
    UINT h = 0;
    DXGI_FORMAT res_fmt = DXGI_FORMAT_UNKNOWN, view_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* arr = nullptr;  // 2 slices
    UINT planes = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE view[3]{};  // slice 0, slice 1, both (view-instanced stereo: views 0 and 1 to slices 0 and 1)
    ID3D12Resource* e2 = nullptr;           // eye 2's copy (a kind with a global slot, ViMain), its state
    D3D12_RESOURCE_STATES e2_state = D3D12_RESOURCE_STATE_COMMON;
};
Arr g_arr[kKinds];
// Eye 2's copy of the game's "Depth Resolve" (the depth the main pass reads through slot 15), made at its first copy
struct DepthResolve {
    ID3D12Resource* game = nullptr;
    ID3D12Resource* e2 = nullptr;
    D3D12_RESOURCE_STATES e2_state = D3D12_RESOURCE_STATE_COMMON;
    UINT planes = 1;
} g_dr;
struct Retired {
    ID3D12Resource* res;
    uint64_t frame;
};
std::vector<Retired> g_retired;
ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // 2 per colour kind
ID3D12DescriptorHeap* g_dsv_heap = nullptr;  // 2
UINT g_rtv_inc = 0, g_dsv_inc = 0;

// the open region (the playback thread only)
struct Region {
    ID3D12GraphicsCommandList* cl = nullptr;
    int slice = 0;
    bool vi = false;          // view-instanced (stereo_vi.h): slice 0's region draws both slices, slice 1's skips those draws
    bool all_arrays = true;   // every target of the current bind is one of the six (a view-instanced draw may run)
    unsigned published = 0;   // ViMain: kinds copied out since their last writing (bit k)
    unsigned bound = 0;       // kinds bound as the current targets (bit k)
    unsigned dirty = ~0u;     // ViMain: kinds written (a draw made, a clear) since their last copy out (bit k)
    unsigned touched = 0, known = 0;   // bit (kind * 2 + plane): that plane's state known
    D3D12_RESOURCE_STATES state[kKinds][2]{};  // per plane (the depth's two: the game moves them one at a time)
} g_r;
unsigned plane_bits(int k) { return (g_arr[k].planes > 1 ? 3u : 1u) << (k * 2); }
void set_state(int k, int plane, D3D12_RESOURCE_STATES s) {  // plane -1: every plane
    for (int p = 0; p < 2; ++p)
        if (plane < 0 || plane == p) {
            g_r.state[k][p] = s;
            g_r.known |= 1u << (k * 2 + p);
        }
}
uint64_t g_frame_seen = ~0ull;
int g_pass_in_frame = 0;
uint64_t g_vi_frame = ~0ull;  // the frame whose slice-0 region drew both eyes (view-instanced)
std::atomic<uint64_t> g_vi_regions[2]{};
// the region's target view: slice 0's region of a view-instanced frame binds both slices
int region_view_index() { return g_r.vi && g_r.slice == 0 ? 2 : g_r.slice; }
struct LastBind {  // the list's last bind outside a region (the playback thread)
    ID3D12GraphicsCommandList* cl = nullptr;
    unsigned n = 0;
    bool single = false, has_ds = false;
    D3D12_CPU_DESCRIPTOR_HANDLE rts[8]{}, ds{};
} g_last;

bool is_depth(int k) { return k == kDepth; }
D3D12_RESOURCE_STATES write_state(int k) { return is_depth(k) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET; }

void fail(const char* why) {
    if (!g_failed.exchange(true)) {
        std::snprintf(g_fail_reason, sizeof(g_fail_reason), "%s", why);
        log::warn("[prepass] stood down: %s (the game's own targets from now on)", why);
    }
}

// The kind of a game view, by its resource's name; -1 when not one of the six.
int kind_of(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource** res, DXGI_FORMAT* fmt) {
    int f = 0;
    const void* r = d3d::view_resource(h.ptr, &f);
    *fmt = static_cast<DXGI_FORMAT>(f);
    if (!r) return -1;
    const char* name = d3d::resource_name(r);
    if (!name) return -1;
    for (int k = 0; k < kind_count(); ++k)
        if (std::strcmp(name, kNames[k]) == 0) {
            *res = static_cast<ID3D12Resource*>(const_cast<void*>(r));
            return k;
        }
    return -1;
}
int kind_of_resource(const ID3D12Resource* res) {
    for (int k = 0; k < kind_count(); ++k)
        if (g_arr[k].game && g_arr[k].game == res) return k;
    return -1;
}

bool heaps() {
    if (g_rtv_heap && g_dsv_heap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 * kKinds, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 3;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsv_heap)))) return false;
    g_rtv_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsv_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

// The array standing in for this game target (made, or made again when the game re-made its own).
bool ensure(int k, ID3D12Resource* res, DXGI_FORMAT view_fmt) {
    Arr& a = g_arr[k];
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (a.arr && a.game == res && a.w == d.Width && a.h == d.Height && a.res_fmt == d.Format) return true;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1) {
        fail("a pre-pass target is not a single 2D texture");
        return false;
    }
    if (!g_dev && FAILED(res->GetDevice(IID_PPV_ARGS(&g_dev)))) return false;
    if (!heaps()) {
        fail("no descriptor heaps");
        return false;
    }
    if (a.arr) g_retired.push_back({a.arr, state::presents.load()});  // the GPU may still use it: released later
    if (a.e2) g_retired.push_back({a.e2, state::presents.load()});
    a = Arr{};
    D3D12_RESOURCE_DESC ad = d;
    ad.DepthOrArraySize = 2;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    const DXGI_FORMAT vf = view_fmt != DXGI_FORMAT_UNKNOWN ? view_fmt : d.Format;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = vf;
    cv.DepthStencil = {1.0f, 0};
    ID3D12Resource* arr = nullptr;
    const HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, write_state(k), is_depth(k) ? &cv : nullptr,
                                                      IID_PPV_ARGS(&arr));
    if (FAILED(hr) || !arr) {
        char why[96];
        std::snprintf(why, sizeof(why), "the %s array not made (%#lx)", kNames[k], static_cast<unsigned long>(hr));
        fail(why);
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO fi{d.Format, 0};
    a.planes = SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi))) && fi.PlaneCount ? fi.PlaneCount : 1;
    for (int s = 0; s < 3; ++s) {  // s 2: both slices (the view-instanced draws' target)
        const UINT first = s == 2 ? 0u : static_cast<UINT>(s), count = s == 2 ? 2u : 1u;
        if (is_depth(k)) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = vf;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dv.Texture2DArray = {0, first, count};
            a.view[s] = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(s) * g_dsv_inc;
            g_dev->CreateDepthStencilView(arr, &dv, a.view[s]);
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = vf;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray = {0, first, count, 0};
            a.view[s] = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(k * 3 + s) * g_rtv_inc;
            g_dev->CreateRenderTargetView(arr, &rv, a.view[s]);
        }
    }
    a.game = res;
    a.w = d.Width;
    a.h = d.Height;
    a.res_fmt = d.Format;
    a.view_fmt = vf;
    a.arr = arr;
    wchar_t nm[96];
    std::swprintf(nm, 96, L"RDRVR prepass %S", kNames[k]);
    arr->SetName(nm);
    log::info("[prepass] %s: a 2-slice array %llux%u (format %d, view %d, %u plane(s))", kNames[k], static_cast<unsigned long long>(d.Width),
              d.Height, static_cast<int>(d.Format), static_cast<int>(vf), a.planes);
    if (g_i2.load(std::memory_order_relaxed) && kSlot[k] >= 0) {  // eye 2's copy, read by view 1 through the slot
        D3D12_RESOURCE_DESC ed = d;  // (a depth format keeps its depth-stencil flag)
        ed.Flags &= ~(is_depth(k) ? D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
                                  : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        ID3D12Resource* e2 = nullptr;
        if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e2))) && e2) {
            std::swprintf(nm, 96, L"RDRVR eye 2 %S", kNames[k]);
            e2->SetName(nm);
            a.e2 = e2;
            a.e2_state = D3D12_RESOURCE_STATE_COMMON;
            vi::eye2_texture(kSlot[k], e2, res);
        } else {
            log::warn("[prepass] %s: eye 2's copy not made (view 1 reads eye 1's)", kNames[k]);
        }
    }
    return true;
}

void release_retired() {
    const uint64_t now = state::presents.load();
    for (size_t i = 0; i < g_retired.size();)
        if (now > g_retired[i].frame + 4) {
            g_retired[i].res->Release();
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            ++i;
        }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

// The region's end: each touched target's slice copied into the game's target, from the states seen (per plane).
// ---- "prepass gpu [reset]": GPU timestamps on the region's list (the game's queue's clock): each region's span, and
// the copies inside it (publishes, the depth resolve, the region end's). Frames sorted by view-instanced or not, so
// "vi ab <n>" (view instancing every other n frames) compares both in the same view. Read back kFrames frames later.
namespace gpu {
constexpr int kFrames = 4, kMax = 4096;
enum Tag : uint8_t { kBegin0, kEnd0, kBegin1, kEnd1, kCopyBegin, kCopyEnd };
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_rb = nullptr;
uint64_t* g_mapped = nullptr;
bool g_on = false, g_failed = false;
struct Rec {
    int n = 0;
    Tag tag[kMax];
    bool vi = false, resolved = false, complete = false;
};
Rec g_rec[kFrames];
int g_slot = -1;
struct Sum {
    uint64_t frames = 0;
    double span = 0, r0 = 0, r1 = 0, copies = 0;
};
Sum g_sum[2];  // [vi]
uint64_t g_marks = 0, g_resolves = 0, g_harvests = 0, g_unresolved = 0, g_bad = 0;
double g_last[4] = {0, 0, 0, 0};
std::mutex g_mutex;
double g_tick_ms = 0;
bool ensure() {
    if (g_heap) return true;
    if (g_failed || !g_dev) return false;
    ID3D12CommandQueue* q = state::present_queue.load();
    UINT64 hz = 0;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * kMax, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC r{};
    r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    r.Width = kFrames * kMax * 8;
    r.Height = r.DepthOrArraySize = r.MipLevels = 1;
    r.SampleDesc.Count = 1;
    r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!q || FAILED(q->GetTimestampFrequency(&hz)) || !hz || FAILED(g_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap))) ||
        FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_rb))) ||
        FAILED(g_rb->Map(0, nullptr, reinterpret_cast<void**>(&g_mapped)))) {
        g_failed = true;
        log::warn("[prepass] gpu timers not made");
        return false;
    }
    g_tick_ms = 1000.0 / static_cast<double>(hz);
    return true;
}
void harvest(int slot) {  // that slot's frame, resolved kFrames frames ago
    Rec& rc = g_rec[slot];
    ++g_harvests;
    if (!rc.resolved || !rc.complete) {
        ++g_unresolved;
        return;
    }
    const uint64_t* t = g_mapped + slot * kMax;
    double b0 = -1, e0 = -1, b1 = -1, e1 = -1, copies = 0, cb = -1;
    for (int i = 0; i < rc.n; ++i) {
        const double v = static_cast<double>(t[i]) * g_tick_ms;
        switch (rc.tag[i]) {
            case kBegin0: b0 = v; break;
            case kEnd0: e0 = v; break;
            case kBegin1: b1 = v; break;
            case kEnd1: e1 = v; break;
            case kCopyBegin: cb = v; break;
            case kCopyEnd:
                if (cb >= 0) copies += v - cb;
                cb = -1;
                break;
        }
    }
    if (b0 < 0 || e0 < b0 || b1 < e0 || e1 < b1 || e1 - b0 > 200) {  // (an incomplete or wrapped frame)
        ++g_bad;
        g_last[0] = b0, g_last[1] = e0, g_last[2] = b1, g_last[3] = e1;
        return;
    }
    std::lock_guard lock(g_mutex);
    Sum& s = g_sum[rc.vi ? 1 : 0];
    ++s.frames;
    s.span += e1 - b0;
    s.r0 += e0 - b0;
    s.r1 += e1 - b1;
    s.copies += copies;
}
void mark(ID3D12GraphicsCommandList* cl, Tag tag) {
    if (!g_on || !ensure()) return;
    if (tag == kBegin0) {
        g_slot = static_cast<int>(state::presents.load() % kFrames);
        harvest(g_slot);
        g_rec[g_slot] = Rec{};
        g_rec[g_slot].vi = g_r.vi;
    }
    if (g_slot < 0) return;
    Rec& rc = g_rec[g_slot];
    if (rc.n >= kMax - 4 && (tag == kCopyBegin || tag == kCopyEnd)) return;  // (room kept for the regions' marks)
    if (rc.n >= kMax) return;
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(g_slot * kMax + rc.n));
    ++g_marks;
    rc.tag[rc.n++] = tag;
    if (tag == kEnd1) {
        cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(g_slot * kMax), static_cast<UINT>(rc.n), g_rb,
                             static_cast<UINT64>(g_slot) * kMax * 8);
        rc.resolved = rc.complete = true;
        ++g_resolves;
        g_slot = -1;
    }
}
std::string status(bool reset) {
    std::lock_guard lock(g_mutex);
    std::string r = g_on ? "gpu timers on" : "gpu timers off (prepass gpu on)";
    for (int v = 0; v < 2; ++v) {
        const Sum& s = g_sum[v];
        const double n = s.frames ? static_cast<double>(s.frames) : 1.0;
        char b[200];
        std::snprintf(b, sizeof(b), " | %s: %llu frames, both regions %.3f ms (eye 1's %.3f, eye 2's %.3f), copies in them %.3f",
                      v ? "view-instanced" : "two passes", static_cast<unsigned long long>(s.frames), s.span / n, s.r0 / n, s.r1 / n, s.copies / n);
        r += b;
    }
    char d[260];
    std::snprintf(d, sizeof(d), " | marks %llu, resolves %llu, harvests %llu (unresolved %llu, out of order %llu: last %.3f %.3f %.3f %.3f)",
                  static_cast<unsigned long long>(g_marks), static_cast<unsigned long long>(g_resolves), static_cast<unsigned long long>(g_harvests),
                  static_cast<unsigned long long>(g_unresolved), static_cast<unsigned long long>(g_bad), g_last[0] - g_last[0], g_last[1] - g_last[0],
                  g_last[2] - g_last[0], g_last[3] - g_last[0]);
    r += d;
    if (reset) g_sum[0] = g_sum[1] = Sum{};
    return r;
}
}  // namespace gpu

void end_region(ID3D12GraphicsCommandList* cl) {
    g_active = false;
    g_end_pending = false;
    unsigned need = 0;
    for (int k = 0; k < kKinds; ++k)
        if (g_r.touched & (1u << k)) need |= plane_bits(k);
    if ((g_r.known & need) != need) {
        fail("a pre-pass target's state not known at the region's end");
        return;
    }
    D3D12_RESOURCE_BARRIER pre[3 * kKinds], post[3 * kKinds];
    UINT n = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!(g_r.touched & (1u << k))) continue;
        const Arr& a = g_arr[k];
        pre[n] = transition(a.arr, write_state(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
        post[n] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state(k));
        ++n;
        for (UINT p = 0; p < a.planes && p < 2; ++p) {
            const D3D12_RESOURCE_STATES st = g_r.state[k][p];
            if (st == D3D12_RESOURCE_STATE_COPY_DEST) {
                fail("a pre-pass target already in COPY_DEST");
                return;
            }
            pre[n] = transition(a.game, st, D3D12_RESOURCE_STATE_COPY_DEST);
            post[n] = transition(a.game, D3D12_RESOURCE_STATE_COPY_DEST, st);
            if (a.planes > 1) pre[n].Transition.Subresource = post[n].Transition.Subresource = p;  // (one mip, one slice)
            ++n;
        }
    }
    const gpu::Tag end_tag = g_r.slice == 0 ? gpu::kEnd0 : gpu::kEnd1;
    if (!n) {
        gpu::mark(cl, end_tag);
        return;
    }
    gpu::mark(cl, gpu::kCopyBegin);
    d3d::raw_barrier(cl, n, pre);
    for (int k = 0; k < kKinds; ++k) {
        if (!(g_r.touched & (1u << k))) continue;
        const Arr& a = g_arr[k];
        for (UINT p = 0; p < a.planes; ++p) {
            D3D12_TEXTURE_COPY_LOCATION dst{a.game, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            dst.SubresourceIndex = p;
            D3D12_TEXTURE_COPY_LOCATION src{a.arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            src.SubresourceIndex = static_cast<UINT>(g_r.slice) + p * 2;  // mip + slice * mips + plane * mips * slices
            d3d::raw_copy_region(cl, &dst, &src);
        }
        g_copies.fetch_add(1, std::memory_order_relaxed);
    }
    d3d::raw_barrier(cl, n, post);
    gpu::mark(cl, gpu::kCopyEnd);
    gpu::mark(cl, end_tag);
    g_marker_ends.fetch_add(1, std::memory_order_relaxed);
}

// ---- [Stereo] ViMain: a kind the game turns from written to read in the region, copied out of its slices before the
// game's barrier: the region's slice into the game's target (from `st`, its states per plane before the barrier), and,
// in a view-instanced slice-0 region, slice 1 into eye 2's copy (left readable by the pixel shaders).
constexpr D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// ---- "prepass grab <kind> <path>": both slices of a kind's array as the next view-instanced frame's slice-0 region
// publishes them (plane 0; each publish in that region copies again, the last one's in the file): "RDA1", width,
// height, footprint format, row bytes, then slice 0's rows and slice 1's (slice 1: what eye 2's copy got).
std::atomic<int> g_grab_phase{0};  // 0 idle, 1 armed (kind g_grab_kind), 2 copied this frame, 3 to finish
int g_grab_kind = -1;
uint64_t g_grab_frame = 0;
ID3D12Resource* g_grab_rb = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_grab_fp[2]{};
UINT64 g_grab_row = 0;
uint32_t g_grab_h = 0;
std::string g_grab_path, g_grab_result;
HANDLE g_grab_done = nullptr;
void grab_tap(ID3D12GraphicsCommandList* cl, int k, ID3D12Resource* arr) {  // arr in COPY_SOURCE
    const int ph = g_grab_phase.load(std::memory_order_acquire);
    if (k != g_grab_kind || !(ph == 1 || (ph == 2 && g_grab_frame == state::presents.load())) || !g_r.vi || g_r.slice != 0) return;
    D3D12_RESOURCE_DESC d = arr->GetDesc();
    UINT rows[2];
    UINT64 rb[2], total = 0;
    const UINT first = 0;  // plane 0: subresources 0 and 1 (mip 0, slices 0 and 1)
    g_dev->GetCopyableFootprints(&d, first, 2, 0, g_grab_fp, rows, rb, &total);
    if (!g_grab_rb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC r{};
        r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        r.Width = total;
        r.Height = r.DepthOrArraySize = r.MipLevels = 1;
        r.SampleDesc.Count = 1;
        r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_grab_rb))))
            return;
    }
    for (UINT s = 0; s < 2; ++s) {
        D3D12_TEXTURE_COPY_LOCATION dl{g_grab_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dl.PlacedFootprint = g_grab_fp[s];
        D3D12_TEXTURE_COPY_LOCATION sl{arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        sl.SubresourceIndex = s;
        d3d::raw_copy_region(cl, &dl, &sl);
    }
    g_grab_row = rb[0];
    g_grab_h = rows[0];
    g_grab_frame = state::presents.load();
    g_grab_phase.store(2, std::memory_order_release);
}
std::string grab_finish() {
    ID3D12CommandQueue* q = state::present_queue.load();
    ID3D12Fence* fence = nullptr;
    if (!q || FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return "ERROR no queue or fence";
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1);
    fence->SetEventOnCompletion(1, ev);
    const bool done = WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0;
    CloseHandle(ev);
    fence->Release();
    if (!done) return "ERROR the copy timed out";
    uint8_t* p = nullptr;
    if (FAILED(g_grab_rb->Map(0, nullptr, reinterpret_cast<void**>(&p))) || !p) return "ERROR Map failed";
    std::string result;
    FILE* f = nullptr;
    if (fopen_s(&f, g_grab_path.c_str(), "wb") != 0 || !f) {
        result = "ERROR cannot open " + g_grab_path;
    } else {
        const uint32_t hdr[5] = {0x31414452u /* RDA1 */, g_grab_fp[0].Footprint.Width, g_grab_h, static_cast<uint32_t>(g_grab_fp[0].Footprint.Format),
                                 static_cast<uint32_t>(g_grab_row)};
        fwrite(hdr, 1, sizeof(hdr), f);
        for (int s = 0; s < 2; ++s)
            for (uint32_t y = 0; y < g_grab_h; ++y)
                fwrite(p + g_grab_fp[s].Offset + static_cast<uint64_t>(y) * g_grab_fp[s].Footprint.RowPitch, 1, static_cast<size_t>(g_grab_row), f);
        fclose(f);
        result = "wrote " + g_grab_path + " (" + kNames[g_grab_kind] + ", both slices)";
    }
    D3D12_RANGE none{0, 0};
    g_grab_rb->Unmap(0, &none);
    return result;
}
void grab_frame_end(uint64_t) {
    if (g_grab_phase.load(std::memory_order_acquire) == 2 && state::presents.load() > g_grab_frame + 1) {
        g_grab_result = grab_finish();
        g_grab_phase = 0;
        SetEvent(g_grab_done);
    }
}

std::atomic<uint64_t> g_pub_kind[kKinds][2];  // publishes per kind, [view-instanced region]
void publish(ID3D12GraphicsCommandList* cl, int k, const D3D12_RESOURCE_STATES st[2]) {
    Arr& a = g_arr[k];
    g_pub_kind[k][g_r.vi ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    if (!a.arr || !a.game) return;
    const bool e2 = a.e2 && g_r.vi && g_r.slice == 0;
    D3D12_RESOURCE_BARRIER pre[4], post[4];
    UINT n = 0;
    pre[n] = transition(a.arr, write_state(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
    post[n++] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state(k));
    for (UINT p = 0; p < a.planes && p < 2; ++p) {
        if (st[p] == D3D12_RESOURCE_STATE_COPY_DEST) return;  // (never seen: nothing to do safely)
        pre[n] = transition(a.game, st[p], D3D12_RESOURCE_STATE_COPY_DEST);
        post[n] = transition(a.game, D3D12_RESOURCE_STATE_COPY_DEST, st[p]);
        if (a.planes > 1) pre[n].Transition.Subresource = post[n].Transition.Subresource = p;
        ++n;
    }
    if (e2) {
        if (a.e2_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            pre[n] = transition(a.e2, a.e2_state, D3D12_RESOURCE_STATE_COPY_DEST);
            post[n++] = transition(a.e2, D3D12_RESOURCE_STATE_COPY_DEST, kReadable);
        }
        a.e2_state = kReadable;
    }
    gpu::mark(cl, gpu::kCopyBegin);
    d3d::raw_barrier(cl, n, pre);
    for (UINT p = 0; p < a.planes; ++p) {
        D3D12_TEXTURE_COPY_LOCATION src{a.arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = static_cast<UINT>(g_r.slice) + p * 2;
        D3D12_TEXTURE_COPY_LOCATION dst{a.game, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        dst.SubresourceIndex = p;
        d3d::raw_copy_region(cl, &dst, &src);
        if (e2) {
            src.SubresourceIndex = 1 + p * 2;
            dst.pResource = a.e2;
            d3d::raw_copy_region(cl, &dst, &src);
        }
    }
    if (g_grab_phase.load(std::memory_order_relaxed)) grab_tap(cl, k, a.arr);
    d3d::raw_barrier(cl, n, post);
    gpu::mark(cl, gpu::kCopyEnd);
    g_publishes.fetch_add(1, std::memory_order_relaxed);
}
UINT plane_count(ID3D12Resource* r) {
    D3D12_FEATURE_DATA_FORMAT_INFO fi{r->GetDesc().Format, 0};
    return g_dev && SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi))) && fi.PlaneCount ? fi.PlaneCount : 1;
}
bool writing(int k, D3D12_RESOURCE_STATES s) {
    return s == write_state(k) || (is_depth(k) && s == D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ---- the walk's end, recorded on the render thread: FUN_1405cbfd0 calls the letterbox clear right after the walk
// (its other callers return elsewhere). An op-0x4a callback there (DeferredAppend, as the DLSS work records its own)
// runs on the playback thread at that point of the stream.
using Letterbox_t = void (*)(void* renderer);
Letterbox_t o_letterbox = nullptr;
using Append_t = void (*)(void* recorder, void* fn, const void* data, uint64_t size);

template <class T>
bool rd(uintptr_t a, T* out) {
    if (!a) return false;
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void walk_end_cb(void*) { g_end_pending.store(true, std::memory_order_release); }

// View-instanced frames are decided on the render thread (pass 2's camera depends on it, camera_lever.cpp) and told to
// the playback thread through the stream: a marker recorded before pass 1 sets the next slice-0 region's choice.
std::atomic<int> g_next_vi{0};  // 1: the next slice-0 region draws both eyes
thread_local bool t_latched_vi = false;  // the render thread's choice for its current frame (latch_vi)
void vi_on_cb(void*) { g_next_vi.store(1, std::memory_order_release); }
void vi_off_cb(void*) { g_next_vi.store(0, std::memory_order_release); }
bool append_marker(void (*fn)(void*)) {
    uintptr_t dev = 0, hrec = 0, rec = 0, vt = 0;
    if (!rd(anchors::addr(anchors::Id::D3dDeviceSingleton), &dev) || !rd(dev + 0x58, &hrec) || !rd(hrec + 0x128, &rec) || !rd(rec, &vt) ||
        vt != anchors::addr(anchors::Id::RecorderVtbl))
        return false;
    reinterpret_cast<Append_t>(anchors::addr(anchors::Id::DeferredAppend))(reinterpret_cast<void*>(rec), reinterpret_cast<void*>(fn), nullptr, 0);
    return true;
}

// ---- [Stereo] PrepassViews, step 1 (observe): the render thread only
struct Rows {
    float wv[16], vi[16], pvp[16], vport[4];  // gWorldView, gViewInverse (b1), gPrevViewProj (rage_prev_proj), gViewport
    bool ok = false;
};
Rows g_seen[2];          // after the walk, pass 1 and pass 2 (the game's own)
float g_e2_wv[16], g_e2_vi[16], g_e2_pvp[16];  // pass 1's prediction of eye 2's
bool g_e2_ok = false, g_e2_pvp_ok = false;
float g_e2_off[3] = {0, 0, 0};  // eye 1's camera position less eye 2's (set_eye2_offset, before set_eye2)
bool g_e2_off_ok = false;
float g_e1_pvp[16];       // eye 1's gPrevViewProj this frame (taa.cpp, before eye 2's update): D's first factor
bool g_e1_pvp_ok = false;
// ---- eye 2's motion vectors in view-instanced draws (KF2VR's lesson: each eye's own previous-to-current transform, in
// double precision). The per-draw gPrevWorldViewProjSingle (rage_prev_world) is the draw's previous world times the
// pass's gPrevViewProj; both passes share the world (eye 1's camera-relative frame), so eye 2's is eye 1's times
// D = inverse(eye 1's gPrevViewProj) x eye 2's. D goes to rage_matrices rows 21-24 (eye 2's gWorldViewProj slot, unused:
// the converter refuses that row), where view 1 of a converted shader multiplies its rage_prev_world rows by it.
// Identity when the previous view-projections are not known (no TAA or DLSS update: no motion vectors used then).
float g_e2_delta[16];
bool g_e2_delta_ok = false;
bool invert4(const double m[16], double out[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::fabs(det) > 1e-30)) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}
uint64_t g_e2_frame = ~0ull;
struct Stats {
    uint64_t frames = 0, refused = 0;
    float wv = 0, vi = 0, eyes_wv = 0, eyes_vi = 0, pvp = 0, vport = 0, e2_pvp = 0;  // the largest differences
    uint64_t pvp_frames = 0, bad_wv = 0, bad_vi = 0, bad_pvp = 0, bad_first = 0;
} g_vs;
std::atomic<uint64_t> g_vs_frames{0};
char g_vs_text[700] = "no frame compared yet";

bool read_raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A shader global's 16 x n bytes from its constant buffer's storage (cb +0x00), by its handle's table entry.
bool global_rows(anchors::Id handle, float* out, size_t bytes, uintptr_t* cb_out = nullptr) {
    int h = 0;
    uintptr_t cb = 0, storage = 0;
    int off = -1;
    const uintptr_t table = anchors::addr(anchors::Id::ShaderGlobalTable);
    if (!anchors::present(handle) || !rd(anchors::addr(handle), &h) || h <= 0 || h > 4096 || !rd(table + static_cast<uintptr_t>(h) * 16, &cb) ||
        !rd(table + static_cast<uintptr_t>(h) * 16 + 8, &off) || off < 0 || off > 0x1000 || !rd(cb, &storage))
        return false;
    if (cb_out) *cb_out = cb;
    return read_raw(storage + static_cast<uintptr_t>(off), out, bytes);
}
// ---- [Stereo] PrepassViews=2, step 2: eye 2's rows written past the declared ends of rage_matrices (b1) and
// rage_prev_proj (stageVI-prepass.md, milestone 3): each buffer's storage swapped once for a larger block from the
// game's own allocator (ucrtbase's malloc, so the game's free stays valid; the old block never freed), then its declared
// size (layout +0x14) grown so the game's own upload carries the rows. The upload's rounded size (256) does not change.
template <class T>
bool wr(uintptr_t a, T v) {
    if (!a) return false;
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_raw(uintptr_t a, const void* src, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool global_cb(anchors::Id handle, uintptr_t* cb, int* off) {
    int h = 0;
    const uintptr_t table = anchors::addr(anchors::Id::ShaderGlobalTable);
    return anchors::present(handle) && rd(anchors::addr(handle), &h) && h > 0 && h <= 4096 &&
           rd(table + static_cast<uintptr_t>(h) * 16, cb) && *cb && rd(table + static_cast<uintptr_t>(h) * 16 + 8, off);
}
struct Grown {
    uintptr_t cb = 0, block = 0;
    uint32_t declared = 0, grown = 0;
};
Grown g_b1, g_pp;           // rage_matrices, rage_prev_proj
int g_grow_state = 0;       // 0 not tried, 1 grown, -1 refused
char g_grow_why[160] = "";
uint64_t g_written = 0, g_kept = 0, g_lost = 0;
uint64_t g_vi_rows_same = 0, g_vi_rows_differ = 0;  // view-instanced frames: pass 2's gWorldView/position as the pre-pass's
float g_vi_rows_max = 0;
uint64_t g_mv_checks = 0, g_mv_unread = 0, g_mv_not_vi = 0, g_mv_no_delta = 0;  // (why a frame was not checked)  // view-instanced frames whose motion-vector transform was checked against pass 2
float g_mv_err = 0;        // the largest relative difference
float g_written_wv[16], g_written_vi[16], g_written_pp[16];

bool grow(Grown& g, uintptr_t cb, uint32_t declared, uint32_t grown, uint32_t alloc) {
    using Malloc_t = void* (*)(size_t);
    static const auto game_malloc = reinterpret_cast<Malloc_t>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "malloc"));
    uintptr_t layout = 0, old = 0, mapped = 0;
    uint32_t size = 0;
    if (!game_malloc || !rd(cb + 0x08, &layout) || !rd(layout + 0x14, &size) || size != declared || !rd(cb, &old) || !old || !rd(cb + 0x18, &mapped))
        return false;
    if (((declared + 0xffu) & ~0xffu) != ((grown + 0xffu) & ~0xffu)) return false;  // the upload's size must not change
    void* blk = game_malloc(alloc);
    if (!blk) return false;
    std::memset(blk, 0, alloc);
    if (!read_raw(old, blk, declared)) return false;  // (the block leaks: the game never frees it either way)
    if (!wr<uintptr_t>(cb, reinterpret_cast<uintptr_t>(blk))) return false;
    if (mapped == old) wr<uintptr_t>(cb + 0x18, reinterpret_cast<uintptr_t>(blk));
    if (!wr<uint32_t>(layout + 0x14, grown)) {
        wr<uintptr_t>(cb, old);  // back as it was
        if (mapped == old) wr<uintptr_t>(cb + 0x18, old);
        return false;
    }
    g = Grown{cb, reinterpret_cast<uintptr_t>(blk), declared, grown};
    return true;
}
void refuse_grow(const char* why) {
    g_grow_state = -1;
    std::snprintf(g_grow_why, sizeof(g_grow_why), "%s", why);
    log::warn("[prepass] views: eye 2's rows not written: %s", why);
}
bool ensure_grown() {
    if (g_grow_state) return g_grow_state > 0;
    uintptr_t cb_wv = 0, cb_vi = 0, cb_pp = 0;
    int off_wv = -1, off_vi = -1, off_pp = -1;
    if (!global_cb(anchors::Id::HandleWorldView, &cb_wv, &off_wv) || !global_cb(anchors::Id::HandleViewInverse, &cb_vi, &off_vi) ||
        !global_cb(anchors::Id::HandlePrevViewProj, &cb_pp, &off_pp)) {
        refuse_grow("a shader global not found");
        return false;
    }
    if (cb_wv != cb_vi || off_wv != 0x40 || off_vi != 0xc0 || off_pp != 0) {
        char why[160];
        std::snprintf(why, sizeof(why), "not the expected layout (b1 %#llx/%#llx, gWorldView +%#x, gViewInverse +%#x, gPrevViewProj +%#x)",
                      static_cast<unsigned long long>(cb_wv), static_cast<unsigned long long>(cb_vi), off_wv, off_vi, off_pp);
        refuse_grow(why);
        return false;
    }
    if (!grow(g_b1, cb_wv, 0x110, 0x200, 0x200)) {
        refuse_grow("rage_matrices not grown (its size is not 0x110, or a read failed)");
        return false;
    }
    if (!grow(g_pp, cb_pp, 0x40, 0x80, 0x100)) {
        refuse_grow("rage_prev_proj not grown (its size is not 0x40, or a read failed)");
        return false;
    }
    g_grow_state = 1;
    log::info("[prepass] views: rage_matrices %#llx grown 0x110 -> 0x200 (storage %#llx), rage_prev_proj %#llx 0x40 -> 0x80 (storage %#llx)",
              static_cast<unsigned long long>(g_b1.cb), static_cast<unsigned long long>(g_b1.block), static_cast<unsigned long long>(g_pp.cb),
              static_cast<unsigned long long>(g_pp.block));
    return true;
}
std::atomic<int> g_size_want{-1};  // the test's "prepass views size on|off": applied on the render thread (1 grown, 0 declared)
void apply_size_want() {
    const int w = g_size_want.exchange(-1);
    if (w < 0 || g_grow_state <= 0) return;
    for (Grown* g : {&g_b1, &g_pp}) {
        uintptr_t layout = 0;
        if (rd(g->cb + 0x08, &layout)) wr<uint32_t>(layout + 0x14, w ? g->grown : g->declared);
    }
    log::info("[prepass] views: the declared sizes %s", w ? "grown (0x200, 0x80)" : "as the game's (0x110, 0x40)");
}
bool still_grown(const Grown& g) {
    uintptr_t storage = 0, layout = 0;
    uint32_t size = 0;
    return rd(g.cb, &storage) && storage == g.block && rd(g.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g.grown;
}
// pass 1, before the pre-pass: rows 17-20 (gWorldView) and 25-28 (gViewInverse) of b1, rows 4-7 of rage_prev_proj
// Eye 2's gWorldView inverted (double precision), packed into rage_matrices rows 29-31 for the converted shaders that
// read an inverse view-projection (tools/sp/dxil/viewid_patch.py patch_inverse: X2 = inv(gWorldView2) x T(camera2 -
// camera1) x gWorldView1 x X1). A perspective's inverse has rows 0 and 1 ending in 0 and row 2 (0, 0, 0, k), so three
// rows hold it: row 29 (inv[0].xyz, inv[2].w), row 30 (inv[1].xyz, 0), row 31 inv[3]. The parts the packing drops are
// checked: their largest, relative to the matrix's largest entry, is in "vi status".
uint64_t g_inv_written = 0, g_inv_failed = 0;
int64_t g_depth_index = -1;
float g_inv_dropped = 0;
void write_inv_wv2(const float* wv) {
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = wv[i];
    float rows[12] = {};
    if (!invert4(a, inv)) {
        ++g_inv_failed;
        write_raw(g_b1.block + 0x1d0, rows, sizeof(rows));
        return;
    }
    double big = 0, dropped = 0;
    for (double x : inv) big = std::max(big, std::fabs(x));
    for (int i : {3, 7, 8, 9, 10}) dropped = std::max(dropped, std::fabs(inv[i]));
    if (big > 0) g_inv_dropped = std::max(g_inv_dropped, static_cast<float>(dropped / big));
    const int map[12] = {0, 1, 2, 11, 4, 5, 6, -1, 12, 13, 14, 15};
    for (int i = 0; i < 12; ++i) rows[i] = map[i] < 0 ? 0.f : static_cast<float>(inv[map[i]]);
    // row 30 .w: the game's scene-depth view index (as a uint), for view 1's index compare (vi::kDepthBufferSlot)
    const int64_t di = vi::game_view_index(vi::kDepthBufferSlot);
    const uint32_t du = di >= 0 && di < 0x7fffffff ? static_cast<uint32_t>(di) : 0xffffffffu;
    std::memcpy(&rows[7], &du, 4);
    g_depth_index = di;
    write_raw(g_b1.block + 0x1d0, rows, sizeof(rows));
    ++g_inv_written;
}
void write_eye2(const float* wv, const float* vi, const float* pvp) {
    if (!ensure_grown()) return;
    apply_size_want();
    {  // the test's sizes back as the game's: the rows are not uploaded then, nothing to check
        uintptr_t layout = 0;
        uint32_t size = 0;
        if (rd(g_b1.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g_b1.declared) return;
    }
    if (!still_grown(g_b1) || !still_grown(g_pp)) {
        refuse_grow("a grown buffer was made again by the game (its storage or size changed)");
        return;
    }
    float pp[16];
    if (pvp) std::memcpy(pp, pvp, sizeof(pp));
    else if (!read_raw(g_pp.block, pp, sizeof(pp))) return;  // no jitter: both eyes' gPrevViewProj are the same
    if (!write_raw(g_b1.block + 0x110, wv, 64) || !write_raw(g_b1.block + 0x190, vi, 64) || !write_raw(g_pp.block + 0x40, pp, 64)) return;
    write_inv_wv2(wv);
    wr<int>(g_b1.cb + 0x24, -1);  // (re-uploaded at its next commit)
    wr<int>(g_pp.cb + 0x24, -1);
    std::memcpy(g_written_wv, wv, 64);
    std::memcpy(g_written_vi, vi, 64);
    std::memcpy(g_written_pp, pp, 64);
    ++g_written;
}
void write_prev_delta(const float* p1, const float* p2) {
    double d[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool ok = false;
    if (p1 && p2) {
        double a[16], inv[16];
        for (int i = 0; i < 16; ++i) a[i] = p1[i];
        if (invert4(a, inv)) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    double s = 0;
                    for (int k = 0; k < 4; ++k) s += inv[r * 4 + k] * static_cast<double>(p2[k * 4 + c]);
                    d[r * 4 + c] = s;
                }
            ok = true;
            for (double x : d) ok = ok && std::isfinite(x);
        }
    }
    if (!ok) d[0] = d[5] = d[10] = d[15] = 1, d[1] = d[2] = d[3] = d[4] = d[6] = d[7] = d[8] = d[9] = d[11] = d[12] = d[13] = d[14] = 0;
    for (int i = 0; i < 16; ++i) g_e2_delta[i] = static_cast<float>(d[i]);
    g_e2_delta_ok = ok;
    write_raw(g_b1.block + 0x150, g_e2_delta, 64);  // rows 21-24 (re-uploaded with the rows write_eye2 marked)
}
// after pass 1's walk: the rows still as written (nothing else writes past the declared ends)
void check_written() {
    if (g_grow_state <= 0) return;
    {
        uintptr_t layout = 0;
        uint32_t size = 0;
        if (rd(g_b1.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g_b1.declared) return;  // (the test's sizes off)
    }
    float a[16], b[16], c[16];
    if (still_grown(g_b1) && still_grown(g_pp) && read_raw(g_b1.block + 0x110, a, 64) && read_raw(g_b1.block + 0x190, b, 64) &&
        read_raw(g_pp.block + 0x40, c, 64) && !std::memcmp(a, g_written_wv, 64) && !std::memcmp(b, g_written_vi, 64) &&
        !std::memcmp(c, g_written_pp, 64))
        ++g_kept;
    else
        ++g_lost;
}

float max_diff(const float* a, const float* b, int n) {
    float m = 0;
    for (int i = 0; i < n; ++i) m = std::fmax(m, std::fabs(a[i] - b[i]));
    return m;
}
void observe_after_walk() {
    const int pass = dual_pass::pass();  // 1, 2 (0 mono)
    if (pass < 1 || pass > 2) return;
    if (pass == 1 && g_views_write.load(std::memory_order_relaxed)) check_written();
    Rows& r = g_seen[pass - 1];
    r.ok = global_rows(anchors::Id::HandleWorldView, r.wv, 64) && global_rows(anchors::Id::HandleViewInverse, r.vi, 64) &&
           global_rows(anchors::Id::HandlePrevViewProj, r.pvp, 64) && global_rows(anchors::Id::HandleViewport, r.vport, 16);
    // the motion-vector transform's check: the walk's last draw's gPrevWorldViewProjSingle, pass 1's times D against the
    // game's own in pass 2 (the same draw: both passes walk the same lists)
    {
        static float pw1[16];
        static bool pw1_ok = false;
        float pw[16];
        const bool got = global_rows(anchors::Id::HandlePrevWorldVP, pw, 64);
        if (!got) ++g_mv_unread;
        if (pass == 1 && !t_latched_vi) ++g_mv_not_vi;
        if (pass == 1 && !g_e2_delta_ok) ++g_mv_no_delta;
        if (pass == 1) {
            pw1_ok = got && t_latched_vi && g_e2_delta_ok;
            if (pw1_ok) std::memcpy(pw1, pw, sizeof(pw1));
        } else if (pw1_ok && got) {
            float pred[16];
            for (int rr = 0; rr < 4; ++rr)
                for (int c = 0; c < 4; ++c) {
                    float s = 0;
                    for (int k = 0; k < 4; ++k) s += pw1[rr * 4 + k] * g_e2_delta[k * 4 + c];
                    pred[rr * 4 + c] = s;
                }
            float scale = 1e-6f;
            for (float x : pw) scale = std::fmax(scale, std::fabs(x));
            g_mv_err = std::fmax(g_mv_err, max_diff(pred, pw, 16) / scale);
            ++g_mv_checks;
            pw1_ok = false;
        }
    }
    if (pass != 2) return;
    const uint64_t f = state::presents.load();
    if (r.ok && t_latched_vi && g_written) {  // a view-instanced frame: pass 2's rows are the ones its pre-pass drew with
        const float dw = max_diff(g_written_wv, r.wv, 16), dv = max_diff(g_written_vi + 12, r.vi + 12, 3);
        g_vi_rows_max = std::fmax(g_vi_rows_max, std::fmax(dw, dv));
        ++(dw == 0.0f && dv == 0.0f ? g_vi_rows_same : g_vi_rows_differ);
    }
    if (!g_seen[0].ok || !r.ok || !g_e2_ok || g_e2_frame != f) {
        ++g_vs.refused;
        return;
    }
    ++g_vs.frames;
    const float dwv = max_diff(g_e2_wv, r.wv, 16), dvi = max_diff(g_e2_vi, r.vi, 16);
    g_vs.wv = std::fmax(g_vs.wv, dwv);
    g_vs.vi = std::fmax(g_vs.vi, dvi);
    if (dwv > 1e-4f) ++g_vs.bad_wv;
    if (dvi > 1e-4f) ++g_vs.bad_vi;
    if ((dwv > 1e-4f || dvi > 1e-4f) && !g_vs.bad_first) {
        g_vs.bad_first = g_vs.frames;
        log::info("[prepass] views: the first mismatch, frame %llu of the comparison: gWorldView %.6f gViewInverse %.6f; predicted wv row 0 "
                  "(%.4f %.4f %.4f %.4f) row 3 (%.4f %.4f %.4f %.4f), the game's (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f); vi row 3 "
                  "predicted (%.3f %.3f %.3f) the game's (%.3f %.3f %.3f) pass 1's (%.3f %.3f %.3f)",
                  static_cast<unsigned long long>(g_vs.frames), dwv, dvi, g_e2_wv[0], g_e2_wv[1], g_e2_wv[2], g_e2_wv[3], g_e2_wv[12], g_e2_wv[13],
                  g_e2_wv[14], g_e2_wv[15], r.wv[0], r.wv[1], r.wv[2], r.wv[3], r.wv[12], r.wv[13], r.wv[14], r.wv[15], g_e2_vi[12], g_e2_vi[13],
                  g_e2_vi[14], r.vi[12], r.vi[13], r.vi[14], g_seen[0].vi[12], g_seen[0].vi[13], g_seen[0].vi[14]);
    }
    g_vs.eyes_wv = std::fmax(g_vs.eyes_wv, max_diff(g_seen[0].wv, r.wv, 16));
    g_vs.eyes_vi = std::fmax(g_vs.eyes_vi, max_diff(g_seen[0].vi, r.vi, 16));
    g_vs.pvp = std::fmax(g_vs.pvp, max_diff(g_seen[0].pvp, r.pvp, 16));
    g_vs.vport = std::fmax(g_vs.vport, max_diff(g_seen[0].vport, r.vport, 4));
    if (g_e2_pvp_ok) {
        ++g_vs.pvp_frames;
        const float dp = max_diff(g_e2_pvp, r.pvp, 16);
        g_vs.e2_pvp = std::fmax(g_vs.e2_pvp, dp);
        if (dp > 1e-4f) ++g_vs.bad_pvp;
    }
    g_e2_ok = false;
    if (g_vs_frames.fetch_add(1, std::memory_order_relaxed) % 600 == 0 || g_vs.frames < 3) {
        std::snprintf(g_vs_text, sizeof(g_vs_text),
                      "%llu frames compared (%llu not): eye 2 predicted in pass 1 against the game's pass 2, largest differences: "
                      "gWorldView %.6f, gViewInverse %.6f | the eyes against each other: gWorldView %.6f, gViewInverse %.6f | "
                      "pass 1 against 2: gPrevViewProj %.6f, gViewport %.6f | eye 2's gPrevViewProj predicted (%llu frames): %.6f | "
                      "frames off by more than 1e-4: gWorldView %llu, gViewInverse %llu, gPrevViewProj %llu",
                      static_cast<unsigned long long>(g_vs.frames), static_cast<unsigned long long>(g_vs.refused), g_vs.wv, g_vs.vi,
                      g_vs.eyes_wv, g_vs.eyes_vi, g_vs.pvp, g_vs.vport, static_cast<unsigned long long>(g_vs.pvp_frames), g_vs.e2_pvp,
                      static_cast<unsigned long long>(g_vs.bad_wv), static_cast<unsigned long long>(g_vs.bad_vi), static_cast<unsigned long long>(g_vs.bad_pvp));
        log::info("[prepass] views: %s", g_vs_text);
    }
}

void hk_letterbox(void* renderer) {
    if (g_views.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5)
        observe_after_walk();
    if (g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) && !g_i2.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5) {
        uintptr_t dev = 0, hrec = 0, rec = 0, vt = 0;
        if (rd(anchors::addr(anchors::Id::D3dDeviceSingleton), &dev) && rd(dev + 0x58, &hrec) && rd(hrec + 0x128, &rec) && rd(rec, &vt) &&
            vt == anchors::addr(anchors::Id::RecorderVtbl)) {
            reinterpret_cast<Append_t>(anchors::addr(anchors::Id::DeferredAppend))(reinterpret_cast<void*>(rec),
                                                                                    reinterpret_cast<void*>(&walk_end_cb), nullptr, 0);
            g_markers.fetch_add(1, std::memory_order_relaxed);
        } else {
            log::limited("prepass.norec", 5, "[prepass] the walk's end: the recorder not found (no marker)");
        }
    }
    o_letterbox(renderer);
}

}  // namespace

void init() {
    g_on = false;  // on only once install() has its hook
}
void install() {
    const bool arrays = config::get_bool("Stereo", "PrepassArrays", false), views = config::get_int("Stereo", "PrepassViews", 0) >= 1;
    if ((!arrays && !views) || anchors::stand_down()) return;
    using anchors::Id;
    if (!anchors::present(Id::LetterboxClear) || !anchors::present(Id::PrepassLetterboxCall) || !anchors::addr(Id::DeferredAppend)) {
        log::warn("[prepass] not installed: this build of RDR.exe has no LetterboxClear/PrepassLetterboxCall");
        return;
    }
    if (!hooks::install("RDR letterbox clear (the pre-pass walk's end, PrepassArrays)", reinterpret_cast<void*>(anchors::addr(Id::LetterboxClear)),
                        reinterpret_cast<void*>(&hk_letterbox), reinterpret_cast<void**>(&o_letterbox)))
        return;
    g_hooked = true;
    g_on = arrays;
    g_i2 = arrays && config::get_bool("Stereo", "ViMain", false);
    if (g_i2) log::info("[prepass] [Stereo] ViMain on (test only: the region to the scene's end, the main pass's targets too)");
    if (arrays) log::info("[prepass] [Stereo] PrepassArrays on (test only: each eye's pre-pass drawn into 2-slice arrays, copied back at the walk's end)");
    if (views && anchors::present(Id::HandleWorldView) && anchors::present(Id::HandleViewInverse) && anchors::present(Id::HandlePrevViewProj) &&
        anchors::present(Id::HandleViewport) && anchors::present(Id::HandlePrevWorldVP)) {
        g_views = true;
        g_views_write = config::get_int("Stereo", "PrepassViews", 0) >= 2;
        log::info("[prepass] [Stereo] PrepassViews on (test only: eye 2's rows predicted in pass 1, compared with the game's pass 2%s)",
                  g_views_write.load() ? "; and written past rage_matrices' and rage_prev_proj's declared ends" : "");
    }
}
bool on() { return g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed); }
bool active() { return g_active.load(std::memory_order_relaxed); }
void set_on(bool want) {
    if (want && !g_hooked.load()) {
        log::warn("[prepass] not on: start with [Stereo] PrepassArrays=1 (the walk's end hook)");
        return;
    }
    g_on = want;
    if (want) trace_start(3);
    log::info("[prepass] %s", want ? "on (from the next pre-pass)" : "off (from the next pre-pass)");
}

D3D12_CPU_DESCRIPTOR_HANDLE clear_dsv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (kind_of(h, &res, &fmt) != kDepth) return h;
    const uint64_t f = state::presents.load();
    if (g_active.load()) {  // the depth cleared again in a region: the last one never ended
        g_active = false;
        fail("a pre-pass region did not end before the next pre-pass");
        return h;
    }
    if (f != g_frame_seen) {
        g_frame_seen = f;
        g_pass_in_frame = 0;
        release_retired();
    }
    const int slice = g_pass_in_frame++;
    if (slice > 1) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return h;
    }
    if (!ensure(kDepth, res, fmt)) return h;
    g_r = Region{};
    g_r.cl = cl;
    g_r.slice = slice;
    // view-instanced stereo: slice 0's region draws both eyes when wanted; slice 1's region follows it in the same frame
    // only (a frame whose slice 0 drew one eye draws slice 1 itself)
    if (slice == 0) {
        g_r.vi = g_next_vi.exchange(0, std::memory_order_acq_rel) != 0;
        g_vi_frame = g_r.vi ? f : ~0ull;
    } else {
        g_r.vi = g_vi_frame == f;
    }
    if (g_r.vi) g_vi_regions[slice].fetch_add(1, std::memory_order_relaxed);
    g_r.touched = 1u << kDepth;
    set_state(kDepth, -1, D3D12_RESOURCE_STATE_DEPTH_WRITE);  // (a depth clear needs it)
    g_end_pending = false;
    g_active = true;
    g_regions.fetch_add(1, std::memory_order_relaxed);
    gpu::mark(cl, slice == 0 ? gpu::kBegin0 : gpu::kBegin1);
    if (g_last.cl == cl) {  // the targets bound before this clear: bound again, the six as the slices
        D3D12_CPU_DESCRIPTOR_HANDLE r2[8], d2{};
        if (bind(cl, g_last.n, g_last.rts, g_last.single, g_last.has_ds ? &g_last.ds : nullptr, r2, &d2))
            d3d::raw_bind(cl, g_last.n, r2, g_last.single, g_last.has_ds ? &d2 : nullptr);
    }
    return g_arr[kDepth].view[region_view_index()];
}

void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (n > 8 || (n && !rts)) {
        g_last.cl = nullptr;
        return;
    }
    g_last.cl = cl;
    g_last.n = n;
    g_last.single = single != 0;
    for (unsigned i = 0; i < (single ? (n ? 1u : 0u) : n); ++i) g_last.rts[i] = rts[i];
    g_last.has_ds = ds != nullptr;
    if (ds) g_last.ds = *ds;
}

D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (cl != g_r.cl) return h;
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    const int k = kind_of(h, &res, &fmt);
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return h;
    }
    if (k <= kDepth || !ensure(k, res, fmt)) return h;
    g_r.touched |= 1u << k;
    g_r.dirty |= 1u << k;
    set_state(k, -1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (a clear needs it)
    return g_arr[k].view[region_view_index()];
}

bool bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
          const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out_rts, D3D12_CPU_DESCRIPTOR_HANDLE* out_ds) {
    if (cl != g_r.cl || n > 8) return false;
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return false;
    }
    bool any = false;
    g_r.bound = 0;
    g_r.all_arrays = true;  // until a target that is not one of the six
    if (single && n >= 1 && rts) {  // a contiguous range: one handle for all n (not substituted one by one)
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        g_r.all_arrays = false;
        if (kind_of(rts[0], &res, &fmt) > kDepth) {
            end_region(cl);
            fail("a pre-pass target bound as a range");
        }
        return false;
    }
    for (unsigned i = 0; i < n && rts; ++i) {
        out_rts[i] = rts[i];
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        const int k = kind_of(rts[i], &res, &fmt);
        if (k <= kDepth || !ensure(k, res, fmt)) {
            g_r.all_arrays = false;
            continue;
        }
        if (!(g_r.known & (1u << (k * 2)))) set_state(k, -1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (its barrier before the region)
        g_r.touched |= 1u << k;
        g_r.bound |= 1u << k;
        out_rts[i] = g_arr[k].view[region_view_index()];
        any = true;
    }
    if (ds) {
        *out_ds = *ds;
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (kind_of(*ds, &res, &fmt) == kDepth && res == g_arr[kDepth].game) {
            *out_ds = g_arr[kDepth].view[region_view_index()];
            g_r.bound |= 1u << kDepth;
            any = true;
        } else {
            g_r.all_arrays = false;
        }
    }
    if (any) g_redirects.fetch_add(1, std::memory_order_relaxed);
    return any;
}

// A draw made (not skipped) in the region: its bound kinds in a writing state are written, to be copied out again.
void drawn(ID3D12GraphicsCommandList* cl) {
    if (cl != g_r.cl || !g_r.bound) return;
    for (int k = 0; k < kKinds; ++k)
        if ((g_r.bound & (1u << k)) && writing(k, g_r.state[k][0])) g_r.dirty |= 1u << k;
}

void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (cl != g_r.cl || !b) return;
    if (g_end_pending.load(std::memory_order_acquire)) {  // the walk ended: copied before this batch
        end_region(cl);
        return;
    }
    for (unsigned i = 0; i < n; ++i) {  // the six's transitions in the region: their states followed, per plane
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const int k = kind_of_resource(b[i].Transition.pResource);
        if (k < 0) continue;
        const UINT sub = b[i].Transition.Subresource;
        const int plane = sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? -1 : static_cast<int>(sub);
        if ((plane >= 0 && plane >= static_cast<int>(g_arr[k].planes)) || b[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            g_r.known &= ~plane_bits(k);  // a split barrier or another subresource: not followed
            continue;
        }
        const int p0 = plane < 0 ? 0 : plane;
        if ((g_r.known & (1u << (k * 2 + p0))) && b[i].Transition.StateBefore != g_r.state[k][p0])
            log::limited("prepass.mismatch", 20, "[prepass] %s plane %d: the game's barrier from %#x, the region had %#x", kNames[k], p0,
                         static_cast<unsigned>(b[i].Transition.StateBefore), static_cast<unsigned>(g_r.state[k][p0]));
        // [Stereo] ViMain: written in the region and now to be read: its slices copied out first (once per writing)
        const D3D12_RESOURCE_STATES after = b[i].Transition.StateAfter;
        if (g_i2.load(std::memory_order_relaxed) && (g_r.touched & (1u << k)) && writing(k, b[i].Transition.StateBefore) &&
            !writing(k, after) && after != D3D12_RESOURCE_STATE_COPY_DEST && (g_r.dirty & (1u << k)) &&
            (g_r.known & plane_bits(k)) == plane_bits(k)) {
            const D3D12_RESOURCE_STATES st[2] = {g_r.state[k][0], g_r.state[k][1]};
            publish(cl, k, st);
            g_r.dirty &= ~(1u << k);
            g_r.published |= 1u << k;
        }
        if (writing(k, after)) g_r.published &= ~(1u << k);
        set_state(k, plane, after);
    }
}

void before_draw(ID3D12GraphicsCommandList* cl) {
    if (cl == g_r.cl && g_end_pending.load(std::memory_order_acquire)) end_region(cl);
}

std::atomic<unsigned> g_ab{0};  // "vi ab <n>": view-instanced every other n frames (0: every frame)
bool latch_vi() {
    const unsigned ab = g_ab.load(std::memory_order_relaxed);
    const bool want = g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) && vi::wanted() &&
                      (ab == 0 || (state::presents.load() / ab) % 2 == 0);
    t_latched_vi = false;
    if (!append_marker(want ? &vi_on_cb : &vi_off_cb)) return false;
    t_latched_vi = want;
    return want;
}

bool vi_frame_now() { return g_vi_frame == state::presents.load(); }
bool copy_resource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!g_i2.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_relaxed) || cl != g_r.cl || !dst ||
        src != g_arr[kDepth].game || !g_arr[kDepth].arr)
        return false;
    // the depth the region drew is in its array: the region's slice (and in a view-instanced slice-0 region slice 1 into
    // eye 2's copy of the destination, read by view 1 through slot 15)
    const Arr& a = g_arr[kDepth];
    if (g_dr.game != dst) {
        if (g_dr.e2) g_retired.push_back({g_dr.e2, state::presents.load()});
        g_dr = DepthResolve{};
        g_dr.game = dst;
        g_dr.planes = plane_count(dst);
    }
    const bool e2 = g_r.vi && g_r.slice == 0;
    if (!g_dr.e2) {  // (made at the first copy, so eye 2's view is written before any view-instanced frame reads it)
        D3D12_RESOURCE_DESC ed = dst->GetDesc();
        ed.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))) && r) {
            r->SetName(L"RDRVR eye 2 Depth Resolve");
            g_dr.e2 = r;
            g_dr.e2_state = D3D12_RESOURCE_STATE_COMMON;
            vi::eye2_texture(kDepthSlot, r, dst);
        }
    }
    const UINT planes = std::min(a.planes, g_dr.planes);
    D3D12_RESOURCE_BARRIER pre[2], post[2];
    UINT n = 0;
    pre[n] = transition(a.arr, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    post[n++] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const bool to_e2 = e2 && g_dr.e2;
    if (to_e2) {
        if (g_dr.e2_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            pre[n] = transition(g_dr.e2, g_dr.e2_state, D3D12_RESOURCE_STATE_COPY_DEST);
            post[n++] = transition(g_dr.e2, D3D12_RESOURCE_STATE_COPY_DEST, kReadable);
        }
        g_dr.e2_state = kReadable;
    }
    gpu::mark(cl, gpu::kCopyBegin);
    d3d::raw_barrier(cl, n, pre);
    for (UINT p = 0; p < planes; ++p) {
        D3D12_TEXTURE_COPY_LOCATION s{a.arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        s.SubresourceIndex = static_cast<UINT>(g_r.slice) + p * 2;
        D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        d.SubresourceIndex = p;
        d3d::raw_copy_region(cl, &d, &s);
        if (to_e2) {
            s.SubresourceIndex = 1 + p * 2;
            d.pResource = g_dr.e2;
            d3d::raw_copy_region(cl, &d, &s);
        }
    }
    d3d::raw_barrier(cl, n, post);
    gpu::mark(cl, gpu::kCopyEnd);
    g_depth_copies.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void scene_end() {
    if (g_i2.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed))
        if (append_marker(&walk_end_cb)) g_markers.fetch_add(1, std::memory_order_relaxed);  // (the region's end: the scene's, not the pre-pass walk's)
}

int last_slice() { return g_frame_seen == state::presents.load() ? g_pass_in_frame - 1 : -1; }

int vi_region(const ID3D12GraphicsCommandList* cl, bool* all_arrays) {
    if (!g_active.load(std::memory_order_relaxed) || cl != g_r.cl || !g_r.vi) return 0;
    if (all_arrays) *all_arrays = g_r.all_arrays;
    return g_r.slice + 1;
}
std::string vi_status() {
    char b[900];
    std::snprintf(b, sizeof(b), "view-instanced regions: slice 0 %llu, slice 1 %llu | pass 2's rows as the pre-pass's: same %llu, differ %llu (max %.7f)"
                  " | motion vectors: D %s, checked %llu frames, largest relative difference %.2e (not checked: unread %llu, not vi %llu, no D %llu)"
                  " | main pass %s: publishes %llu, depth copies %llu | eye 2's inverse gWorldView: %llu written, %llu not invertible, packing drops %.1e | the depth's view index %lld",
                  static_cast<unsigned long long>(g_vi_regions[0].load()), static_cast<unsigned long long>(g_vi_regions[1].load()),
                  static_cast<unsigned long long>(g_vi_rows_same), static_cast<unsigned long long>(g_vi_rows_differ), g_vi_rows_max,
                  g_e2_delta_ok ? "from the updates" : "identity", static_cast<unsigned long long>(g_mv_checks), g_mv_err,
                  static_cast<unsigned long long>(g_mv_unread), static_cast<unsigned long long>(g_mv_not_vi),
                  static_cast<unsigned long long>(g_mv_no_delta), g_i2.load() ? "on" : "off",
                  static_cast<unsigned long long>(g_publishes.load()), static_cast<unsigned long long>(g_depth_copies.load()),
                  static_cast<unsigned long long>(g_inv_written), static_cast<unsigned long long>(g_inv_failed), g_inv_dropped,
                  static_cast<long long>(g_depth_index));
    return b;
}

bool views_wanted() { return g_views.load(std::memory_order_relaxed); }
void set_eye2(const void* vp2, const void* prev_vp) {
    const char* v = static_cast<const char*>(vp2);
    g_e2_ok = read_raw(reinterpret_cast<uintptr_t>(v + 0xc0), g_e2_wv, 64) && read_raw(reinterpret_cast<uintptr_t>(v + 0x140), g_e2_vi, 64);
    g_e2_pvp_ok = prev_vp && read_raw(reinterpret_cast<uintptr_t>(prev_vp), g_e2_pvp, 64);
    g_e2_frame = state::presents.load();
    // (vp2 is eye 2's own viewport: the shaders take the camera's position from gViewInverse, so its rows are pass 2's)
    if (g_e2_ok && g_views_write.load(std::memory_order_relaxed) && g_grow_state >= 0) {
        write_eye2(g_e2_wv, g_e2_vi, g_e2_pvp_ok ? g_e2_pvp : nullptr);
        if (t_latched_vi) write_prev_delta(g_e1_pvp_ok ? g_e1_pvp : nullptr, g_e2_pvp_ok ? g_e2_pvp : nullptr);
    }
    g_e1_pvp_ok = false;
}
void set_eye1_prev(const void* p1) { g_e1_pvp_ok = p1 && read_raw(reinterpret_cast<uintptr_t>(p1), g_e1_pvp, 64); }
void set_eye2_offset(const float* eye1, const float* eye2) {
    for (int i = 0; i < 3; ++i) g_e2_off[i] = eye1[i] - eye2[i];
    g_e2_off_ok = true;
}
void views_size(bool grown) { g_size_want = grown ? 1 : 0; }
std::string views_status() {
    char w[260];
    std::snprintf(w, sizeof(w), " | written: %s, %llu frames written, after the walk kept %llu, lost %llu%s%s",
                  g_grow_state > 0 ? "grown" : g_grow_state < 0 ? "refused" : "not yet", static_cast<unsigned long long>(g_written),
                  static_cast<unsigned long long>(g_kept), static_cast<unsigned long long>(g_lost), g_grow_state < 0 ? ": " : "", g_grow_why);
    return std::string(g_views.load() ? "views on: " : "views off: ") + g_vs_text + (g_views_write.load() ? w : "");
}

bool tracing() { return state::presents.load(std::memory_order_relaxed) < g_trace_until.load(std::memory_order_relaxed); }
void trace_start(int frames) {
    g_trace_until = state::presents.load() + 1 + static_cast<uint64_t>(frames);
    log::info("[prepass] trace: the next %d frame(s)", frames);
}
namespace {
const char* name_of_view(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const void* r = d3d::view_resource(h.ptr, nullptr);
    const char* n = r ? d3d::resource_name(r) : nullptr;
    return n ? n : "?";
}
bool six_name(const char* n) {
    for (const char* k : kNames)
        if (n && std::strcmp(n, k) == 0) return true;
    return false;
}
}  // namespace
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    std::string t;
    bool any = false;
    for (unsigned i = 0; i < n && rts && i < 8; ++i) {
        const char* nm = name_of_view(rts[single ? 0 : i]);
        any |= six_name(nm);
        t += (i ? ", " : "") + std::string(nm);
        if (single) break;
    }
    const char* dn = ds ? name_of_view(*ds) : "-";
    any |= six_name(dn);
    if (!any) return;
    log::info("[prepass] trace frame %llu draws %llu: bind [%s | DS %s]%s%s", static_cast<unsigned long long>(state::presents.load()),
              static_cast<unsigned long long>(d3d::thread_draws()), t.c_str(), dn, single ? " (range)" : "", g_active.load() ? " in a region" : "");
    (void)cl;
}
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    for (unsigned i = 0; i < n && b; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const char* nm = d3d::resource_name(b[i].Transition.pResource);
        if (!six_name(nm)) continue;
        log::info("[prepass] trace frame %llu draws %llu: barrier %s sub %#x: %#x -> %#x flags %u%s",
                  static_cast<unsigned long long>(state::presents.load()), static_cast<unsigned long long>(d3d::thread_draws()), nm,
                  b[i].Transition.Subresource, static_cast<unsigned>(b[i].Transition.StateBefore),
                  static_cast<unsigned>(b[i].Transition.StateAfter), static_cast<unsigned>(b[i].Flags), g_active.load() ? " in a region" : "");
    }
    (void)cl;
}
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth) {
    const char* nm = name_of_view(h);
    if (!six_name(nm)) return;
    log::info("[prepass] trace frame %llu draws %llu: clear %s %s%s", static_cast<unsigned long long>(state::presents.load()),
              static_cast<unsigned long long>(d3d::thread_draws()), depth ? "DSV" : "RTV", nm, g_active.load() ? " in a region" : "");
    (void)cl;
}

void set_ab(unsigned frames) { g_ab = frames; }
std::string gpu_command(const std::string& arg) {
    if (arg == "on" || arg == "off") gpu::g_on = arg == "on";  // (the playback thread reads it: a flag, set between frames)
    if (arg == "pubs") {  // publishes per kind (view-instanced regions, others) and per region since the start
        std::string r = "publishes per region:";
        const double rv = std::max<double>(1.0, static_cast<double>(g_vi_regions[0].load() + g_vi_regions[1].load()));
        const double ro = std::max<double>(1.0, static_cast<double>(g_regions.load()) - rv);
        for (int k = 0; k < kKinds; ++k) {
            char b[120];
            std::snprintf(b, sizeof(b), " %s %.1f/%.1f;", kNames[k], static_cast<double>(g_pub_kind[k][1].load()) / rv,
                          static_cast<double>(g_pub_kind[k][0].load()) / ro);
            r += b;
        }
        return r;
    }
    return gpu::status(arg == "reset");
}

std::string grab(const std::string& kind, const std::string& path) {
    static const char* const keys[kKinds] = {"depth", "collector", "velocity", "grasscolour", "grasslight", "tree", "light", "fsc", "react", "refr"};
    int k = -1;
    for (int i = 0; i < kKinds; ++i)
        if (kind == keys[i]) k = i;
    if (k < 0) return "ERROR kinds: depth collector velocity grasscolour grasslight tree light fsc react refr";
    if (!g_dev || !g_arr[k].arr) return "ERROR that kind's array not made yet";
    if (!g_grab_done) {
        g_grab_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        d3d::add_frame_end_listener(grab_frame_end);
    }
    if (g_grab_phase.load()) return "ERROR a grab is under way";
    if (g_grab_rb && g_grab_kind != k) {
        g_grab_rb->Release();
        g_grab_rb = nullptr;
    }
    g_grab_kind = k;
    g_grab_path = path;
    ResetEvent(g_grab_done);
    g_grab_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_grab_done, 5000) != WAIT_OBJECT_0) {
        int expected = 1;
        g_grab_phase.compare_exchange_strong(expected, 0);
        return "ERROR no view-instanced publish of that kind within 5 s";
    }
    return g_grab_result;
}

std::string status() {
    char b[520];
    std::snprintf(b, sizeof(b), "prepass %s%s%s: regions %llu, walk-end markers %llu, ended by them %llu, copies %llu, redirected binds %llu, passes past two %llu%s%s",
                  g_on.load() ? "on" : "off", g_failed.load() ? " (stood down)" : "", g_active.load() ? " (in a region)" : "",
                  static_cast<unsigned long long>(g_regions.load()), static_cast<unsigned long long>(g_markers.load()),
                  static_cast<unsigned long long>(g_marker_ends.load()), static_cast<unsigned long long>(g_copies.load()),
                  static_cast<unsigned long long>(g_redirects.load()), static_cast<unsigned long long>(g_skipped.load()),
                  g_failed.load() ? ": " : "", g_failed.load() ? g_fail_reason : "");
    return b;
}

}  // namespace rdrvr::prepass
