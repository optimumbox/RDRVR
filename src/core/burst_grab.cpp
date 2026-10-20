#include "core/burst_grab.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"

namespace rdrvr::burst_grab {
namespace {

// The post output target: the Post FXAA Target under FXAA, FXAATarget otherwise (post_target.h).
constexpr int kMaxFrames = 24;
const char* const kEyeTag[3] = {"M", "L", "R"};

struct Slot {
    ID3D12Resource* rb = nullptr;  // one footprint per frame; never released (a late tap may still copy into it)
    uint64_t rb_size = 0;
    bool valid[kMaxFrames] = {};   // recording thread writes, presenting thread reads after the phase handshake
    int recorded = 0;
};
// What one source copies each frame (2026-10-20, "scenevel": the scene colour and the motion vectors of the same frames,
// for the motion vectors' check against how the image moved: tools\sp\mvcheck.py)
struct Tap {
    Slot slot[3];  // 0 mono, 1 left, 2 right
    ID3D12Resource* res = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 row_bytes = 0, frame_bytes = 0;
    D3D12_BOX box{};
    uint32_t w = 0, h = 0, fmt = 0;
    uint64_t src_w = 0;  // the armed target's size (the crop's box is inside it)
    uint32_t src_h = 0;
    const char* tag = "";  // the file name's part ("" for a single source)
};
Tap g_tap[2];
int g_ntaps = 1;

std::mutex g_mutex;
std::string g_prefix, g_result;
HANDLE g_done = nullptr;
// 0 idle, 1 armed, 2 waiting for a UI bind, 3 recording, 4 recorded (one more frame end), 5 finish
std::atomic<int> g_phase{0};
int g_frames = 8, g_frames_waited = 0;
int g_frame_i = 0, g_fxaa_binds = 0, g_doubles = 0, g_monos = 0;  // recording thread
bool g_swap = false;
uint64_t g_rtv[8];
int g_nrtv = 0;
uint64_t g_first_frame = 0;
int g_source = 0;  // 0 the post output, 1 the scene colour, 2 the Velocity RT, 3 both of those (scenevel)
struct Source {
    size_t field;
    const char* name;
};
const Source kSource[3] = {{0, nullptr}, {0x10, "FullScreenCopy"}, {0x3f0, "Velocity RT"}};
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;

void record_tap(Tap& t, ID3D12GraphicsCommandList* cl, int eye) {
    Slot& s = t.slot[eye];
    if (g_frame_i >= g_frames || s.valid[g_frame_i]) return;
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!d3d::watched_state(t.res, &wcl, &st) || wcl != cl) return;  // this frame's copy is not measured
    // the crop box and footprint are the armed target's: never copied from a target the game made again since (live:
    // its barrier is in this list) at another size or format
    const D3D12_RESOURCE_DESC d = t.res->GetDesc();
    if (d.Width != t.src_w || d.Height != t.src_h || static_cast<uint32_t>(d.Format) != t.fmt) return;
    const auto state = static_cast<D3D12_RESOURCE_STATES>(st);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = s.rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = t.fp;
    dl.PlacedFootprint.Offset = static_cast<UINT64>(g_frame_i) * t.frame_bytes;
    sl.pResource = t.res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &t.box);
    if (transition) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    s.valid[g_frame_i] = true;
    ++s.recorded;
}

void record(ID3D12GraphicsCommandList* cl, int eye) {
    for (int i = 0; i < g_ntaps; ++i) record_tap(g_tap[i], cl, eye);
}

void bind_tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    int ph = g_phase.load(std::memory_order_acquire);
    if ((ph != 2 && ph != 3) || n == 0 || !rts) return;
    bool target = false;
    for (int i = 0; i < g_nrtv; ++i) target = target || rts[0].ptr == g_rtv[i];
    if (!target) return;
    bool ui = ds != nullptr;
    if (ph == 2) {
        if (ui) {
            g_fxaa_binds = g_frame_i = g_doubles = g_monos = 0;
            g_phase.store(3, std::memory_order_release);
        }
        return;
    }
    if (!ui) {
        // the first pass's eye: its post output is complete at the second bind; the scene colour and the motion
        // vectors are its run's own at the first (the second run's at the second: the eyes' were the same, 2026-10-13)
        if (++g_fxaa_binds == (g_source != 0 ? 1 : 2)) record(cl, g_swap ? 2 : 1);
        return;
    }
    if (g_fxaa_binds >= 2) {
        record(cl, g_swap ? 1 : 2);  // the last pass's eye
        ++g_doubles;
    } else if (g_fxaa_binds == 1) {
        record(cl, 0);
        ++g_monos;
    }
    g_fxaa_binds = 0;
    if (++g_frame_i >= g_frames) g_phase.store(4, std::memory_order_release);
}

