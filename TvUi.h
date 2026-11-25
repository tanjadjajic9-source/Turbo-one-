#pragma once
// The TV screen: what to show and how the controller moves around it. There is no Windows code in here. The Windows part only
// reads the controller into a PadState and draws the list of shapes this produces, so everything else is tested natively.
//
// Four screens: Convert (start and stop, the original and the painting side by side while it works, progress, the detail level),
// Results (every finished picture next to its original, and a big view of one), Settings, and Phone (the address and a code to scan).
//
// Screen Sleep: while pictures are converting and the setting is on, the TV shows one dim still frame and after that nothing is drawn
// at all, so the graphics chip spends everything on painting. Any controller button wakes the screen for a while; the press that wakes
// it does nothing else (so a half-asleep press can never stop a conversion by accident). When the conversion ends, or something needs
// attention, the screen wakes by itself.

#include "AppCore.h"
#include "ImageUtil.h"
#include "QrCode.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace XboxOTFM {

// ---------------------------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------------------------
enum PadBits : uint32_t {
    kPadA = 1u << 0,
    kPadB = 1u << 1,
    kPadX = 1u << 2,
    kPadY = 1u << 3,
    kPadUp = 1u << 4,
    kPadDown = 1u << 5,
    kPadLeft = 1u << 6,
    kPadRight = 1u << 7,
    kPadLB = 1u << 8,
    kPadRB = 1u << 9,
    kPadView = 1u << 10,
    kPadMenu = 1u << 11
};

struct PadState {
    uint32_t buttons = 0;   // PadBits held right now
    float stickX = 0.0f;    // left stick, -1 (left) .. 1 (right)
    float stickY = 0.0f;    // left stick, -1 (down) .. 1 (up)
    bool connected = false;
};

// ---------------------------------------------------------------------------------------------
// What to draw. Everything is placed on a 1920 x 1080 page; the Windows code scales that to the real screen.
// ---------------------------------------------------------------------------------------------
constexpr float kTvWidth = 1920.0f;
constexpr float kTvHeight = 1080.0f;

enum class TvColor : uint8_t {
    Background, Panel, PanelRaised, Line, Text, Muted, Accent, OnAccent, Good, Warn, Bad, Shade, ChartLine, ChartGrid,
    Black,   // the sleeping screen
    Dim,     // text on the sleeping screen
    Count
};
enum class TvFont : uint8_t { Display, Title, Heading, Body, BodyBold, Small, Count };
enum class TvAlign : uint8_t { Left, Center, Right };

uint32_t TvPaletteArgb(TvColor color);   // 0xAARRGGBB
float TvFontSize(TvFont font);           // pixels on the 1920 x 1080 page

struct TvItem {
    enum class Kind : uint8_t {
        Fill,     // filled rectangle (rounded when radius > 0)
        Outline,  // rectangle outline
        Text,     // text inside the rectangle (clipped to it)
        Lines,    // connected line segments through `points` (x0, y0, x1, y1, ...)
        Image,    // a picture stretched into the rectangle
        Badge     // a filled circle with a letter (a controller button)
    };
    Kind kind = Kind::Fill;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float radius = 0.0f;
    float thickness = 1.0f;
    TvColor color = TvColor::Text;
    TvFont font = TvFont::Body;
    TvAlign align = TvAlign::Left;
    bool wrap = false;                         // Text: wrap onto several lines instead of staying on one
    bool smooth = false;                       // Image: smooth enlargement instead of crisp squares
    std::string text;
    std::vector<float> points;
    std::shared_ptr<const Rgb8Image> image;
    uint64_t imageKey = 0;                     // same key = same picture (lets the drawing code keep the converted copy)
};

struct TvScene {
    std::vector<TvItem> items;
};

class TvBuilder;

enum class TvTab : int { Convert = 0, Results = 1, Settings = 2, Phone = 3 };
constexpr int kTvTabCount = 4;

// ---------------------------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------------------------
class TvModel {
public:
    explicit TvModel(AppCore& core);
    ~TvModel();
    TvModel(const TvModel&) = delete;
    TvModel& operator=(const TvModel&) = delete;

    // Call once per frame with the controller and the seconds since the previous call.
    void Update(const PadState& pad, double dtSeconds);
    // The shapes for the current moment (the dim still frame while the screen sleeps).
    void BuildScene(TvScene& scene);

    // True while something on screen changes quickly enough to deserve many redraws per second.
    bool NeedsFastRedraw() const;
    // Seconds between two redraws when nobody pressed anything; 0 means "draw only when something changed" (the screen is asleep).
    double RedrawInterval() const;
    // How long the main loop may rest between two controller reads (longer while the screen sleeps).
    int LoopSleepMs() const;
    // True once after something changed that needs a redraw (a press, a new picture, a message). While the screen sleeps this is true only
    // for the one dim frame.
    bool TakeDirty();
    // The screen lost its picture (the graphics were reset): draw again.
    void Invalidate();
    // True while the dim sleeping frame is what the screen shows.
    bool Asleep() const { return m_asleep; }

