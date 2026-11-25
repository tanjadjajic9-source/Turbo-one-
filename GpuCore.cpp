#include "GpuCore.h"
#include "WebBridge.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

// The GitHub build also bakes a copy of the compiled GPU programs into the app itself (ShaderBlobs.h, written by build-xbox.yml).
// It is only a safety net: a program is taken from it when it cannot be read from the package's Shaders folder, so the chip path does not
// depend on exactly how the package was assembled. Builds without that header behave exactly as before.
#if __has_include("ShaderBlobs.h")
#include "ShaderBlobs.h"
#define OT_HAS_BUILT_IN_SHADERS 1
#endif

namespace XboxOTFM {

namespace {

struct KernelFile {
    const wchar_t* file;
    const char* label;
};

// Order must match enum class GpuKernel in GpuCore.h.
constexpr KernelFile kKernelFiles[kGpuKernelCount] = {
    { L"CSBlur.cso", "CSBlur" },
    { L"CSBackground.cso", "CSBackground" },
    { L"CSEdgeMap.cso", "CSEdgeMap" },
    { L"CSErrorMap.cso", "CSErrorMap" },
    { L"CSScout.cso", "CSScout" },
    { L"CSEvaluate.cso", "CSEvaluate" },
    { L"CSPick.cso", "CSPick" },
    { L"CSNudge.cso", "CSNudge" },
    { L"CSCommit.cso", "CSCommit" },
    { L"CSPaintCard.cso", "CSPaintCard" },
    { L"CSMeasure.cso", "CSMeasure" },
};

constexpr uint32_t kMaxFailureLogLines = 40; // never flood crash_log.txt
constexpr uint64_t kStagingChunk = 8ull << 20;

// ---- hung-GPU parking lot ------------------------------------------------------------------------
// If the GPU stops answering (timeout) it may still be running our last job. Freeing the buffers under it would be a
// use-after-free ON the GPU, so in that one case everything the GPU could touch is parked here (kept alive) for the rest
// of the process instead of being released. This costs some video memory once per hang, which is why GPU compute is
// switched off for the session after kMaxParkedSets hangs.
std::mutex& ParkMutex() { static std::mutex* m = new std::mutex(); return *m; }
std::vector<IUnknown*>& ParkedObjects() { static std::vector<IUnknown*>* v = new std::vector<IUnknown*>(); return *v; }
std::vector<HANDLE>& ParkedHandles() { static std::vector<HANDLE>* v = new std::vector<HANDLE>(); return *v; }
uint32_t& ParkedSets() { static uint32_t n = 0; return n; }

template <class T>
void ParkPtr(Microsoft::WRL::ComPtr<T>& p) {
    if (!p) return;
    IUnknown* raw = p.Get();
    raw->AddRef();
    ParkedObjects().push_back(raw);
    p.Reset();
}

void GpuLog(const std::string& message) {
    try {
        WebBridge::WriteCrashLogToDisk("[GPU] " + message);
    } catch (...) {
    }
}

std::string HexHr(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buf);
}

std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '?');
    return s;
}

inline uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& outBytes) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    const std::streamoff size = static_cast<std::streamoff>(ifs.tellg());
    if (size <= 0) return false;
    outBytes.resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(size));
    return !ifs.fail();
}

// Looks for Shaders\<filename> inside the installed app package (normal case), then relative to the working folder.
bool LoadShaderBytecode(const wchar_t* filename, std::vector<uint8_t>& outBytes, std::wstring& outTriedPath) {
    outBytes.clear();
    outTriedPath.clear();
    try {
        const auto installPath = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
        outTriedPath = std::wstring(installPath.c_str()) + L"\\Shaders\\" + filename;
        if (ReadWholeFile(outTriedPath, outBytes)) return true;
    } catch (...) {
    }
    const std::wstring relative = std::wstring(L"Shaders\\") + filename;
    if (outTriedPath.empty()) outTriedPath = relative;
    return ReadWholeFile(relative, outBytes);
}

