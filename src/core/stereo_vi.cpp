#include "core/stereo_vi.h"
#include "core/hook_prof.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/camera_lever.h"
#include "core/recorder_skip.h"
#include "core/config.h"
#include "core/hooks.h"
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
    int ds_writes = 3;  // the depth's planes it can write: 1 depth, 2 stencil (prepass::drawn: only those copied out)
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
    std::atomic<bool> remake{false};          // "vi load <dir> all": made again from the newly loaded shaders
    const char* why = "";
};

std::atomic<bool> g_configured{false}, g_on{true};
std::atomic<bool> g_query_draws{true};
std::atomic<bool> g_skip{true};      // slice 1's twinned draws skipped ("vi skip off": eye 2 draws them itself, the A/B)
std::atomic<bool> g_pred_off{false}; // pass 2's main pass ignores the game's predication ("vi pred off": the A/B)
// [Stereo] ViUnpredicated ("vi pred0 off|on"; 2026-10-14, the player's distant cacti flickering in one eye): slice 0's
// region drops the game's predication, which is eye 1's occlusion and hid eye 2's view of a draw too
std::atomic<bool> g_pred0_off{false};
// [Stereo] ViMain (I2): the main pass in the regions too. Slice 1's region then holds eye 2's main pass, whose draws the
// game predicates by eye 2's own queries, which see nothing once its pre-pass draws are skipped: its predication is
// dropped instead (the draws it still makes, the untwinned, all drawn) and its queried draws need not be redrawn.
std::atomic<bool> g_main{false};  // slice 1's draws inside an occlusion query drawn ("vi qdraw on|off": the A/B)
// [Stereo] ViKeepEye1Queries (default off since the river water has no twin; 2026-10-14, the player's save: the water flat and grey-brown in both eyes, its
// reflection gone): both eyes' scenes use the same occlusion query slots and resolve them into the same buffers, eye 2's
// last. Eye 2's queries see nothing under the skip (its draws skipped, the predication dropped), so its resolve wrote
// "nothing visible" over eye 1's results, and the game, reading them, faded the water's reflection map out (probe: slice
// 1's region resolves slots [0..9) into the buffers slice 0's region resolved). Eye 2's occlusion resolves are dropped
// under the skip: the game reads eye 1's counts ("vi qkeep on|off"). Since the river water shaders have no twin (eye 2's
// region draws the water itself, its queries count), dropping eye 2's resolves left the water broken after each load
// until a two-pass frame (an alt-tab); the headset, the same day: resolves made, the water right.
std::atomic<bool> g_keep_eye1_queries{false};
std::atomic<uint64_t> g_resolves_dropped{0};
ID3D12Device2* g_dev = nullptr;
int g_tier = -1;
std::string g_dir;

SRWLOCK g_lock = SRWLOCK_INIT;  // g_kept, g_by_pso, the shader maps
std::vector<std::unique_ptr<Kept>> g_kept;
std::unordered_map<ID3D12PipelineState*, Kept*> g_by_pso;
std::atomic<uint64_t> g_by_pso_gen{0};  // records added (kept_of's caches)
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vs_conv, g_ps_conv;
// vp_<vs hash>_<ps hash>.dxil: a vertex shader for that pair only, before vs_<hash> (2026-10-14: the light quads' view 1
// reads the light's sphere from $Globals, whose layout is the pixel shader's)
std::unordered_map<uint64_t, std::vector<uint8_t>> g_vp_conv;
uint64_t vp_key(uint64_t vs, uint64_t ps) { return vs ^ (ps * 0x9e3779b97f4a7c15ull); }
std::unordered_set<uint64_t> g_ps_safe;
// "vi dsoff <ps,...> stencil|depth|both" / "vi dsoff off": twins of those pixel shaders made without the stencil and/or
// depth test (the A/B of 2026-10-14: eye 2's light quads drew no pixel at all)
std::unordered_set<uint64_t> g_dsoff;
int g_dsoff_what = 0;  // 1 stencil, 2 depth, 3 both
std::unordered_set<uint64_t> g_no_twin;  // <ViShaders>/no_twin.txt (stereo_vs.py NO_TWIN_PS): eye 1 only on purpose
// [Stereo] ViSkipRecord's safety: a draw in slice 0's region with no twin (a shader not converted: one the dump never saw)
// draws eye 1 only, and a skipped second scene records nothing for eye 2: eye 2's scene is recorded whole for a while
std::atomic<uint64_t> g_untwinned_at{0}, g_untwinned_n{0};
constexpr uint64_t kUntwinnedHold = 600;  // presents (~6.7 s at 90 Hz)
// ... and a twin still being made (queued, or made this frame: usable from the next) holds it for a few presents only
// (2026-10-14, the players' "shaders not working in one eye": a cold driver cache makes each twin slow, so a new area's
// pipelines drew in eye 1 only for seconds)
std::atomic<uint64_t> g_pending_at{0}, g_pending_n{0};
constexpr uint64_t kPendingHold = 3;
int g_twin_delay_ms = 0;  // [Stereo] ViTwinDelayMs (a test's: each twin made that much later, as a cold driver cache)
std::unordered_set<uint64_t> g_no_depth;  // <ViShaders>\no_depth.txt (tools/sp/dxil/depth_readers.py): cannot read the depth

std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<Kept*> g_queue;
bool g_worker = false;

std::atomic<uint64_t> g_kept_n{0}, g_made{0}, g_failed{0}, g_no_vs{0}, g_no_ps{0}, g_stages{0};
std::atomic<uint64_t> g_draws_both{0}, g_draws_skipped{0}, g_draws_plain{0}, g_clears_skipped{0}, g_rebinds{0}, g_draws_queried{0};

