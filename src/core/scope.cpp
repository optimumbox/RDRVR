#include "core/scope.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/aim.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/state.h"
#include "core/xr.h"
#include "core/xr_blit.h"

namespace rdrvr::scope {
namespace {

constexpr uint32_t kImg = 512;  // the lens swapchain's edge: the largest patch
constexpr int kSlots = 3;       // the copy lists' ring

std::mutex g_mutex;  // the settings: the commands write, the presenting thread reads
bool g_on = true, g_all = false;  // g_all: every long gun, not only the scoped ones (tests)
float g_zoom = 4.0f, g_size = 0.045f, g_off[3] = {0.0f, 0.07f, -0.10f};
int g_eye = -1;  // -1 automatic (the eye off the barrel's line), 0 left, 1 right
bool g_weapon[64] = {};

XrSwapchain g_sc = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_images;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
XrSwapchain g_mask = XR_NULL_HANDLE;  // the lens's frame: black round it, the crosshair (filled once, kMaskEdge square)
constexpr uint32_t kMaskEdge = 512;
constexpr float kMaskScale = 1.6f;  // the mask quad's edge over the lens's (black to the lens quad's corners, then fading)
ID3D12CommandAllocator* g_alloc[kSlots] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_slot_value[kSlots] = {};
uint64_t g_next = 0;
bool g_ready = false, g_failed = false;

// this frame's lens (prepare -> layer)
bool g_have = false;
XrPosef g_pose{};
uint32_t g_patch = 0;
std::atomic<uint64_t> g_frames{0}, g_busy{0}, g_off_image{0};
std::atomic<int> g_last_eye{-1}, g_last_patch{0};
std::atomic<float> g_last_dist{0.0f};

// The mask's texel at (u, v), both -kMaskScale/2..kMaskScale/2 in lens diameters from the centre (premultiplied black):
// clear inside the lens but for the crosshair (a thin cross, thick posts from the edge), a soft dark rim, black out past
// the lens quad's corners, then fading to the mask's edge
float mask_alpha(float u, float v, float px) {
    const float r = std::sqrt(u * u + v * v);
    auto ramp = [px](float x, float a, float b) { return std::clamp((x - a) / std::max(b - a, px), 0.0f, 1.0f); };
    const float lens = 0.5f;
    if (r > lens) return r < 0.72f ? 1.0f : 1.0f - ramp(r, 0.72f, 0.5f * kMaskScale);
    const float a = ramp(r, lens * 0.88f, lens);  // the rim
    const float au = std::fabs(u), av = std::fabs(v);
    const float thin = 0.0035f, thick = 0.014f, post = lens * 0.45f;
    auto line = [px](float d, float half) { return std::clamp(half + 0.5f * px - d, 0.0f, px) / px; };
    float cross = std::max(line(av, thin), line(au, thin));
    if (r > post && au > av) cross = std::max(cross, line(av, thick));  // the side posts
    if (r > post && v < 0.0f && av > au) cross = std::max(cross, line(au, thick));  // the bottom post
    return std::max(a, cross);
}
bool fill_mask(XrSession session, ID3D12Device* dev, DXGI_FORMAT fmt) {
    ID3D12CommandQueue* q = xr::image_queue();
    if (!q) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = static_cast<int64_t>(fmt);
    ci.sampleCount = 1;
    ci.width = ci.height = kMaskEdge;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &g_mask))) {
        log::error("[scope] the mask's xrCreateSwapchain failed");
        g_mask = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_mask, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_mask, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_mask, &ai, &index)) || index >= ni) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    XrResult wr = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; tries < 10 && wr == XR_TIMEOUT_EXPIRED; ++tries) wr = xrWaitSwapchainImage(g_mask, &wi);
    const bool waited = wr == XR_SUCCESS;
    bool ok = waited;
    ID3D12Resource* dst = imgs[index].texture;
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool done = false, submitted = false;
    ok = ok &&
         SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up))) &&
         SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
         SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
         SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    uint8_t* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (ok) ok = SUCCEEDED(up->Map(0, &none, reinterpret_cast<void**>(&p))) && p;
    if (ok) {
        const float px = kMaskScale / static_cast<float>(kMaskEdge);
        for (uint32_t y = 0; y < kMaskEdge; ++y) {
            uint8_t* row = p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            for (uint32_t x = 0; x < kMaskEdge; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) * px - 0.5f * kMaskScale, v = 0.5f * kMaskScale - (static_cast<float>(y) + 0.5f) * px;
                row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 0;  // black, premultiplied
                row[x * 4 + 3] = static_cast<uint8_t>(std::lround(255.0f * mask_alpha(u, v, px)));
            }
        }
        up->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = dst;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = up;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp;
        list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        ok = SUCCEEDED(list->Close());
        if (ok) {
            d3d::submit_internal(q, list);
            submitted = true;
            ok = SUCCEEDED(q->Signal(fence, 1));
            HANDLE ev = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
            done = ev && SUCCEEDED(fence->SetEventOnCompletion(1, ev)) && WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0;
            if (ev) CloseHandle(ev);
            ok = ok && done;
        }
    }
    if (waited) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ok = XR_SUCCEEDED(xrReleaseSwapchainImage(g_mask, &ri)) && ok;
    }
    if (list && (done || !submitted)) list->Release();
    if (fence && (done || !submitted)) fence->Release();
    if (alloc && (done || !submitted)) alloc->Release();
    if (up && (done || !submitted)) up->Release();
    if (!ok) log::error("[scope] filling the mask failed");
    return ok;
}

