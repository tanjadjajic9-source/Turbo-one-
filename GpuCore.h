#pragma once
// The graphics-chip plumbing, and nothing else: a compute queue of its own, one root signature for every program, buffers with
// tracked states and automatic barriers, short submissions, and safe handling of a chip that stops answering. It knows nothing about
// strokes or pictures (that is Converter.h). The rules here are the ones the old training code proved on the console:
//   * a submission holds at most about 1.5 billion operations (and at most kSubmitMaxDispatches launches), so the console's watchdog
//     never sees a long job;
//   * a launch never binds the same buffer as input and as output;
//   * ONE piece of code (Dispatch) tracks every buffer's state and inserts every barrier;
//   * every root slot is always bound, and launches over 65,535 groups on a side are refused;
//   * the 26 settings numbers match field for field between StrokeConstants here and cbuffer StrokeConstants in Strokes.hlsl;
//   * a chip that never answers has its buffers parked (kept alive) instead of freed under it; after two hangs it stays off until restart.

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace XboxOTFM {

// 26 DWORDs. Must match `cbuffer StrokeConstants` in Strokes.hlsl field for field.
struct StrokeConstants {
    uint32_t gFirst = 0;
    uint32_t gBatch = 0;
    uint32_t gSize = 0;
    uint32_t gCands = 0;
    uint32_t gSlices = 1;
    uint32_t gStroke = 0;
    uint32_t gSeed = 0;
    uint32_t gRound = 0;
    uint32_t gMode = 0;
    uint32_t gCardFloats = 0;
    uint32_t gStrokeEnd = 0;
    uint32_t gOutSize = 0;
    uint32_t gRowFirst = 0;
    uint32_t gRowCount = 0;
    uint32_t gTilesPerRow = 0;
    float gSizeLo = 0.0f;
    float gSizeHi = 0.0f;
    float gAspectLo = 0.0f;
    float gAspectHi = 0.0f;
    float gSoftLo = 0.0f;
    float gSoftHi = 0.0f;
    float gOpacityLo = 0.0f;
    float gOpacityHi = 0.0f;
    float gSigma = 0.0f;
    float gEdgeFollow = 0.0f;
    float gStepScale = 0.0f;
};
static_assert(sizeof(StrokeConstants) == 26 * sizeof(uint32_t), "StrokeConstants must be exactly 26 DWORDs (it is sent as root constants)");

// A D3D12 buffer plus the resource state this code believes it is in.
struct GpuBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint64_t bytes = 0;
    bool fixedState = false; // UPLOAD / READBACK heaps never change state
    bool uavDirty = false;   // written by a compute shader since the last UAV barrier
};

// Order must match kKernelFiles in GpuCore.cpp (and the program order in Strokes.hlsl's header comments).
enum class GpuKernel : uint32_t {
    Blur = 0,
    Background,
    EdgeMap,
    ErrorMap,
    Scout,
    Evaluate,
    Pick,
    Nudge,
    Commit,
    PaintCard,
    Measure,
    Count
};
constexpr uint32_t kGpuKernelCount = static_cast<uint32_t>(GpuKernel::Count);
const char* GpuKernelName(GpuKernel k);

struct GpuBinding {
    GpuBuffer* buf = nullptr;
    uint64_t byteOffset = 0;
};

struct GpuDispatch {
    GpuKernel kernel = GpuKernel::Blur;
    StrokeConstants cb;
    GpuBinding srv[4];
    GpuBinding uav[3];
    uint32_t gx = 1;
    uint32_t gy = 1;
    uint32_t gz = 1;
    double work = 0.0; // approximate arithmetic operations, used to split long jobs into several submissions
};

