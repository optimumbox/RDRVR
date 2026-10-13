#include "core/stereo_vi.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/pso.h"
#include "core/state.h"

namespace rdrvr::vi {
namespace {

uint64_t fnv(const void* p, size_t n) {  // as pso.cpp names the shaders
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// A game pipeline kept for its twin: its desc with every pointer's data copied.
struct Kept {
    ID3D12PipelineState* game = nullptr;
    ID3D12RootSignature* root = nullptr;  // a reference held
    std::vector<uint8_t> vs, ps;
    uint64_t vs_hash = 0, ps_hash = 0;
    bool other_stages = false;  // DS, HS, GS or stream output: no twin
    D3D12_BLEND_DESC blend{};
    UINT sample_mask = 0;
    D3D12_RASTERIZER_DESC rast{};
    D3D12_DEPTH_STENCIL_DESC depth{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    std::vector<std::string> semantics;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut{};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology{};
    D3D12_RT_FORMAT_ARRAY rtv{};
    DXGI_FORMAT dsv{};
    DXGI_SAMPLE_DESC sample{1, 0};
    UINT node = 0;
    D3D12_PIPELINE_STATE_FLAGS flags{};
    std::atomic<ID3D12PipelineState*> twin{nullptr};
    std::atomic<uint64_t> made_frame{~0ull};  // the frame the twin was made in: usable from the next frame
    int state = 0;                            // 0 queued, 1 made, -1 not made (why below), the worker's only
    const char* why = "";
};

std::atomic<bool> g_configured{false}, g_on{true};
ID3D12Device2* g_dev = nullptr;
int g_tier = -1;
std::string g_dir;

SRWLOCK g_lock = SRWLOCK_INIT;  // g_kept, g_by_pso, the shader maps
std::vector<std::unique_ptr<Kept>> g_kept;
std::unordered_map<ID3D12PipelineState*, Kept*> g_by_pso;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vs_conv, g_ps_conv;
std::unordered_set<uint64_t> g_ps_safe;

std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<Kept*> g_queue;
bool g_worker = false;

std::atomic<uint64_t> g_kept_n{0}, g_made{0}, g_failed{0}, g_no_vs{0}, g_no_ps{0}, g_stages{0};
std::atomic<uint64_t> g_draws_both{0}, g_draws_skipped{0}, g_draws_plain{0}, g_clears_skipped{0}, g_rebinds{0};

std::string hex(uint64_t h) {
    char b[20];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return b;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? std::fread(out->data(), 1, out->size(), f) : 0;
    std::fclose(f);
    return n > 32 && got == out->size() && std::memcmp(out->data(), "DXBC", 4) == 0;
}

// vs_<hash>.dxil, ps_<hash>.dxil and ps_safe.txt from the folder (replacing what was loaded)
void load_shaders(const std::string& dir) {
    std::unordered_map<uint64_t, std::vector<uint8_t>> vs, ps;
    std::unordered_set<uint64_t> safe;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.dxil").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;  // vs_<16 hex>.dxil
            if (name.size() != 3 + 16 + 5 || (name.compare(0, 3, "vs_") != 0 && name.compare(0, 3, "ps_") != 0)) continue;
            const uint64_t hash = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
            std::vector<uint8_t> b;
            if (!hash || !read_file(dir + "\\" + name, &b)) continue;
            (name[0] == 'v' ? vs : ps).emplace(hash, std::move(b));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (FILE* f = nullptr; fopen_s(&f, (dir + "\\ps_safe.txt").c_str(), "r") == 0 && f) {
        char line[64];
        while (std::fgets(line, sizeof(line), f))
            if (const uint64_t x = std::strtoull(line, nullptr, 16)) safe.insert(x);
        std::fclose(f);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_vs_conv.swap(vs);
    g_ps_conv.swap(ps);
    g_ps_safe.swap(safe);
    const size_t nv = g_vs_conv.size(), np = g_ps_conv.size(), ns = g_ps_safe.size();
    ReleaseSRWLockExclusive(&g_lock);
    log::info("[vi] shaders from %s: %zu vertex, %zu pixel converted, %zu pixel listed safe", dir.c_str(), nv, np, ns);
}

// ---- the twin: the game's desc as a pipeline stream, the converted shaders and a 2-view instancing subobject
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to pointer alignment, as the stream wants
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T v{};
};
struct Stream {
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC> layout;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE> cut;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT> node;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, D3D12_PIPELINE_STATE_FLAGS> flags;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
};
#pragma warning(pop)
const D3D12_VIEW_INSTANCE_LOCATION kLocations[2] = {{0, 0}, {0, 1}};  // view i: viewport 0, render-target array slice i

void make_twin(Kept* k) {
    k->state = -1;
    if (k->other_stages) {
        k->why = "other stages";
        g_stages.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::vector<uint8_t>* vs = nullptr;
    const std::vector<uint8_t>* ps = nullptr;
    bool ps_ok = k->ps.empty();
    AcquireSRWLockShared(&g_lock);
    if (auto it = g_vs_conv.find(k->vs_hash); it != g_vs_conv.end()) vs = &it->second;
    if (auto it = g_ps_conv.find(k->ps_hash); !k->ps.empty() && it != g_ps_conv.end()) {
        ps = &it->second;
        ps_ok = true;
    }
    if (!k->ps.empty() && g_ps_safe.count(k->ps_hash)) ps_ok = true;
    ReleaseSRWLockShared(&g_lock);  // (the maps' vectors stay while a load does not run: loads are the test's, between frames)
    if (!vs) {
        k->why = "vertex shader not converted";
        g_no_vs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!ps_ok) {
        k->why = "pixel shader not converted or listed safe";
        g_no_ps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Stream s;
    s.root.v = k->root;
    s.vs.v = {vs->data(), vs->size()};
    s.ps.v = ps ? D3D12_SHADER_BYTECODE{ps->data(), ps->size()} : D3D12_SHADER_BYTECODE{k->ps.data(), k->ps.size()};
    s.blend.v = k->blend;
    s.sample_mask.v = k->sample_mask;
    s.rast.v = k->rast;
    s.depth.v = k->depth;
    s.layout.v = {k->layout.empty() ? nullptr : k->layout.data(), static_cast<UINT>(k->layout.size())};
    s.cut.v = k->cut;
    s.topology.v = k->topology;
    s.rtv.v = k->rtv;
    s.dsv.v = k->dsv;
    s.sample.v = k->sample;
    s.node.v = k->node;
    s.flags.v = k->flags;
    s.views.v = {2, kLocations, D3D12_VIEW_INSTANCING_FLAG_NONE};
    const D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
    ID3D12PipelineState* twin = nullptr;
    const HRESULT hr = g_dev->CreatePipelineState(&sd, IID_PPV_ARGS(&twin));
    if (FAILED(hr) || !twin) {
        k->why = "CreatePipelineState refused";
        if (g_failed.fetch_add(1, std::memory_order_relaxed) < 10)
            log::warn("[vi] twin of vs %016llx ps %016llx refused (%#lx)", static_cast<unsigned long long>(k->vs_hash),
                      static_cast<unsigned long long>(k->ps_hash), static_cast<unsigned long>(hr));
        return;
    }
    k->state = 1;
    k->made_frame = state::presents.load();
    k->twin = twin;
    g_made.fetch_add(1, std::memory_order_relaxed);
}

void worker() {
    for (;;) {
        Kept* k = nullptr;
        {
            std::unique_lock lock(g_q_mutex);
            g_q_cv.wait(lock, [] { return !g_queue.empty(); });
            k = g_queue.front();
            g_queue.pop_front();
        }
        if (!k->twin.load()) make_twin(k);
    }
}

void enqueue(Kept* k) {
    std::lock_guard lock(g_q_mutex);
    if (!g_worker) {
        g_worker = true;
        std::thread(worker).detach();
    }
    g_queue.push_back(k);
    g_q_cv.notify_one();
}

// The twin to draw with now (made in an earlier frame, so both regions of a frame agree), or null.
ID3D12PipelineState* twin_of(ID3D12PipelineState* game) {
    if (!game) return nullptr;
    AcquireSRWLockShared(&g_lock);
    auto it = g_by_pso.find(game);
    Kept* k = it == g_by_pso.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    if (!k) return nullptr;
    ID3D12PipelineState* t = k->twin.load(std::memory_order_acquire);
    return t && k->made_frame.load(std::memory_order_relaxed) < state::presents.load(std::memory_order_relaxed) ? t : nullptr;
}

// ---- the playback thread's lists
struct ListState {
    ID3D12PipelineState* game = nullptr;   // the game's pipeline (set or the Reset's)
    ID3D12PipelineState* bound = nullptr;  // what is bound: the game's or its twin
    ID3D12GraphicsCommandList1* cl1 = nullptr;
};
thread_local std::unordered_map<ID3D12GraphicsCommandList*, ListState> t_lists;

ListState& list(ID3D12GraphicsCommandList* cl) {
    ListState& s = t_lists[cl];
    if (!s.cl1) cl->QueryInterface(IID_PPV_ARGS(&s.cl1));  // (a reference kept for the list's life)
    return s;
}

// What the draw needs bound: the twin in slice 0's region (all targets the arrays), else the game's.
ID3D12PipelineState* wanted_for(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* game, int* region, ID3D12PipelineState** twin) {
    bool arrays = false;
    *region = prepass::vi_region(cl, &arrays);
    *twin = *region && arrays ? twin_of(game) : nullptr;
    return *region == 1 && *twin ? *twin : game;
}

void bind(ID3D12GraphicsCommandList* cl, ListState& s, ID3D12PipelineState* p) {
    if (p == s.bound) return;
    pso::raw_set_pipeline(cl, p);
    if (p != s.game && s.cl1) s.cl1->SetViewInstanceMask(3);
    s.bound = p;
    g_rebinds.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void init() {
    if (!config::get_bool("Stereo", "ViPrepass", false)) return;
    if (!config::get_bool("Stereo", "PrepassArrays", false) || config::get_int("Stereo", "PrepassViews", 0) < 2) {
        log::warn("[vi] [Stereo] ViPrepass needs PrepassArrays=1 and PrepassViews=2: off");
        return;
    }
    g_dir = config::get_string("Stereo", "ViShaders", "");
    if (!g_dir.empty()) load_shaders(g_dir);
    g_on = config::get_bool("Stereo", "ViOn", true);
    g_configured = true;
    log::info("[vi] view-instanced pre-pass configured (%s)", g_on.load() ? "on" : "off until \"vi on\"");
}

void on_pipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* pso) {
    if (!g_configured.load(std::memory_order_relaxed) || !d || !pso) return;
    if (!g_dev) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&g_dev))) ||
            FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))) ||
            o3.ViewInstancingTier == D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED) {
            log::warn("[vi] no view instancing on this device: off");
            g_configured = false;
            return;
        }
        g_tier = static_cast<int>(o3.ViewInstancingTier);
        log::info("[vi] view instancing tier %d", g_tier);
    }
    auto k = std::make_unique<Kept>();
    k->game = pso;
    k->root = d->pRootSignature;
    if (k->root) k->root->AddRef();
    auto copy = [](const D3D12_SHADER_BYTECODE& b, std::vector<uint8_t>* out) {
        if (b.pShaderBytecode && b.BytecodeLength)
            out->assign(static_cast<const uint8_t*>(b.pShaderBytecode), static_cast<const uint8_t*>(b.pShaderBytecode) + b.BytecodeLength);
    };
    copy(d->VS, &k->vs);
    copy(d->PS, &k->ps);
    k->vs_hash = k->vs.empty() ? 0 : fnv(k->vs.data(), k->vs.size());
    k->ps_hash = k->ps.empty() ? 0 : fnv(k->ps.data(), k->ps.size());
    k->other_stages = d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength || d->StreamOutput.NumEntries;
    k->blend = d->BlendState;
    k->sample_mask = d->SampleMask;
    k->rast = d->RasterizerState;
    k->depth = d->DepthStencilState;
    if (d->InputLayout.pInputElementDescs) {
        k->layout.assign(d->InputLayout.pInputElementDescs, d->InputLayout.pInputElementDescs + d->InputLayout.NumElements);
        k->semantics.reserve(k->layout.size());
        for (auto& e : k->layout) {
            k->semantics.emplace_back(e.SemanticName ? e.SemanticName : "");
            e.SemanticName = k->semantics.back().c_str();  // (reserved: the strings do not move)
        }
    }
    k->cut = d->IBStripCutValue;
    k->topology = d->PrimitiveTopologyType;
    k->rtv.NumRenderTargets = d->NumRenderTargets;
    for (int i = 0; i < 8; ++i) k->rtv.RTFormats[i] = d->RTVFormats[i];
    k->dsv = d->DSVFormat;
    k->sample = d->SampleDesc;
    k->node = d->NodeMask;
    k->flags = d->Flags;
    Kept* raw = k.get();
    AcquireSRWLockExclusive(&g_lock);
    g_by_pso[pso] = raw;
    g_kept.push_back(std::move(k));
    ReleaseSRWLockExclusive(&g_lock);
    g_kept_n.fetch_add(1, std::memory_order_relaxed);
    enqueue(raw);
}