// "vi scan <dir>": every shader container in the process's memory (the game loads its effects whole at boot, though it
// makes their pipelines only where they are drawn: the places a "vi dump" never saw). A DXBC container: "DXBC", a
// checksum, version 1, its size, its chunks; the stage from the program chunk's version token (SHDR/SHEX: type in bits
// 16+, 0 pixel, 1 vertex; DXIL: the same). Copied under SEH (memory can change under the scan).
int container_stage(const uint8_t* b, size_t avail, uint32_t* size_out) {
    __try {
        if (avail < 32 || std::memcmp(b, "DXBC", 4) != 0) return -1;
        uint32_t ver, size, nchunks;
        std::memcpy(&ver, b + 20, 4);
        std::memcpy(&size, b + 24, 4);
        std::memcpy(&nchunks, b + 28, 4);
        if (ver != 1 || size < 32 || size > (8u << 20) || size > avail || nchunks == 0 || nchunks > 64 || 32 + nchunks * 4 > size) return -1;
        int stage = -1;
        for (uint32_t c = 0; c < nchunks; ++c) {
            uint32_t off;
            std::memcpy(&off, b + 32 + c * 4, 4);
            if (off + 8 > size) return -1;
            uint32_t len;
            std::memcpy(&len, b + off + 4, 4);
            if (off + 8 + len > size) return -1;
            const uint8_t* fourcc = b + off;
            if ((!std::memcmp(fourcc, "SHDR", 4) || !std::memcmp(fourcc, "SHEX", 4) || !std::memcmp(fourcc, "DXIL", 4)) && len >= 4) {
                uint32_t tok;
                std::memcpy(&tok, b + off + 8, 4);
                stage = static_cast<int>(tok >> 16);
            }
        }
        *size_out = size;
        return stage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool copy_seh(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
    std::unordered_map<uint64_t, std::vector<uint8_t>> vs, ps, vp;
    std::unordered_set<uint64_t> safe;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.dxil").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;  // vs_<16 hex>.dxil
            if (name.size() == 3 + 16 + 1 + 16 + 5 && name.compare(0, 3, "vp_") == 0) {  // vp_<vs>_<ps>.dxil
                const uint64_t hv = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
                const uint64_t hp = std::strtoull(name.substr(20, 16).c_str(), nullptr, 16);
                std::vector<uint8_t> b;
                if (hv && hp && read_file(dir + "\\" + name, &b)) vp.emplace(vp_key(hv, hp), std::move(b));
                continue;
            }
            if (name.size() != 3 + 16 + 5 || (name.compare(0, 3, "vs_") != 0 && name.compare(0, 3, "ps_") != 0)) continue;
            const uint64_t hash = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
            std::vector<uint8_t> b;
            if (!hash || !read_file(dir + "\\" + name, &b)) continue;
            (name[0] == 'v' ? vs : ps).emplace(hash, std::move(b));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::unordered_set<uint64_t> no_depth;
    if (FILE* f = nullptr; fopen_s(&f, (dir + "\\no_depth.txt").c_str(), "r") == 0 && f) {
        char line[64];
        while (std::fgets(line, sizeof(line), f))  // vs_<hash> or ps_<hash>
            if (const uint64_t x = std::strlen(line) > 3 ? std::strtoull(line + 3, nullptr, 16) : 0) no_depth.insert(x);
        std::fclose(f);
    }
    std::unordered_set<uint64_t> no_twin;
    if (FILE* f = nullptr; fopen_s(&f, (dir + "\\no_twin.txt").c_str(), "r") == 0 && f) {
        char line[64];
        while (std::fgets(line, sizeof(line), f))
            if (const uint64_t x = std::strtoull(line, nullptr, 16)) no_twin.insert(x);
        std::fclose(f);
    }
    // (a converted pixel shader the list says to leave alone is not used: an older release's file left in the folder,
    // e.g. 0.8.10.1's river water, broke the water)
    for (const uint64_t x : no_twin)
        if (ps.erase(x)) log::info("[vi] ps_%016llx.dxil ignored: listed in no_twin.txt (an older set's file)", (unsigned long long)x);
    if (FILE* f = nullptr; fopen_s(&f, (dir + "\\ps_safe.txt").c_str(), "r") == 0 && f) {
        char line[64];
        while (std::fgets(line, sizeof(line), f))
            if (const uint64_t x = std::strtoull(line, nullptr, 16)) safe.insert(x);
        std::fclose(f);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_vs_conv.swap(vs);
    g_ps_conv.swap(ps);
    g_vp_conv.swap(vp);
    g_ps_safe.swap(safe);
    g_no_depth.swap(no_depth);
    g_no_twin.swap(no_twin);
    const size_t nv = g_vs_conv.size(), np = g_ps_conv.size(), ns = g_ps_safe.size(), nd = g_no_depth.size(), nvp = g_vp_conv.size();
    ReleaseSRWLockExclusive(&g_lock);
    log::info("[vi] shaders from %s: %zu vertex, %zu pixel converted, %zu pixel listed safe, %zu that cannot read the depth, %zu pair "
              "vertex shaders", dir.c_str(), nv, np, ns, nd, nvp);
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
// view i: render-target array slice i; view 1 has its own viewport and scissor (index 1: the game's viewport, the scissor
// widened to it). The game clips what it draws through a door or a window (an interior) to that opening's screen
// rectangle, worked out for eye 1's camera; eye 2's opening is elsewhere on its screen, so eye 2 lost the interior
// (2026-10-13, the headset: a wall through the station's window in the right eye only). Unclipped, it is hidden where
// it should be by the depth test, like the rest of the scene.
const D3D12_VIEW_INSTANCE_LOCATION kLocations[2] = {{0, 0}, {1, 1}};

// [Stereo] TwinCache (2026-10-15, on): each twin made is stored in a D3D12 pipeline library saved to
// <game>\RDRVR_twins.bin, and a later session loads it from there (well under a millisecond) instead of compiling it
// (tens of ms on a cold driver cache; while a twin is being made, eye 2's scene is recorded whole: the CPU spikes riding
// into a town). A twin's name is its shaders' and states' content, so another shader set or state finds nothing and is
// made again. A library from another driver or GPU is refused by the runtime: a new one is started. Saved every 20 s
// while new twins came (the game ends with TerminateProcess: nothing runs at exit).
std::mutex g_lib_mutex;                // the library's calls (its loads, stores and serializing)
ID3D12PipelineLibrary1* g_lib = nullptr;
std::vector<uint8_t> g_lib_blob;       // the file's bytes, which the library reads from as long as it lives
std::atomic<int> g_lib_new{0};
std::atomic<uint64_t> g_lib_hits{0}, g_lib_stored{0};
bool g_lib_tried = false;
constexpr size_t kLibMaxBytes = 768ull << 20;  // a larger file (old shader sets' twins piling up) is started again

std::wstring lib_path() { return std::wstring(log::game_dir()) + L"RDRVR_twins.bin"; }

void lib_save() {
    std::vector<uint8_t> buf;
    {
        std::lock_guard lock(g_lib_mutex);
        if (!g_lib || g_lib_new.load() == 0) return;
        buf.resize(g_lib->GetSerializedSize());
        if (buf.empty() || FAILED(g_lib->Serialize(buf.data(), buf.size()))) {
            log::warn("[vi] twin cache: not serialized");
            return;
        }
        g_lib_new = 0;
    }
    const std::wstring path = lib_path(), tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;
    const bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        log::info("[vi] twin cache saved: %.1f MB (%llu loaded from it this session, %llu new)", buf.size() / 1048576.0,
                  static_cast<unsigned long long>(g_lib_hits.load()), static_cast<unsigned long long>(g_lib_stored.load()));
    else
        DeleteFileW(tmp.c_str());
}

void lib_open() {  // under g_lib_mutex, once, from the first twin
    if (g_lib_tried) return;
    g_lib_tried = true;
    if (!config::get_bool("Stereo", "TwinCache", true)) return;
    const std::wstring path = lib_path();
    if (FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0 && static_cast<size_t>(n) <= kLibMaxBytes) {
            g_lib_blob.resize(static_cast<size_t>(n));
            if (std::fread(g_lib_blob.data(), 1, g_lib_blob.size(), f) != g_lib_blob.size()) g_lib_blob.clear();
        }
        std::fclose(f);
    }
    HRESULT hr = E_FAIL;
    if (!g_lib_blob.empty()) {
        hr = g_dev->CreatePipelineLibrary(g_lib_blob.data(), g_lib_blob.size(), IID_PPV_ARGS(&g_lib));
        if (SUCCEEDED(hr))
            log::info("[vi] twin cache: %.1f MB from %ls", g_lib_blob.size() / 1048576.0, path.c_str());
        else
            log::info("[vi] twin cache: %ls not used (%#lx: another driver or GPU): a new one", path.c_str(), static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        g_lib_blob.clear();
        g_lib = nullptr;
        hr = g_dev->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_lib));
        if (FAILED(hr)) {
            g_lib = nullptr;
            log::warn("[vi] twin cache: no pipeline library (%#lx): twins made each session", static_cast<unsigned long>(hr));
            return;
        }
    }
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            lib_save();
        }
    }).detach();
}