class GpuCore {
public:
    static constexpr uint32_t kConstantDwords = 26;
    static constexpr double kSubmitWorkThreshold = 1.5e9;   // operations recorded before a submit
    static constexpr uint32_t kSubmitMaxDispatches = 1500;  // launches recorded before a submit (tiny launches are slow for another reason)
    static constexpr uint32_t kMaxGpuWaitMs = 15000;        // a chip that does not answer within 15 s is declared hung
    static constexpr uint32_t kMaxDispatchDim = 65535;      // D3D12 limit per dispatch dimension
    static constexpr uint32_t kLinearGroupsPerRow = 32768;  // must match kLinearRowThreads / 64 in Strokes.hlsl
    static constexpr uint32_t kMaxParkedSets = 2;

    GpuCore();
    ~GpuCore();
    GpuCore(const GpuCore&) = delete;
    GpuCore& operator=(const GpuCore&) = delete;

    // Creates the compute queue, root signature and one pipeline per program (from the package's Shaders folder, or the copy built into
    // the app). Everything that goes wrong is written to the log in ONE go. Safe to call again (releases first).
    HRESULT Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue);
    void Release();
    bool Ready() const { return m_ready && !m_faulted; }
    bool Faulted() const { return m_faulted; }
    ID3D12Device* Device() const { return m_device.Get(); }

    // Buffers (device memory, readable and writable by the programs). A fresh buffer is zero-filled. Destroy them before Release().
    HRESULT CreateBuffer(uint64_t bytes, GpuBuffer& out);
    void DestroyBuffer(GpuBuffer& buf);
    uint64_t BytesInUse() const { return m_bytesInUse; }

    // Recording and submitting programs. Dispatch() starts a recording by itself when there is none, and submits by itself whenever
    // enough work has been recorded. FinishAndWait() submits what is recorded and waits until the chip is done.
    HRESULT Dispatch(const GpuDispatch& d);
    HRESULT FinishAndWait();
    void Abort();
    bool Recording() const { return m_recording; }

    // Moves numbers between the processor and a buffer. Both finish and wait for everything recorded so far first, then do the copy and
    // wait for it (big transfers go through the staging buffers in pieces).
    HRESULT Upload(GpuBuffer& dst, uint64_t dstOffsetBytes, const void* data, uint64_t bytes);
    HRESULT Download(GpuBuffer& src, uint64_t srcOffsetBytes, void* out, uint64_t bytes);

    // Lets the developer tests free everything that was parked after a simulated hang.
    static void ReleaseParkedObjectsForTests();
    static uint32_t ParkedSetCount();

    // Helpers for the launch grids.
    static void SetImageGrid(GpuDispatch& d, uint32_t width, uint32_t height, uint32_t pictures);     // 8 x 8 threads, depth = pictures
    static void SetLinearGrid(GpuDispatch& d, uint64_t threads);                                      // 64 threads per group, folded into 2-D

    std::string Report() const { return m_report; }

private:
    HRESULT CreateRaw(uint64_t bytes, D3D12_HEAP_TYPE heapType, GpuBuffer& out);
    HRESULT BeginLocked();
    HRESULT Flush(bool wait);
    HRESULT WaitQueue();
    HRESULT EnsureStaging(uint64_t bytes);
    void Park();
    void ReleaseAll();
    HRESULT NoteFailure(const char* stage, HRESULT hr);
    void TransitionTo(GpuBuffer& buf, D3D12_RESOURCE_STATES state);

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_graphicsQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    void* m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    D3D12_COMMAND_LIST_TYPE m_listType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso[kGpuKernelCount];

    GpuBuffer m_upload;
    GpuBuffer m_readback;
    uint64_t m_stagingBytes = 0;

    std::vector<ID3D12Resource*> m_tracked; // one extra reference to every buffer made here (so a hang can park them)
    uint64_t m_bytesInUse = 0;

    bool m_ready = false;
    bool m_faulted = false;
    bool m_recording = false;
    bool m_inFlight = false;
    double m_pendingWork = 0.0;
    uint32_t m_pendingDispatches = 0;
    uint32_t m_failureLogCount = 0;
    std::string m_report;
};

} // namespace XboxOTFM