bool configured() { return g_configured.load(std::memory_order_relaxed); }

bool wanted() { return g_configured.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && g_made.load() > 0; }

ID3D12PipelineState* on_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (!g_configured.load(std::memory_order_relaxed)) return ps;
    ListState& s = list(cl);
    s.game = ps;
    int region = 0;
    ID3D12PipelineState* twin = nullptr;
    ID3D12PipelineState* p = wanted_for(cl, ps, &region, &twin);
    if (p != ps && s.cl1) s.cl1->SetViewInstanceMask(3);
    s.bound = p;
    return p;
}

void on_reset(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (!g_configured.load(std::memory_order_relaxed)) return;
    ListState& s = list(cl);
    s.game = s.bound = ps;
}

bool before_draw(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return false;
    ListState& s = list(cl);
    int region = 0;
    ID3D12PipelineState* twin = nullptr;
    ID3D12PipelineState* p = wanted_for(cl, s.game, &region, &twin);
    if (region == 2 && twin) {  // drawn for both eyes in slice 0's region
        g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (s.game) bind(cl, s, p);  // (out of a region a twin left bound goes back to the game's)
    if (region == 1) (p != s.game ? g_draws_both : g_draws_plain).fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool skip_clear(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed) || prepass::vi_region(cl, nullptr) != 2) return false;
    g_clears_skipped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string command(const std::vector<std::string>& t) {
    if (!g_configured.load()) return "ERROR vi not configured ([Stereo] ViPrepass=1 with PrepassArrays=1, PrepassViews=2)";
    const std::string sub = t.size() >= 2 ? t[1] : "status";
    if (sub == "on" || sub == "off") {
        g_on = sub == "on";
        return std::string("vi ") + sub + " (from the next pre-pass)";
    }
    if (sub == "load") {
        if (t.size() >= 3) g_dir = t[2];
        if (g_dir.empty()) return "ERROR no folder";
        load_shaders(g_dir);
        size_t n = 0;
        AcquireSRWLockShared(&g_lock);
        std::vector<Kept*> again;
        for (auto& k : g_kept)
            if (!k->twin.load()) again.push_back(k.get());
        ReleaseSRWLockShared(&g_lock);
        for (Kept* k : again) {
            enqueue(k);
            ++n;
        }
        return "loaded " + g_dir + "; " + std::to_string(n) + " pipelines queued for twins";
    }
    if (sub == "dump" && t.size() >= 3) {  // every kept pipeline's shaders, once each: vs_<hash>.dxbc, ps_<hash>.dxbc
        std::unordered_set<uint64_t> seen;
        size_t n = 0;
        AcquireSRWLockShared(&g_lock);
        for (auto& k : g_kept) {
            for (int st = 0; st < 2; ++st) {
                const std::vector<uint8_t>& b = st ? k->ps : k->vs;
                const uint64_t h = st ? k->ps_hash : k->vs_hash;
                if (b.empty() || !seen.insert(h * 2 + st).second) continue;
                FILE* f = nullptr;
                if (fopen_s(&f, (t[2] + (st ? "\\ps_" : "\\vs_") + hex(h) + ".dxbc").c_str(), "wb") == 0 && f) {
                    std::fwrite(b.data(), 1, b.size(), f);
                    std::fclose(f);
                    ++n;
                }
            }
        }
        ReleaseSRWLockShared(&g_lock);
        return "dumped " + std::to_string(n) + " shaders to " + t[2];
    }
    if (sub == "why" && t.size() >= 3) {  // vi why <vs hash>: each kept pipeline of that vertex shader and its twin's fate
        const uint64_t h = std::strtoull(t[2].c_str(), nullptr, 16);
        std::string r;
        AcquireSRWLockShared(&g_lock);
        for (auto& k : g_kept)
            if (k->vs_hash == h) r += "ps " + hex(k->ps_hash) + ": " + (k->twin.load() ? "twin" : k->why) + "; ";
        ReleaseSRWLockShared(&g_lock);
        return r.empty() ? "no pipeline of that vertex shader" : r;
    }
    char b[640];
    std::snprintf(b, sizeof(b),
                  "vi %s, tier %d | pipelines kept %llu, twins %llu (refused %llu; no converted vs %llu, ps %llu; other stages %llu) | "
                  "slice 0: draws for both eyes %llu, as the game's %llu | slice 1: draws skipped %llu, clears skipped %llu | rebinds %llu | %s",
                  wanted() ? "on" : g_on.load() ? "on, no twin yet" : "off", g_tier, static_cast<unsigned long long>(g_kept_n.load()),
                  static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_failed.load()),
                  static_cast<unsigned long long>(g_no_vs.load()), static_cast<unsigned long long>(g_no_ps.load()),
                  static_cast<unsigned long long>(g_stages.load()), static_cast<unsigned long long>(g_draws_both.load()),
                  static_cast<unsigned long long>(g_draws_plain.load()), static_cast<unsigned long long>(g_draws_skipped.load()),
                  static_cast<unsigned long long>(g_clears_skipped.load()), static_cast<unsigned long long>(g_rebinds.load()),
                  prepass::vi_status().c_str());
    return b;
}

}  // namespace rdrvr::vi