// The twin's name: everything its desc holds but pointers (the shaders' bytes, the root signature's blob)
std::wstring twin_name(const Kept* k, const Stream& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    auto mix64 = [&](uint64_t v) { mix(&v, sizeof(v)); };
    mix64(fnv(s.vs.v.pShaderBytecode, s.vs.v.BytecodeLength));
    mix64(s.ps.v.BytecodeLength ? fnv(s.ps.v.pShaderBytecode, s.ps.v.BytecodeLength) : 0);
    mix64(pso::root_signature_hash(k->root));
    mix(&s.blend.v, sizeof(s.blend.v));
    mix(&s.sample_mask.v, sizeof(s.sample_mask.v));
    mix(&s.rast.v, sizeof(s.rast.v));
    mix(&s.depth.v, sizeof(s.depth.v));
    for (size_t i = 0; i < k->layout.size(); ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = k->layout[i];
        if (e.SemanticName) mix(e.SemanticName, std::strlen(e.SemanticName) + 1);
        const uint32_t v[6] = {e.SemanticIndex, static_cast<uint32_t>(e.Format), e.InputSlot, e.AlignedByteOffset,
                               static_cast<uint32_t>(e.InputSlotClass), e.InstanceDataStepRate};
        mix(v, sizeof(v));
    }
    mix(&s.cut.v, sizeof(s.cut.v));
    mix(&s.topology.v, sizeof(s.topology.v));
    mix(&s.rtv.v, sizeof(s.rtv.v));
    mix(&s.dsv.v, sizeof(s.dsv.v));
    mix(&s.sample.v, sizeof(s.sample.v));
    mix(&s.node.v, sizeof(s.node.v));
    mix(&s.flags.v, sizeof(s.flags.v));
    mix(kLocations, sizeof(kLocations));
    wchar_t b[24];
    std::swprintf(b, 24, L"t%016llx", static_cast<unsigned long long>(h));
    return b;
}

void make_twin(Kept* k) {
    k->state = -1;
    // "vi load <dir> all": a pipeline whose shaders are no twin (or refused) in the new set loses its old twin (leaked:
    // frames in flight use it); before 2026-10-14 the old twin stayed bound, so a no-twin A/B tested nothing
    k->twin.store(nullptr);
    if (k->other_stages) {
        k->why = "other stages";
        g_stages.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::vector<uint8_t>* vs = nullptr;
    const std::vector<uint8_t>* ps = nullptr;
    bool ps_ok = k->ps.empty();
    AcquireSRWLockShared(&g_lock);
    if (auto it = g_vp_conv.find(vp_key(k->vs_hash, k->ps_hash)); !k->ps.empty() && it != g_vp_conv.end()) vs = &it->second;
    else if (auto it2 = g_vs_conv.find(k->vs_hash); it2 != g_vs_conv.end()) vs = &it2->second;
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
    if (g_dsoff_what && g_dsoff.count(k->ps_hash)) {
        if (g_dsoff_what & 1) s.depth.v.StencilEnable = FALSE;
        if (g_dsoff_what & 2) s.depth.v.DepthEnable = FALSE;
    }
    s.layout.v = {k->layout.empty() ? nullptr : k->layout.data(), static_cast<UINT>(k->layout.size())};
    s.cut.v = k->cut;
    s.topology.v = k->topology;
    s.rtv.v = k->rtv;
    s.dsv.v = k->dsv;
    s.sample.v = k->sample;
    s.node.v = k->node;
    s.flags.v = k->flags;
    s.views.v = {2, kLocations, D3D12_VIEW_INSTANCING_FLAG_NONE};
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
    ID3D12PipelineState* twin = nullptr;
    std::wstring name;
    bool cache = false;
    {
        std::lock_guard lock(g_lib_mutex);
        lib_open();
        cache = g_lib != nullptr && pso::root_signature_hash(k->root) != 0;
        if (cache) {
            name = twin_name(k, s);
            if (SUCCEEDED(g_lib->LoadPipeline(name.c_str(), &sd, IID_PPV_ARGS(&twin))) && twin) g_lib_hits.fetch_add(1, std::memory_order_relaxed);
            else twin = nullptr;
        }
    }
    const bool loaded = twin != nullptr;
    const HRESULT hr = loaded ? S_OK : g_dev->CreatePipelineState(&sd, IID_PPV_ARGS(&twin));
    if (SUCCEEDED(hr) && twin && cache && !loaded) {
        std::lock_guard lock(g_lib_mutex);
        if (SUCCEEDED(g_lib->StorePipeline(name.c_str(), twin))) {  // (refused: that name holds another desc, or is taken)
            g_lib_stored.fetch_add(1, std::memory_order_relaxed);
            g_lib_new.fetch_add(1, std::memory_order_relaxed);
        }
    }
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
        if (g_twin_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_twin_delay_ms));
        if (!k->twin.load() || k->remake.exchange(false)) make_twin(k);  // (a replaced twin is leaked: frames in flight use it)
    }
}

void enqueue(Kept* k) {
    std::lock_guard lock(g_q_mutex);
    if (!g_worker) {  // (several: a cold driver cache takes tens of ms a pipeline)
        g_worker = true;
        const unsigned n = std::thread::hardware_concurrency() / 4;
        for (unsigned i = 0; i < (n < 2 ? 2 : n > 6 ? 6 : n); ++i) std::thread(worker).detach();
    }
    g_queue.push_back(k);
    g_q_cv.notify_one();
}

