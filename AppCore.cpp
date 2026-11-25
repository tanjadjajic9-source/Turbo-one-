// The C file functions used below (fopen and friends) are fine to use here: every result is checked. Microsoft's compiler flags
// them as "unsafe" and, in an Xbox app, treats that as an error, so the warning is switched off before any header is read.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "AppCore.h"

#include "StrokeCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace XboxOTFM {

namespace {

constexpr size_t kMaxToasts = 80;
constexpr size_t kMaxLogLines = 400;
constexpr size_t kMaxPreviews = 32;
constexpr uint64_t kChipBudgetBytes = 512ull * 1024 * 1024;   // chip memory one group of pictures may use
constexpr uint32_t kMaxGroup = 32;
constexpr int64_t kLiveWindowMs = 6000;                       // the converter reads canvases for this long after the last call of NoteLiveInterest
constexpr int64_t kLiveEveryMs = 900;

using Clock = std::chrono::steady_clock;

uint64_t NowUnix() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

int64_t NowMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

double SecondsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileAtomically(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::error_code ec2;
            std::filesystem::remove(path, ec2);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
        ok = !ec;
    }
    if (!ok) {
        std::error_code ec3;
        std::filesystem::remove(tmp, ec3);
    }
    return ok;
}

const char* StateName(ConvertState s) {
    switch (s) {
        case ConvertState::Converting: return "converting";
        case ConvertState::Stopping: return "stopping";
        case ConvertState::Done: return "done";
        case ConvertState::Problem: return "problem";
        default: return "idle";
    }
}

const char* GpuStateName(GpuState s) {
    switch (s) {
        case GpuState::Ready: return "ready";
        case GpuState::Failed: return "failed";
        default: return "none";
    }
}

const char* FlightName(uint32_t flight) {
    switch (flight) {
        case 0: return "Wash";
        case 1: return "Form";
        default: return "Detail";
    }
}

std::string HexText(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::string DurationText(double seconds) {
    char buf[64];
    if (seconds < 0.0) return "-";
    if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), seconds < 10.0 ? "%.1f s" : "%.0f s", seconds);
        return buf;
    }
    const int total = static_cast<int>(seconds + 0.5);
    if (total < 3600) {
        std::snprintf(buf, sizeof(buf), "%d min %d s", total / 60, total % 60);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%d h %d min", total / 3600, (total / 60) % 60);
    return buf;
}

std::string CountText(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

CardKey KeyFor(uint64_t pictureHash, const ConvertParams& cp) {
    CardKey k;
    k.pictureHash = pictureHash;
    k.strokes = cp.strokes;
    k.fit = cp.fit;
    k.seed = cp.seed;
    k.candidates = cp.candidates;
    k.rounds = cp.rounds;
    k.engine = kCardEngineVersion;
    return k;
}

// The codes Windows uses when it takes the graphics device away or resets it (DEVICE_REMOVED, DEVICE_HUNG, DEVICE_RESET, DRIVER_INTERNAL_ERROR).
bool IsDeviceLostCode(HRESULT hr) {
    const uint32_t u = static_cast<uint32_t>(hr);
    return u == 0x887A0005u || u == 0x887A0006u || u == 0x887A0007u || u == 0x887A0020u;
}

// The first line of a self-test report that says FAILED (for the one-line note).
std::string FirstFailure(const std::string& report) {
    std::istringstream in(report);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("FAILED") != std::string::npos) return line;
    }
    return std::string();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
const char* ResizeStyleName(ResizeStyle style) {
    switch (style) {
        case ResizeStyle::Sharp: return "sharp";
        case ResizeStyle::Smooth: return "smooth";
        default: return "auto";
    }
}

void WriteSettingsJson(JsonWriter& w, const AppSettings& s) {
    w.BeginObject();
    w.Field("preset", s.preset);
    w.Field("seed", s.seed);
    w.Field("screenSleep", s.screenSleep);
    w.Field("skipDone", s.skipDone);
    w.Field("candidates", s.candidates);
    w.Field("rounds", s.rounds);
    w.Field("resize", ResizeStyleName(s.resize));
    w.EndObject();
}