    // ---- for tests ----
    TvTab Tab() const { return static_cast<TvTab>(m_tab); }
    bool DialogOpen() const { return m_dialog.open; }
    bool ViewerOpen() const { return m_viewerOpen; }
    bool JobRunning() const { return m_jobBusy.load(); }
    int Focus() const { return m_focus[m_tab]; }
    size_t ResultCount() const { return m_cards.size(); }
    double AwakeSecondsLeft() const { return m_awakeLeft > 0.0 ? m_awakeLeft : 0.0; }
    uint32_t SleepFramesBuilt() const { return m_sleepFrames; }
    void WaitForJob(int maxMs);
    void WaitForTiles(int maxMs);        // until the helper thread has made every picture that was asked for
    static constexpr double kWakeSeconds = 15.0;
    static constexpr int kResultColumns = 5;
    static constexpr int kResultRows = 3;

private:
    struct SettingRow {
        enum class Kind { Choice, Toggle, Action };
        std::string label;
        std::string help;
        Kind kind = Kind::Choice;
        std::function<std::string()> value;
        std::function<void(int)> change;   // -1 / +1 = left / right, 0 = the A button
        bool danger = false;
    };
    struct Dialog {
        bool open = false;
        std::string title;
        std::vector<std::string> lines;
        std::string yesLabel;
        bool danger = false;
        double openFor = 0.0;
        std::function<void()> onYes;
    };
    struct ShownToast {
        std::string level;
        std::string text;
        double age = 0.0;
    };
    struct TileEntry {
        std::shared_ptr<const Rgb8Image> image;
        uint64_t used = 0;
    };
    struct TileRequest {
        std::string key;
        CardInfo card;
        uint32_t size = 128;
        bool original = false;
    };

    void Refresh();
    void HandlePress(uint32_t pressed);
    void HandleConvert(uint32_t pressed);
    void HandleResults(uint32_t pressed);
    void HandleSettings(uint32_t pressed);
    void HandleViewer(uint32_t pressed);
    void MoveFocus(int tab, int delta, int count, int visibleRows);
    void SwitchTab(int tab);
    void BuildRows();
    bool StartJob(const std::string& label, std::function<void()> work);
    void OpenDialog(const std::string& title, std::vector<std::string> lines, const std::string& yesLabel, bool danger, std::function<void()> onYes);
    bool BusyForActions() const;
    bool Converting() const;
    void StartConverting(bool redo);
    void AskToDeleteCard(size_t index);
    void ConsiderSleep(bool woke);

    // the pictures of finished cards, made on a helper thread (painting a card from its dials can take a moment)
    std::shared_ptr<const Rgb8Image> Tile(const CardInfo& card, uint32_t size, bool original);
    std::shared_ptr<const Rgb8Image> BestTile(const CardInfo& card, uint32_t bigSize, uint32_t smallSize, bool original);
    void TileThreadMain();

    // drawing
    void DrawTopBar(TvBuilder& b) const;
    void DrawConvert(TvBuilder& b);
    void DrawConvertProgress(TvBuilder& b, float x, float y, float w) const;
    void DrawConvertIdle(TvBuilder& b, float x, float y, float w) const;
    void DrawLastRun(TvBuilder& b, float x, float y, float w, float h) const;
    void DrawLivePanel(TvBuilder& b, float x, float y, float w, float h);
    void DrawResults(TvBuilder& b);
    void DrawSettings(TvBuilder& b) const;
    void DrawPhone(TvBuilder& b) const;
    void DrawFooter(TvBuilder& b) const;
    void DrawToasts(TvBuilder& b) const;
    void DrawViewer(TvBuilder& b);
    void DrawDialog(TvBuilder& b) const;
    void DrawSleep(TvBuilder& b);

    AppCore& m_core;

    // what is on screen
    int m_tab = 0;
    int m_focus[kTvTabCount] = { 0, 0, 0, 0 };
    int m_scroll[kTvTabCount] = { 0, 0, 0, 0 };
    bool m_viewerOpen = false;
    Dialog m_dialog;
    double m_clock = 0.0;
    bool m_dirty = true;

    // Screen Sleep
    double m_awakeLeft = kWakeSeconds;   // seconds the screen stays awake after the last press
    bool m_asleep = false;
    bool m_sleepFrameNeeded = false;
    uint32_t m_sleepFrames = 0;
    double m_sinceLiveInterest = 99.0;

    // controller bookkeeping
    uint32_t m_prevButtons = 0;
    double m_holdTime = 0.0;
    double m_nextRepeat = 0.4;

    // copies of core state, refreshed ten times a second (twice a second while asleep)
    double m_sinceRefresh = 99.0;
    AppSnapshot m_snap;
    std::shared_ptr<const LiveView> m_live;
    uint64_t m_liveShown = ~0ull;
    std::vector<CardInfo> m_cards;       // newest first
    uint64_t m_cardsShown = ~0ull;
    std::deque<ShownToast> m_toasts;
    uint64_t m_lastToastId = 0;

    // settings screen
    std::vector<SettingRow> m_rows;
    int m_quickChoice = 1;

    // phone screen
    std::string m_qrUrl;
    std::shared_ptr<Rgb8Image> m_qrImage;
    uint64_t m_qrKey = 0;

    // background jobs (deleting, the self-test: anything that may wait for the worker)
    std::atomic<bool> m_jobBusy{ false };
    mutable std::mutex m_jobMutex;
    std::string m_jobLabel;
    std::thread m_jobThread;

    // pictures of cards
    std::mutex m_tileMutex;
    std::condition_variable m_tileCv;
    std::deque<TileRequest> m_tileQueue;                 // the newest request is at the front
    std::map<std::string, TileEntry> m_tiles;
    std::set<std::string> m_tileAsked;                   // queued or being made right now
    std::set<std::string> m_tileFailed;                  // could not be made (not asked for again until the picture set changes)
    uint64_t m_tileUse = 0;
    bool m_tileBusy = false;
    bool m_tileStop = false;
    std::atomic<bool> m_tilesChanged{ false };
    std::thread m_tileThread;
};

} // namespace XboxOTFM