// Copies the copy of a compiled GPU program that was baked into the app at build time (if this build has one).
bool CopyBuiltInShader(const char* label, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
#ifdef OT_HAS_BUILT_IN_SHADERS
    for (const auto& blob : EmbeddedShaders::kBlobs) {
        if (std::strcmp(blob.name, label) == 0 && blob.size >= 4) {
            outBytes.assign(blob.data, blob.data + blob.size);
            return true;
        }
    }
#else
    (void)label;
#endif
    return false;
}

} // namespace

const char* GpuKernelName(GpuKernel k) {
    const uint32_t i = static_cast<uint32_t>(k);
    return i < kGpuKernelCount ? kKernelFiles[i].label : "?";
}

void GpuCore::SetImageGrid(GpuDispatch& d, uint32_t width, uint32_t height, uint32_t pictures) {
    d.gx = (width + 7) / 8;
    d.gy = (height + 7) / 8;
    d.gz = pictures;
}

// 64 threads per group, folded into 2-D so even huge lists stay under the 65535-groups-per-dimension limit.
void GpuCore::SetLinearGrid(GpuDispatch& d, uint64_t threads) {
    const uint64_t groups = std::max<uint64_t>(1, (threads + 63) / 64);
    d.gx = static_cast<uint32_t>(std::min<uint64_t>(groups, kLinearGroupsPerRow));
    d.gy = static_cast<uint32_t>((groups + kLinearGroupsPerRow - 1) / kLinearGroupsPerRow);
    d.gz = 1;
}

uint32_t GpuCore::ParkedSetCount() {
    std::lock_guard<std::mutex> lock(ParkMutex());
    return ParkedSets();
}

void GpuCore::ReleaseParkedObjectsForTests() {
    std::lock_guard<std::mutex> lock(ParkMutex());
    for (IUnknown* p : ParkedObjects()) p->Release();
    ParkedObjects().clear();
    for (HANDLE h : ParkedHandles()) CloseHandle(h);
    ParkedHandles().clear();
    ParkedSets() = 0;
}

GpuCore::GpuCore() {}

GpuCore::~GpuCore() {
    ReleaseAll();
}

// =================================================================================================
// Initialisation / release
// =================================================================================================