// Each thread's cache of g_by_pso (the playback thread sets ~1700 pipelines a frame; the lock and the map cost ~60 ns
// each): direct-mapped by address, emptied when any record is added (a pipeline made at a released one's address).
constexpr size_t kPsoCache = 512;
struct PsoCacheEntry {
    ID3D12PipelineState* game = nullptr;
    Kept* kept = nullptr;
};
thread_local PsoCacheEntry t_pso_cache[kPsoCache];
thread_local uint64_t t_pso_gen = ~0ull;
Kept* kept_of(ID3D12PipelineState* game) {  // (Kept records are never freed: a list may keep the pointer)
    if (!game) return nullptr;
    const uint64_t gen = g_by_pso_gen.load(std::memory_order_acquire);
    if (gen != t_pso_gen) {
        for (PsoCacheEntry& e : t_pso_cache) e = PsoCacheEntry{};
        t_pso_gen = gen;
    }
    PsoCacheEntry& e = t_pso_cache[(reinterpret_cast<uintptr_t>(game) >> 6) % kPsoCache];
    if (e.game == game) return e.kept;
    AcquireSRWLockShared(&g_lock);
    auto it = g_by_pso.find(game);
    Kept* k = it == g_by_pso.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    e = PsoCacheEntry{game, k};  // (a pipeline without a record too: null)
    return k;
}
// The twin to draw with now (made in an earlier frame, so both regions of a frame agree), or null.
ID3D12PipelineState* twin_of_kept(const Kept* k) {
    if (!k) return nullptr;
    ID3D12PipelineState* t = k->twin.load(std::memory_order_acquire);
    return t && k->made_frame.load(std::memory_order_relaxed) < state::presents.load(std::memory_order_relaxed) ? t : nullptr;
}

// ---- "vi plain": slice 0's region's draws made as the game's (eye 1 only), by pipeline and whether all targets were arrays
struct PlainStat {
    uint64_t n = 0, n_arrays = 0;
};
std::mutex g_plain_mutex;
std::unordered_map<ID3D12PipelineState*, PlainStat> g_plain;
std::atomic<bool> g_plain_on{false};  // (counted from the first "vi plain": a lock a draw costs the playback thread)
void note_plain(ID3D12PipelineState* game, bool arrays) {
    std::lock_guard lock(g_plain_mutex);
    PlainStat& st = g_plain[game];
    ++st.n;
    if (arrays) ++st.n_arrays;
}

// ---- the playback thread's lists
struct ListState {
    ID3D12PipelineState* game = nullptr;   // the game's pipeline (set or the Reset's)
    ID3D12PipelineState* bound = nullptr;  // what is bound: the game's or its twin
    ID3D12GraphicsCommandList1* cl1 = nullptr;
    Kept* kept = nullptr;                  // the game pipeline's record (its twin), looked up as it is set
    int queries_open = 0;  // occlusion queries begun and not ended on this list
    D3D12_VIEWPORT vp{};   // the game's viewport 0 and scissor 0 since the list's Reset (kLocations: view 1's are index 1)
    D3D12_RECT sc{};
    bool have_vp = false, have_sc = false;
};
thread_local std::unordered_map<ID3D12GraphicsCommandList*, ListState> t_lists;

thread_local ID3D12GraphicsCommandList* t_last_cl = nullptr;  // the last list's state (unordered_map nodes do not move)
thread_local ListState* t_last_s = nullptr;
ListState& list(ID3D12GraphicsCommandList* cl) {
    if (cl == t_last_cl) return *t_last_s;
    ListState& s = t_lists[cl];
    if (!s.cl1) cl->QueryInterface(IID_PPV_ARGS(&s.cl1));  // (a reference kept for the list's life)
    t_last_cl = cl, t_last_s = &s;
    return s;
}

using SetViewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using SetScissors_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
SetViewports_t o_SetViewports = nullptr;
SetScissors_t o_SetScissors = nullptr;
std::atomic<uint64_t> g_rs_dups{0};
D3D12_RECT wide_of(const ListState& s) {
    return D3D12_RECT{static_cast<LONG>(s.vp.TopLeftX), static_cast<LONG>(s.vp.TopLeftY), static_cast<LONG>(s.vp.TopLeftX + s.vp.Width),
                      static_cast<LONG>(s.vp.TopLeftY + s.vp.Height)};
}
// A twin bound: viewport and scissor 1 for view 1 (the game's viewport; the scissor widened to it)
void rs_for_twin(ID3D12GraphicsCommandList* cl, const ListState& s) {
    if (!o_SetViewports || !o_SetScissors || !s.have_vp) return;
    const D3D12_VIEWPORT v[2] = {s.vp, s.vp};
    o_SetViewports(cl, 2, v);
    const D3D12_RECT r[2] = {s.have_sc ? s.sc : wide_of(s), wide_of(s)};
    o_SetScissors(cl, 2, r);
    g_rs_dups.fetch_add(1, std::memory_order_relaxed);
}
ListState& list(ID3D12GraphicsCommandList* cl);
void STDMETHODCALLTYPE hk_SetViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
    hookprof::Scope hp_(hookprof::kViewport);
    ListState& s = list(cl);
    if (n >= 1 && v) s.vp = v[0], s.have_vp = true;
    if (n == 1 && s.bound && s.bound != s.game) return rs_for_twin(cl, s);
    HP_ORIG(hp_, o_SetViewports(cl, n, v));
}
void STDMETHODCALLTYPE hk_SetScissors(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
    hookprof::Scope hp_(hookprof::kScissor);
    ListState& s = list(cl);
    if (n >= 1 && r) s.sc = r[0], s.have_sc = true;
    if (n == 1 && s.bound && s.bound != s.game && s.have_vp) return rs_for_twin(cl, s);
    HP_ORIG(hp_, o_SetScissors(cl, n, r));
}

// What the draw needs bound: the twin in slice 0's region (all targets the arrays), else the game's.
ID3D12PipelineState* wanted_for(ID3D12GraphicsCommandList* cl, const ListState& s, int* region, ID3D12PipelineState** twin) {
    bool arrays = false;
    *region = prepass::vi_region(cl, &arrays);
    *twin = *region && arrays ? twin_of_kept(s.kept) : nullptr;
    return *region == 1 && *twin ? *twin : s.game;
}

