// Does D3D12 take a flat (Texture2D) view on one slice of a 2-slice texture, and does a shader declaring Texture2D read
// that slice? The question behind giving the game's targets a second slice instead of copying eye 2 out (2026-10-14).
// Built and run by viewtest.ps1: the debug layer and GPU-based validation on, their messages printed.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstdio>

#define CHECK(x)                                                                         \
    do {                                                                                 \
        HRESULT hr_ = (x);                                                               \
        if (FAILED(hr_)) {                                                               \
            std::printf("FAILED %s -> %#lx\n", #x, static_cast<unsigned long>(hr_));    \
            dump();                                                                      \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

ID3D12InfoQueue* g_iq = nullptr;
void dump() {
    if (!g_iq) return;
    const UINT64 n = g_iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        g_iq->GetMessage(i, nullptr, &len);
        auto* m = static_cast<D3D12_MESSAGE*>(malloc(len));
        g_iq->GetMessage(i, m, &len);
        std::printf("  [debug %d] %s\n", static_cast<int>(m->Severity), m->pDescription);
        free(m);
    }
    g_iq->ClearStoredMessages();
}

const char* kCs = R"(
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
RWStructuredBuffer<float4> o : register(u0);
SamplerState s : register(s0);
[numthreads(1, 1, 1)] void main() {
    o[0] = t0.Load(int3(1, 1, 0));
    o[1] = t1.Load(int3(1, 1, 0));
    o[2] = t2.Load(int3(1, 1, 0));
    o[3] = t1.SampleLevel(s, float2(0.5, 0.5), 0);
}
)";

int main() {
    ID3D12Debug1* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
        dbg->EnableDebugLayer();
        dbg->SetEnableGPUBasedValidation(TRUE);
    }
    ID3D12Device* dev = nullptr;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    dev->QueryInterface(IID_PPV_ARGS(&g_iq));

    // the 2-slice target: slice 0 cleared to 0.25, slice 1 to 0.75
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 2, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* tex = nullptr;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex)));
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ID3D12Resource* buf = nullptr;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf)));
    D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* rb = nullptr;
    CHECK(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));

    ID3D12DescriptorHeap *rtvh = nullptr, *srvh = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh)));
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvh)));
    const UINT ri = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT si = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto rtv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvh->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * ri; return h; };
    auto srv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = srvh->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * si; return h; };

    std::printf("RTV 0: TEXTURE2D on the 2-slice texture (the game's own RTV, unchanged)\n");
    D3D12_RENDER_TARGET_VIEW_DESC rv{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2D};
    dev->CreateRenderTargetView(tex, &rv, rtv(0));
    dump();
    std::printf("RTV 1: TEXTURE2DARRAY slice 1\n");
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rv.Texture2DArray = {0, 1, 1, 0};
    dev->CreateRenderTargetView(tex, &rv, rtv(1));
    dump();
    std::printf("SRV t0: TEXTURE2D on the 2-slice texture (the game's own SRV, unchanged)\n");
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(tex, &sv, srv(0));
    dump();
    std::printf("SRV t1: TEXTURE2DARRAY first 1, size 1 (eye 2's view), read as Texture2D\n");
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray = {0, 1, 1, 1, 0, 0};
    dev->CreateShaderResourceView(tex, &sv, srv(1));
    dump();
    std::printf("SRV t2: TEXTURE2DARRAY first 0, size 1, read as Texture2D\n");
    sv.Texture2DArray = {0, 1, 0, 1, 0, 0};
    dev->CreateShaderResourceView(tex, &sv, srv(2));
    dump();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
    uv.Buffer = {0, 16, 16, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    dev->CreateUnorderedAccessView(buf, nullptr, &uv, srv(3));

    // root signature: a table t0-t2, u0; a static sampler
    D3D12_DESCRIPTOR_RANGE rg[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER rpm{};
    rpm.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rpm.DescriptorTable = {2, rg};
    D3D12_STATIC_SAMPLER_DESC ss{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
    ss.MaxLOD = 1000;
    D3D12_ROOT_SIGNATURE_DESC rsd{1, &rpm, 1, &ss};
    ID3DBlob *rsb = nullptr, *err = nullptr, *csb = nullptr;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err));
    ID3D12RootSignature* rs = nullptr;
    CHECK(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)));
    if (FAILED(D3DCompile(kCs, strlen(kCs), "cs", nullptr, nullptr, "main", "cs_5_1", 0, 0, &csb, &err))) {
        std::printf("compile: %s\n", static_cast<const char*>(err->GetBufferPointer()));
        return 1;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS = {csb->GetBufferPointer(), csb->GetBufferSize()};
    ID3D12PipelineState* pso = nullptr;
    CHECK(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* q = nullptr;
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ID3D12CommandAllocator* ca = nullptr;
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)));
    ID3D12GraphicsCommandList* cl = nullptr;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl)));
    const float c0[4] = {0.25f, 0.25f, 0.25f, 1}, c1[4] = {0.75f, 0.75f, 0.75f, 1};
    cl->ClearRenderTargetView(rtv(0), c0, 0, nullptr);  // the flat RTV: slice 0 only?
    cl->ClearRenderTargetView(rtv(1), c1, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = {tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    cl->ResourceBarrier(1, &b);
    cl->SetComputeRootSignature(rs);
    cl->SetDescriptorHeaps(1, &srvh);
    cl->SetComputeRootDescriptorTable(0, srvh->GetGPUDescriptorHandleForHeapStart());
    cl->SetPipelineState(pso);
    cl->Dispatch(1, 1, 1);
    b.Transition = {buf, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(rb, 0, buf, 0, 64);
    CHECK(cl->Close());
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    ID3D12Fence* f = nullptr;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    q->Signal(f, 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    f->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 5000);
    std::printf("after the dispatch:\n");
    dump();
    float* m = nullptr;
    CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&m)));
    std::printf("t0 (flat SRV)            %.3f  (slice 0 is 0.25)\n", m[0]);
    std::printf("t1 (array SRV, slice 1)  %.3f  (slice 1 is 0.75)\n", m[4]);
    std::printf("t2 (array SRV, slice 0)  %.3f\n", m[8]);
    std::printf("t1 sampled               %.3f\n", m[12]);
    std::printf("device removed: %#lx\n", static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
    return 0;
}