bool ApplySettingsPatch(AppSettings& s, const JsonValue& patch, std::vector<std::string>* changed) {
    if (!patch.IsObject()) return false;
    auto note = [&](const char* key) { if (changed) changed->push_back(key); };
    auto whole = [](double v, double lo, double hi) { return static_cast<uint32_t>(std::clamp(std::floor(v + 0.5), lo, hi)); };
    double n = 0.0;
    bool b = false;
    std::string text;
    if (patch.GetNumber("preset", n)) {
        const uint32_t v = whole(n, 0, 3);
        if (v != s.preset) { s.preset = v; note("preset"); }
    }
    if (patch.GetNumber("seed", n)) {
        const uint32_t v = whole(n, 0, 999999);
        if (v != s.seed) { s.seed = v; note("seed"); }
    }
    if (patch.GetNumber("candidates", n)) {
        const uint32_t v = whole(n, 8, 256);
        if (v != s.candidates) { s.candidates = v; note("candidates"); }
    }
    if (patch.GetNumber("rounds", n)) {
        const uint32_t v = whole(n, 0, 8);
        if (v != s.rounds) { s.rounds = v; note("rounds"); }
    }
    if (patch.GetBool("screenSleep", b) && b != s.screenSleep) { s.screenSleep = b; note("screenSleep"); }
    if (patch.GetBool("skipDone", b) && b != s.skipDone) { s.skipDone = b; note("skipDone"); }
    if (patch.GetString("resize", text)) {
        ResizeStyle v = s.resize;
        if (text == "auto") v = ResizeStyle::Auto;
        else if (text == "sharp") v = ResizeStyle::Sharp;
        else if (text == "smooth") v = ResizeStyle::Smooth;
        if (v != s.resize) { s.resize = v; note("resize"); }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The chip guard: stops a running conversion and keeps the chip for whoever needs it
// ---------------------------------------------------------------------------------------------
class AppCore::ChipGuard {
public:
    explicit ChipGuard(AppCore& core) : m_core(core) {
        m_core.m_swapping.store(true);
        m_core.m_stopConvert.store(true);                    // the worker notices within a fraction of a second
        m_lock = std::unique_lock<std::mutex>(m_core.m_engineMutex);
        m_core.m_stopConvert.store(false);
    }
    ~ChipGuard() {
        if (m_lock.owns_lock()) m_lock.unlock();
        m_core.m_swapping.store(false);
    }
    ChipGuard(const ChipGuard&) = delete;
    ChipGuard& operator=(const ChipGuard&) = delete;
private:
    AppCore& m_core;
    std::unique_lock<std::mutex> m_lock;
};

// ---------------------------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------------------------
AppCore::AppCore(std::string dataRoot, std::string appVersion, const DatasetOptions& pictureOptions)
    : m_root(std::move(dataRoot))
    , m_version(std::move(appVersion))
    , m_dataset(m_root + "/pictures", pictureOptions)
    , m_cards(m_root + "/cards")
    , m_conv(m_gpu) {
}

AppCore::~AppCore() {
    Stop();
}

void AppCore::Start() {
    if (m_started.exchange(true)) return;
    std::error_code ec;
    std::filesystem::create_directories(m_root, ec);

    std::vector<std::string> warnings;
    m_dataset.Open(&warnings);
    for (const std::string& w : warnings) {
        AppendLog("Pictures: " + w);
        PushToast("warn", w);
    }
    std::string err;
    if (!m_cards.Init(&err)) {
        AppendLog("Cards folder problem: " + err);
        PushToast("warn", "Converted pictures can't be saved right now: " + err);
    }
    AppendLog("App " + m_version + " started. " + std::to_string(m_dataset.Size()) + " saved picture(s) and " + std::to_string(m_cards.Count()) +
              " saved card(s) found.");
    LoadSettingsFile();
    m_worker = std::thread(&AppCore::WorkerMain, this);
}

void AppCore::Stop() {
    if (!m_started.load() || m_stop.load()) {
        if (m_worker.joinable()) m_worker.join();
        return;
    }
    m_stopConvert.store(true);
    m_stop.store(true);
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
    }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    SaveSettingsFileIfDirty(true);
}

void AppCore::PrepareForSuspend(int maxMs) {
    if (!m_started.load() || m_stop.load()) return;
    if (IsConverting()) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_resumeAfter = true;
        }
        m_stopConvert.store(true);
        const auto deadline = Clock::now() + std::chrono::milliseconds(maxMs);
        while (IsConverting() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        AppendLog("[App] Converting was paused because the app is being put to sleep. Finished cards are saved.");
    }
    SaveSettingsFileIfDirty(true);
}

bool AppCore::Resume() {
    ConvertRequest req;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_resumeAfter || !m_haveLastRequest) return false;
        m_resumeAfter = false;
        req = m_lastRequest;
    }
    req.resume = true;
    std::string err;
    if (!StartConvert(req, &err)) {
        AppendLog("[App] Could not carry on converting: " + err);
        PushToast("warn", "Could not carry on converting: " + err);
        return false;
    }
    PushToast("info", "Welcome back. Converting carries on with the pictures that are left.");
    return true;
}

bool AppCore::ResumeAfterSuspend() {
    return Resume();
}

void AppCore::WaitIdleForTests(int maxMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(maxMs);
    while (Clock::now() < deadline) {
        bool pending;
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            pending = m_haveRequest;
        }
        if (!pending && !IsConverting()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void AppCore::SetConvertOverrideForTests(const ConvertParams* p) {
    std::lock_guard<std::mutex> lock(m_testMutex);
    m_haveOverride = p != nullptr;
    if (p) m_override = *p;
}

// ---------------------------------------------------------------------------------------------
// Graphics chip
// ---------------------------------------------------------------------------------------------
void AppCore::SetGpuState(GpuState state, const std::string& note, const std::string& report) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_gpuState = state;
    m_gpuNote = note;
    m_gpuReport = report;
}

GpuState AppCore::GetGpuState() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_gpuState;
}

bool AppCore::AttachGpu(ID3D12Device* device, ID3D12CommandQueue* queue) {
    bool ready = false;
    {
        ChipGuard guard(*this);
        m_gpuLost.store(false);
        m_conv.Free();
        m_gpu.Release();
        SetGpuState(GpuState::NotStarted, "The graphics chip is starting...", "");
        const HRESULT hr = m_gpu.Initialize(device, queue);
        const std::string gpuReport = m_gpu.Report();
        if (FAILED(hr) || !m_gpu.Ready()) {
            const std::string note = "The graphics chip could not be started (error " + HexText(static_cast<uint32_t>(hr)) +
                                     "). Converting needs it. The activity log has the details.";
            SetGpuState(GpuState::Failed, note, gpuReport);
            AppendLog("[Chip] " + note);
            if (!gpuReport.empty()) AppendLog("[Chip] " + gpuReport);
            PushToast("error", "The graphics chip could not be started, so pictures can't be converted. See Settings > About.");
        } else {
            std::string report;
            const HRESULT st = m_conv.SelfTest(report);
            std::istringstream in(report);
            std::string line;
            while (std::getline(in, line)) AppendLog("[Chip] " + line);
            if (SUCCEEDED(st)) {
                SetGpuState(GpuState::Ready, "The graphics chip is ready. It passed all five self-test checks.", report);
                AppendLog("[Chip] The graphics chip passed its self-test.");
                ready = true;
            } else {
                const std::string why = FirstFailure(report);
                const std::string note = "The graphics chip failed its self-test, so it is not trusted and nothing is converted. " + why;
                SetGpuState(GpuState::Failed, note, report);
                AppendLog("[Chip] " + note);
                PushToast("error", "The graphics chip failed its self-test, so pictures can't be converted. See Settings > About.");
                m_conv.Free();
            }
        }
    }
    if (ready) Resume();   // a conversion that was cut short by a chip reset carries on
    return ready;
}