bool ensure(XrSession session, DXGI_FORMAT fmt) {
    if (g_ready) return g_fmt == fmt;
    if (g_failed) return false;
    g_failed = true;
    ID3D12Device* dev = state::device.load();
    if (!dev) return false;
    g_fmt = fmt;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = static_cast<int64_t>(fmt);
    ci.sampleCount = 1;
    ci.width = ci.height = kImg;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &g_sc))) {
        log::error("[scope] xrCreateSwapchain failed");
        g_sc = XR_NULL_HANDLE;
        return false;
    }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages(g_sc, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs) g_images.push_back(im.texture);
    for (auto& a : g_alloc)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(g_list->Close()) || FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return false;
    if (!fill_mask(session, dev, fmt)) return false;
    g_ready = true;
    g_failed = false;
    log::info("[scope] lens swapchain %ux%u format %d (%u images)", kImg, kImg, static_cast<int>(fmt), ni);
    return true;
}

// The shortest turn from +z (a quad's front) onto n.
XrQuaternionf facing(const float n[3]) {
    const float w = 1.0f + n[2];
    if (w < 1e-4f) return {0, 1, 0, 0};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + w * w);
    return {-n[1] / l, n[0] / l, 0.0f, w / l};
}
// v rotated by the inverse of q
void unrotate(const XrQuaternionf& q, const float v[3], float o[3]) {
    const float x = -q.x, y = -q.y, z = -q.z, w = q.w;
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}
// a LOCAL point in eye e's image (pixels from the top-left of its iw x ih content); false behind the eye
bool project(const XrView& v, const float l[3], uint32_t iw, uint32_t ih, float* px, float* py) {
    const float d[3] = {l[0] - v.pose.position.x, l[1] - v.pose.position.y, l[2] - v.pose.position.z};
    float e[3];
    unrotate(v.pose.orientation, d, e);
    if (e[2] > -0.01f) return false;
    const float tx = e[0] / -e[2], ty = e[1] / -e[2];
    const float tl = std::tan(v.fov.angleLeft), tr = std::tan(v.fov.angleRight), tu = std::tan(v.fov.angleUp), td = std::tan(v.fov.angleDown);
    *px = (tx - tl) / (tr - tl) * static_cast<float>(iw);
    *py = (tu - ty) / (tu - td) * static_cast<float>(ih);
    return true;
}
float dist_to_line(const float p[3], const float a[3], const float dir[3]) {
    const float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float t = ap[0] * dir[0] + ap[1] * dir[1] + ap[2] * dir[2];
    float s = 0;
    for (int k = 0; k < 3; ++k) {
        const float c = ap[k] - dir[k] * t;
        s += c * c;
    }
    return std::sqrt(s);
}
bool scoped(int w) {
    if (w < 0 || w >= 64) return false;
    return g_weapon[w];
}

}  // namespace

