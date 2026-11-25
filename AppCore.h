#pragma once
// The brain of the app: one background worker that converts pictures into cards of dials on the graphics chip, plus the state that the
// phone web page and the TV screen both show. Nothing in here is Windows-specific (so it can be tested anywhere).
//
// What happens when a conversion is started: the pictures that still need a card are cut into groups (as many as fit on the chip at once),
// every group is painted by the Converter, and every finished card is saved at once (StrokeStore), so a stop, a crash or a suspend loses at
// most the group that was being painted. Pictures that already have a card made with the same settings are skipped.

#include "Converter.h"
#include "Dataset.h"
#include "GpuCore.h"
#include "ImageUtil.h"
#include "Json.h"
#include "StrokeStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace XboxOTFM {

enum class ConvertState : uint32_t {
    Idle = 0,        // ready (also after a stop)
    Converting = 1,
    Stopping = 2,    // stop was asked for, the chip is finishing its current step
    Done = 3,        // the last run finished
    Problem = 4      // the last run ended with an error
};

enum class GpuState : uint32_t {
    NotStarted = 0,  // the graphics chip has not been started (yet, or the screen could not be started)
    Ready = 1,       // started and passed its self-test
    Failed = 2       // could not be started, or failed its self-test: no conversion is allowed
};

struct Toast {
    uint64_t id = 0;
    std::string level; // "info", "good", "warn", "error"
    std::string text;
};

struct AppSettings {
    uint32_t preset = 1;          // 0 Low, 1 Medium, 2 High, 3 Ultra (see PresetFor in Converter.h)
    uint32_t seed = 1;            // every random choice of the converter is a hash of this: the same seed gives the same cards
    bool screenSleep = true;      // while converting, the TV stops drawing (the chip then spends everything on painting)
    bool skipDone = true;         // pictures that already have a card made with the same settings are left out
    uint32_t candidates = 64;     // K: candidate strokes tried for every stroke
    uint32_t rounds = 3;          // R: tuning rounds for every stroke
    ResizeStyle resize = ResizeStyle::Auto;   // how small pictures are enlarged when they are added
};
const char* ResizeStyleName(ResizeStyle style);
void WriteSettingsJson(JsonWriter& w, const AppSettings& settings);
// Applies the keys of `patch` that are present (numbers are rounded and kept inside their range, unknown keys are ignored).
// Returns false when `patch` is not an object. `changed` lists the keys whose value really changed.
bool ApplySettingsPatch(AppSettings& settings, const JsonValue& patch, std::vector<std::string>* changed);

struct ConvertRequest {
    std::vector<uint64_t> ids;    // pictures to convert (ids from the picture set); empty = the whole set
    bool redo = false;            // convert pictures that already have a card as well
    bool resume = false;          // carrying on after an interruption: pictures that already have a card are always left out
};

// What the last finished (or stopped) run did.
struct RunSummary {
    bool valid = false;
    uint32_t preset = 0, strokes = 0, fit = 0;
    uint32_t pictures = 0;        // pictures that got a card in the run
    uint32_t skipped = 0;         // pictures left out because they already had a card
    double seconds = 0.0;         // time spent painting
    double strokesPerSecond = 0.0;       // all strokes of all pictures together, per second of painting
    double secondsPerPicture = 0.0;
    double errorAverage = 0.0, errorBest = 0.0, errorWorst = 0.0;   // leftover error, 0..255 scale
    uint32_t emptyStrokes = 0;    // strokes that could not lower the error (stored empty), all pictures together
    bool stopped = false;         // the run was stopped before every picture was done
    uint64_t finishedUnix = 0;
};

// One picture of the running group, original and painted so far (for the live view on the screens).
struct LiveView {
    uint64_t version = 0;
    uint64_t pictureId = 0;
    std::string name;
    uint32_t index = 0;           // place in the group, 0-based
    uint32_t groupSize = 0;
    uint32_t strokesDone = 0, strokesTotal = 0;
    double error = 0.0;
    Rgb8Image original;           // 128 x 128, as it is in the picture set
    Rgb8Image painted;            // the working size
};

// Everything the screens need, in one copy.
struct AppSnapshot {
    std::string version;
    uint64_t uptimeSeconds = 0;
    std::string lanUrl;
    uint64_t latestToastId = 0;

    GpuState gpu = GpuState::NotStarted;
    std::string gpuNote;
    std::string gpuReport;        // the self-test lines

    uint64_t pictureCount = 0, pictureCapacity = 0, pictureBytes = 0, pictureVersion = 0;
    bool picturesPersistent = false;
    uint64_t cardCount = 0, cardBytes = 0, cardVersion = 0;
    bool cardsPersistent = false;