void AppCore::DetachGpu(bool deviceLost) {
    {
        if (IsConverting()) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_resumeAfter = true;
        }
        ChipGuard guard(*this);
        m_gpuLost.store(false);
        m_conv.Free();
        m_gpu.Release();
        SetGpuState(GpuState::NotStarted, deviceLost ? "The graphics chip was reset by the system; restarting it..." : "The graphics chip is switched off.", "");
    }
    AppendLog(deviceLost ? "[Chip] Graphics device lost: recovering." : "[Chip] Graphics chip released.");
}

void AppCore::MarkGraphicsUnavailable(const std::string& why) {
    {
        if (IsConverting()) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_resumeAfter = true;
        }
        ChipGuard guard(*this);
        m_gpuLost.store(false);
        m_conv.Free();
        m_gpu.Release();
        SetGpuState(GpuState::Failed, why, "");
    }
    AppendLog("[Chip] " + why);
    PushToast("error", why);
}

bool AppCore::RunSelfTestAgain(std::string* report, std::string* error) {
    if (IsConverting()) {
        if (error) *error = "Stop converting first. The self-test needs the whole chip.";
        return false;
    }
    if (!m_gpu.Ready()) {
        if (error) *error = "The graphics chip is not running, so there is nothing to test.";
        return false;
    }
    bool ok = false;
    std::string rep;
    {
        ChipGuard guard(*this);
        const HRESULT st = m_conv.SelfTest(rep);
        ok = SUCCEEDED(st);
        if (ok) {
            SetGpuState(GpuState::Ready, "The graphics chip is ready. It passed all five self-test checks.", rep);
        } else {
            SetGpuState(GpuState::Failed, "The graphics chip failed its self-test, so it is not trusted and nothing is converted. " + FirstFailure(rep), rep);
            m_conv.Free();
        }
    }
    std::istringstream in(rep);
    std::string line;
    while (std::getline(in, line)) AppendLog("[Chip] " + line);
    AppendLog(ok ? "[Chip] The self-test passed." : "[Chip] The self-test FAILED.");
    if (report) *report = rep;
    if (!ok && error) *error = "The graphics chip failed its self-test.";
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
AppSettings AppCore::GetSettings() const {
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    return m_settings;
}

std::string AppCore::SettingsJson() const {
    JsonWriter w;
    WriteSettingsJson(w, GetSettings());
    return w.Take();
}

bool AppCore::PatchSettings(const JsonValue& patch, std::string* error, std::vector<std::string>* changedOut) {
    std::vector<std::string> changed;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        if (!ApplySettingsPatch(m_settings, patch, &changed)) {
            if (error) *error = "The settings must be sent as a JSON object.";
            return false;
        }
        if (!changed.empty()) {
            m_settingsDirty = true;
            m_settingsDirtySince = Clock::now();
        }
    }
    if (!changed.empty()) {
        std::string list;
        for (const std::string& k : changed) list += (list.empty() ? "" : ", ") + k;
        AppendLog("Settings changed: " + list);
    }
    if (changedOut) *changedOut = std::move(changed);
    return true;
}

void AppCore::SetNumberSetting(const char* key, double value) {
    JsonValue patch;
    patch.type = JsonValue::Type::Object;
    JsonValue n;
    n.type = JsonValue::Type::Number;
    n.number = value;
    patch.members[key] = n;
    PatchSettings(patch, nullptr, nullptr);
}

void AppCore::SetBoolSetting(const char* key, bool value) {
    JsonValue patch;
    patch.type = JsonValue::Type::Object;
    JsonValue b;
    b.type = JsonValue::Type::Bool;
    b.boolean = value;
    patch.members[key] = b;
    PatchSettings(patch, nullptr, nullptr);
}

void AppCore::SetTextSetting(const char* key, const char* value) {
    JsonValue patch;
    patch.type = JsonValue::Type::Object;
    JsonValue t;
    t.type = JsonValue::Type::String;
    t.text = value ? value : "";
    patch.members[key] = t;
    PatchSettings(patch, nullptr, nullptr);
}

void AppCore::ResetSettingsToRecommended() {
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        m_settings = AppSettings();
        m_settingsDirty = true;
        m_settingsDirtySince = Clock::now();
    }
    PushToast("info", "Settings were set back to the recommended values.");
    AppendLog("Settings reset to the recommended values.");
}

void AppCore::LoadSettingsFile() {
    std::string text;
    if (!ReadWholeFile(m_root + "/settings.json", text)) return;
    JsonValue v;
    std::string err;
    if (!JsonParser::Parse(text, v, &err) || !v.IsObject()) {
        AppendLog("settings.json could not be read (" + err + "); using the recommended settings.");
        return;
    }
    const JsonValue* s = v.Find("settings");   // an older version of the app wrote "config" instead: those settings mean something else and are ignored
    if (!s) return;
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    ApplySettingsPatch(m_settings, *s, nullptr);
}