ID3D12Resource* find_target(char* postfx, size_t field, const char* name) {
    void* obj = *reinterpret_cast<void**>(postfx + field);
    char where[64] = "?";
    const void* found = obj ? d3d::resource_in_object(obj, 0x100, name, where, sizeof(where)) : nullptr;
    if (!found) found = d3d::unique_resource(name);
    return static_cast<ID3D12Resource*>(const_cast<void*>(found));
}

std::string arm_tap(Tap& t, char* postfx, int source, ID3D12Resource* tap, const char* tname, int crop_w, int crop_h);

std::string arm(uint64_t frame, int crop_w, int crop_h) {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx object (NOT MEASURED)";
    const char* tname = post_target::name(postfx);
    ID3D12Resource* tap = find_target(postfx, post_target::field(postfx), tname);
    if (!tap) return std::string("ERROR no single live resource named \"") + tname + "\" (NOT MEASURED)";
    g_nrtv = d3d::rtv_handles_of(tap, g_rtv, 8);
    if (g_nrtv == 0) return std::string("ERROR no render-target view of ") + tname + " seen (NOT MEASURED)";
    g_ntaps = g_source == 3 ? 2 : 1;
    std::string err;
    for (int i = 0; i < g_ntaps && err.empty(); ++i) {
        g_tap[i].tag = g_source == 3 ? (i == 0 ? "scene-" : "vel-") : "";
        err = arm_tap(g_tap[i], postfx, g_source == 3 ? 1 + i : g_source, tap, tname, crop_w, crop_h);
    }
    if (!err.empty()) return err;
    g_swap = camera_lever::swap_order();
    g_first_frame = frame + 1;
    for (int i = 0; i < g_ntaps; ++i) d3d::watch_add(g_tap[i].res);
    log::info("[burst] armed at frame %llu: %u x %u crop at (%u, %u) of %s, %d frames", static_cast<unsigned long long>(frame), g_tap[0].w,
              g_tap[0].h, g_tap[0].box.left, g_tap[0].box.top, g_source == 3 ? "FullScreenCopy and the Velocity RT" : g_source ? kSource[g_source].name : tname,
              g_frames);
    return {};
}

std::string arm_tap(Tap& t, char* postfx, int source, ID3D12Resource* tap, const char* tname, int crop_w, int crop_h) {
    ID3D12Resource* res = tap;  // the binds of the post output tap; the source is copied there
    if (source != 0) {
        tname = kSource[source].name;
        res = find_target(postfx, kSource[source].field, tname);
        if (!res) return std::string("ERROR no single live resource named \"") + tname + "\" (NOT MEASURED)";
    }
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 ||
        d.SampleDesc.Count != 1)
    {
        char why[160];
        std::snprintf(why, sizeof(why), "ERROR %s is not a single-subresource 2D texture (dimension %d, mips %u, slices %u, samples %u; "
                      "single-pass stereo's arrays: \"prepass burst\"; NOT MEASURED)", tname, static_cast<int>(d.Dimension), d.MipLevels, d.DepthOrArraySize, d.SampleDesc.Count);
        return why;
    }
    uint32_t w = static_cast<uint32_t>(crop_w), h = static_cast<uint32_t>(crop_h);
    if (source != 0) {  // the scene colour and the motion vectors are at the render size: the crop within it
        if (w > d.Width) w = static_cast<uint32_t>(d.Width);
        if (h > d.Height) h = d.Height;
    }
    if (w == 0 || h == 0 || w > d.Width || h > d.Height) return "ERROR crop larger than the target";
    t.box.left = static_cast<UINT>((d.Width - w) / 2);
    t.box.top = (d.Height - h) / 2;
    t.box.right = t.box.left + w;
    t.box.bottom = t.box.top + h;
    t.box.front = 0;
    t.box.back = 1;
    D3D12_RESOURCE_DESC cd = d;  // the crop's footprint
    cd.Width = w;
    cd.Height = h;
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
    UINT rows = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&cd, 0, 1, 0, &t.fp, &rows, &t.row_bytes, &total);
    t.frame_bytes = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    UINT64 need = t.frame_bytes * static_cast<UINT64>(g_frames);  // kept between grabs (a late tap may still copy)
    bool ok = true;
    for (Slot& s : t.slot) {
        if (!s.rb || s.rb_size < need) {
            if (s.rb) s.rb->Release();
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = need;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            s.rb = nullptr;
            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&s.rb));
            s.rb_size = s.rb ? need : 0;
        }
        ok = ok && s.rb;
        std::memset(s.valid, 0, sizeof(s.valid));
        s.recorded = 0;
    }
    if (!g_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev->Release();
    if (!ok || !g_fence) return "ERROR could not create the readback objects";
    t.w = w;
    t.h = h;
    t.fmt = static_cast<uint32_t>(d.Format);
    t.src_w = d.Width;
    t.src_h = d.Height;
    t.res = res;
    return {};
}