void bind(ID3D12GraphicsCommandList* cl, ListState& s, ID3D12PipelineState* p) {
    if (p == s.bound) return;
    pso::raw_set_pipeline(cl, p);
    if (p != s.game && s.cl1) s.cl1->SetViewInstanceMask(3);
    if (p != s.game) rs_for_twin(cl, s);
    s.bound = p;
    g_rebinds.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace
bool untwinned_recent() {
    const uint64_t at = g_untwinned_at.load(std::memory_order_relaxed), pt = g_pending_at.load(std::memory_order_relaxed),
                   now = state::presents.load(std::memory_order_relaxed);
    return (at && at + kUntwinnedHold >= now) || (pt && pt + kPendingHold >= now);
}
uint64_t untwinned_count() { return g_untwinned_n.load(std::memory_order_relaxed); }

void install_rs_hooks(ID3D12GraphicsCommandList* cl) {
    if (o_SetViewports) return;
    hooks::install("CL::RSSetViewports (ViMain)", hooks::vtable_entry(cl, 21), hk_SetViewports, &o_SetViewports);
    hooks::install("CL::RSSetScissorRects (ViMain)", hooks::vtable_entry(cl, 22), hk_SetScissors, &o_SetScissors);
}

void init() {
    if (!config::get_bool("Stereo", "ViPrepass", false)) return;
    if (!config::get_bool("Stereo", "PrepassArrays", false) || config::get_int("Stereo", "PrepassViews", 0) < 2) {
        log::warn("[vi] [Stereo] ViPrepass needs PrepassArrays=1 and PrepassViews=2: off");
        return;
    }
    g_dir = config::get_string("Stereo", "ViShaders", "");
    if (!g_dir.empty() && g_dir.find(':') == std::string::npos && g_dir[0] != '\\' && g_dir[0] != '/') {
        char gd[MAX_PATH * 2] = {};  // a relative folder (the release's RDRVR_shaders) is in the game's folder
        WideCharToMultiByte(CP_ACP, 0, log::game_dir(), -1, gd, sizeof(gd), nullptr, nullptr);
        g_dir = std::string(gd) + g_dir;
    }
    if (!g_dir.empty()) load_shaders(g_dir);
    g_on = config::get_bool("Stereo", "ViOn", true);
    g_twin_delay_ms = config::get_int("Stereo", "ViTwinDelayMs", 0);
    g_main = config::get_bool("Stereo", "ViMain", false);
    if (g_main) g_query_draws = false;
    g_keep_eye1_queries = config::get_bool("Stereo", "ViKeepEye1Queries", false);
    g_pred0_off = config::get_bool("Stereo", "ViUnpredicated", false);
    g_configured = true;
    log::info("[vi] view-instanced pre-pass configured (%s)", g_on.load() ? "on" : "off until \"vi on\"");
}

void on_pipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState* pso) {
    if (!g_configured.load(std::memory_order_relaxed) || !d || !pso) return;
    g_by_pso_gen.fetch_add(1, std::memory_order_release);  // (any new pipeline, recorded or not: kept_of's caches)
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
    {
        const D3D12_DEPTH_STENCIL_DESC& z = d->DepthStencilState;
        auto ops = [](const D3D12_DEPTH_STENCILOP_DESC& o) {
            return o.StencilFailOp != D3D12_STENCIL_OP_KEEP || o.StencilDepthFailOp != D3D12_STENCIL_OP_KEEP || o.StencilPassOp != D3D12_STENCIL_OP_KEEP;
        };
        k->ds_writes = (z.DepthEnable && z.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 1 : 0) |
                       (z.StencilEnable && z.StencilWriteMask && (ops(z.FrontFace) || ops(z.BackFace)) ? 2 : 0);
    }
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
    g_by_pso_gen.fetch_add(1, std::memory_order_release);  // (the threads' caches of g_by_pso start again)
    g_kept.push_back(std::move(k));
    ReleaseSRWLockExclusive(&g_lock);
    g_kept_n.fetch_add(1, std::memory_order_relaxed);
    enqueue(raw);
}

bool configured() { return g_configured.load(std::memory_order_relaxed); }

bool viewport_extent(ID3D12GraphicsCommandList* cl, uint32_t* right, uint32_t* bottom) {
    if (!g_configured.load(std::memory_order_relaxed)) return false;
    const ListState& s = list(cl);
    if (!s.have_vp) return false;
    const D3D12_VIEWPORT& v = s.vp;
    if (v.TopLeftX < 0 || v.TopLeftY < 0 || v.Width <= 0 || v.Height <= 0) return false;
    *right = static_cast<uint32_t>(std::ceil(v.TopLeftX + v.Width));
    *bottom = static_cast<uint32_t>(std::ceil(v.TopLeftY + v.Height));
    return true;
}

ID3D12RootSignature* bound_root(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return nullptr;
    const ListState& s = list(cl);
    return s.kept ? s.kept->root : nullptr;
}
bool game_viewport(ID3D12GraphicsCommandList* cl, D3D12_VIEWPORT* vp) {
    if (!g_configured.load(std::memory_order_relaxed)) return false;
    const ListState& s = list(cl);
    if (!s.have_vp) return false;
    *vp = s.vp;
    return true;
}
void raw_viewport(ID3D12GraphicsCommandList* cl, const D3D12_VIEWPORT& vp) {
    const D3D12_RECT r{static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY), static_cast<LONG>(vp.TopLeftX + vp.Width),
                       static_cast<LONG>(vp.TopLeftY + vp.Height)};
    if (o_SetViewports) o_SetViewports(cl, 1, &vp);
    if (o_SetScissors) o_SetScissors(cl, 1, &r);
}
void rebind(ID3D12GraphicsCommandList* cl) {
    ListState& s = list(cl);
    if (!s.bound) s.bound = s.game;
    if (s.bound) pso::raw_set_pipeline(cl, s.bound);
    if (s.bound && s.bound != s.game) return rs_for_twin(cl, s);
    if (s.have_vp && o_SetViewports) o_SetViewports(cl, 1, &s.vp);
    if (o_SetScissors) {
        const D3D12_RECT r = s.have_sc ? s.sc : wide_of(s);
        if (s.have_sc || s.have_vp) o_SetScissors(cl, 1, &r);
    }
}

bool may_read_depth(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return true;
    const Kept* k = list(cl).kept;
    if (!k) return true;
    AcquireSRWLockShared(&g_lock);
    const bool r = !g_no_depth.count(k->vs_hash) || (k->ps_hash && !g_no_depth.count(k->ps_hash));
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int writes_depth(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return -1;
    const ListState& s = list(cl);
    return !s.game ? -1 : s.kept ? s.kept->ds_writes : -1;  // (the list's cached record: no lock a draw)
}

bool wanted() {
    return g_configured.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && g_made.load() > 0 &&
           (!g_main.load(std::memory_order_relaxed) || heap_ready());  // (ViMain: view 1 reads eye 2's views unconditionally)
}

ID3D12PipelineState* on_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (!g_configured.load(std::memory_order_relaxed)) return ps;
    ListState& s = list(cl);
    if (s.game != ps || !s.kept) s.kept = kept_of(ps);
    s.game = ps;
    int region = 0;
    ID3D12PipelineState* twin = nullptr;
    ID3D12PipelineState* p = wanted_for(cl, s, &region, &twin);
    if (p != ps && s.cl1) s.cl1->SetViewInstanceMask(3);
    if (p != ps && s.bound != p) rs_for_twin(cl, s);
    s.bound = p;
    return p;
}