void AppCore::SaveSettingsFileIfDirty(bool force) {
    std::string body;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        if (!m_settingsDirty) return;
        if (!force && Clock::now() - m_settingsDirtySince < std::chrono::seconds(1)) return;
        JsonWriter w;
        w.BeginObject();
        w.Field("v", 2);
        w.Key("settings");
        WriteSettingsJson(w, m_settings);
        w.EndObject();
        body = w.Take();
        m_settingsDirty = false;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_root, ec);
    if (!WriteFileAtomically(m_root + "/settings.json", body)) {
        AppendLog("Could not save settings.json.");
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        m_settingsDirty = true;
        m_settingsDirtySince = Clock::now();
    }
}

// ---------------------------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------------------------
bool AppCore::IsConverting() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state == ConvertState::Converting || m_state == ConvertState::Stopping;
}

bool AppCore::StartConvert(const ConvertRequest& request, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    if (!m_started.load() || m_stop.load()) return fail("The app is shutting down.");
    if (m_swapping.load()) return fail("The graphics chip is restarting. Try again in a moment.");
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_gpuState != GpuState::Ready) return fail(m_gpuNote.empty() ? std::string("The graphics chip is not ready.") : m_gpuNote);
    }
    if (m_dataset.Size() == 0) return fail("There are no pictures yet. Add some from your phone first.");

    const AppSettings s = GetSettings();
    PresetInfo pi = PresetFor(static_cast<DetailPreset>(std::min(s.preset, 3u)));
    uint32_t strokes = pi.strokes, fit = pi.fit;
    {
        std::lock_guard<std::mutex> lock(m_testMutex);
        if (m_haveOverride) { strokes = m_override.strokes; fit = m_override.fit; }
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_state == ConvertState::Converting || m_state == ConvertState::Stopping) return fail("Pictures are being converted already.");
        m_state = ConvertState::Converting;
        m_stateText = "Starting...";
        m_problem.clear();
        m_run = RunStatus();
        m_run.preset = s.preset;
        m_run.strokes = strokes;
        m_run.fit = fit;
        m_lastRequest = request;
        m_lastRequest.resume = false;
        m_haveLastRequest = true;
        m_resumeAfter = false;
    }
    m_stopConvert.store(false);
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_request = request;
        m_requestSettings = s;
        m_haveRequest = true;
    }
    m_jobCv.notify_all();
    return true;
}

void AppCore::StopConvert() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_state != ConvertState::Converting) return;
        m_state = ConvertState::Stopping;
        m_stateText = "Stopping...";
    }
    m_stopConvert.store(true);
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
    }
    m_jobCv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------------
void AppCore::WorkerMain() {
    while (!m_stop.load()) {
        bool have = false;
        ConvertRequest request;
        AppSettings settings;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait_for(lock, std::chrono::milliseconds(200), [this]() { return m_stop.load() || m_haveRequest; });
            if (m_haveRequest) {
                have = true;
                request = std::move(m_request);
                settings = m_requestSettings;
                m_haveRequest = false;
            }
        }
        if (have) {
            if (m_stop.load()) {
                FinishRun(ConvertState::Idle, "Stopped", "");
                break;
            }
            try {
                RunConvert(request, settings);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> engine(m_engineMutex);   // RunConvert has let go of the chip; nobody else may use it meanwhile
                MarkProblem(std::string("An unexpected error stopped the conversion: ") + ex.what());
            } catch (...) {
                std::lock_guard<std::mutex> engine(m_engineMutex);
                MarkProblem("An unexpected error stopped the conversion.");
            }
        }
        ReportLostPictures();
        SaveSettingsFileIfDirty(false);
    }
}

void AppCore::ReportLostPictures() {
    const size_t lost = m_dataset.TakeUnreadableCount();
    if (lost == 0) return;
    const std::string text = std::to_string(lost) + (lost == 1 ? " picture" : " pictures") +
                             " could not be read back from the Xbox's storage and " + (lost == 1 ? "was" : "were") +
                             " taken out of the set.";
    AppendLog("Pictures: " + text);
    PushToast("warn", text);
}

void AppCore::FinishRun(ConvertState state, const std::string& text, const std::string& problem) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = state;
    m_stateText = text;
    m_problem = problem;
    m_run.etaSeconds = -1.0;
    m_run.strokesPerSecond = 0.0;
}

void AppCore::MarkProblem(const std::string& text) {
    m_conv.Free();
    AppendLog("[Convert] " + text);
    PushToast("error", text);
    FinishRun(ConvertState::Problem, "Needs attention", text);
    PublishLive(nullptr);
}

void AppCore::PublishLive(std::shared_ptr<const LiveView> view) {
    std::lock_guard<std::mutex> lock(m_liveMutex);
    m_live = std::move(view);
}

void AppCore::NoteLiveInterest() {
    m_liveUntilMs.store(NowMs() + kLiveWindowMs);
}

std::shared_ptr<const LiveView> AppCore::GetLive() const {
    std::lock_guard<std::mutex> lock(m_liveMutex);
    return m_live;
}