void init() {
    std::lock_guard lock(g_mutex);
    g_on = config::get_bool("Scope", "Enabled", true);
    g_zoom = std::clamp(config::get_float("Scope", "Zoom", 4.0f), 1.0f, 20.0f);
    g_size = std::clamp(config::get_float("Scope", "LensSize", 0.045f), 0.01f, 0.2f);
    std::istringstream off(config::get_string("Scope", "LensOffset", "0 0.07 -0.10"));
    for (float& c : g_off) off >> c;
    std::istringstream ws(config::get_string("Scope", "Weapons", "RollingBlock Carcano"));
    std::string tok;
    while (ws >> tok) {
        if (!tok.empty() && tok.back() == ',') tok.pop_back();
        for (int w = 0; w < 64; ++w) {
            const char* t = holster::weapon_token(w);
            if (!t || !*t) break;
            if (_stricmp(t, tok.c_str()) == 0) g_weapon[w] = true;
        }
    }
    log::info("[scope] %s, zoom %.1f, lens %.3f m at (%.3f %.3f %.3f) in the gun's axes", g_on ? "on" : "off", g_zoom, g_size, g_off[0], g_off[1],
              g_off[2]);
}

bool prepare(const XrView* views, XrSession session, ID3D12CommandQueue* queue, ID3D12Resource* const eyes[2], uint32_t iw, uint32_t ih,
             DXGI_FORMAT fmt) {
    g_have = false;
    float zoom, size, off[3];
    int force_eye;
    {
        std::lock_guard lock(g_mutex);
        if (!g_on) return false;
        const int w = holster::gun_in_hand();
        if (!(g_all ? w >= 8 && w <= 20 : scoped(w))) return false;
        zoom = g_zoom;
        size = g_size;
        std::memcpy(off, g_off, sizeof(off));
        force_eye = g_eye;
    }
    if (!queue || !eyes[0] || !eyes[1] || !camera_lever::eyes_follow_head()) return false;
    body::BodyPoints bp;
    if (!body::body_points(&bp) || !bp.cam_ok || !bp.gun_frame_ok) return false;
    // the eyepiece: the gun's frame (axes as columns: x right, y up, z back) + the offset; the target: the reticle's
    // point, else 100 m along the barrel (-z)
    const float* R = bp.gun_frame_R;
    float lw[3], tw[3];
    for (int k = 0; k < 3; ++k) lw[k] = bp.gun_frame_o[k] + R[k * 3] * off[0] + R[k * 3 + 1] * off[1] + R[k * 3 + 2] * off[2];
    bool hot = false;
    float deg = 0;
    if (!aim::reticle_target(tw, &hot, &deg, 0, false))
        for (int k = 0; k < 3; ++k) tw[k] = lw[k] - R[k * 3 + 2] * 100.0f;
    float l[3], t[3];
    if (!camera_lever::world_to_local(bp.cam, lw, l) || !camera_lever::world_to_local(bp.cam, tw, t)) return false;
    float dir[3] = {t[0] - l[0], t[1] - l[1], t[2] - l[2]};
    const float tl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(tl > 0.5f)) return false;
    for (float& c : dir) c /= tl;
    float ep[2][3];
    for (int e = 0; e < 2; ++e) ep[e][0] = views[e].pose.position.x, ep[e][1] = views[e].pose.position.y, ep[e][2] = views[e].pose.position.z;
    const int src = force_eye >= 0 ? force_eye : (dist_to_line(ep[0], l, dir) > dist_to_line(ep[1], l, dir) ? 0 : 1);
    const float mid[3] = {0.5f * (ep[0][0] + ep[1][0]), 0.5f * (ep[0][1] + ep[1][1]), 0.5f * (ep[0][2] + ep[1][2])};
    float to_eye[3] = {mid[0] - l[0], mid[1] - l[1], mid[2] - l[2]};
    const float de = std::sqrt(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
    if (!(de > 0.05f)) return false;
    for (float& c : to_eye) c /= de;
    // the patch: the lens's angle from the eyes over the zoom, in the source eye's pixels (per tangent unit at its centre)
    const XrView& v = views[src];
    const float ppt = static_cast<float>(iw) / (std::tan(v.fov.angleRight) - std::tan(v.fov.angleLeft));
    const float patch_t = 2.0f * std::tan(0.5f * (2.0f * std::atan(0.5f * size / de)) / zoom);
    const uint32_t S = static_cast<uint32_t>(std::clamp(patch_t * ppt, 8.0f, static_cast<float>(std::min({kImg, iw, ih}))));
    float px, py;
    if (!project(v, t, iw, ih, &px, &py)) return false;
    const long x0 = std::lround(px - 0.5f * S), y0 = std::lround(py - 0.5f * S);
    if (x0 < 0 || y0 < 0 || x0 + static_cast<long>(S) > static_cast<long>(iw) || y0 + static_cast<long>(S) > static_cast<long>(ih)) {
        g_off_image.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!ensure(session, fmt)) return false;
    const int slot = static_cast<int>(g_next % kSlots);
    if (g_fence->GetCompletedValue() < g_slot_value[slot]) {
        g_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_sc, &ai, &index)) || index >= g_images.size()) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 20000000;  // 20 ms
    const bool waited = xrWaitSwapchainImage(g_sc, &wi) == XR_SUCCESS;
    bool ok = waited && SUCCEEDED(g_alloc[slot]->Reset()) && SUCCEEDED(g_list->Reset(g_alloc[slot], nullptr));
    if (ok) {
        ID3D12Resource* dst = g_images[index];
        ID3D12Resource* srcimg = eyes[src];
        D3D12_RESOURCE_BARRIER b[2]{};
        for (auto& x : b) {
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        b[0].Transition.pResource = dst;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.pResource = srcimg;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_list->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = dst;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = srcimg;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x0) + S, static_cast<UINT>(y0) + S, 1};
        g_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        g_list->ResourceBarrier(2, b);
        if (g_frames.load(std::memory_order_relaxed) == 0)
            log::info("[scope] first lens: eye %d, patch %u px at (%ld %ld) of %ux%u, image %u", src, S, x0, y0, iw, ih, index);
        ok = SUCCEEDED(g_list->Close());
        if (ok) {
            d3d::submit_internal(queue, g_list);
            g_slot_value[slot] = ++g_next;
            ok = SUCCEEDED(queue->Signal(g_fence, g_slot_value[slot]));
        }
    }
    if (waited) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ok = XR_SUCCEEDED(xrReleaseSwapchainImage(g_sc, &ri)) && ok;
    }
    if (!ok) return false;
    g_pose.orientation = facing(to_eye);
    g_pose.position = {l[0], l[1], l[2]};
    g_patch = S;
    g_have = true;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_last_eye.store(src, std::memory_order_relaxed);
    g_last_patch.store(static_cast<int>(S), std::memory_order_relaxed);
    g_last_dist.store(tl, std::memory_order_relaxed);
    return true;
}