HRESULT GpuCore::Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue) {
    ReleaseAll();
    m_report.clear();
    if (!device || !graphicsQueue) {
        GpuLog("Initialize was called without a D3D12 device or command queue.");
        return E_INVALIDARG;
    }
    {
        std::lock_guard<std::mutex> lock(ParkMutex());
        if (ParkedSets() >= kMaxParkedSets) {
            GpuLog("The graphics chip is switched off for this session: it stopped answering " + std::to_string(ParkedSets()) +
                   " times. Restart the app to try again.");
            return E_FAIL;
        }
    }
    GpuLog("---- graphics chip start-up ----");

    auto fail = [&](const std::string& what, HRESULT hr) -> HRESULT {
        GpuLog("FAILED: " + what + " (HRESULT " + HexHr(hr) + ").");
        m_report = what;
        ReleaseAll();
        return FAILED(hr) ? hr : E_FAIL;
    };

    m_device = device;
    m_graphicsQueue = graphicsQueue;
    m_faulted = false;
    m_failureLogCount = 0;

    // Heavy compute must not share the queue that presents the TV picture, or the UI would freeze behind it.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
    if (SUCCEEDED(hr)) {
        m_listType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        GpuLog("using a dedicated compute queue");
    } else {
        m_queue = m_graphicsQueue;
        m_listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        GpuLog("dedicated compute queue unavailable (HRESULT " + HexHr(hr) + "); sharing the graphics queue");
    }

    hr = device->CreateCommandAllocator(m_listType, IID_PPV_ARGS(&m_alloc));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
    hr = device->CreateCommandList(0, m_listType, m_alloc.Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    m_list->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) return fail("CreateFence", hr);
    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) return fail("CreateEvent", E_FAIL);

    // Root signature: [0] 26 x 32-bit constants (cbuffer b0), [1..4] four root SRVs (t0..t3), [5..7] three root UAVs (u0..u2)
    D3D12_ROOT_PARAMETER params[8] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kConstantDwords;
    for (UINT i = 0; i < 4; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1 + i].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0; i < 3; ++i) {
        params[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[5 + i].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 8;
    rsDesc.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errBlob;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr)) return fail("D3D12SerializeRootSignature", hr);
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr)) return fail("CreateRootSignature", hr);

    // One pipeline per program. Every problem is collected so ONE log shows everything that is wrong.
    std::string problems;
    std::string builtInNotes;
    uint32_t builtInCount = 0;
    for (uint32_t k = 0; k < kGpuKernelCount; ++k) {
        std::vector<uint8_t> bytecode;
        std::wstring triedPath;
        const bool fileRead = LoadShaderBytecode(kKernelFiles[k].file, bytecode, triedPath);
        const bool fileValid = fileRead && bytecode.size() >= 4 && std::memcmp(bytecode.data(), "DXBC", 4) == 0;
        if (!fileValid) {
            // Not usable from the Shaders folder: try the copy that is built into the app.
            std::vector<uint8_t> builtIn;
            if (CopyBuiltInShader(kKernelFiles[k].label, builtIn)) {
                builtInNotes += std::string("  ") + kKernelFiles[k].label +
                                (fileRead ? ": the file is not a compiled shader: " : ": file not found: ") + NarrowAscii(triedPath) + "\n";
                bytecode.swap(builtIn);
                ++builtInCount;
            } else if (!fileRead) {
                problems += std::string("  missing shader file: ") + NarrowAscii(triedPath) + "\n";
                continue;
            } else {
                problems += std::string("  not a compiled shader (no DXBC header): ") + NarrowAscii(triedPath) + "\n";
                continue;
            }
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS.pShaderBytecode = bytecode.data();
        psoDesc.CS.BytecodeLength = bytecode.size();
        const HRESULT psoHr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso[k]));
        if (FAILED(psoHr)) {
            problems += std::string("  CreateComputePipelineState failed for ") + kKernelFiles[k].label + " (HRESULT " + HexHr(psoHr) + ")\n";
        }
    }
    if (!problems.empty()) {
        GpuLog("shader problems:\n" + problems);
        return fail("could not load all graphics programs", HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }
    if (builtInCount > 0) {
        GpuLog("these graphics programs were not usable from the package's Shaders folder, so the copy built into the app is used instead:\n" + builtInNotes);
    }
    GpuLog("all " + std::to_string(kGpuKernelCount) + " graphics programs loaded (" + std::to_string(kGpuKernelCount - builtInCount) +
           " from the Shaders folder, " + std::to_string(builtInCount) + " from the copy built into the app)");

    m_ready = true;
    return S_OK;
}

void GpuCore::Release() {
    ReleaseAll();
}

void GpuCore::Park() {
    std::lock_guard<std::mutex> lock(ParkMutex());
    ++ParkedSets();
    GpuLog("the graphics chip never finished its last job: its buffers are kept alive instead of being freed under it (hang " +
           std::to_string(ParkedSets()) + " of " + std::to_string(kMaxParkedSets) + " allowed this session)");
    for (ID3D12Resource* r : m_tracked) ParkedObjects().push_back(r); // the reference held here moves to the parking lot
    m_tracked.clear();
    m_upload = GpuBuffer();
    m_readback = GpuBuffer();
    for (auto& pso : m_pso) ParkPtr(pso);
    ParkPtr(m_rootSig);
    ParkPtr(m_list);
    ParkPtr(m_alloc);
    ParkPtr(m_fence);
    if (m_queue.Get() != m_graphicsQueue.Get()) ParkPtr(m_queue); // the graphics queue belongs to the app
    if (m_fenceEvent) {
        ParkedHandles().push_back(static_cast<HANDLE>(m_fenceEvent)); // a fence may still signal it later
        m_fenceEvent = nullptr;
    }
}