void AppCore::RunConvert(const ConvertRequest& request, const AppSettings& s) {
    std::lock_guard<std::mutex> engine(m_engineMutex);
    if (m_stopConvert.load() || m_stop.load()) {
        FinishRun(ConvertState::Idle, "Stopped", "");
        return;
    }
    if (!m_gpu.Ready()) {
        MarkProblem("The graphics chip is not ready, so nothing was converted.");
        return;
    }

    // ---- what to make
    const PresetInfo pi = PresetFor(static_cast<DetailPreset>(std::min(s.preset, 3u)));
    ConvertParams cp;
    cp.strokes = pi.strokes;
    cp.fit = pi.fit;
    cp.seed = s.seed;
    cp.candidates = s.candidates;
    cp.rounds = s.rounds;
    cp.chunk = 0;
    {
        std::lock_guard<std::mutex> lock(m_testMutex);
        if (m_haveOverride) {
            cp.strokes = m_override.strokes;
            cp.fit = m_override.fit;
            cp.chunk = m_override.chunk;
        }
    }
    const uint32_t presetIndex = std::min(s.preset, 3u);
    const char* presetName = PresetName(presetIndex);

    // ---- which pictures
    const bool skip = request.resume || (s.skipDone && !request.redo);
    const std::vector<DatasetCardInfo> all = m_dataset.List(0, m_dataset.Size());
    const bool filter = !request.ids.empty();
    const std::set<uint64_t> wanted(request.ids.begin(), request.ids.end());
    std::vector<DatasetCardInfo> todo;
    uint32_t skipped = 0;
    for (const DatasetCardInfo& p : all) {
        if (filter && wanted.find(p.id) == wanted.end()) continue;
        if (skip && m_cards.Has(KeyFor(p.hash, cp))) {
            ++skipped;
            continue;
        }
        todo.push_back(p);
    }
    if (todo.empty()) {
        std::string text;
        if (skipped > 0) {
            text = std::string("Everything is already converted at ") + presetName + " (" + std::to_string(skipped) +
                   (skipped == 1 ? " picture" : " pictures") + "). Pick another detail level, or turn off \"Skip finished pictures\" to do it again.";
        } else {
            text = "There were no pictures to convert.";
        }
        AppendLog("[Convert] " + text);
        PushToast("info", text);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_run.picturesSkipped = skipped;
        }
        FinishRun(ConvertState::Done, "Nothing to do", "");
        return;
    }

    // ---- groups: as few as the chip allows, about the same size each
    const uint32_t perGroupMax = std::max(1u, Converter::MaxPictures(cp, kChipBudgetBytes, kMaxGroup));
    const uint32_t groupCount = static_cast<uint32_t>((todo.size() + perGroupMax - 1) / perGroupMax);
    const uint32_t perGroup = static_cast<uint32_t>((todo.size() + groupCount - 1) / groupCount);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_run.picturesTotal = static_cast<uint32_t>(todo.size());
        m_run.picturesDone = 0;
        m_run.picturesSkipped = skipped;
        m_run.groupCount = groupCount;
        m_run.strokesTotal = cp.strokes;
        m_stateText = "Converting";
    }
    AppendLog(std::string("[Convert] Starting: ") + std::to_string(todo.size()) + " picture(s) at " + presetName + " (" + std::to_string(cp.strokes) +
              " strokes, working size " + std::to_string(cp.fit) + " px, seed " + std::to_string(cp.seed) + ", " + std::to_string(cp.candidates) +
              " candidates, " + std::to_string(cp.rounds) + " tuning rounds), " + std::to_string(groupCount) + " group(s)" +
              (skipped ? ", " + std::to_string(skipped) + " already done" : std::string()) + ".");

    // ---- run
    const auto runStart = Clock::now();
    RunSummary sum;
    sum.preset = presetIndex;
    sum.strokes = cp.strokes;
    sum.fit = cp.fit;
    sum.skipped = skipped;
    double paintSeconds = 0.0, errorSum = 0.0;
    bool stopped = false, problem = false, deviceLost = false;
    std::string problemText;
    uint32_t saveFailures = 0;
    size_t next = 0;
    uint32_t groupIndex = 0;
    int64_t lastLiveMs = 0;

    while (next < todo.size()) {
        if (m_stopConvert.load() || m_stop.load()) {
            stopped = true;
            break;
        }
        ++groupIndex;
        const size_t take = std::min<size_t>(perGroup, todo.size() - next);
        std::vector<PictureIn> inputs;
        std::vector<DatasetCardInfo> infos;
        for (size_t i = 0; i < take; ++i) {
            const DatasetCardInfo& info = todo[next + i];
            PictureIn in;
            if (!m_dataset.GetImage(info.id, in.image, nullptr) || !in.image.Valid()) {
                AppendLog("[Convert] Picture " + std::to_string(info.id) + " could not be read and was skipped.");
                continue;
            }
            in.id = info.id;
            inputs.push_back(std::move(in));
            infos.push_back(info);
        }
        next += take;
        const uint32_t doneBefore = static_cast<uint32_t>(next - take);
        if (inputs.empty()) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_run.picturesDone = static_cast<uint32_t>(next);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_run.groupIndex = groupIndex;
            m_run.groupSize = static_cast<uint32_t>(inputs.size());
            m_run.flight = 0;
            m_run.strokesDone = 0;
            m_run.errorNow = 0.0;
        }

        const size_t remainingAfter = todo.size() - next;
        auto progress = [&](const ConvertProgress& p) -> bool {
            if (m_stopConvert.load() || m_stop.load()) return false;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_run.flight = p.flight;
                m_run.strokesDone = p.strokesDone;
                m_run.strokesTotal = p.strokesTotal;
                m_run.errorNow = p.errorNow;
                m_run.strokesPerSecond = p.strokesPerSecond;
                m_run.seconds = SecondsBetween(runStart, Clock::now());
                const double groupFraction = p.strokesTotal ? static_cast<double>(p.strokesDone) / p.strokesTotal : 0.0;
                m_run.fraction = std::min(1.0, (doneBefore + groupFraction * inputs.size()) / static_cast<double>(todo.size()));
                if (p.strokesPerSecond > 0.0 && p.strokesDone > 0) {
                    const double left = static_cast<double>(p.strokesTotal - p.strokesDone) * inputs.size() +
                                        static_cast<double>(remainingAfter) * p.strokesTotal;
                    m_run.etaSeconds = left / p.strokesPerSecond;
                }
            }
            const int64_t nowMs = NowMs();
            if (nowMs < m_liveUntilMs.load() && nowMs - lastLiveMs >= kLiveEveryMs) {
                lastLiveMs = nowMs;
                const uint32_t idx = static_cast<uint32_t>(static_cast<int64_t>(p.seconds / 3.0) % static_cast<int64_t>(inputs.size()));
                Rgb8Image canvas;
                if (m_conv.ReadCanvas(idx, canvas) && canvas.Valid()) {
                    auto view = std::make_shared<LiveView>();
                    {
                        std::lock_guard<std::mutex> lock(m_liveMutex);
                        view->version = ++m_liveVersion;
                    }
                    view->pictureId = inputs[idx].id;
                    view->name = infos[idx].name;
                    view->index = idx;
                    view->groupSize = static_cast<uint32_t>(inputs.size());
                    view->strokesDone = p.strokesDone;
                    view->strokesTotal = p.strokesTotal;
                    view->error = p.errorNow;
                    view->original = inputs[idx].image;
                    view->painted = std::move(canvas);
                    PublishLive(std::move(view));
                }
            }
            return true;
        };

        std::vector<PictureOut> outs;
        const auto groupStart = Clock::now();
        const HRESULT hr = m_conv.Convert(inputs, cp, outs, progress);
        const double groupSeconds = SecondsBetween(groupStart, Clock::now());
        if (hr == E_ABORT) {
            stopped = true;
            break;
        }
        if (FAILED(hr)) {
            // Windows took the device away? (the error says so, or the device itself says so)
            ID3D12Device* dev = m_gpu.Device();
            if (IsDeviceLostCode(hr) || (dev && FAILED(dev->GetDeviceRemovedReason()))) {
                deviceLost = true;
                break;
            }
        }
        if (FAILED(hr) || outs.size() != inputs.size()) {
            problem = true;
            problemText = "The graphics chip reported an error (" + HexText(static_cast<uint32_t>(hr)) + ") while converting. " +
                          (m_gpu.Faulted() ? "It stopped answering, so it is switched off until the app is restarted. " : "") +
                          "Pictures finished before this are saved.";
            if (m_gpu.Faulted()) SetGpuState(GpuState::Failed, "The graphics chip stopped answering. Restart the app to try again.", m_gpuReport);
            break;
        }
        paintSeconds += groupSeconds;
        const double groupSps = groupSeconds > 0.0 ? static_cast<double>(cp.strokes) * outs.size() / groupSeconds : 0.0;
        for (size_t i = 0; i < outs.size(); ++i) {
            const PictureOut& o = outs[i];
            CardInfo ci;
            ci.key = KeyFor(infos[i].hash, cp);
            ci.name = infos[i].name;
            ci.preset = presetIndex;
            for (int f = 0; f < 3; ++f) ci.perFlight[f] = o.perFlight[f];
            ci.emptyStrokes = o.emptyStrokes;
            ci.errorRms = o.errorRms;
            ci.seconds = groupSeconds;
            ci.strokesPerSecond = groupSps;
            ci.groupSize = static_cast<uint32_t>(outs.size());
            std::string err;
            if (!m_cards.Save(ci, o.dials.data(), o.dials.size(), &err)) {
                ++saveFailures;
                if (saveFailures == 1) {
                    AppendLog("[Convert] A card could not be saved: " + err);
                    PushToast("warn", "A converted picture could not be saved: " + err);
                }
                continue;
            }
            ++sum.pictures;
            sum.emptyStrokes += o.emptyStrokes;
            errorSum += o.errorRms;
            if (sum.pictures == 1) sum.errorBest = sum.errorWorst = o.errorRms;
            sum.errorBest = std::min(sum.errorBest, o.errorRms);
            sum.errorWorst = std::max(sum.errorWorst, o.errorRms);
        }
        {
            char errText[32];
            std::snprintf(errText, sizeof(errText), "%.1f", outs.empty() ? 0.0 : outs[0].errorRms);
            AppendLog("[Convert] Group " + std::to_string(groupIndex) + " of " + std::to_string(groupCount) + ": " + std::to_string(outs.size()) +
                      " picture(s) in " + DurationText(groupSeconds) + ", " + CountText(static_cast<uint64_t>(groupSps)) +
                      " strokes/s, leftover error " + errText + " (first picture).");
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_run.picturesDone = static_cast<uint32_t>(next);
            m_run.fraction = std::min(1.0, static_cast<double>(next) / static_cast<double>(todo.size()));
        }
    }

    // ---- the end
    sum.valid = sum.pictures > 0;
    sum.seconds = paintSeconds;
    sum.strokesPerSecond = paintSeconds > 0.0 ? static_cast<double>(cp.strokes) * sum.pictures / paintSeconds : 0.0;
    sum.secondsPerPicture = sum.pictures > 0 ? paintSeconds / sum.pictures : 0.0;
    sum.errorAverage = sum.pictures > 0 ? errorSum / sum.pictures : 0.0;
    sum.stopped = stopped;
    sum.finishedUnix = NowUnix();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (sum.valid) m_last = sum;
        m_run.seconds = SecondsBetween(runStart, Clock::now());
    }
    PublishLive(nullptr);

    if (deviceLost) {
        // Windows took the graphics device away (the console went to sleep, a driver reset, ...). Nothing is wrong with the pictures: the
        // screen code builds a new device and calls AttachGpu, and the conversion then carries on with the pictures that are left.
        m_conv.Free();
        m_gpuLost.store(true);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_resumeAfter = true;
        }
        SetGpuState(GpuState::NotStarted, "The graphics chip was reset by the system and is being restarted...", "");
        AppendLog("[Chip] The system reset the graphics chip while converting. Pictures finished before this are saved; converting carries on when the chip is back.");
        PushToast("warn", "The graphics chip was reset by the system. Converting carries on as soon as it is back.");
        FinishRun(ConvertState::Idle, "Waiting for the graphics chip", "");
        return;
    }
    if (problem) {
        MarkProblem(problemText);
        return;
    }
    if (stopped) {
        const std::string text = sum.pictures > 0 ? "Stopped. " + std::to_string(sum.pictures) + (sum.pictures == 1 ? " picture is" : " pictures are") + " done and saved."
                                                  : std::string("Stopped. No picture was finished.");
        AppendLog("[Convert] " + text);
        PushToast("info", text);
        FinishRun(ConvertState::Idle, "Stopped", "");
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Converted %u %s at %s in %s. Speed %s strokes/s. Average leftover error %.1f.", sum.pictures,
                  sum.pictures == 1 ? "picture" : "pictures", presetName, DurationText(paintSeconds).c_str(),
                  CountText(static_cast<uint64_t>(sum.strokesPerSecond)).c_str(), sum.errorAverage);
    AppendLog(std::string("[Convert] ") + buf);
    PushToast("good", buf);
    if (saveFailures > 0) PushToast("warn", std::to_string(saveFailures) + " card(s) could not be saved (is the storage full?).");
    FinishRun(ConvertState::Done, "Done", "");
}