void stop(const std::string& result) {
    d3d::set_bind_tap(d3d::kBindTapBurst, nullptr);
    for (Tap& t : g_tap) {
        if (t.res) d3d::watch_remove(t.res);
        t.res = nullptr;
    }
    g_result = result;
    g_phase = 0;
    SetEvent(g_done);
}

std::string finish() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_fence) return "ERROR no present queue";
    q->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 3000) != WAIT_OBJECT_0) return "ERROR the copies timed out (NOT MEASURED)";
    std::string out;
    for (int ti = 0; ti < g_ntaps; ++ti) {
    Tap& t = g_tap[ti];
    for (int e = 0; e < 3; ++e) {
        Slot& s = t.slot[e];
        if (!s.recorded) continue;
        uint8_t* p = nullptr;
        D3D12_RANGE r{0, static_cast<SIZE_T>(s.rb_size)};
        if (FAILED(s.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
            out += std::string(" | ") + kEyeTag[e] + " Map failed";
            continue;
        }
        std::string path = g_prefix + "-" + t.tag + kEyeTag[e] + ".rdb";
        std::wstring wpath(path.begin(), path.end()), tmp = wpath + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            const uint32_t hdr[8] = {0x31424452u /* RDB1 */, t.w, t.h, t.fmt, static_cast<uint32_t>(e - 1),
                                     static_cast<uint32_t>(g_frames), static_cast<uint32_t>(g_first_frame),
                                     static_cast<uint32_t>(t.row_bytes)};
            fwrite(hdr, 1, sizeof(hdr), f);
            for (int i = 0; i < g_frames; ++i) {
                uint32_t v = s.valid[i] ? 1u : 0u;
                fwrite(&v, 1, 4, f);
                for (uint32_t y = 0; y < t.h; ++y)
                    fwrite(p + static_cast<UINT64>(i) * t.frame_bytes + t.fp.Offset + static_cast<UINT64>(y) * t.fp.Footprint.RowPitch, 1,
                           static_cast<size_t>(t.row_bytes), f);
            }
            ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        D3D12_RANGE none{0, 0};
        s.rb->Unmap(0, &none);
        char line[96];
        std::snprintf(line, sizeof(line), " | %s%s %d of %d frames %s", t.tag, kEyeTag[e], s.recorded, g_frames,
                      ok ? "written" : "NOT WRITTEN");
        out += line;
    }
    }
    char head[160];
    std::snprintf(head, sizeof(head), "from frame %llu, %u x %u format %u: %d double, %d mono frame(s)",
                  static_cast<unsigned long long>(g_first_frame), g_tap[0].w, g_tap[0].h, g_tap[0].fmt, g_doubles, g_monos);
    return out.empty() ? std::string("ERROR nothing captured (NOT MEASURED): ") + head : head + out;
}

int g_crop_w = 640, g_crop_h = 360;

void on_frame_end(uint64_t frame) {
    int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1) {
        std::string err = arm(frame, g_crop_w, g_crop_h);
        if (!err.empty()) {
            stop(err);
            return;
        }
        g_frames_waited = 0;
        d3d::set_bind_tap(d3d::kBindTapBurst, bind_tap);
        g_phase.store(2, std::memory_order_release);
    } else if (ph == 2 || ph == 3) {
        if (++g_frames_waited > g_frames + 12) stop("ERROR the bind sequence was not seen in time (NOT MEASURED)");
    } else if (ph == 4) {
        g_phase = 5;
    } else if (ph == 5) {
        d3d::set_bind_tap(d3d::kBindTapBurst, nullptr);
        stop(finish());
    }
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
}

std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms, int source) {
    std::lock_guard lock(g_mutex);
    g_prefix = prefix;
    g_source = source < 0 || source > 3 ? 0 : source;
    g_frames = frames < 2 ? 2 : frames > kMaxFrames ? kMaxFrames : frames;
    g_crop_w = crop_w;
    g_crop_h = crop_h;
    ResetEvent(g_done);
    g_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_phase.compare_exchange_strong(expected, 0);
        return "ERROR no frame within the timeout (NOT MEASURED)";
    }
    log::info("[burst] %s", g_result.c_str());
    return g_result;
}

}  // namespace rdrvr::burst_grab