    AppSettings settings;

    ConvertState state = ConvertState::Idle;
    std::string stateText;        // short line for the headline
    std::string problem;
    uint32_t runPreset = 1, runStrokes = 0, runFit = 0;
    uint32_t picturesTotal = 0, picturesDone = 0, picturesSkipped = 0;
    uint32_t groupIndex = 0, groupCount = 0, groupSize = 0;
    uint32_t flight = 0;          // 0 Wash, 1 Form, 2 Detail
    uint32_t strokesDone = 0, strokesTotal = 0;   // per picture, in the current group
    double errorNow = 0.0;        // leftover error of the current group
    double strokesPerSecond = 0.0;   // all pictures of the group together
    double seconds = 0.0;         // since the run started
    double etaSeconds = -1.0;
    double fraction = 0.0;        // 0..1 of the whole run
    uint64_t liveVersion = 0;
    RunSummary last;

    bool busy = false;
    std::string busyText;
};

class AppCore {
public:
    // dataRoot: a writable folder (created if needed). Pictures, cards and settings live inside it.
    // pictureOptions: the picture limit and memory use (the defaults are what the app ships with; tests use smaller ones).
    AppCore(std::string dataRoot, std::string appVersion, const DatasetOptions& pictureOptions = DatasetOptions());
    ~AppCore();
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // Loads settings, pictures and saved cards (never starts converting by itself), then starts the worker.
    void Start();
    // Stops a running conversion, saves the settings, stops the worker.
    void Stop();
    // Called when the system is about to suspend the app: stops converting (finished cards are already saved) and remembers to carry on.
    void PrepareForSuspend(int maxMs = 3000);
    // After a suspend (or after the graphics chip was reset): carries on with the pictures that are left. Returns true if it started again.
    bool ResumeAfterSuspend();

    // ---- graphics chip (called by the screen code) ----
    // Both stop a running conversion first. AttachGpu starts the chip and runs the five-check self-test; it returns true when all passed.
    bool AttachGpu(ID3D12Device* device, ID3D12CommandQueue* queue);
    void DetachGpu(bool deviceLost);
    // The screen code could not build a graphics device at all (or gave up after it kept failing): the chip is shown as failed, with this
    // sentence as the reason, instead of "starting...". A later AttachGpu can still bring the chip back.
    void MarkGraphicsUnavailable(const std::string& why);
    // Runs the self-test again (refused while converting). The report has one line per check.
    bool RunSelfTestAgain(std::string* report, std::string* error);
    GpuState GetGpuState() const;
    // True when a conversion found the graphics device removed or reset by the system. The screen code then rebuilds its device, calls
    // DetachGpu(true) and AttachGpu(...): the interrupted conversion carries on by itself once the chip is back.
    bool GpuLost() const { return m_gpuLost.load(); }

    // ---- conversion (thread-safe; return at once) ----
    bool StartConvert(const ConvertRequest& request, std::string* error);
    void StopConvert();
    bool IsConverting() const;

    // ---- settings ----
    bool PatchSettings(const JsonValue& patch, std::string* error, std::vector<std::string>* changed = nullptr);
    void SetNumberSetting(const char* key, double value);
    void SetBoolSetting(const char* key, bool value);
    void SetTextSetting(const char* key, const char* value);
    AppSettings GetSettings() const;
    std::string SettingsJson() const;
    void ResetSettingsToRecommended();

    // ---- what the screens show ----
    AppSnapshot Snapshot() const;
    std::string SnapshotJson() const;
    // The screens call this while they show the live view; the converter reads canvases only for a few seconds after the last call.
    void NoteLiveInterest();
    std::shared_ptr<const LiveView> GetLive() const;

    // ---- pictures and cards ----
    Dataset& Pictures() { return m_dataset; }
    StrokeStore& Cards() { return m_cards; }
    // The painted picture of a saved card at `size` x `size` pixels (16..512), painted by the processor from the card's dials (cached).
    bool PaintedPicture(const std::string& cardId, uint32_t size, Rgb8Image& out);
    // The original picture of a saved card at `size` x `size`, from the picture set. False when the picture is not in the set any more.
    bool OriginalPicture(const CardInfo& card, uint32_t size, Rgb8Image& out);
    // Removes saved cards (and their cached pictures). Return how many were removed.
    size_t DeleteCards(const std::vector<std::string>& cardIds);
    size_t DeleteAllCards();