// ---------------------------------------------------------------------------------------------
// Pictures and cards
// ---------------------------------------------------------------------------------------------
bool AppCore::PaintedPicture(const std::string& cardId, uint32_t size, Rgb8Image& out) {
    size = std::clamp(size, 16u, 512u);
    CardInfo known;
    if (!IsSafeCardId(cardId) || !m_cards.Get(cardId, known)) return false;   // a card that is gone is not served from the cache either
    const std::string key = cardId + "@" + std::to_string(size);
    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        for (auto it = m_previews.begin(); it != m_previews.end(); ++it) {
            if (it->first == key) {
                out = *it->second;
                return true;
            }
        }
    }
    CardInfo info;
    std::vector<uint8_t> dials;
    std::string err;
    if (!m_cards.Load(cardId, &info, &dials, &err)) return false;
    Rgb8Image img;
    if (!StrokeCodec::PaintCardBytes(dials.data(), dials.size(), size, img) || !img.Valid()) return false;
    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        m_previews.emplace_back(key, std::make_shared<const Rgb8Image>(img));
        while (m_previews.size() > kMaxPreviews) m_previews.pop_front();
    }
    out = std::move(img);
    return true;
}

bool AppCore::OriginalPicture(const CardInfo& card, uint32_t size, Rgb8Image& out) {
    size = std::clamp(size, 16u, 512u);
    DatasetCardInfo di;
    if (!m_dataset.FindByHash(card.key.pictureHash, di)) return false;
    Rgb8Image img;
    if (!m_dataset.GetImage(di.id, img, nullptr) || !img.Valid()) return false;
    if (img.w != size || img.h != size) img = ResizeRgb(img.px.data(), img.w, img.h, size, size);
    if (!img.Valid()) return false;
    out = std::move(img);
    return true;
}