int layers(XrSpace space, XrCompositionLayerQuad out[2]) {
    if (!g_have) return 0;
    float size;
    {
        std::lock_guard lock(g_mutex);
        size = g_size;
    }
    XrCompositionLayerQuad& q = out[0];  // the lens: opaque (its corners under the mask's black)
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_sc;
    q.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_patch), static_cast<int32_t>(g_patch)}};
    q.pose = g_pose;
    q.size = {size, size};
    XrCompositionLayerQuad& m = out[1];  // the mask over it
    m = q;
    m.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    m.subImage.swapchain = g_mask;
    m.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(kMaskEdge), static_cast<int32_t>(kMaskEdge)}};
    m.size = {size * kMaskScale, size * kMaskScale};
    return 2;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string w;
    in >> w;  // "scope"
    std::lock_guard lock(g_mutex);
    while (in >> w) {
        if (w == "on") g_on = true;
        else if (w == "off") g_on = false;
        else if (w == "zoom") in >> g_zoom, g_zoom = std::clamp(g_zoom, 1.0f, 20.0f);
        else if (w == "size") in >> g_size, g_size = std::clamp(g_size, 0.01f, 0.2f);
        else if (w == "offset") in >> g_off[0] >> g_off[1] >> g_off[2];
        else if (w == "all") {
            in >> w;
            g_all = w == "on";
        } else if (w == "eye") {
            in >> w;
            g_eye = w == "left" ? 0 : w == "right" ? 1 : -1;
        } else return "ERROR scope [on|off] [zoom x] [size m] [offset x y z] [eye auto|left|right] [all on|off]";
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "scope %s%s, zoom %.1f, lens %.3f m at (%.3f %.3f %.3f), eye %s | gun in hand %d (%s) | lens frames %llu, busy %llu, off image %llu; "
                  "last: eye %d, patch %d px, target %.1f m",
                  g_on ? "on" : "off", g_all ? " (all long guns)" : "", g_zoom, g_size, g_off[0], g_off[1], g_off[2],
                  g_eye < 0 ? "auto" : g_eye == 0 ? "left" : "right", holster::gun_in_hand(), holster::weapon_token(holster::gun_in_hand()),
                  static_cast<unsigned long long>(g_frames.load()), static_cast<unsigned long long>(g_busy.load()),
                  static_cast<unsigned long long>(g_off_image.load()), g_last_eye.load(), g_last_patch.load(), g_last_dist.load());
    return b;
}

}  // namespace rdrvr::scope