void GpuCore::ReleaseAll() {
    // A chip that stopped answering (but was not removed) may still be running our last job: park, do not free.
    if (m_inFlight && m_faulted && m_device && SUCCEEDED(m_device->GetDeviceRemovedReason())) {
        Park();
    }
    // Let in-flight work finish before freeing what it uses (skipped if the chip is already known to be dead).
    if (m_inFlight && !m_faulted && m_queue && m_fence && m_fenceEvent) {
        WaitQueue();
    }
    if (m_recording && m_list) m_list->Close();

    m_ready = false;
    m_recording = false;
    m_inFlight = false;
    m_pendingWork = 0.0;
    m_pendingDispatches = 0;

    m_upload = GpuBuffer();
    m_readback = GpuBuffer();
    m_stagingBytes = 0;
    for (ID3D12Resource* r : m_tracked) r->Release();
    m_tracked.clear();
    m_bytesInUse = 0;

    for (auto& pso : m_pso) pso.Reset();
    m_rootSig.Reset();
    if (m_fenceEvent) {
        CloseHandle(static_cast<HANDLE>(m_fenceEvent));
        m_fenceEvent = nullptr;
    }
    m_fence.Reset();
    m_list.Reset();
    m_alloc.Reset();
    m_queue.Reset();
    m_graphicsQueue.Reset();
    m_device.Reset();
}

// =================================================================================================
// Buffers
// =================================================================================================

HRESULT GpuCore::CreateRaw(uint64_t byteSize, D3D12_HEAP_TYPE heapType, GpuBuffer& out) {
    out = GpuBuffer();
    if (!m_device) return E_NOT_VALID_STATE;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(256, AlignUp(byteSize, 256));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    bool fixedState = false;
    if (heapType == D3D12_HEAP_TYPE_DEFAULT) {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else if (heapType == D3D12_HEAP_TYPE_UPLOAD) {
        initial = D3D12_RESOURCE_STATE_GENERIC_READ;
        fixedState = true;
    } else {
        initial = D3D12_RESOURCE_STATE_COPY_DEST;
        fixedState = true;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) return hr;

    resource->AddRef(); // the tracking reference (so a hang can park the buffer)
    m_tracked.push_back(resource.Get());
    out.res = resource;
    out.state = initial;
    out.bytes = desc.Width;
    out.fixedState = fixedState;
    out.uavDirty = false;
    return S_OK;
}

HRESULT GpuCore::CreateBuffer(uint64_t bytes, GpuBuffer& out) {
    const HRESULT hr = CreateRaw(bytes, D3D12_HEAP_TYPE_DEFAULT, out);
    if (SUCCEEDED(hr)) m_bytesInUse += out.bytes;
    return hr;
}

void GpuCore::DestroyBuffer(GpuBuffer& buf) {
    if (buf.res) {
        ID3D12Resource* raw = buf.res.Get();
        // A chip that stopped answering may still be running a job that uses this buffer: then the tracking reference stays, and Park()
        // (or ReleaseAll() when the chip turns out to be gone) deals with it. Only on a healthy chip is the buffer really freed here.
        const bool maybeStillInUse = m_inFlight && m_faulted;
        auto it = std::find(m_tracked.begin(), m_tracked.end(), raw);
        if (it != m_tracked.end() && !maybeStillInUse) {
            m_tracked.erase(it);
            raw->Release();
        }
        m_bytesInUse = m_bytesInUse >= buf.bytes ? m_bytesInUse - buf.bytes : 0;
    }
    buf = GpuBuffer();
}

HRESULT GpuCore::EnsureStaging(uint64_t bytes) {
    const uint64_t want = std::min<uint64_t>(std::max<uint64_t>(bytes, 1ull << 16), kStagingChunk);
    if (m_upload.res && m_readback.res && m_stagingBytes >= want) return S_OK;
    // the old staging buffers go away only when the chip is idle (callers wait before they ask for staging)
    DestroyBuffer(m_upload);
    DestroyBuffer(m_readback);
    HRESULT hr = CreateRaw(want, D3D12_HEAP_TYPE_UPLOAD, m_upload);
    if (FAILED(hr)) return hr;
    hr = CreateRaw(want, D3D12_HEAP_TYPE_READBACK, m_readback);
    if (FAILED(hr)) return hr;
    m_stagingBytes = want;
    return S_OK;
}

// =================================================================================================
// Queue + command recording
// =================================================================================================

HRESULT GpuCore::WaitQueue() {
    if (!m_queue || !m_fence || !m_fenceEvent || !m_device) return E_NOT_VALID_STATE;
    HANDLE ev = static_cast<HANDLE>(m_fenceEvent);

    const uint64_t target = ++m_fenceValue;
    HRESULT hr = m_queue->Signal(m_fence.Get(), target);
    if (FAILED(hr)) {
        m_faulted = true;
        return hr;
    }

    uint32_t waitedMs = 0;
    while (m_fence->GetCompletedValue() < target) {
        hr = m_fence->SetEventOnCompletion(target, ev);
        if (FAILED(hr)) return hr;
        const DWORD waitResult = WaitForSingleObject(ev, 250);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_FAILED) return E_FAIL;
        const HRESULT removed = m_device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            m_faulted = true;
            return removed;
        }
        waitedMs += 250;
        if (waitedMs >= kMaxGpuWaitMs) {
            m_faulted = true;
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }

    m_inFlight = false;
    const HRESULT removed = m_device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        m_faulted = true;
        return removed;
    }
    return S_OK;
}