void on_reset(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (!g_configured.load(std::memory_order_relaxed)) return;
    ListState& s = list(cl);
    s.game = s.bound = ps;
    s.kept = kept_of(ps);
    s.queries_open = 0;
    s.have_vp = s.have_sc = false;  // (a Reset clears them)
}

// "vi drawn2 on|off|list": slice 1's region's draws that are made (no twin, a query's), by shader pair: what a skipped
// second-scene recording (recorder_skip.h) would lose
std::atomic<bool> g_drawn2_on{false};
std::mutex g_drawn2_lock;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_drawn2;
void note_drawn2(const ListState& s) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_by_pso.find(s.game);
    const Kept* k = it == g_by_pso.end() ? nullptr : it->second;
    const std::pair<uint64_t, uint64_t> key{k ? k->vs_hash : 0, k ? k->ps_hash : 0};
    ReleaseSRWLockShared(&g_lock);
    std::lock_guard l(g_drawn2_lock);
    ++g_drawn2[key];
}

bool before_draw(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return false;
    ListState& s = list(cl);
    if (prepass::tracing() && s.game) {  // "prepass trace": each draw's shaders, beside the targets' binds
        AcquireSRWLockShared(&g_lock);
        auto it = g_by_pso.find(s.game);
        const Kept* k = it == g_by_pso.end() ? nullptr : it->second;
        const uint64_t vh = k ? k->vs_hash : 0, ph = k ? k->ps_hash : 0;
        ReleaseSRWLockShared(&g_lock);
        bool arrays = false;
        const int region = prepass::vi_region(cl, &arrays);
        log::info("[prepass] trace draw: vs %016llx ps %016llx region %d%s", static_cast<unsigned long long>(vh),
                  static_cast<unsigned long long>(ph), region, arrays ? " (arrays)" : "");
    }
    int region = 0;
    ID3D12PipelineState* twin = nullptr;
    ID3D12PipelineState* p = wanted_for(cl, s, &region, &twin);
    if (region == 2 && twin && !g_skip.load(std::memory_order_relaxed)) {  // the A/B: eye 2 draws it itself
        if (s.game) bind(cl, s, s.game);
        return false;
    }
    if (region == 2 && twin) {  // drawn for both eyes in slice 0's region
        // ... except inside an occlusion query (the probe, 2026-10-13: the pre-pass's queries are resolved and predicate
        // the main pass's draws in the same pass; skipped here, eye 2's queries saw nothing, its main pass skipped those
        // objects and eye 1's pixels stayed in the shared target). Drawn again as the game's, with pass 2's same math, so
        // the depth is equal and the query counts what eye 2 sees (KF2VR's lesson: occlusion per eye).
        if (s.queries_open > 0 && g_query_draws.load(std::memory_order_relaxed)) {
            g_draws_queried.fetch_add(1, std::memory_order_relaxed);
            if (s.game) bind(cl, s, s.game);
            return false;
        }
        g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (region == 2 && g_drawn2_on.load(std::memory_order_relaxed)) note_drawn2(s);
    if (s.game) bind(cl, s, p);  // (out of a region a twin left bound goes back to the game's)
    if (region == 1) (p != s.game ? g_draws_both : g_draws_plain).fetch_add(1, std::memory_order_relaxed);
    const char* why = s.kept ? s.kept->why : "";
    bool pend_arrays = false;  // (into the arrays only: elsewhere the game's pipeline is drawn though a twin is made)
    if (region == 1 && p == s.game && s.kept && !why[0] && (prepass::vi_region(cl, &pend_arrays), pend_arrays)) {
        // a twin still being made (or made this frame)
        const uint64_t now = state::presents.load(std::memory_order_relaxed);
        if (g_pending_at.exchange(now, std::memory_order_relaxed) != now) g_pending_n.fetch_add(1, std::memory_order_relaxed);
    } else if (region == 1 && p == s.game && s.kept && !s.kept->twin.load(std::memory_order_relaxed)) {  // never made
        AcquireSRWLockShared(&g_lock);
        const bool on_purpose = std::strstr(why, "not converted") && g_no_twin.count(s.kept->ps_hash) != 0;
        ReleaseSRWLockShared(&g_lock);
        if (!on_purpose) {
            const uint64_t now = state::presents.load(std::memory_order_relaxed);
            if (g_untwinned_at.exchange(now, std::memory_order_relaxed) + kUntwinnedHold < now) {
                g_untwinned_n.fetch_add(1, std::memory_order_relaxed);
                log::limited("vi.untwinned", 20, "[vi] vs %016llx ps %016llx drew without a twin (%s): eye 2's scene recorded whole for a while (convert it: vi dump)",
                             static_cast<unsigned long long>(s.kept->vs_hash), static_cast<unsigned long long>(s.kept->ps_hash), why);
            }
        }
    }
    if (region == 1 && p == s.game && g_plain_on.load(std::memory_order_relaxed)) {
        bool arrays = false;
        prepass::vi_region(cl, &arrays);
        note_plain(s.game, arrays);
    }
    return false;
}

// ---- the query probe: per region (0 outside, 1 slice 0's, 2 slice 1's) and kind, counts; the index and offset ranges
struct QStat {
    uint64_t n = 0;
    uint64_t lo = ~0ull, hi = 0;  // the query indices (or the predicate offsets) seen
};
std::mutex g_qmutex;
QStat g_q[5][4];  // 0 before any region this frame, 1 in slice 0's, 2 in slice 1's, 3 after slice 0's, 4 after slice 1's
std::unordered_map<ID3D12Resource*, int> g_pred_res;  // predicate buffers seen (by region bits)
std::atomic<bool> g_qprobe_on{false};  // (from the first "vi queries": a lock a query costs the playback thread)
void on_query_impl(ID3D12GraphicsCommandList* cl, int kind, UINT index, ID3D12Resource* res, UINT64 off, UINT n) {
    if (!g_qprobe_on.load(std::memory_order_relaxed)) return;
    bool arrays = false;
    int region = prepass::vi_region(cl, &arrays);
    if (!region) region = prepass::last_slice() < 0 ? 0 : 3 + prepass::last_slice();
    std::lock_guard lock(g_qmutex);
    QStat& q = g_q[region][kind];
    q.n += n;
    const uint64_t v = kind == 3 ? off : index;
    if (v < q.lo) q.lo = v;
    if (v + (kind == 2 ? n : 1) > q.hi) q.hi = v + (kind == 2 ? n : 1);
    if (res) g_pred_res[res] |= 1 << (region * 2 + (kind == 3 ? 0 : 1));  // bit 2r: predicate, 2r+1: resolve target
}

void on_query(ID3D12GraphicsCommandList* cl, int kind, D3D12_QUERY_TYPE type, UINT index, ID3D12Resource* res, UINT64 off, UINT n) {
    if (!g_configured.load(std::memory_order_relaxed)) return;
    if (kind <= 1 && (type == D3D12_QUERY_TYPE_OCCLUSION || type == D3D12_QUERY_TYPE_BINARY_OCCLUSION)) {
        ListState& s = list(cl);
        s.queries_open = kind == 0 ? s.queries_open + 1 : (s.queries_open > 0 ? s.queries_open - 1 : 0);
    }
    on_query_impl(cl, kind, index, res, off, n);
}

bool drop_resolve(ID3D12GraphicsCommandList* cl, D3D12_QUERY_TYPE type) {
    if (!g_configured.load(std::memory_order_relaxed) || !g_keep_eye1_queries.load(std::memory_order_relaxed) ||
        !g_skip.load(std::memory_order_relaxed))
        return false;
    if (type != D3D12_QUERY_TYPE_OCCLUSION && type != D3D12_QUERY_TYPE_BINARY_OCCLUSION) return false;
    if (prepass::vi_region(cl, nullptr) != 2) return false;
    g_resolves_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool drop_predication(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed)) return false;
    if (g_main.load(std::memory_order_relaxed) && prepass::vi_region(cl, nullptr) == 2) return true;
    if (g_pred0_off.load(std::memory_order_relaxed) && prepass::vi_region(cl, nullptr) == 1) return true;
    if (!g_pred_off.load(std::memory_order_relaxed)) return false;
    return prepass::vi_region(cl, nullptr) == 0 && prepass::last_slice() == 1 && prepass::vi_frame_now();
}