size_t AppCore::DeleteCards(const std::vector<std::string>& cardIds) {
    size_t removed = 0;
    for (const std::string& id : cardIds) {
        if (IsSafeCardId(id) && m_cards.Delete(id)) ++removed;
    }
    if (removed > 0) {
        {
            std::lock_guard<std::mutex> lock(m_previewMutex);
            m_previews.clear();
        }
        AppendLog("[Cards] " + std::to_string(removed) + " saved card(s) were deleted.");
    }
    return removed;
}

size_t AppCore::DeleteAllCards() {
    const size_t n = m_cards.Clear();
    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        m_previews.clear();
    }
    AppendLog("[Cards] All " + std::to_string(n) + " saved card(s) were deleted.");
    return n;
}

// ---------------------------------------------------------------------------------------------
// Reading state
// ---------------------------------------------------------------------------------------------
void AppCore::SetLanUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lanUrl = url;
}

AppSnapshot AppCore::Snapshot() const {
    AppSnapshot s;
    s.version = m_version;
    s.settings = GetSettings();
    s.pictureCount = m_dataset.Size();
    s.pictureCapacity = m_dataset.Capacity();
    s.pictureBytes = m_dataset.DiskBytes();
    s.pictureVersion = m_dataset.Version();
    s.picturesPersistent = m_dataset.Persistent();
    s.cardCount = m_cards.Count();
    s.cardBytes = m_cards.TotalBytes();
    s.cardVersion = m_cards.Version();
    s.cardsPersistent = m_cards.Persistent();
    s.uptimeSeconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_startTime).count());
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        s.lanUrl = m_lanUrl;
        s.gpu = m_gpuState;
        s.gpuNote = m_gpuNote;
        s.gpuReport = m_gpuReport;
        s.state = m_state;
        s.stateText = m_stateText;
        s.problem = m_problem;
        s.runPreset = m_run.preset;
        s.runStrokes = m_run.strokes;
        s.runFit = m_run.fit;
        s.picturesTotal = m_run.picturesTotal;
        s.picturesDone = m_run.picturesDone;
        s.picturesSkipped = m_run.picturesSkipped;
        s.groupIndex = m_run.groupIndex;
        s.groupCount = m_run.groupCount;
        s.groupSize = m_run.groupSize;
        s.flight = m_run.flight;
        s.strokesDone = m_run.strokesDone;
        s.strokesTotal = m_run.strokesTotal;
        s.errorNow = m_run.errorNow;
        s.strokesPerSecond = m_run.strokesPerSecond;
        s.seconds = m_run.seconds;
        s.etaSeconds = m_run.etaSeconds;
        s.fraction = m_run.fraction;
        s.last = m_last;
        s.busyText = m_busyText;
        s.busy = !m_busyText.empty();
    }
    {
        std::lock_guard<std::mutex> lock(m_liveMutex);
        s.liveVersion = m_live ? m_live->version : 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_noticeMutex);
        s.latestToastId = m_nextToastId - 1;
    }
    return s;
}