HRESULT GpuCore::BeginLocked() {
    if (!m_alloc || !m_list || !m_rootSig) return E_NOT_VALID_STATE;
    if (m_inFlight) {
        // Never reset an allocator the GPU may still be reading.
        const HRESULT waitHr = WaitQueue();
        if (FAILED(waitHr)) return waitHr;
    }
    if (m_recording) Abort(); // a previous recording was never submitted; start clean

    HRESULT hr = m_alloc->Reset();
    if (FAILED(hr)) return hr;
    hr = m_list->Reset(m_alloc.Get(), nullptr);
    if (FAILED(hr)) return hr;
    m_list->SetComputeRootSignature(m_rootSig.Get());
    m_recording = true;
    m_pendingWork = 0.0;
    m_pendingDispatches = 0;
    return S_OK;
}

HRESULT GpuCore::Flush(bool wait) {
    if (!m_recording) return E_NOT_VALID_STATE;
    const HRESULT closeHr = m_list->Close();
    m_recording = false;
    if (FAILED(closeHr)) return closeHr;
    ID3D12CommandList* lists[] = { m_list.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    m_inFlight = true;
    // A single allocator cannot be reset while the chip still runs its list, so the caller waits here; the next recording starts by itself.
    return wait ? WaitQueue() : S_OK;
}

HRESULT GpuCore::FinishAndWait() {
    if (!m_recording && !m_inFlight) return S_OK;
    if (m_recording) {
        const HRESULT hr = Flush(false);
        if (FAILED(hr)) return NoteFailure("submitting", hr);
    }
    const HRESULT hr = WaitQueue();
    if (FAILED(hr)) return NoteFailure("waiting for the chip", hr);
    return S_OK;
}

void GpuCore::Abort() {
    if (m_recording && m_list) m_list->Close();
    m_recording = false;
}

void GpuCore::TransitionTo(GpuBuffer& buf, D3D12_RESOURCE_STATES state) {
    if (buf.fixedState || buf.state == state) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = buf.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = buf.state;
    b.Transition.StateAfter = state;
    m_list->ResourceBarrier(1, &b);
    buf.state = state;
    buf.uavDirty = false;
}

HRESULT GpuCore::Dispatch(const GpuDispatch& d) {
    if (!m_ready || m_faulted) return E_NOT_VALID_STATE;
    if (!m_recording) {
        const HRESULT beginHr = BeginLocked();
        if (FAILED(beginHr)) return NoteFailure("starting a recording", beginHr);
    }
    if (d.gx == 0 || d.gy == 0 || d.gz == 0) return S_OK;
    if (d.gx > kMaxDispatchDim || d.gy > kMaxDispatchDim || d.gz > kMaxDispatchDim) {
        GpuLog("refused an oversized launch (" + std::to_string(d.gx) + " x " + std::to_string(d.gy) + " x " + std::to_string(d.gz) +
               " groups) for program " + GpuKernelName(d.kernel));
        return E_INVALIDARG;
    }
    const uint32_t kernelIndex = static_cast<uint32_t>(d.kernel);
    if (kernelIndex >= kGpuKernelCount || !m_pso[kernelIndex]) return E_NOT_VALID_STATE;

    // 1) Which state must each distinct buffer be in for this launch?
    struct Need {
        GpuBuffer* buf;
        D3D12_RESOURCE_STATES state;
    };
    Need needs[7];
    uint32_t needCount = 0;
    auto addNeed = [&](GpuBuffer* b, D3D12_RESOURCE_STATES s) -> bool {
        if (!b) return true;
        for (uint32_t i = 0; i < needCount; ++i) {
            if (needs[i].buf == b) return needs[i].state == s; // same buffer, two states in one launch = bug
        }
        needs[needCount].buf = b;
        needs[needCount].state = s;
        ++needCount;
        return true;
    };
    for (const GpuBinding& s : d.srv) {
        if (!addNeed(s.buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
            GpuLog(std::string("refused a launch of ") + GpuKernelName(d.kernel) + ": one buffer is bound as input and as output");
            return E_INVALIDARG;
        }
    }
    for (const GpuBinding& u : d.uav) {
        if (!addNeed(u.buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            GpuLog(std::string("refused a launch of ") + GpuKernelName(d.kernel) + ": one buffer is bound as input and as output");
            return E_INVALIDARG;
        }
    }

    // 2) Transition barriers where the state differs, a UAV barrier where the previous launch wrote the buffer.
    D3D12_RESOURCE_BARRIER barriers[8];
    uint32_t barrierCount = 0;
    for (uint32_t i = 0; i < needCount; ++i) {
        GpuBuffer* b = needs[i].buf;
        D3D12_RESOURCE_BARRIER barrier = {};
        if (b->state != needs[i].state) {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = b->res.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = b->state;
            barrier.Transition.StateAfter = needs[i].state;
            barriers[barrierCount++] = barrier;
            b->state = needs[i].state;
            b->uavDirty = false;
        } else if (needs[i].state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && b->uavDirty) {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = b->res.Get();
            barriers[barrierCount++] = barrier;
            b->uavDirty = false;
        }
    }
    if (barrierCount > 0) m_list->ResourceBarrier(barrierCount, barriers);

    // 3) Bind and launch. Every root slot is given a valid address: a slot this program does not use is pointed at a
    //    buffer that is already bound (so it is already in the right state) and the program never touches it.
    m_list->SetPipelineState(m_pso[kernelIndex].Get());
    m_list->SetComputeRoot32BitConstants(0, kConstantDwords, &d.cb, 0);
    const GpuBinding* firstSrv = nullptr;
    for (const GpuBinding& s : d.srv) {
        if (s.buf) { firstSrv = &s; break; }
    }
    const GpuBinding* firstUav = nullptr;
    for (const GpuBinding& u : d.uav) {
        if (u.buf) { firstUav = &u; break; }
    }
    for (UINT i = 0; i < 4; ++i) {
        const GpuBinding* s = d.srv[i].buf ? &d.srv[i] : firstSrv;
        if (s) m_list->SetComputeRootShaderResourceView(1 + i, s->buf->res->GetGPUVirtualAddress() + s->byteOffset);
    }
    for (UINT i = 0; i < 3; ++i) {
        const GpuBinding* u = d.uav[i].buf ? &d.uav[i] : firstUav;
        if (u) m_list->SetComputeRootUnorderedAccessView(5 + i, u->buf->res->GetGPUVirtualAddress() + u->byteOffset);
    }
    m_list->Dispatch(d.gx, d.gy, d.gz);
    for (const GpuBinding& u : d.uav) {
        if (u.buf) u.buf->uavDirty = true;
    }

    // 4) Keep every submission short: long jobs are cut into several submits so no single one can hit a GPU timeout.
    m_pendingWork += d.work;
    ++m_pendingDispatches;
    if (m_pendingWork >= kSubmitWorkThreshold || m_pendingDispatches >= kSubmitMaxDispatches) {
        const HRESULT hr = Flush(true);
        if (FAILED(hr)) return NoteFailure("submitting", hr);
    }
    return S_OK;
}

HRESULT GpuCore::NoteFailure(const char* stage, HRESULT hr) {
    if (m_device) {
        const HRESULT removed = m_device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            m_faulted = true;
            if (m_failureLogCount < kMaxFailureLogLines) GpuLog("device removed reason " + HexHr(removed));
        }
    }
    if (m_failureLogCount < kMaxFailureLogLines) {
        ++m_failureLogCount;
        GpuLog(std::string("graphics chip step failed while ") + stage + " (HRESULT " + HexHr(hr) + ")");
    }
    return hr;
}

// =================================================================================================
// Moving numbers between the processor and the chip
// =================================================================================================

HRESULT GpuCore::Upload(GpuBuffer& dst, uint64_t dstOffset, const void* data, uint64_t bytes) {
    if (!m_ready || m_faulted) return E_NOT_VALID_STATE;
    if (bytes == 0) return S_OK;
    if (!dst.res || dstOffset + bytes > dst.bytes) return E_INVALIDARG;
    HRESULT hr = FinishAndWait();
    if (FAILED(hr)) return hr;
    hr = EnsureStaging(bytes);
    if (FAILED(hr)) return NoteFailure("making the staging buffers", hr);
    const uint8_t* src = static_cast<const uint8_t*>(data);
    uint64_t done = 0;
    while (done < bytes) {
        const uint64_t n = std::min<uint64_t>(bytes - done, m_stagingBytes);
        uint8_t* mapped = nullptr;
        hr = m_upload.res->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) return NoteFailure("mapping the upload buffer", hr);
        std::memcpy(mapped, src + done, static_cast<size_t>(n));
        m_upload.res->Unmap(0, nullptr);
        hr = BeginLocked();
        if (FAILED(hr)) return NoteFailure("starting an upload", hr);
        TransitionTo(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyBufferRegion(dst.res.Get(), dstOffset + done, m_upload.res.Get(), 0, n);
        hr = FinishAndWait();
        if (FAILED(hr)) return hr;
        done += n;
    }
    return S_OK;
}

HRESULT GpuCore::Download(GpuBuffer& src, uint64_t srcOffset, void* out, uint64_t bytes) {
    if (!m_ready || m_faulted) return E_NOT_VALID_STATE;
    if (bytes == 0) return S_OK;
    if (!src.res || srcOffset + bytes > src.bytes) return E_INVALIDARG;
    HRESULT hr = FinishAndWait();
    if (FAILED(hr)) return hr;
    hr = EnsureStaging(bytes);
    if (FAILED(hr)) return NoteFailure("making the staging buffers", hr);
    uint8_t* dest = static_cast<uint8_t*>(out);
    uint64_t done = 0;
    while (done < bytes) {
        const uint64_t n = std::min<uint64_t>(bytes - done, m_stagingBytes);
        hr = BeginLocked();
        if (FAILED(hr)) return NoteFailure("starting a download", hr);
        TransitionTo(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyBufferRegion(m_readback.res.Get(), 0, src.res.Get(), srcOffset + done, n);
        hr = FinishAndWait();
        if (FAILED(hr)) return hr;
        uint8_t* mapped = nullptr;
        D3D12_RANGE range;
        range.Begin = 0;
        range.End = static_cast<SIZE_T>(n);
        hr = m_readback.res->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) return NoteFailure("mapping the readback buffer", hr);
        std::memcpy(dest + done, mapped, static_cast<size_t>(n));
        D3D12_RANGE emptyRange;
        emptyRange.Begin = 0;
        emptyRange.End = 0;
        m_readback.res->Unmap(0, &emptyRange);
        done += n;
    }
    return S_OK;
}

} // namespace XboxOTFM