bool main_pass() { return g_main.load(std::memory_order_relaxed); }

bool skip_clear(ID3D12GraphicsCommandList* cl) {
    if (!g_configured.load(std::memory_order_relaxed) || !g_skip.load(std::memory_order_relaxed)) return false;
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
    if (sub == "heap") return heap_status();
    if (sub == "drawn2" && t.size() >= 3) {
        if (t[2] == "on" || t[2] == "off") {
            if (t[2] == "on") {
                std::lock_guard l(g_drawn2_lock);
                g_drawn2.clear();
            }
            g_drawn2_on = t[2] == "on";
            return std::string("slice 1's made draws counted: ") + t[2];
        }
        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v;
        {
            std::lock_guard l(g_drawn2_lock);
            for (auto& [k, n] : g_drawn2) v.push_back({n, k});
        }
        std::sort(v.rbegin(), v.rend());
        std::string o = "slice 1's made draws by vs/ps (" + std::to_string(v.size()) + " pairs):";
        for (auto& [n, k] : v) {
            char b[80];
            std::snprintf(b, sizeof(b), " %016llx/%016llx %llu", static_cast<unsigned long long>(k.first), static_cast<unsigned long long>(k.second),
                          static_cast<unsigned long long>(n));
            o += b;
        }
        return o;
    }
    if (sub == "pass2" && t.size() >= 4 && t[2] == "slots")  // vi pass2 slots <hex mask> (recorder_skip.h)
        return recorder_skip::slots(static_cast<uint32_t>(std::strtoul(t[3].c_str(), nullptr, 16)));
    if (sub == "pass2" && t.size() >= 5 && t[2] == "cut")  // vi pass2 cut <rva hex> on|off (recorder_skip.h)
        return recorder_skip::cut(static_cast<uintptr_t>(std::strtoull(t[3].c_str(), nullptr, 16)), t[4] == "on");
    if (sub == "pass2" && t.size() >= 3) {  // vi pass2 skip|draw: a view-instanced frame's second scene not walked / walked
        camera_lever::set_vi_skip2(t[2] == "skip");
        return std::string("second scenes of view-instanced frames: ") + (t[2] == "skip" ? "recorded without draws" : "recorded whole") +
               " (" + std::to_string(camera_lever::vi_skipped2()) + " so far) | " + recorder_skip::status();
    }
    if (sub == "plain") {  // vi plain [reset]: slice 0's region's eye-1-only draws, most first (counted from the first ask)
        if (!g_plain_on.exchange(true)) return "slice 0's eye-1-only draws counted from now: ask again";
        std::vector<std::pair<ID3D12PipelineState*, PlainStat>> v;
        {
            std::lock_guard lock(g_plain_mutex);
            if (t.size() >= 3 && t[2] == "reset") {
                g_plain.clear();
                return "reset";
            }
            v.assign(g_plain.begin(), g_plain.end());
        }
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second.n > y.second.n; });
        std::string r = std::to_string(v.size()) + " pipelines: ";
        AcquireSRWLockShared(&g_lock);
        for (size_t i = 0; i < v.size() && i < 40; ++i) {
            auto it = g_by_pso.find(v[i].first);
            const Kept* k = it == g_by_pso.end() ? nullptr : it->second;
            char x[256];
            std::snprintf(x, sizeof(x), "%llu (all arrays %llu) vs %s ps %s %s; ", static_cast<unsigned long long>(v[i].second.n),
                          static_cast<unsigned long long>(v[i].second.n_arrays), k ? hex(k->vs_hash).c_str() : "?",
                          k ? hex(k->ps_hash).c_str() : "?", k ? (k->twin.load() ? "twin" : k->why) : "not kept");
            r += x;
        }
        ReleaseSRWLockShared(&g_lock);
        return r;
    }
    if (sub == "cull" && t.size() >= 3) {  // vi cull on|off: [Stereo] ViUnionCull
        camera_lever::set_vi_cull(t[2] == "on");
        return std::string("union cull ") + t[2] + " (" + std::to_string(camera_lever::vi_culls()) + " plane swaps so far)";
    }
    if (sub == "camfix" && t.size() >= 3) {
        camera_lever::set_vi_camfix(t[2] == "on");
        return std::string("pass 2 from eye 1's position: ") + t[2];
    }
    if (sub == "skip" && t.size() >= 3) {
        g_skip = t[2] == "on";
        return std::string("slice 1's twinned draws ") + (g_skip.load() ? "skipped" : "drawn by eye 2 itself (A/B)");
    }
    if (sub == "dsoff" && t.size() >= 3) {
        AcquireSRWLockExclusive(&g_lock);
        g_dsoff.clear();
        g_dsoff_what = 0;
        if (t[2] != "off") {
            std::stringstream ss(t[2]);
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) g_dsoff.insert(std::strtoull(item.c_str(), nullptr, 16));
            const std::string w = t.size() >= 4 ? t[3] : "stencil";
            g_dsoff_what = w == "depth" ? 2 : w == "both" ? 3 : 1;
        }
        std::vector<Kept*> again;
        for (auto& k : g_kept)
            if (k->ps_hash && (g_dsoff.count(k->ps_hash) || t[2] == "off")) {
                k->remake = true;
                again.push_back(k.get());
            }
        ReleaseSRWLockExclusive(&g_lock);
        for (Kept* k : again) enqueue(k);
        return "twins without " + std::string(g_dsoff_what == 2 ? "the depth test" : g_dsoff_what == 3 ? "depth and stencil tests" : g_dsoff_what ? "the stencil test" : "nothing") +
               " for " + std::to_string(g_dsoff.size()) + " pixel shaders; " + std::to_string(again.size()) + " pipelines remade";
    }
    if (sub == "pred" && t.size() >= 3) {
        g_pred_off = t[2] == "off";
        return std::string("pass 2's predication ") + (g_pred_off.load() ? "ignored (A/B)" : "as the game's");
    }
    if (sub == "pred0" && t.size() >= 3) {
        g_pred0_off = t[2] == "off";
        return std::string("slice 0's region's predication ") + (g_pred0_off.load() ? "dropped (both eyes draw all)" : "as the game's (eye 1's occlusion)");
    }
    if (sub == "qkeep" && t.size() >= 3) {
        g_keep_eye1_queries = t[2] == "on";
        return std::string("eye 2's occlusion resolves ") + (g_keep_eye1_queries.load() ? "dropped (eye 1's counts kept)" : "made (the A/B)") +
               " (" + std::to_string(g_resolves_dropped.load()) + " dropped so far)";
    }
    if (sub == "qdraw" && t.size() >= 3) {
        g_query_draws = t[2] == "on";
        return std::string("eye 2's queried draws ") + (g_query_draws.load() ? "drawn" : "skipped (the bug's A/B)");
    }
    if (sub == "load") {
        if (t.size() >= 3) g_dir = t[2];
        if (g_dir.empty()) return "ERROR no folder";
        load_shaders(g_dir);
        size_t n = 0;
        AcquireSRWLockShared(&g_lock);
        std::vector<Kept*> again;
        const bool all = t.size() >= 4 && t[3] == "all";
        for (auto& k : g_kept)
            if (!k->twin.load() || all) {
                if (all) k->remake = true;
                again.push_back(k.get());
            }
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
    if (sub == "scan" && t.size() >= 3) {  // vi scan <dir>: every vertex and pixel shader in memory (see container_stage)
        std::unordered_set<uint64_t> seen;
        size_t vs_n = 0, ps_n = 0, other = 0, written = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        std::vector<uint8_t> blob;
        for (uint8_t* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi); a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
            const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            if (mbi.State != MEM_COMMIT || !(mbi.Protect & rd) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
            const uint8_t* base = static_cast<const uint8_t*>(mbi.BaseAddress);
            const size_t len = mbi.RegionSize;
            for (size_t i = 0; i + 32 <= len; i += 4) {  // (containers are 4-aligned in practice)
                if (base[i] != 'D' || base[i + 1] != 'X' || base[i + 2] != 'B' || base[i + 3] != 'C') continue;
                uint32_t size = 0;
                const int stage = container_stage(base + i, len - i, &size);
                if (stage < 0) continue;
                if (stage != 0 && stage != 1) {
                    ++other;
                    continue;
                }
                blob.resize(size);
                if (!copy_seh(blob.data(), base + i, size)) continue;
                const uint64_t h = fnv(blob.data(), size);
                if (!seen.insert(h * 2 + stage).second) continue;
                (stage ? vs_n : ps_n)++;
                const std::string path = t[2] + (stage ? "\\vs_" : "\\ps_") + hex(h) + ".dxbc";
                if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
                FILE* f = nullptr;
                if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
                    std::fwrite(blob.data(), 1, size, f);
                    std::fclose(f);
                    ++written;
                }
                i += (size & ~3u) - 4;
            }
        }
        return "scanned: " + std::to_string(vs_n) + " vertex, " + std::to_string(ps_n) + " pixel shaders (" + std::to_string(other) +
               " other stages), " + std::to_string(written) + " new files in " + t[2];
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
    if (sub == "queries") {  // the query probe: per region, Begin/End/Resolve/Predicate counts and ranges; "queries reset"
        if (!g_qprobe_on.exchange(true)) return "the query probe counts from now: ask again";
        std::lock_guard lock(g_qmutex);
        if (t.size() >= 3 && t[2] == "reset") {
            for (auto& r : g_q)
                for (auto& q : r) q = QStat{};
            g_pred_res.clear();
            return "reset";
        }
        static const char* const kKind[4] = {"begin", "end", "resolve", "predicate"};
        std::string out;
        static const char* const kWhere[5] = {"before the regions:", " | slice 0 region:", " | slice 1 region:", " | after slice 0 (pass 1's main):",
                                              " | after slice 1 (pass 2's main):"};
        for (int r = 0; r < 5; ++r) {
            out += kWhere[r];
            for (int k = 0; k < 4; ++k) {
                char x[96];
                std::snprintf(x, sizeof(x), " %s %llu [%llu..%llu)", kKind[k], static_cast<unsigned long long>(g_q[r][k].n),
                              static_cast<unsigned long long>(g_q[r][k].n ? g_q[r][k].lo : 0), static_cast<unsigned long long>(g_q[r][k].hi));
                out += x;
            }
        }
        for (auto& [res, bits] : g_pred_res) {
            char x[64];
            std::snprintf(x, sizeof(x), " | buffer %p bits %x", static_cast<void*>(res), bits);
            out += x;
        }
        return out;
    }
    char b[768];
    std::snprintf(b, sizeof(b),
                  "vi %s, tier %d | pipelines kept %llu, twins %llu (refused %llu; no converted vs %llu, ps %llu; other stages %llu) | eye 2 recorded whole: untwinned %llu, pending frames %llu | "
                  "slice 0: draws for both eyes %llu, as the game's %llu | slice 1: draws skipped %llu, drawn for their queries %llu, clears skipped %llu | rebinds %llu | %s",
                  wanted() ? "on" : g_on.load() ? "on, no twin yet" : "off", g_tier, static_cast<unsigned long long>(g_kept_n.load()),
                  static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_failed.load()),
                  static_cast<unsigned long long>(g_no_vs.load()), static_cast<unsigned long long>(g_no_ps.load()),
                  static_cast<unsigned long long>(g_stages.load()), static_cast<unsigned long long>(g_untwinned_n.load()),
                  static_cast<unsigned long long>(g_pending_n.load()), static_cast<unsigned long long>(g_draws_both.load()),
                  static_cast<unsigned long long>(g_draws_plain.load()), static_cast<unsigned long long>(g_draws_skipped.load()),
                  static_cast<unsigned long long>(g_draws_queried.load()),
                  static_cast<unsigned long long>(g_clears_skipped.load()), static_cast<unsigned long long>(g_rebinds.load()),
                  prepass::vi_status().c_str());
    return b;
}

}  // namespace rdrvr::vi