    void SetLanUrl(const std::string& url);
    // Tells the picture set how to ask the system how much storage is free (the Xbox build provides this). Call before Start().
    void SetFreeSpaceReader(Dataset::FreeSpaceReader reader) { m_dataset.SetFreeSpaceReader(std::move(reader)); }
    void SetExtraLogReader(std::function<std::string()> reader) { m_extraLog = std::move(reader); }
    void SetLogSink(std::function<void(const std::string&)> sink) { m_logSink = std::move(sink); }

    std::vector<Toast> ToastsAfter(uint64_t afterId) const;
    void PushToast(const std::string& level, const std::string& text);
    void AppendLog(const std::string& line);
    std::string LogText() const;

    // For the developer tests.
    void WaitIdleForTests(int maxMs = 10000);                 // until no conversion is running
    void SetConvertOverrideForTests(const ConvertParams* p);  // replaces strokes / fit of the preset (nullptr = back to normal)
    Converter& ConverterForTests() { return m_conv; }

private:
    struct RunStatus {
        uint32_t preset = 1, strokes = 0, fit = 0;
        uint32_t picturesTotal = 0, picturesDone = 0, picturesSkipped = 0;
        uint32_t groupIndex = 0, groupCount = 0, groupSize = 0;
        uint32_t flight = 0, strokesDone = 0, strokesTotal = 0;
        double errorNow = 0.0, strokesPerSecond = 0.0, seconds = 0.0, etaSeconds = -1.0, fraction = 0.0;
    };

    void WorkerMain();
    void RunConvert(const ConvertRequest& request, const AppSettings& settings);
    void FinishRun(ConvertState state, const std::string& text, const std::string& problem);
    void MarkProblem(const std::string& text);
    void ReportLostPictures();
    void SaveSettingsFileIfDirty(bool force);
    void LoadSettingsFile();
    void SetGpuState(GpuState state, const std::string& note, const std::string& report);
    // Asks a running conversion to stop and holds the chip until it goes out of scope (no conversion can start meanwhile).
    class ChipGuard;
    friend class ChipGuard;
    bool Resume();
    void PublishLive(std::shared_ptr<const LiveView> view);

    const std::string m_root;
    const std::string m_version;
    Dataset m_dataset;
    StrokeStore m_cards;
    GpuCore m_gpu;
    Converter m_conv;

    // settings
    mutable std::mutex m_cfgMutex;
    AppSettings m_settings;
    bool m_settingsDirty = false;
    std::chrono::steady_clock::time_point m_settingsDirtySince{};

    // worker
    std::thread m_worker;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_started{ false };
    std::atomic<bool> m_stopConvert{ false };
    std::atomic<bool> m_swapping{ false };   // the chip is being started or stopped: no conversion may start now
    std::atomic<bool> m_gpuLost{ false };    // the system took the graphics device away while converting
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    bool m_haveRequest = false;
    ConvertRequest m_request;
    AppSettings m_requestSettings;
    std::mutex m_engineMutex;   // held by the worker while a conversion runs; the screen code takes it to swap the chip

    // state shown to the screens
    mutable std::mutex m_stateMutex;
    ConvertState m_state = ConvertState::Idle;
    std::string m_stateText = "Ready";
    std::string m_problem;
    RunStatus m_run;
    RunSummary m_last;
    GpuState m_gpuState = GpuState::NotStarted;
    std::string m_gpuNote = "The graphics chip has not been started yet.";
    std::string m_gpuReport;
    std::string m_lanUrl;
    std::string m_busyText;
    ConvertRequest m_lastRequest;
    bool m_haveLastRequest = false;
    bool m_resumeAfter = false;
    std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();

    // the live view
    std::atomic<int64_t> m_liveUntilMs{ 0 };
    mutable std::mutex m_liveMutex;
    std::shared_ptr<const LiveView> m_live;
    uint64_t m_liveVersion = 0;

    // painted pictures of cards (newest last)
    std::mutex m_previewMutex;
    std::deque<std::pair<std::string, std::shared_ptr<const Rgb8Image>>> m_previews;

    // notices and logs
    mutable std::mutex m_noticeMutex;
    std::deque<Toast> m_toasts;
    uint64_t m_nextToastId = 1;
    std::deque<std::string> m_logs;
    std::function<std::string()> m_extraLog;
    std::function<void(const std::string&)> m_logSink;
    std::mutex m_sinkMutex;     // the log sink is never called by two threads at once

    // tests
    std::mutex m_testMutex;
    bool m_haveOverride = false;
    ConvertParams m_override;
};

} // namespace XboxOTFM