std::string AppCore::SnapshotJson() const {
    const AppSnapshot s = Snapshot();
    JsonWriter w;
    w.BeginObject();
    w.Field("version", s.version);
    w.Field("uptime", s.uptimeSeconds);
    w.Field("lanUrl", s.lanUrl);
    w.Field("latestToast", s.latestToastId);
    w.Key("gpu").BeginObject().Field("state", GpuStateName(s.gpu)).Field("note", s.gpuNote).Field("report", s.gpuReport).EndObject();
    w.Key("pictures").BeginObject();
    w.Field("count", s.pictureCount);
    w.Field("capacity", s.pictureCapacity);
    w.Field("bytes", s.pictureBytes);
    w.Field("version", s.pictureVersion);
    w.Field("persistent", s.picturesPersistent);
    w.EndObject();
    w.Key("cards").BeginObject();
    w.Field("count", s.cardCount);
    w.Field("bytes", s.cardBytes);
    w.Field("version", s.cardVersion);
    w.Field("persistent", s.cardsPersistent);
    w.EndObject();
    w.Key("settings");
    WriteSettingsJson(w, s.settings);
    w.Key("presets").BeginArray();
    for (uint32_t i = 0; i < 4; ++i) {
        const PresetInfo pi = PresetFor(static_cast<DetailPreset>(i));
        w.BeginObject().Field("name", PresetName(i)).Field("strokes", pi.strokes).Field("fit", pi.fit).EndObject();
    }
    w.EndArray();
    w.Key("convert").BeginObject();
    w.Field("state", StateName(s.state));
    w.Field("text", s.stateText);
    w.Field("problem", s.problem);
    w.Field("preset", s.runPreset);
    w.Field("presetName", PresetName(std::min(s.runPreset, 3u)));
    w.Field("strokes", s.runStrokes);
    w.Field("fit", s.runFit);
    w.Field("total", s.picturesTotal);
    w.Field("done", s.picturesDone);
    w.Field("skipped", s.picturesSkipped);
    w.Field("group", s.groupIndex);
    w.Field("groups", s.groupCount);
    w.Field("groupSize", s.groupSize);
    w.Field("flight", s.flight);
    w.Field("flightName", FlightName(s.flight));
    w.Field("strokesDone", s.strokesDone);
    w.Field("strokesTotal", s.strokesTotal);
    w.Field("error", s.errorNow);
    w.Field("speed", s.strokesPerSecond);
    w.Field("seconds", s.seconds);
    w.Key("eta");
    if (s.etaSeconds >= 0.0) w.Value(s.etaSeconds); else w.Null();
    w.Field("fraction", s.fraction);
    w.Field("liveVersion", s.liveVersion);
    w.EndObject();
    w.Key("last").BeginObject();
    w.Field("valid", s.last.valid);
    w.Field("preset", s.last.preset);
    w.Field("presetName", PresetName(std::min(s.last.preset, 3u)));
    w.Field("strokes", s.last.strokes);
    w.Field("fit", s.last.fit);
    w.Field("pictures", s.last.pictures);
    w.Field("skipped", s.last.skipped);
    w.Field("seconds", s.last.seconds);
    w.Field("speed", s.last.strokesPerSecond);
    w.Field("secondsPerPicture", s.last.secondsPerPicture);
    w.Field("errorAvg", s.last.errorAverage);
    w.Field("errorBest", s.last.errorBest);
    w.Field("errorWorst", s.last.errorWorst);
    w.Field("empty", s.last.emptyStrokes);
    w.Field("stopped", s.last.stopped);
    w.Field("finished", s.last.finishedUnix);
    w.EndObject();
    w.Field("busy", s.busy);
    w.Field("busyText", s.busyText);
    w.EndObject();
    return w.Take();
}

// ---------------------------------------------------------------------------------------------
// Toasts and logs
// ---------------------------------------------------------------------------------------------
void AppCore::PushToast(const std::string& level, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_noticeMutex);
    Toast t;
    t.id = m_nextToastId++;
    t.level = level;
    t.text = text;
    m_toasts.push_back(std::move(t));
    while (m_toasts.size() > kMaxToasts) m_toasts.pop_front();
}

std::vector<Toast> AppCore::ToastsAfter(uint64_t afterId) const {
    std::lock_guard<std::mutex> lock(m_noticeMutex);
    std::vector<Toast> out;
    for (const Toast& t : m_toasts) {
        if (t.id > afterId) out.push_back(t);
    }
    return out;
}

void AppCore::AppendLog(const std::string& line) {
    char stamp[64];
    const uint64_t up = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_startTime).count());
    std::snprintf(stamp, sizeof(stamp), "[%02llu:%02llu:%02llu] ", static_cast<unsigned long long>(up / 3600),
                  static_cast<unsigned long long>((up / 60) % 60), static_cast<unsigned long long>(up % 60));
    const std::string full = std::string(stamp) + line;
    {
        std::lock_guard<std::mutex> lock(m_noticeMutex);
        m_logs.push_back(full);
        while (m_logs.size() > kMaxLogLines) m_logs.pop_front();
    }
    if (m_logSink) {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_logSink(full);
    }
}

std::string AppCore::LogText() const {
    std::string out;
    if (m_extraLog) {
        out += m_extraLog();
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "---- app log ----\n";
    }
    std::lock_guard<std::mutex> lock(m_noticeMutex);
    for (const std::string& l : m_logs) {
        out += l;
        out += '\n';
    }
    return out;
}

} // namespace XboxOTFM
