#include "TvUi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace XboxOTFM {

// ---------------------------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------------------------
uint32_t TvPaletteArgb(TvColor color) {
    switch (color) {
        case TvColor::Background: return 0xFF0B1626u;
        case TvColor::Panel:      return 0xFF132338u;
        case TvColor::PanelRaised:return 0xFF1D3452u;
        case TvColor::Line:       return 0xFF2E4766u;
        case TvColor::Text:       return 0xFFF2F5FAu;
        case TvColor::Muted:      return 0xFFA9BAD0u;
        case TvColor::Accent:     return 0xFFFF7A4Du;
        case TvColor::OnAccent:   return 0xFF0B1626u;
        case TvColor::Good:       return 0xFF4CC38Au;
        case TvColor::Warn:       return 0xFFF2B544u;
        case TvColor::Bad:        return 0xFFFF6B6Bu;
        case TvColor::Shade:      return 0xB8000000u;
        case TvColor::ChartLine:  return 0xFF7FB8FFu;
        case TvColor::ChartGrid:  return 0xFF24384Fu;
        case TvColor::Black:      return 0xFF000000u;
        case TvColor::Dim:        return 0xFF46566Cu;
        default:                  return 0xFFFFFFFFu;
    }
}

float TvFontSize(TvFont font) {
    switch (font) {
        case TvFont::Display:  return 80.0f;
        case TvFont::Title:    return 46.0f;
        case TvFont::Heading:  return 34.0f;
        case TvFont::Body:     return 28.0f;
        case TvFont::BodyBold: return 28.0f;
        case TvFont::Small:    return 23.0f;
        default:               return 28.0f;
    }
}

namespace {

constexpr float kMarginX = 88.0f;
constexpr float kRightEdge = kTvWidth - kMarginX;      // 1832
constexpr float kContentTop = 156.0f;
constexpr float kContentBottom = 936.0f;
constexpr int kSettingsVisible = 10;
constexpr size_t kMaxTiles = 160;
constexpr uint32_t kBigSize = 512;      // the big view of one finished picture
constexpr uint32_t kMidSize = 256;      // the "latest result" on the Convert screen
constexpr uint32_t kSmallSize = 128;    // the grid, and the stand-in while a bigger one is being made

// ---- text helpers (ASCII only) ----
std::string Int(uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    int count = 0;
    for (size_t i = digits.size(); i > 0; --i) {
        out.insert(out.begin(), digits[i - 1]);
        if (++count % 3 == 0 && i > 1) out.insert(out.begin(), ',');
    }
    return out;
}

std::string Fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string Duration(double sec) {
    if (!std::isfinite(sec) || sec < 0) return "-";
    const long s = static_cast<long>(sec + 0.5);
    if (s < 60) return std::to_string(s) + " s";
    const long m = (s + 30) / 60;
    if (m < 60) return std::to_string(m) + " min";
    const long hrs = s / 3600;
    const long mm = (s % 3600 + 30) / 60;
    if (hrs < 48) return std::to_string(hrs) + " h" + (mm > 0 ? " " + std::to_string(mm) + " min" : std::string());
    return std::to_string(hrs / 24) + " days";
}

// short times get a decimal (4.2 s), longer ones read like Duration()
std::string TimeText(double sec) {
    if (!std::isfinite(sec) || sec < 0) return "-";
    return sec < 10.0 ? Fixed(sec, 1) + " s" : Duration(sec);
}

std::string AgoFromUnix(uint64_t unixSeconds) {
    if (unixSeconds == 0) return std::string();
    const double now = static_cast<double>(std::time(nullptr));
    const double s = std::max(0.0, now - static_cast<double>(unixSeconds));
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(static_cast<long>(s / 60 + 0.5)) + " min ago";
    if (s < 86400) return std::to_string(static_cast<long>(s / 3600 + 0.5)) + " h ago";
    return std::to_string(static_cast<long>(s / 86400 + 0.5)) + " days ago";
}

std::string Bytes(uint64_t n) {
    if (n < 1024) return std::to_string(n) + " B";
    if (n < 1024 * 1024) return std::to_string((n + 512) / 1024) + " KB";
    return Fixed(static_cast<double>(n) / (1024.0 * 1024.0), n < 10ull * 1024 * 1024 ? 1 : 0) + " MB";
}

// leftover error on the 0..255 scale: more digits when it is small
std::string ErrText(double e) {
    if (!std::isfinite(e) || e <= 0.0) return "-";
    return Fixed(e, e >= 100.0 ? 0 : (e >= 10.0 ? 1 : 2));
}

std::string RateText(double r) {
    if (!std::isfinite(r) || r <= 0.0) return "-";
    if (r >= 100.0) return Int(static_cast<uint64_t>(r + 0.5));
    return Fixed(r, r >= 10.0 ? 1 : 2);
}

// Cuts text that is too long for `widthPx` (the drawing code also clips, this keeps an ellipsis visible).
std::string Fit(const std::string& s, float widthPx, TvFont font) {
    const float size = TvFontSize(font);
    const size_t maxChars = static_cast<size_t>(std::max(4.0f, widthPx / (size * 0.55f)));
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars - 3) + "...";
}

float TextWidth(const std::string& s, TvFont font) {
    return static_cast<float>(s.size()) * TvFontSize(font) * 0.54f;
}

// room for the label of a footer hint (a little more than the text needs, so a wide font does not cut the last letter)
float LabelWidth(const std::string& label) { return TextWidth(label, TvFont::Body) * 1.1f + 12.0f; }
float ChipWidth(const std::string& chip) { return TextWidth(chip, TvFont::Small) + 28.0f; }
// how far the next hint starts after this one
float HintAdvance(const std::string& label) { return 58.0f + LabelWidth(label) + 36.0f; }
float ChipAdvance(const std::string& chip, const std::string& label) { return ChipWidth(chip) + 12.0f + LabelWidth(label) + 36.0f; }

// the bigger font when the text fits, the next smaller one when it does not
TvFont ValueFont(const std::string& s, float widthPx) {
    return TextWidth(s, TvFont::Heading) <= widthPx ? TvFont::Heading : TvFont::Body;
}

uint64_t KeyHash(const std::string& s) {
    return Fnv1a64(reinterpret_cast<const uint8_t*>(s.data()), s.size()) | 1ull;
}

std::string PresetTitle(uint32_t preset) { return PresetName(std::min(preset, 3u)); }

// "Medium: 1,500 strokes at 96 px"
std::string PresetLine(uint32_t preset) {
    const PresetInfo p = PresetFor(static_cast<DetailPreset>(std::min(preset, 3u)));
    return std::string(p.name) + ": " + Int(p.strokes) + " strokes at " + std::to_string(p.fit) + " px";
}

std::string PresetAbout(uint32_t preset) {
    static const char* const kBlurb[4] = {
        "The fastest, with the least detail.",
        "A good balance of speed and detail.",
        "More detail, and it takes longer.",
        "The most detail, and the slowest."
    };
    const uint32_t i = std::min(preset, 3u);
    const PresetInfo p = PresetFor(static_cast<DetailPreset>(i));
    return std::string(p.name) + " paints each picture with " + Int(p.strokes) + " strokes, at " + std::to_string(p.fit) + " by " +
           std::to_string(p.fit) + " pixels. " + kBlurb[i];
}

const char* FlightName(uint32_t flight) {
    static const char* const kNames[3] = { "Wash", "Form", "Detail" };
    return kNames[std::min(flight, 2u)];
}

const char* FlightAbout(uint32_t flight) {
    static const char* const kText[3] = {
        "Wash: big soft strokes that lay down the broad colours.",
        "Form: medium strokes that build the main shapes.",
        "Detail: small crisp strokes that follow the edges."
    };
    return kText[std::min(flight, 2u)];
}

// The value in `values` closest to `current`, then one step in `dir` (0 = forward, wrapping round).
double NextInList(const double* values, size_t count, double current, int dir, bool wrap) {
    size_t best = 0;
    double bestDiff = 1e300;
    for (size_t i = 0; i < count; ++i) {
        const double scale = std::max(1e-9, std::fabs(values[i]) + std::fabs(current));
        const double diff = std::fabs(values[i] - current) / scale;
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    long idx = static_cast<long>(best) + (dir == 0 ? 1 : dir);
    const long n = static_cast<long>(count);
    if (idx < 0) idx = wrap ? n - 1 : 0;
    if (idx >= n) idx = wrap ? 0 : n - 1;
    return values[idx];
}

// The self-test report has one long line per check. On the TV a check that passed is shown as just its name; a check that failed (or
// anything unexpected) keeps its explanation, cut to a sensible length.
std::string CompactReport(const std::string& report) {
    std::string out;
    size_t start = 0;
    while (start < report.size()) {
        size_t nl = report.find('\n', start);
        if (nl == std::string::npos) nl = report.size();
        std::string line = report.substr(start, nl - start);
        start = nl + 1;
        if (line.empty()) continue;
        const size_t okAt = line.find(" ok:");
        if (okAt != std::string::npos) {
            line = line.substr(0, okAt + 3);
        } else if (line.size() > 150) {
            line = line.substr(0, 147) + "...";
        }
        if (!out.empty()) out += "\n";
        out += line;
    }
    return out;
}

const double kCandidateValues[] = { 16, 32, 64, 128, 256 };
const double kRoundValues[] = { 0, 1, 2, 3, 4, 6, 8 };
const double kSeedValues[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 42, 100, 1000 };

template <size_t N>
size_t Count(const double (&)[N]) { return N; }

} // namespace

// ---------------------------------------------------------------------------------------------
// Scene builder
// ---------------------------------------------------------------------------------------------
class TvBuilder {
public:
    explicit TvBuilder(TvScene& scene) : m_scene(scene) {}

    void Fill(float x, float y, float w, float h, TvColor c, float radius = 0.0f) {
        TvItem it;
        it.kind = TvItem::Kind::Fill;
        it.x = x; it.y = y; it.w = w; it.h = h; it.color = c; it.radius = radius;
        m_scene.items.push_back(std::move(it));
    }
    void Outline(float x, float y, float w, float h, TvColor c, float thickness = 2.0f, float radius = 0.0f) {
        TvItem it;
        it.kind = TvItem::Kind::Outline;
        it.x = x; it.y = y; it.w = w; it.h = h; it.color = c; it.thickness = thickness; it.radius = radius;
        m_scene.items.push_back(std::move(it));
    }
    void Text(float x, float y, float w, float h, const std::string& text, TvFont font, TvColor c,
              TvAlign align = TvAlign::Left, bool wrap = false) {
        if (text.empty()) return;
        TvItem it;
        it.kind = TvItem::Kind::Text;
        it.x = x; it.y = y; it.w = w; it.h = h; it.text = text; it.font = font; it.color = c; it.align = align; it.wrap = wrap;
        m_scene.items.push_back(std::move(it));
    }
    void Lines(const std::vector<float>& points, TvColor c, float thickness) {
        if (points.size() < 4) return;
        TvItem it;
        it.kind = TvItem::Kind::Lines;
        it.points = points; it.color = c; it.thickness = thickness;
        m_scene.items.push_back(std::move(it));
    }
    void Image(float x, float y, float w, float h, std::shared_ptr<const Rgb8Image> image, uint64_t key, bool smooth = false) {
        if (!image || !image->Valid()) return;
        TvItem it;
        it.kind = TvItem::Kind::Image;
        it.x = x; it.y = y; it.w = w; it.h = h; it.image = std::move(image); it.imageKey = key; it.smooth = smooth;
        m_scene.items.push_back(std::move(it));
    }
    void Badge(float cx, float cy, float r, const std::string& letter, TvColor c) {
        TvItem it;
        it.kind = TvItem::Kind::Badge;
        it.x = cx - r; it.y = cy - r; it.w = 2 * r; it.h = 2 * r; it.text = letter; it.color = c;
        m_scene.items.push_back(std::move(it));
    }
    // "(A) Convert": returns where the next hint can start.
    float Hint(float x, float y, const std::string& letter, TvColor badge, const std::string& label) {
        Badge(x + 22, y + 22, 22, letter, badge);
        const float w = LabelWidth(label);
        Text(x + 58, y + 4, w, 40, label, TvFont::Body, TvColor::Text);
        return x + HintAdvance(label);
    }
    // "[LB RB] Switch screen" for buttons that have no coloured badge.
    float Chip(float x, float y, const std::string& chip, const std::string& label) {
        const float cw = ChipWidth(chip);
        Outline(x, y + 4, cw, 40, TvColor::Muted, 2.0f, 10.0f);
        Text(x, y + 8, cw, 34, chip, TvFont::Small, TvColor::Muted, TvAlign::Center);
        const float w = LabelWidth(label);
        Text(x + cw + 12, y + 4, w, 40, label, TvFont::Body, TvColor::Text);
        return x + ChipAdvance(chip, label);
    }

private:
    TvScene& m_scene;
};

// ---------------------------------------------------------------------------------------------
// Model: set-up and data
// ---------------------------------------------------------------------------------------------
TvModel::TvModel(AppCore& core) : m_core(core) {
    BuildRows();
    Refresh();
    try {
        m_tileThread = std::thread([this]() { TileThreadMain(); });
    } catch (...) {
        m_core.AppendLog("[Screen] The helper that paints pictures for the TV could not start.");
    }
}

TvModel::~TvModel() {
    {
        std::lock_guard<std::mutex> lock(m_tileMutex);
        m_tileStop = true;
        m_tileQueue.clear();
    }
    m_tileCv.notify_all();
    if (m_tileThread.joinable()) m_tileThread.join();
    if (m_jobThread.joinable()) m_jobThread.join();
}

void TvModel::WaitForJob(int maxMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (m_jobBusy.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void TvModel::WaitForTiles(int maxMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(m_tileMutex);
            if (m_tileQueue.empty() && !m_tileBusy) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool TvModel::Converting() const {
    return m_snap.state == ConvertState::Converting || m_snap.state == ConvertState::Stopping;
}

bool TvModel::TakeDirty() {
    if (m_asleep) {
        // asleep: the one dim frame, and nothing else
        m_dirty = false;
        m_tilesChanged.store(false);
        const bool need = m_sleepFrameNeeded;
        m_sleepFrameNeeded = false;
        return need;
    }
    bool d = m_dirty;
    m_dirty = false;
    if (m_tilesChanged.exchange(false)) d = true;
    return d;
}

void TvModel::Invalidate() {
    m_dirty = true;
    m_sleepFrameNeeded = m_asleep;
}

bool TvModel::NeedsFastRedraw() const {
    if (m_asleep) return false;
    return m_dialog.open || m_viewerOpen || !m_toasts.empty() || m_jobBusy.load() || m_snap.busy;
}

double TvModel::RedrawInterval() const {
    if (m_asleep) return 0.0;
    if (NeedsFastRedraw()) return 1.0 / 30.0;
    if (Converting()) return 0.15;
    return 0.25;
}

int TvModel::LoopSleepMs() const {
    return m_asleep ? 40 : 6;
}

void TvModel::Refresh() {
    m_sinceRefresh = 0.0;
    const uint64_t pictureVersionBefore = m_snap.pictureVersion;
    m_snap = m_core.Snapshot();

    // the picture being painted now
    if (m_snap.liveVersion != m_liveShown) {
        m_live = m_core.GetLive();
        m_liveShown = m_snap.liveVersion;
        m_dirty = true;
    }

    // the finished pictures
    if (m_snap.cardVersion != m_cardsShown) {
        m_cards = m_core.Cards().List();
        m_cardsShown = m_snap.cardVersion;
        const int res = static_cast<int>(TvTab::Results);
        const int n = static_cast<int>(m_cards.size());
        m_focus[res] = std::clamp(m_focus[res], 0, std::max(0, n - 1));
        const int rows = (n + kResultColumns - 1) / kResultColumns;
        m_scroll[res] = std::clamp(m_scroll[res], 0, std::max(0, rows - kResultRows));
        const int row = m_focus[res] / kResultColumns;
        if (row < m_scroll[res]) m_scroll[res] = row;
        if (row >= m_scroll[res] + kResultRows) m_scroll[res] = row - kResultRows + 1;
        if (m_viewerOpen && n == 0) m_viewerOpen = false;
        m_dirty = true;
    }
    // a picture that was added again can be found again (its original had failed to load before)
    if (pictureVersionBefore != m_snap.pictureVersion) {
        std::lock_guard<std::mutex> lock(m_tileMutex);
        m_tileFailed.clear();
    }

    // messages from the core
    {
        const std::vector<Toast> fresh = m_core.ToastsAfter(m_lastToastId);
        for (const Toast& t : fresh) {
            m_lastToastId = std::max(m_lastToastId, t.id);
            ShownToast st;
            st.level = t.level;
            st.text = t.text;
            m_toasts.push_back(std::move(st));
            while (m_toasts.size() > 3) m_toasts.pop_front();
            m_dirty = true;
        }
    }

    // the phone address as a code
    if (m_snap.lanUrl != m_qrUrl) {
        m_qrUrl = m_snap.lanUrl;
        m_qrImage.reset();
        QrMatrix qr;
        if (!m_qrUrl.empty() && MakeQrCode(m_qrUrl, qr)) {
            auto img = std::make_shared<Rgb8Image>(RenderQrImage(qr, 6));
            m_qrImage = img;
            m_qrKey = KeyHash(m_qrUrl);
        }
        m_dirty = true;
    }
}

// ---------------------------------------------------------------------------------------------
// Model: pictures of finished cards (made on a helper thread)
// ---------------------------------------------------------------------------------------------
std::shared_ptr<const Rgb8Image> TvModel::Tile(const CardInfo& card, uint32_t size, bool original) {
    const std::string key = std::string(original ? "o:" : "p:") + card.id + ":" + std::to_string(size);
    std::lock_guard<std::mutex> lock(m_tileMutex);
    const auto it = m_tiles.find(key);
    if (it != m_tiles.end()) {
        it->second.used = ++m_tileUse;
        return it->second.image;
    }
    if (m_tileFailed.count(key) == 0 && m_tileAsked.insert(key).second) {
        TileRequest r;
        r.key = key;
        r.card = card;
        r.size = size;
        r.original = original;
        m_tileQueue.push_front(std::move(r));   // what was asked for last is made first
        m_tileCv.notify_one();
    }
    return nullptr;
}

// the big picture when it is ready, otherwise the small one (both are asked for; the small one is made first)
std::shared_ptr<const Rgb8Image> TvModel::BestTile(const CardInfo& card, uint32_t bigSize, uint32_t smallSize, bool original) {
    std::shared_ptr<const Rgb8Image> a = Tile(card, bigSize, original);
    if (a) return a;
    return Tile(card, smallSize, original);
}

void TvModel::TileThreadMain() {
    for (;;) {
        TileRequest req;
        {
            std::unique_lock<std::mutex> lock(m_tileMutex);
            m_tileCv.wait(lock, [this]() { return m_tileStop || !m_tileQueue.empty(); });
            if (m_tileStop) return;
            req = std::move(m_tileQueue.front());
            m_tileQueue.pop_front();
            m_tileBusy = true;
        }
        Rgb8Image img;
        bool ok = false;
        try {
            ok = req.original ? m_core.OriginalPicture(req.card, req.size, img) : m_core.PaintedPicture(req.card.id, req.size, img);
        } catch (...) {
            ok = false;
        }
        {
            std::lock_guard<std::mutex> lock(m_tileMutex);
            m_tileBusy = false;
            m_tileAsked.erase(req.key);
            if (ok && img.Valid()) {
                TileEntry e;
                e.image = std::make_shared<const Rgb8Image>(std::move(img));
                e.used = ++m_tileUse;
                m_tiles[req.key] = std::move(e);
                while (m_tiles.size() > kMaxTiles) {   // the one used longest ago goes
                    auto oldest = m_tiles.begin();
                    for (auto it = m_tiles.begin(); it != m_tiles.end(); ++it) {
                        if (it->second.used < oldest->second.used) oldest = it;
                    }
                    m_tiles.erase(oldest);
                }
                m_tilesChanged.store(true);
            } else {
                m_tileFailed.insert(req.key);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Model: controller
// ---------------------------------------------------------------------------------------------
void TvModel::Update(const PadState& pad, double dt) {
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    m_clock += dt;
    m_sinceRefresh += dt;
    m_sinceLiveInterest += dt;
    if (m_dialog.open) m_dialog.openFor += dt;
    for (ShownToast& t : m_toasts) t.age += dt;
    while (!m_toasts.empty() && m_toasts.front().age > (m_toasts.front().level == "error" ? 9.0 : 6.0)) m_toasts.pop_front();

    uint32_t buttons = pad.buttons;
    if (pad.stickY > 0.6f) buttons |= kPadUp;
    else if (pad.stickY < -0.6f) buttons |= kPadDown;
    if (pad.stickX > 0.6f) buttons |= kPadRight;
    else if (pad.stickX < -0.6f) buttons |= kPadLeft;

    const uint32_t edge = buttons & ~m_prevButtons;   // buttons that went down just now (no key repeats)
    uint32_t pressed = edge;
    const uint32_t dirMask = kPadUp | kPadDown | kPadLeft | kPadRight;
    const uint32_t heldDirs = buttons & dirMask;
    if (heldDirs != 0 && heldDirs == (m_prevButtons & dirMask)) {
        m_holdTime += dt;
        if (m_holdTime >= m_nextRepeat) {
            pressed |= heldDirs;
            m_nextRepeat += 0.09;
        }
    } else {
        m_holdTime = 0.0;
        m_nextRepeat = 0.4;
    }
    m_prevButtons = buttons;

    // Screen Sleep: while the screen sleeps nothing acts on the controller, and the press that wakes it does nothing else
    bool woke = false;
    if (edge != 0) {
        if (m_asleep) woke = true;
        m_awakeLeft = kWakeSeconds;
    } else if (m_awakeLeft > 0.0) {
        m_awakeLeft -= dt;
    }
    if (m_asleep) pressed = 0;

    if (pressed != 0) HandlePress(pressed);

    if (m_sinceRefresh >= (m_asleep ? 0.5 : 0.1) || pressed != 0 || woke) Refresh();
    ConsiderSleep(woke);

    // keep the live pair coming while it is on screen
    if (!m_asleep && m_tab == static_cast<int>(TvTab::Convert) && Converting() && m_sinceLiveInterest >= 0.9) {
        m_core.NoteLiveInterest();
        m_sinceLiveInterest = 0.0;
    }
}

void TvModel::ConsiderSleep(bool woke) {
    const bool wanted = m_snap.settings.screenSleep && Converting();
    const bool asleepNow = wanted && m_awakeLeft <= 0.0 && !woke;
    if (asleepNow && !m_asleep) {
        m_asleep = true;
        m_sleepFrameNeeded = true;
        m_dirty = true;
    } else if (!asleepNow && m_asleep) {
        m_asleep = false;
        m_dirty = true;
    }
}

void TvModel::SwitchTab(int tab) {
    m_tab = std::clamp(tab, 0, kTvTabCount - 1);
    m_viewerOpen = false;
    m_dirty = true;
}

void TvModel::MoveFocus(int tab, int delta, int count, int visibleRows) {
    if (count <= 0) { m_focus[tab] = 0; m_scroll[tab] = 0; return; }
    m_focus[tab] = std::clamp(m_focus[tab] + delta, 0, count - 1);
    if (m_focus[tab] < m_scroll[tab]) m_scroll[tab] = m_focus[tab];
    if (m_focus[tab] >= m_scroll[tab] + visibleRows) m_scroll[tab] = m_focus[tab] - visibleRows + 1;
    m_scroll[tab] = std::clamp(m_scroll[tab], 0, std::max(0, count - visibleRows));
}

bool TvModel::BusyForActions() const {
    return m_jobBusy.load() || m_snap.busy;
}

bool TvModel::StartJob(const std::string& label, std::function<void()> work) {
    bool expected = false;
    if (!m_jobBusy.compare_exchange_strong(expected, true)) return false;
    if (m_jobThread.joinable()) m_jobThread.join();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobLabel = label;
    }
    try {
        m_jobThread = std::thread([this, work]() {
            try { work(); } catch (...) {}
            m_jobBusy.store(false);
        });
    } catch (...) {
        m_jobBusy.store(false);
        return false;
    }
    return true;
}

void TvModel::OpenDialog(const std::string& title, std::vector<std::string> lines, const std::string& yesLabel, bool danger, std::function<void()> onYes) {
    m_dialog = Dialog();
    m_dialog.open = true;
    m_dialog.title = title;
    m_dialog.lines = std::move(lines);
    m_dialog.yesLabel = yesLabel;
    m_dialog.danger = danger;
    m_dialog.onYes = std::move(onYes);
    m_dirty = true;
}

void TvModel::HandlePress(uint32_t pressed) {
    m_dirty = true;
    if (m_dialog.open) {
        if (m_dialog.openFor < 0.35) return; // a double press must not answer a question the person has not read yet
        if (pressed & kPadA) {
            std::function<void()> yes = std::move(m_dialog.onYes);
            m_dialog = Dialog();
            if (yes) yes();
        } else if (pressed & kPadB) {
            m_dialog = Dialog();
        }
        return;
    }
    if (m_viewerOpen) { HandleViewer(pressed); return; }
    if (pressed & kPadLB) { SwitchTab((m_tab + kTvTabCount - 1) % kTvTabCount); return; }
    if (pressed & kPadRB) { SwitchTab((m_tab + 1) % kTvTabCount); return; }
    if (pressed & kPadMenu) { SwitchTab(static_cast<int>(TvTab::Settings)); return; }
    if (pressed & kPadView) { SwitchTab(m_tab == static_cast<int>(TvTab::Phone) ? static_cast<int>(TvTab::Convert) : static_cast<int>(TvTab::Phone)); return; }
    switch (static_cast<TvTab>(m_tab)) {
        case TvTab::Convert: HandleConvert(pressed); break;
        case TvTab::Results: HandleResults(pressed); break;
        case TvTab::Settings: HandleSettings(pressed); break;
        case TvTab::Phone: break;
    }
}

void TvModel::StartConverting(bool redo) {
    if (m_snap.busy) { m_core.PushToast("info", "One moment, the Xbox is busy."); return; }
    ConvertRequest request;
    request.redo = redo;
    std::string error;
    if (!m_core.StartConvert(request, &error)) {
        m_core.PushToast("warn", error.empty() ? std::string("Converting could not start.") : error);
    }
}

void TvModel::HandleConvert(uint32_t pressed) {
    if (pressed & kPadA) {
        if (m_snap.state == ConvertState::Converting) m_core.StopConvert();
        else if (m_snap.state == ConvertState::Stopping) m_core.PushToast("info", "Stopping. Pictures that are already finished are kept.");
        else StartConverting(false);
    }
    if (!Converting()) {
        if (pressed & (kPadLeft | kPadRight)) {
            const int now = static_cast<int>(std::min<uint32_t>(m_snap.settings.preset, 3));
            const int next = std::clamp(now + ((pressed & kPadRight) ? 1 : -1), 0, 3);
            if (next != now) m_core.SetNumberSetting("preset", next);
        }
        if ((pressed & kPadX) && m_snap.cardCount > 0 && m_snap.pictureCount > 0) {
            OpenDialog("Convert every picture again?",
                       { "Every picture is painted again at " + PresetLine(m_snap.settings.preset) + ". Finished pictures made with these settings are replaced by the new ones.",
                         "It takes as long as the first time." },
                       "Convert again", false,
                       [this]() { StartConverting(true); });
        }
    }
    if (pressed & kPadY) {
        const bool on = !m_snap.settings.screenSleep;
        m_core.SetBoolSetting("screenSleep", on);
        m_core.PushToast("info", on ? "Screen Sleep is on. The TV goes dim while pictures are converting." : "Screen Sleep is off. The TV keeps showing the progress.");
    }
}

void TvModel::HandleResults(uint32_t pressed) {
    const int tab = static_cast<int>(TvTab::Results);
    const int n = static_cast<int>(m_cards.size());
    if (n == 0) return;
    int& f = m_focus[tab];
    if (pressed & kPadLeft) f = std::max(0, f - 1);
    if (pressed & kPadRight) f = std::min(n - 1, f + 1);
    if (pressed & kPadUp) f = (f >= kResultColumns) ? f - kResultColumns : f;
    if (pressed & kPadDown) {
        if (f + kResultColumns < n) f += kResultColumns;
        else if (f / kResultColumns < (n - 1) / kResultColumns) f = n - 1;   // the last row is shorter
    }
    const int row = f / kResultColumns;
    const int rows = (n + kResultColumns - 1) / kResultColumns;
    if (row < m_scroll[tab]) m_scroll[tab] = row;
    if (row >= m_scroll[tab] + kResultRows) m_scroll[tab] = row - kResultRows + 1;
    m_scroll[tab] = std::clamp(m_scroll[tab], 0, std::max(0, rows - kResultRows));
    if (pressed & kPadA) m_viewerOpen = true;
    if (pressed & kPadX) AskToDeleteCard(static_cast<size_t>(f));
}

void TvModel::HandleViewer(uint32_t pressed) {
    const int tab = static_cast<int>(TvTab::Results);
    const int n = static_cast<int>(m_cards.size());
    int& f = m_focus[tab];
    if (pressed & (kPadA | kPadB)) {
        m_viewerOpen = false;
    } else if ((pressed & kPadLeft) && n > 0) {
        f = (f + n - 1) % n;
    } else if ((pressed & kPadRight) && n > 0) {
        f = (f + 1) % n;
    } else if ((pressed & kPadX) && n > 0) {
        AskToDeleteCard(static_cast<size_t>(f));
    }
    if (n > 0) {   // keep the highlighted picture inside the grid behind the viewer
        const int row = f / kResultColumns;
        const int rows = (n + kResultColumns - 1) / kResultColumns;
        if (row < m_scroll[tab]) m_scroll[tab] = row;
        if (row >= m_scroll[tab] + kResultRows) m_scroll[tab] = row - kResultRows + 1;
        m_scroll[tab] = std::clamp(m_scroll[tab], 0, std::max(0, rows - kResultRows));
    }
}

void TvModel::AskToDeleteCard(size_t index) {
    if (index >= m_cards.size()) return;
    const CardInfo card = m_cards[index];
    OpenDialog("Delete this finished picture?",
               { "\"" + card.name + "\" (" + PresetTitle(card.preset) + ", " + Int(card.key.strokes) + " strokes)",
                 "The original picture stays in your set, so you can convert it again. This cannot be undone." },
               "Delete", true,
               [this, card]() {
                   if (m_jobBusy.load()) { m_core.PushToast("info", "One moment, the Xbox is busy."); return; }
                   StartJob("Deleting...", [this, card]() {
                       const size_t removed = m_core.DeleteCards({ card.id });
                       m_core.PushToast(removed > 0 ? "good" : "warn", removed > 0 ? "Deleted." : "That finished picture was already gone.");
                   });
               });
}

void TvModel::HandleSettings(uint32_t pressed) {
    const int tab = static_cast<int>(TvTab::Settings);
    const int count = static_cast<int>(m_rows.size());
    if (pressed & kPadUp) MoveFocus(tab, -1, count, kSettingsVisible);
    if (pressed & kPadDown) MoveFocus(tab, 1, count, kSettingsVisible);
    int dir = 0;
    bool act = false;
    if (pressed & kPadLeft) { dir = -1; act = true; }
    else if (pressed & kPadRight) { dir = 1; act = true; }
    else if (pressed & kPadA) { dir = 0; act = true; }
    const int f = m_focus[tab];
    if (act && f >= 0 && f < count) m_rows[static_cast<size_t>(f)].change(dir);
}

void TvModel::BuildRows() {
    m_rows.clear();
    auto add = [this](const std::string& label, SettingRow::Kind kind, const std::string& help,
                      std::function<std::string()> value, std::function<void(int)> change, bool danger = false) {
        SettingRow r;
        r.label = label;
        r.kind = kind;
        r.help = help;
        r.value = std::move(value);
        r.change = std::move(change);
        r.danger = danger;
        m_rows.push_back(std::move(r));
    };
    auto onOff = [](bool on) { return std::string(on ? "On" : "Off"); };

    add("Detail level", SettingRow::Kind::Choice,
        "How many brush strokes each picture gets, and the size it is painted at while converting. More strokes make a closer copy and take longer. "
        "Low: 500 strokes at 64 px. Medium: 1,500 at 96 px. High: 3,000 at 128 px. Ultra: 6,000 at 128 px. "
        "A finished picture is kept as a small card of dials, so it can be painted at any size afterwards.",
        [this]() { return PresetLine(m_core.GetSettings().preset); },
        [this](int dir) {
            const int now = static_cast<int>(std::min<uint32_t>(m_core.GetSettings().preset, 3));
            const int next = dir == 0 ? (now + 1) % 4 : std::clamp(now + dir, 0, 3);   // A goes round, Left and Right stop at the ends
            m_core.SetNumberSetting("preset", next);
        });

    add("Screen Sleep", SettingRow::Kind::Toggle,
        "While pictures are converting, the TV shows only a dim, still screen, so the Xbox puts all its power into painting. "
        "Press any controller button to wake the screen for a moment. It wakes by itself when the conversion ends or needs attention.",
        [this, onOff]() { return onOff(m_core.GetSettings().screenSleep); },
        [this](int) { m_core.SetBoolSetting("screenSleep", !m_core.GetSettings().screenSleep); });

    add("Skip finished pictures", SettingRow::Kind::Toggle,
        "Pictures that are already converted with the same detail level and settings are left out, so a second run only does the new ones. "
        "Turn it off to convert everything again.",
        [this, onOff]() { return onOff(m_core.GetSettings().skipDone); },
        [this](int) { m_core.SetBoolSetting("skipDone", !m_core.GetSettings().skipDone); });

    add("Candidate strokes", SettingRow::Kind::Choice,
        "For every stroke, the chip tries this many different strokes and keeps the one that helps most. More candidates give a closer painting and take longer: "
        "128 takes about twice as long as 64, for roughly 7 percent less leftover error. 64 is the recommended value. "
        "Finished pictures made with another value are kept, and the picture is converted again.",
        [this]() { return Int(m_core.GetSettings().candidates); },
        [this](int dir) {
            const double now = static_cast<double>(m_core.GetSettings().candidates);
            m_core.SetNumberSetting("candidates", NextInList(kCandidateValues, Count(kCandidateValues), now, dir, dir == 0));
        });

    add("Tuning rounds", SettingRow::Kind::Choice,
        "After choosing the best candidate, the chip nudges its dials this many times and keeps every change that lowers the error. "
        "More rounds are a little closer and a little slower. 3 is the recommended value.",
        [this]() { return std::to_string(m_core.GetSettings().rounds); },
        [this](int dir) {
            const double now = static_cast<double>(m_core.GetSettings().rounds);
            m_core.SetNumberSetting("rounds", NextInList(kRoundValues, Count(kRoundValues), now, dir, dir == 0));
        });

    add("Seed", SettingRow::Kind::Choice,
        "Every random choice the converter makes follows from this number, so the same seed always paints the same card. "
        "Change it to get a different painting of the same picture.",
        [this]() { return std::to_string(m_core.GetSettings().seed); },
        [this](int dir) {
            const double now = static_cast<double>(m_core.GetSettings().seed);
            m_core.SetNumberSetting("seed", NextInList(kSeedValues, Count(kSeedValues), now, dir, dir == 0));
        });

    add("Enlarge small pictures", SettingRow::Kind::Choice,
        "Pictures are stored at 128 by 128 pixels. Automatic copies pixels when a small picture with few colours (pixel art) is enlarged, so the edges stay sharp, "
        "and blends the pixels of everything else. Sharp copies pixels for every picture. Smooth always blends them, which suits photos. "
        "This only affects pictures added from now on, and the phone page has the same setting.",
        [this]() {
            const ResizeStyle r = m_core.GetSettings().resize;
            return std::string(r == ResizeStyle::Auto ? "Automatic" : (r == ResizeStyle::Sharp ? "Sharp" : "Smooth"));
        },
        [this](int dir) {
            static const char* const kStyles[3] = { "auto", "sharp", "smooth" };
            const ResizeStyle r = m_core.GetSettings().resize;
            int idx = r == ResizeStyle::Auto ? 0 : (r == ResizeStyle::Sharp ? 1 : 2);
            idx = dir == 0 ? (idx + 1) % 3 : std::clamp(idx + dir, 0, 2);
            m_core.SetTextSetting("resize", kStyles[idx]);
        });

    add("Run the self-test again", SettingRow::Kind::Action,
        "When the app starts, the graphics chip paints and measures a few test pictures, and the results must match the processor exactly. "
        "Converting is only allowed when it passes. Press A to run the five checks again (not while converting). The result is shown on the right.",
        []() { return std::string("press A"); },
        [this](int dir) {
            if (dir != 0) return;
            if (Converting()) { m_core.PushToast("info", "Stop converting first. The self-test needs the graphics chip to itself."); return; }
            if (BusyForActions()) { m_core.PushToast("info", "One moment, the Xbox is busy."); return; }
            StartJob("Testing the chip...", [this]() {
                std::string report, error;
                const bool ok = m_core.RunSelfTestAgain(&report, &error);
                if (ok) m_core.PushToast("good", "The graphics chip passed all five self-test checks.");
                else m_core.PushToast("error", error.empty() ? std::string("The graphics chip failed its self-test.") : error);
            });
        });

    add("Delete all finished pictures", SettingRow::Kind::Action,
        "Removes every finished picture (the cards of dials). Your original pictures stay in your set, and you can convert them again. This cannot be undone.",
        [this]() { return m_snap.cardCount == 0 ? std::string("none to delete") : Int(m_snap.cardCount) + " pictures"; },
        [this](int dir) {
            if (dir != 0) return;
            if (m_snap.cardCount == 0) { m_core.PushToast("info", "There are no finished pictures."); return; }
            if (Converting()) { m_core.PushToast("info", "Stop converting first."); return; }
            OpenDialog("Delete all finished pictures?",
                       { "This removes all " + Int(m_snap.cardCount) + " finished pictures. Your original pictures stay in your set, so you can convert them again.",
                         "This cannot be undone." },
                       "Delete all", true,
                       [this]() {
                           if (m_jobBusy.load()) { m_core.PushToast("info", "One moment, the Xbox is busy."); return; }
                           StartJob("Deleting...", [this]() {
                               const size_t n = m_core.DeleteAllCards();
                               m_core.PushToast("good", n == 1 ? std::string("Deleted 1 finished picture.") : "Deleted " + Int(n) + " finished pictures.");
                           });
                       });
        }, true);

    add("Reset all settings", SettingRow::Kind::Action,
        "Every setting goes back to the recommended value. Your pictures and finished pictures are not touched.",
        []() { return std::string("press A"); },
        [this](int dir) {
            if (dir != 0) return;
            OpenDialog("Reset all settings?",
                       { "Every setting goes back to the recommended value. Your pictures and finished pictures are not touched." },
                       "Reset settings", false,
                       [this]() { m_core.ResetSettingsToRecommended(); m_core.PushToast("good", "The settings are back to the recommended values."); });
        });
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
void TvModel::BuildScene(TvScene& scene) {
    scene.items.clear();
    scene.items.reserve(600);
    TvBuilder b(scene);
    if (m_asleep) {
        DrawSleep(b);
        return;
    }
    b.Fill(0, 0, kTvWidth, kTvHeight, TvColor::Background);
    DrawTopBar(b);
    switch (static_cast<TvTab>(m_tab)) {
        case TvTab::Convert: DrawConvert(b); break;
        case TvTab::Results: DrawResults(b); break;
        case TvTab::Settings: DrawSettings(b); break;
        case TvTab::Phone: DrawPhone(b); break;
    }
    DrawFooter(b);
    DrawToasts(b);
    if (m_viewerOpen) DrawViewer(b);
    if (m_dialog.open) DrawDialog(b);
}

void TvModel::DrawSleep(TvBuilder& b) {
    ++m_sleepFrames;
    b.Fill(0, 0, kTvWidth, kTvHeight, TvColor::Black);
    // the words move a little every time the screen goes to sleep, so a TV that is left like this for hours never keeps one spot lit
    const float dx = static_cast<float>(static_cast<int>(m_sleepFrames % 3) - 1) * 70.0f;
    const float dy = static_cast<float>(static_cast<int>(m_sleepFrames % 5) - 2) * 50.0f;
    b.Text(160.0f + dx, 440.0f + dy, kTvWidth - 320.0f, 56.0f, "Converting your pictures", TvFont::Heading, TvColor::Dim, TvAlign::Center);
    b.Text(160.0f + dx, 506.0f + dy, kTvWidth - 320.0f, 80.0f,
           "The screen rests so the Xbox can paint faster. Press any button to see the progress.",
           TvFont::Small, TvColor::Dim, TvAlign::Center, true);
}

void TvModel::DrawTopBar(TvBuilder& b) const {
    // logo: four small squares
    b.Fill(kMarginX, 58, 20, 20, TvColor::Text, 4);
    b.Fill(kMarginX + 24, 58, 20, 20, TvColor::Muted, 4);
    b.Fill(kMarginX, 82, 20, 20, TvColor::Line, 4);
    b.Fill(kMarginX + 24, 82, 20, 20, TvColor::Accent, 4);
    b.Text(kMarginX + 64, 56, 280, 52, "Image Trainer", TvFont::Heading, TvColor::Text);

    static const char* const kNames[kTvTabCount] = { "Convert", "Results", "Settings", "Phone" };
    const float tabsX = 496.0f, tabW = 176.0f;
    b.Outline(430, 64, 56, 40, TvColor::Muted, 2.0f, 10.0f);
    b.Text(430, 68, 56, 34, "LB", TvFont::Small, TvColor::Muted, TvAlign::Center);
    for (int i = 0; i < kTvTabCount; ++i) {
        const float x = tabsX + static_cast<float>(i) * tabW;
        const bool on = i == m_tab;
        if (on) {
            b.Fill(x + 4, 56, tabW - 8, 56, TvColor::PanelRaised, 12);
            b.Fill(x + 24, 106, tabW - 48, 5, TvColor::Accent, 2);
        }
        b.Text(x, 66, tabW, 40, kNames[i], on ? TvFont::BodyBold : TvFont::Body, on ? TvColor::Text : TvColor::Muted, TvAlign::Center);
    }
    const float rbX = tabsX + static_cast<float>(kTvTabCount) * tabW + 12;
    b.Outline(rbX, 64, 56, 40, TvColor::Muted, 2.0f, 10.0f);
    b.Text(rbX, 68, 56, 34, "RB", TvFont::Small, TvColor::Muted, TvAlign::Center);

    // status pill
    std::string label;
    TvColor color = TvColor::Muted;
    switch (m_snap.state) {
        case ConvertState::Converting: label = "Converting " + std::to_string(static_cast<int>(std::clamp(m_snap.fraction, 0.0, 1.0) * 100.0 + 0.5)) + "%"; color = TvColor::Good; break;
        case ConvertState::Stopping: label = "Stopping..."; color = TvColor::Warn; break;
        case ConvertState::Problem: label = "Needs attention"; color = TvColor::Bad; break;
        case ConvertState::Done: label = m_snap.stateText.empty() ? std::string("Done") : m_snap.stateText; color = TvColor::Accent; break;
        default: label = m_snap.stateText == "Stopped" ? "Stopped" : "Ready"; break;
    }
    if (m_snap.state != ConvertState::Converting && m_snap.state != ConvertState::Stopping && m_snap.state != ConvertState::Problem) {
        if (m_snap.gpu == GpuState::Failed) { label = "Chip problem"; color = TvColor::Bad; }
        else if (m_snap.gpu != GpuState::Ready) {
            label = m_snap.stateText == "Waiting for the graphics chip" ? "Waiting for the chip" : "Starting the chip";
            color = TvColor::Warn;
        }
    }
    std::string job;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        job = m_jobLabel;
    }
    if (m_jobBusy.load() && !job.empty()) { label = job; color = TvColor::Warn; }
    else if (m_snap.busy && !m_snap.busyText.empty() && !Converting()) { label = m_snap.busyText; color = TvColor::Warn; }
    const float pillX = 1480.0f, pillW = kRightEdge - pillX;
    b.Fill(pillX, 60, pillW, 48, TvColor::Panel, 24);
    b.Outline(pillX, 60, pillW, 48, color, 2.0f, 24);
    b.Fill(pillX + 20, 77, 14, 14, color, 7);
    b.Text(pillX + 44, 66, pillW - 56, 38, Fit(label, pillW - 64, TvFont::Body), TvFont::Body, TvColor::Text);
}

// ---- Convert ---------------------------------------------------------------------------------
void TvModel::DrawConvert(TvBuilder& b) {
    const AppSnapshot& s = m_snap;
    const float lx = kMarginX, lw = 912.0f;
    const float rx = 1032.0f, rw = kRightEdge - rx;
    const bool run = Converting();

    // headline
    b.Fill(lx, kContentTop, lw, 176, TvColor::Panel, 16);
    std::string big, bigLabel, title, sub;
    TvColor titleColor = TvColor::Text;
    const std::string presetName = PresetTitle(run ? s.runPreset : s.settings.preset);
    if (run) {
        big = std::to_string(static_cast<int>(std::clamp(s.fraction, 0.0, 1.0) * 100.0 + 0.5)) + "%";
        bigLabel = "of the whole run";
    } else {
        big = Int(s.pictureCount);
        bigLabel = s.pictureCount == 1 ? "picture in your set" : "pictures in your set";
    }
    if (s.state == ConvertState::Converting) {
        title = "Converting at " + presetName;
        titleColor = TvColor::Good;
        sub = (m_live && m_live->groupSize > 0)
                  ? "Now painting " + m_live->name + " (picture " + std::to_string(m_live->index + 1) + " of " + std::to_string(m_live->groupSize) + " in this batch)."
                  : std::string("Getting the first picture ready...");
    } else if (s.state == ConvertState::Stopping) {
        title = "Stopping...";
        titleColor = TvColor::Warn;
        sub = "Finishing the stroke it is on. Finished pictures are kept.";
    } else if (s.state == ConvertState::Problem) {
        title = "Needs attention";
        titleColor = TvColor::Bad;
        sub = "The last run stopped early. Pictures finished before this are saved. The details are below.";
    } else if (s.gpu == GpuState::Failed) {
        title = "Graphics chip problem";
        titleColor = TvColor::Bad;
        sub = "Nothing can be converted until the chip passes its self-test. The details are below.";
    } else if (s.gpu != GpuState::Ready) {
        title = s.stateText == "Waiting for the graphics chip" ? "Waiting for the chip" : "Starting the chip";
        titleColor = TvColor::Warn;
        sub = "Converting can start as soon as the graphics chip is ready.";
    } else if (s.state == ConvertState::Done) {
        title = s.stateText.empty() ? std::string("Done") : s.stateText;
        titleColor = TvColor::Accent;
        if (s.stateText == "Nothing to do") {
            sub = "Every picture already has a finished version at " + presetName + ". Press X to convert them again.";
        } else if (s.last.valid) {
            sub = "Converted " + Int(s.last.pictures) + (s.last.pictures == 1 ? " picture" : " pictures") + " at " + PresetTitle(s.last.preset) + " in " + TimeText(s.last.seconds) + ".";
        }
    } else {
        title = s.stateText == "Stopped" ? "Stopped" : "Ready";
        titleColor = TvColor::Good;
        sub = s.pictureCount == 0 ? std::string("No pictures yet. Add some from your phone: press View for the address.")
                                  : Int(s.pictureCount) + (s.pictureCount == 1 ? " picture is" : " pictures are") + " ready to convert at " + presetName + ".";
    }
    // the big number's block decides where the title can start
    const float numW = std::max(TextWidth(big, TvFont::Display), TextWidth(bigLabel, TvFont::Small)) * 1.12f + 12.0f;
    const float tx = lx + 32.0f + numW + 36.0f;
    const float tw = lx + lw - 30.0f - tx;
    b.Text(lx + 32, kContentTop + 6, numW, 100, big, TvFont::Display, TvColor::Text);
    b.Text(lx + 32, kContentTop + 108, numW, 36, bigLabel, TvFont::Small, TvColor::Muted);
    const TvFont titleFont = TextWidth(title, TvFont::Title) * 1.1f <= tw ? TvFont::Title : TvFont::Heading;
    b.Text(tx, kContentTop + 14, tw, 56, Fit(title, tw, titleFont), titleFont, titleColor);
    b.Text(tx, kContentTop + 76, tw, 92, Fit(sub, tw * 3.0f - 60.0f, TvFont::Small), TvFont::Small, TvColor::Muted, TvAlign::Left, true);

    if (run) DrawConvertProgress(b, lx, 352.0f, lw);
    else DrawConvertIdle(b, lx, 352.0f, lw);

    DrawLivePanel(b, rx, kContentTop, rw, kContentBottom - kContentTop);
}

void TvModel::DrawConvertProgress(TvBuilder& b, float x, float y, float w) const {
    const AppSnapshot& s = m_snap;
    // the bar
    b.Fill(x, y, w, 26, TvColor::Panel, 13);
    const float fill = static_cast<float>(std::clamp(s.fraction, 0.0, 1.0)) * (w - 8.0f);
    if (fill > 1.0f) b.Fill(x + 4, y + 4, std::max(fill, 18.0f), 18, TvColor::Accent, 9);

    struct Tile { std::string label, value, sub; };
    const std::string pics = s.picturesTotal > 0 ? Int(s.picturesDone) + " of " + Int(s.picturesTotal) : std::string("-");
    // groupIndex counts from 1 (0 until the first batch starts)
    std::string picsSub;
    if (s.groupCount > 1 && s.groupIndex > 0) picsSub = "batch " + std::to_string(s.groupIndex) + " of " + std::to_string(s.groupCount);
    else if (s.groupSize > 1) picsSub = Int(s.groupSize) + " painted together";
    if (picsSub.empty() && s.picturesSkipped > 0) picsSub = Int(s.picturesSkipped) + " skipped";
    const std::string strokes = s.strokesTotal > 0 ? Int(s.strokesDone) + " of " + Int(s.strokesTotal) : std::string("-");
    const std::string strokesSub = s.groupSize > 0 ? "flight " + std::to_string(s.flight + 1) + " of 3: " + FlightName(s.flight) : std::string();
    const Tile tiles[6] = {
        { "Pictures done", pics, picsSub },
        { "Strokes per picture", strokes, strokesSub },
        { "Speed", RateText(s.strokesPerSecond), "strokes per second" },
        { "Leftover error", ErrText(s.errorNow), "lower is better" },
        { "Time so far", TimeText(s.seconds), std::string() },
        { "Time left", s.etaSeconds >= 0.0 ? "about " + Duration(s.etaSeconds) : std::string("working it out"), s.etaSeconds >= 0.0 ? "a rough guess" : std::string() }
    };
    const float gap = 16.0f, tw = (w - 2 * gap) / 3.0f, th = 120.0f;
    for (int i = 0; i < 6; ++i) {
        const float tx = x + static_cast<float>(i % 3) * (tw + gap);
        const float ty = y + 46.0f + static_cast<float>(i / 3) * (th + gap);
        b.Fill(tx, ty, tw, th, TvColor::Panel, 14);
        b.Text(tx + 18, ty + 8, tw - 36, 30, tiles[i].label, TvFont::Small, TvColor::Muted);
        const TvFont f = ValueFont(tiles[i].value, tw - 36);
        b.Text(tx + 18, ty + 38, tw - 36, 46, Fit(tiles[i].value, tw - 36, f), f, TvColor::Text);
        b.Text(tx + 18, ty + 84, tw - 36, 30, Fit(tiles[i].sub, tw - 36, TvFont::Small), TvFont::Small, TvColor::Muted);
    }

    // the three flights (where the picture is now), what this flight does, and what Screen Sleep is about to do
    const float py = y + 46.0f + 2 * (th + gap);   // 46 + 2 * 136 = 318 below the bar
    const float ph = kContentBottom - py;
    b.Fill(x, py, w, ph, TvColor::Panel, 14);
    uint32_t split[3] = { 0, 0, 0 };
    SplitStrokes(s.strokesTotal, split);
    const float fgap = 12.0f, fw = (w - 28.0f - 2 * fgap) / 3.0f, fh = 100.0f;
    uint32_t first = 0;
    for (uint32_t f = 0; f < 3; ++f) {
        const float fx = x + 14.0f + static_cast<float>(f) * (fw + fgap), fy = py + 14.0f;
        const bool done = s.strokesTotal > 0 && f < s.flight;
        const bool now = s.strokesTotal > 0 && f == s.flight;
        double share = 0.0;
        if (done) share = 1.0;
        else if (now && split[f] > 0) share = std::clamp((static_cast<double>(s.strokesDone) - static_cast<double>(first)) / static_cast<double>(split[f]), 0.0, 1.0);
        b.Fill(fx, fy, fw, fh, now ? TvColor::PanelRaised : TvColor::Background, 12);
        if (now) b.Outline(fx, fy, fw, fh, TvColor::Good, 2.0f, 12.0f);
        b.Text(fx + 16, fy + 8, fw - 130, 34, FlightName(f), now ? TvFont::BodyBold : TvFont::Body, now ? TvColor::Text : TvColor::Muted);
        b.Text(fx + fw - 16 - 90, fy + 12, 90, 30, done ? "done" : (now ? "now" : "next"), TvFont::Small, (done || now) ? TvColor::Good : TvColor::Muted, TvAlign::Right);
        b.Fill(fx + 16, fy + 52, fw - 32, 10, TvColor::Line, 5);
        if (share > 0.01) b.Fill(fx + 16, fy + 52, std::max(10.0f, (fw - 32) * static_cast<float>(share)), 10, TvColor::Good, 5);
        b.Text(fx + 16, fy + 68, fw - 32, 28, Int(split[f]) + " strokes", TvFont::Small, TvColor::Muted);
        first += split[f];
    }
    b.Text(x + 28, py + 126, w - 56, 34, Fit(FlightAbout(s.flight), w - 56, TvFont::BodyBold), TvFont::BodyBold, TvColor::Text);
    std::string note;
    if (s.state == ConvertState::Stopping) {
        note = "Stopping at the next safe moment. Pictures that are already finished are kept.";
    } else if (s.settings.screenSleep) {
        const int secs = std::max(1, static_cast<int>(std::ceil(m_awakeLeft)));
        note = "Screen Sleep is on: in " + std::to_string(secs) + " s the TV goes dim so the Xbox can paint faster. Press any button to wake it. Press Y to turn Screen Sleep off.";
    } else {
        note = "Screen Sleep is off, so the TV keeps drawing this screen. Press Y to turn it on and let the Xbox paint faster.";
    }
    b.Text(x + 28, py + 164, w - 56, ph - 170, note, TvFont::Small, TvColor::Muted, TvAlign::Left, true);
}

void TvModel::DrawConvertIdle(TvBuilder& b, float x, float y, float w) const {
    const AppSnapshot& s = m_snap;
    const uint32_t chosen = std::min<uint32_t>(s.settings.preset, 3);
    b.Text(x, y, w, 32, "Detail level (Left and Right change it)", TvFont::Small, TvColor::Muted);
    const float gap = 16.0f, cw = (w - 3 * gap) / 4.0f, cy = y + 40.0f, ch = 150.0f;
    for (uint32_t i = 0; i < 4; ++i) {
        const PresetInfo p = PresetFor(static_cast<DetailPreset>(i));
        const float cx = x + static_cast<float>(i) * (cw + gap);
        const bool on = i == chosen;
        b.Fill(cx, cy, cw, ch, on ? TvColor::PanelRaised : TvColor::Panel, 14);
        if (on) b.Outline(cx, cy, cw, ch, TvColor::Accent, 3.0f, 14.0f);
        b.Text(cx, cy + 14, cw, 44, p.name, TvFont::Heading, on ? TvColor::Text : TvColor::Muted, TvAlign::Center);
        b.Text(cx, cy + 68, cw, 36, Int(p.strokes) + " strokes", TvFont::Body, TvColor::Text, TvAlign::Center);
        b.Text(cx, cy + 106, cw, 32, std::to_string(p.fit) + " px", TvFont::Small, TvColor::Muted, TvAlign::Center);
    }
    b.Text(x, cy + ch + 14, w, 76, PresetAbout(chosen), TvFont::Small, TvColor::Muted, TvAlign::Left, true);

    const float py = cy + ch + 100.0f;   // 352 + 40 + 150 + 100 = 642
    DrawLastRun(b, x, py, w, kContentBottom - py);
}

void TvModel::DrawLastRun(TvBuilder& b, float x, float y, float w, float h) const {
    const AppSnapshot& s = m_snap;
    TvColor color = TvColor::Muted;
    std::string title, text;
    if (s.state == ConvertState::Problem) {
        color = TvColor::Bad;
        title = "Converting stopped and needs attention";
        text = (s.problem.empty() ? std::string("Something went wrong.") : s.problem) + " More details are in the log on the phone page.";
    } else if (s.gpu == GpuState::Failed) {
        color = TvColor::Bad;
        title = "The graphics chip can't be used";
        text = s.gpuNote.empty() ? std::string("The graphics chip is not available.") : s.gpuNote;
    } else if (s.gpu != GpuState::Ready) {
        color = TvColor::Warn;
        title = "Starting the graphics chip";
        text = "The chip paints and measures a few test pictures first. Converting can start in a moment.";
    } else if (s.pictureCount == 0) {
        color = TvColor::Warn;
        title = "No pictures yet";
        text = "Add pictures from your phone. Press View to see the address and the code to scan.";
    } else if (!s.picturesPersistent) {
        color = TvColor::Warn;
        title = "Pictures aren't being saved";
        text = "The Xbox couldn't write the pictures to its storage. They work for now but will be gone when the app closes.";
    } else if (!s.last.valid) {
        color = TvColor::Good;
        title = "Ready";
        text = "Press A to convert " + (s.pictureCount == 1 ? std::string("the picture") : "all " + Int(s.pictureCount) + " pictures") + " at " + PresetTitle(s.settings.preset) +
               ". The original and its painting show on the right while it works.";
    } else {
        // the numbers of the last run
        const RunSummary& r = s.last;
        b.Fill(x, y, w, h, TvColor::Panel, 14);
        const std::string head = r.stopped ? "Last run (stopped early)" : "Last run";
        b.Text(x + 28, y + 8, w - 300, 36, head, TvFont::BodyBold, TvColor::Text);
        b.Text(x + w - 280, y + 12, 252, 32, AgoFromUnix(r.finishedUnix), TvFont::Small, TvColor::Muted, TvAlign::Right);
        struct Row { const char* label; std::string value; };
        std::string pictures = Int(r.pictures) + " converted";
        if (r.skipped > 0) pictures += ", " + Int(r.skipped) + " skipped";
        const Row rows[5] = {
            { "Detail", PresetTitle(r.preset) + " (" + Int(r.strokes) + " strokes, " + std::to_string(r.fit) + " px)" },
            { "Pictures", pictures },
            { "Time", TimeText(r.seconds) + ", " + TimeText(r.secondsPerPicture) + " per picture" },
            { "Speed", RateText(r.strokesPerSecond) + " strokes per second" },
            { "Leftover error", ErrText(r.errorAverage) + " on average (best " + ErrText(r.errorBest) + ", worst " + ErrText(r.errorWorst) + ")" }
        };
        const float rowH = (h - 56.0f) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            const float ry = y + 50.0f + static_cast<float>(i) * rowH;
            b.Text(x + 28, ry, 230, rowH, rows[i].label, TvFont::Small, TvColor::Muted);
            b.Text(x + 270, ry, w - 300, rowH, Fit(rows[i].value, w - 300, TvFont::Small), TvFont::Small, TvColor::Text);
        }
        return;
    }
    b.Fill(x, y, w, h, TvColor::Panel, 14);
    b.Fill(x, y + 10, 8, h - 20, color, 4);
    b.Text(x + 32, y + 10, w - 60, 36, Fit(title, w - 60, TvFont::BodyBold), TvFont::BodyBold, TvColor::Text);
    b.Text(x + 32, y + 50, w - 60, h - 56, text, TvFont::Small, TvColor::Muted, TvAlign::Left, true);
}

void TvModel::DrawLivePanel(TvBuilder& b, float x, float y, float w, float h) {
    b.Fill(x, y, w, h, TvColor::Panel, 16);
    const float pad = 20.0f, gap = 12.0f;
    const float side = (w - 2 * pad - gap) / 2.0f;   // 374
    const float ix = x + pad;
    // the block (captions, two pictures, two lines of text) is about 600 high: it sits in the middle of the panel
    const float off = std::max(0.0f, (h - 640.0f) / 2.0f);
    const float cy = y + 16.0f + off, iy = y + 62.0f + off;
    const bool run = Converting();

    std::shared_ptr<const Rgb8Image> orig, paint;
    std::string captionA = "Original", captionB = "Painted so far";
    std::string line1, line2;
    uint64_t keyO = 0, keyP = 0;
    if (run && m_live && m_live->original.Valid() && m_live->painted.Valid()) {
        orig = std::shared_ptr<const Rgb8Image>(m_live, &m_live->original);
        paint = std::shared_ptr<const Rgb8Image>(m_live, &m_live->painted);
        keyO = KeyHash("live:" + std::to_string(m_live->version) + ":o");
        keyP = KeyHash("live:" + std::to_string(m_live->version) + ":p");
        line1 = Fit(m_live->name, w - 2 * pad, TvFont::BodyBold);
        line2 = "Picture " + std::to_string(m_live->index + 1) + " of " + std::to_string(m_live->groupSize) + " in this batch. Stroke " + Int(m_live->strokesDone) + " of " +
                Int(m_live->strokesTotal) + ", leftover error " + ErrText(m_live->error) + ".";
    } else if (!run && !m_cards.empty()) {
        const CardInfo& c = m_cards.front();
        orig = BestTile(c, kMidSize, kSmallSize, true);
        paint = BestTile(c, kMidSize, kSmallSize, false);
        keyO = KeyHash("o:" + c.id + (orig && orig->w == kMidSize ? ":m" : ":s"));
        keyP = KeyHash("p:" + c.id + (paint && paint->w == kMidSize ? ":m" : ":s"));
        captionA = "Original";
        captionB = "Painted from the dials";
        line1 = "Latest result: " + Fit(c.name, w - 2 * pad - 220.0f, TvFont::BodyBold);
        line2 = PresetTitle(c.preset) + ", " + Int(c.key.strokes) + " strokes. Leftover error " + ErrText(c.errorRms) + ". Open Results to look closer.";
    }
    b.Text(ix, cy, side, 32, captionA, TvFont::Small, TvColor::Muted);
    b.Text(ix + side + gap, cy, side, 32, captionB, TvFont::Small, TvColor::Muted);
    if (orig && paint) {
        b.Image(ix, iy, side, side, orig, keyO, true);
        b.Image(ix + side + gap, iy, side, side, paint, keyP, true);
        b.Outline(ix - 1, iy - 1, side + 2, side + 2, TvColor::Line, 2.0f);
        b.Outline(ix + side + gap - 1, iy - 1, side + 2, side + 2, TvColor::Line, 2.0f);
        b.Text(ix, iy + side + 20, w - 2 * pad, 40, line1, TvFont::BodyBold, TvColor::Text);
        b.Text(ix, iy + side + 62, w - 2 * pad, 100, line2, TvFont::Small, TvColor::Muted, TvAlign::Left, true);
        return;
    }
    // nothing to show yet: two empty frames, and the reason underneath
    b.Outline(ix, iy, side, side, TvColor::Line, 2.0f, 12.0f);
    b.Outline(ix + side + gap, iy, side, side, TvColor::Line, 2.0f, 12.0f);
    const std::string msg = run ? "The first picture appears in a moment."
                                : "Pictures you convert show up here: the original on the left and its painting on the right.";
    b.Text(ix, iy + side + 24, w - 2 * pad, 110, msg, TvFont::Body, TvColor::Muted, TvAlign::Left, true);
}

// ---- Results ---------------------------------------------------------------------------------
void TvModel::DrawResults(TvBuilder& b) {
    const int tab = static_cast<int>(TvTab::Results);
    const int n = static_cast<int>(m_cards.size());
    b.Text(kMarginX, kContentTop - 6, 1000, 46, "Finished pictures", TvFont::Heading, TvColor::Text);
    if (n == 0) {
        b.Fill(kMarginX, 230, kRightEdge - kMarginX, 640, TvColor::Panel, 16);
        b.Text(kMarginX + 140, 400, kRightEdge - kMarginX - 280, 300,
               "Nothing converted yet. Go to the Convert screen and press A. Every finished picture shows up here next to its original, newest first.",
               TvFont::Body, TvColor::Muted, TvAlign::Center, true);
        return;
    }
    b.Text(kMarginX, kContentTop + 38, 1600, 34, Int(static_cast<uint64_t>(n)) + (n == 1 ? " finished picture, " : " finished pictures, ") + Bytes(m_snap.cardBytes) + ". Newest first. Original on the left, painting on the right.",
           TvFont::Small, TvColor::Muted);

    const float tileW = 310.0f, tileH = 216.0f, gapX = 24.0f, rowPitch = 232.0f, img = 140.0f;
    const float totalW = kResultColumns * tileW + (kResultColumns - 1) * gapX;
    const float startX = kMarginX + (kRightEdge - kMarginX - totalW) / 2.0f;
    const float startY = 240.0f;
    const int firstRow = m_scroll[tab];
    for (int r = 0; r < kResultRows; ++r) {
        for (int c = 0; c < kResultColumns; ++c) {
            const int idx = (firstRow + r) * kResultColumns + c;
            if (idx >= n) break;
            const CardInfo& card = m_cards[static_cast<size_t>(idx)];
            const float x = startX + static_cast<float>(c) * (tileW + gapX);
            const float y = startY + static_cast<float>(r) * rowPitch;
            b.Fill(x, y, tileW, tileH, TvColor::Panel, 12);
            const std::shared_ptr<const Rgb8Image> o = Tile(card, kSmallSize, true);
            const std::shared_ptr<const Rgb8Image> p = Tile(card, kSmallSize, false);
            if (o) b.Image(x + 12, y + 12, img, img, o, KeyHash("o:" + card.id + ":" + std::to_string(kSmallSize)), true);
            else b.Fill(x + 12, y + 12, img, img, TvColor::Line, 4);
            if (p) b.Image(x + 12 + img + 6, y + 12, img, img, p, KeyHash("p:" + card.id + ":" + std::to_string(kSmallSize)), true);
            else b.Fill(x + 12 + img + 6, y + 12, img, img, TvColor::Line, 4);
            b.Text(x + 12, y + 156, tileW - 24, 30, Fit(card.name, tileW - 24, TvFont::BodyBold), TvFont::BodyBold, TvColor::Text);
            b.Text(x + 12, y + 186, tileW - 24, 26, Fit(PresetTitle(card.preset) + ", error " + ErrText(card.errorRms), tileW - 24, TvFont::Small), TvFont::Small, TvColor::Muted);
            if (idx == m_focus[tab]) b.Outline(x - 3, y - 3, tileW + 6, tileH + 6, TvColor::Accent, 4.0f, 14.0f);
        }
    }
    const int rows = (n + kResultColumns - 1) / kResultColumns;
    if (rows > kResultRows) {
        const float trackY = startY, trackH = kResultRows * rowPitch - 16.0f, tx = kRightEdge - 8.0f;
        b.Fill(tx, trackY, 6, trackH, TvColor::Line, 3);
        const float thumbH = std::max(40.0f, trackH * static_cast<float>(kResultRows) / static_cast<float>(rows));
        const float thumbY = trackY + (trackH - thumbH) * static_cast<float>(firstRow) / static_cast<float>(std::max(1, rows - kResultRows));
        b.Fill(tx, thumbY, 6, thumbH, TvColor::Muted, 3);
    }
}

void TvModel::DrawViewer(TvBuilder& b) {
    b.Fill(0, 0, kTvWidth, kTvHeight, TvColor::Shade);
    const int f = m_focus[static_cast<int>(TvTab::Results)];
    if (f < 0 || f >= static_cast<int>(m_cards.size())) return;
    const CardInfo& c = m_cards[static_cast<size_t>(f)];
    const float px = 260, py = 40, pw = 1400, ph = 1000;
    b.Fill(px, py, pw, ph, TvColor::Panel, 20);
    b.Outline(px, py, pw, ph, TvColor::Line, 2.0f, 20.0f);
    b.Text(px + 40, py + 20, pw - 440, 50, Fit(c.name, pw - 440, TvFont::Heading), TvFont::Heading, TvColor::Text);
    b.Text(px + pw - 380, py + 28, 340, 36, "Picture " + std::to_string(f + 1) + " of " + std::to_string(m_cards.size()), TvFont::Small, TvColor::Muted, TvAlign::Right);

    const float side = 640.0f, iy = py + 84.0f;
    const float ox = px + 40.0f, pxx = px + 40.0f + side + 40.0f;
    const std::shared_ptr<const Rgb8Image> bigO = Tile(c, kBigSize, true);
    const std::shared_ptr<const Rgb8Image> bigP = Tile(c, kBigSize, false);
    const std::shared_ptr<const Rgb8Image> o = bigO ? bigO : Tile(c, kSmallSize, true);
    const std::shared_ptr<const Rgb8Image> p = bigP ? bigP : Tile(c, kSmallSize, false);
    if (o) b.Image(ox, iy, side, side, o, KeyHash("o:" + c.id + (bigO ? ":big" : ":small")), true);
    else b.Fill(ox, iy, side, side, TvColor::Line, 4);
    if (p) b.Image(pxx, iy, side, side, p, KeyHash("p:" + c.id + (bigP ? ":big" : ":small")), true);
    else b.Fill(pxx, iy, side, side, TvColor::Line, 4);
    b.Outline(ox - 1, iy - 1, side + 2, side + 2, TvColor::Line, 2.0f);
    b.Outline(pxx - 1, iy - 1, side + 2, side + 2, TvColor::Line, 2.0f);

    b.Text(ox, iy + side + 8, side, 34, "Original", TvFont::Small, TvColor::Muted);
    const bool sharper = !bigP;
    b.Text(pxx, iy + side + 8, side, 34, sharper ? "Painted from the dials (drawing a sharper copy...)" : "Painted from the dials, at 512 px", TvFont::Small, sharper ? TvColor::Warn : TvColor::Muted);

    const float ty = iy + side + 50.0f;
    b.Text(ox, ty, pw - 80, 38,
           PresetTitle(c.preset) + ": " + Int(c.key.strokes) + " strokes (Wash " + Int(c.perFlight[0]) + ", Form " + Int(c.perFlight[1]) + ", Detail " + Int(c.perFlight[2]) + ")",
           TvFont::BodyBold, TvColor::Text);
    std::string numbers = "Leftover error " + ErrText(c.errorRms) + " (0 to 255, lower is better). ";
    numbers += c.emptyStrokes == 0 ? std::string("No empty strokes. ") : Int(c.emptyStrokes) + " empty strokes. ";
    numbers += "Made in " + TimeText(c.seconds) + " at " + RateText(c.strokesPerSecond) + " strokes per second";
    if (c.groupSize > 1) numbers += " (a batch of " + std::to_string(c.groupSize) + " pictures)";
    numbers += ". " + AgoFromUnix(c.createdUnix);
    b.Text(ox, ty + 42, pw - 80, 70, numbers, TvFont::Small, TvColor::Muted, TvAlign::Left, true);

    float hx = px + 40;
    const float hy = py + ph - 62;
    hx = b.Hint(hx, hy, "B", TvColor::Bad, "Back");
    hx = b.Hint(hx, hy, "X", TvColor::ChartLine, "Delete");
    b.Chip(hx, hy, "Left  Right", "Other pictures");
}

// ---- Settings --------------------------------------------------------------------------------
void TvModel::DrawSettings(TvBuilder& b) const {
    const int tab = static_cast<int>(TvTab::Settings);
    const int count = static_cast<int>(m_rows.size());
    const float lx = kMarginX, lw = 1040.0f, rowH = 72.0f;
    b.Fill(lx, kContentTop, lw, kContentBottom - kContentTop, TvColor::Panel, 16);
    const int first = m_scroll[tab];
    for (int i = 0; i < kSettingsVisible; ++i) {
        const int idx = first + i;
        if (idx >= count) break;
        const SettingRow& row = m_rows[static_cast<size_t>(idx)];
        const float y = kContentTop + 18 + static_cast<float>(i) * rowH;
        const bool on = idx == m_focus[tab];
        if (on) {
            b.Fill(lx + 14, y, lw - 52, rowH - 6, TvColor::PanelRaised, 12);
            b.Outline(lx + 14, y, lw - 52, rowH - 6, TvColor::Accent, 3.0f, 12.0f);
        }
        b.Text(lx + 40, y + 16, 440, 40, Fit(row.label, 440, TvFont::Body), on ? TvFont::BodyBold : TvFont::Body, row.danger ? TvColor::Warn : TvColor::Text);
        const std::string value = row.value();
        if (row.kind == SettingRow::Kind::Toggle) {
            const bool isOn = value == "On";
            b.Fill(lx + lw - 52 - 150, y + 14, 120, 40, isOn ? TvColor::Good : TvColor::Line, 20);
            b.Text(lx + lw - 52 - 150, y + 18, 120, 34, value, TvFont::Small, isOn ? TvColor::OnAccent : TvColor::Text, TvAlign::Center);
        } else {
            b.Text(lx + 480, y + 16, lw - 52 - 480 - 30, 40, Fit(value, lw - 52 - 510, TvFont::Body), TvFont::Body, on ? TvColor::Text : TvColor::Muted, TvAlign::Right);
        }
    }
    if (count > kSettingsVisible) {
        const float trackY = kContentTop + 18, trackH = kSettingsVisible * rowH - 6;
        b.Fill(lx + lw - 24, trackY, 6, trackH, TvColor::Line, 3);
        const float thumbH = std::max(40.0f, trackH * static_cast<float>(kSettingsVisible) / static_cast<float>(count));
        const float room = trackH - thumbH;
        const float thumbY = trackY + room * static_cast<float>(first) / static_cast<float>(std::max(1, count - kSettingsVisible));
        b.Fill(lx + lw - 24, thumbY, 6, thumbH, TvColor::Muted, 3);
    }

    // explanation of the highlighted setting
    const float rx = lx + lw + 32, rw = kRightEdge - rx;
    b.Fill(rx, kContentTop, rw, kContentBottom - kContentTop, TvColor::Panel, 16);
    const int f = m_focus[tab];
    if (f >= 0 && f < count) {
        const SettingRow& row = m_rows[static_cast<size_t>(f)];
        b.Text(rx + 32, kContentTop + 20, rw - 64, 32, "About this setting", TvFont::Small, TvColor::Muted);
        b.Text(rx + 32, kContentTop + 56, rw - 64, 50, Fit(row.label, rw - 64, TvFont::Heading), TvFont::Heading, TvColor::Text);
        const bool selfTest = row.label == "Run the self-test again";
        b.Text(rx + 32, kContentTop + 112, rw - 64, selfTest ? 250.0f : 470.0f, row.help, TvFont::Small, TvColor::Muted, TvAlign::Left, true);
        if (selfTest && !m_snap.gpuReport.empty()) {
            b.Text(rx + 32, kContentTop + 372, rw - 64, 30, "Last self-test", TvFont::Small, TvColor::Muted);
            b.Text(rx + 32, kContentTop + 404, rw - 64, 300, CompactReport(m_snap.gpuReport), TvFont::Small, TvColor::Text, TvAlign::Left, true);
        }
        const char* how = row.kind == SettingRow::Kind::Toggle ? "Press A, Left or Right to switch it."
                        : row.kind == SettingRow::Kind::Choice ? "Press Left or Right to change it. A moves to the next value."
                        : "Press A to do it.";
        b.Text(rx + 32, kContentBottom - 70, rw - 64, 56, how, TvFont::Small, TvColor::Accent, TvAlign::Left, true);
    }
}

// ---- Phone -----------------------------------------------------------------------------------
void TvModel::DrawPhone(TvBuilder& b) const {
    b.Text(kMarginX, kContentTop, 1100, 60, "Control it from your phone", TvFont::Title, TvColor::Text);
    const bool haveNet = !m_snap.lanUrl.empty() && m_snap.lanUrl.rfind("http://127.0.0.1", 0) != 0;
    const float lw = 1000.0f;
    b.Text(kMarginX, kContentTop + 90, lw, 360,
           "1.  Connect your phone to the same Wi-Fi as this Xbox.\n\n"
           "2.  Point your phone's camera at the code, or type the address below into your phone's browser.\n\n"
           "3.  On the page you can add pictures, convert them, and compare every original with its painting.",
           TvFont::Body, TvColor::Text, TvAlign::Left, true);
    b.Fill(kMarginX, 640, lw, 130, TvColor::Panel, 16);
    if (haveNet) {
        b.Text(kMarginX + 30, 650, lw - 60, 34, "Address", TvFont::Small, TvColor::Muted);
        b.Text(kMarginX + 30, 686, lw - 60, 70, m_snap.lanUrl, TvFont::Title, TvColor::Accent);
    } else {
        b.Text(kMarginX + 30, 654, lw - 60, 100, "The Xbox is not connected to a network yet. Connect it to Wi-Fi or a network cable, then this screen shows the address.",
               TvFont::Body, TvColor::Warn, TvAlign::Left, true);
    }
    b.Text(kMarginX, 800, lw, 100, "Anyone on your Wi-Fi who knows this address can open the page, so use it on a network you trust. Keep this app open on the TV while it converts.",
           TvFont::Small, TvColor::Muted, TvAlign::Left, true);

    const float qx = 1210.0f, qy = kContentTop + 30, qs = 560.0f;
    b.Fill(qx - 20, qy - 20, qs + 40, qs + 40, TvColor::Text, 20);
    if (haveNet && m_qrImage && m_qrImage->Valid()) {
        b.Image(qx, qy, qs, qs, m_qrImage, m_qrKey);
    } else {
        b.Text(qx, qy + 200, qs, 160, "No code yet", TvFont::Heading, TvColor::Background, TvAlign::Center);
    }
}

// ---- footer, messages, questions ---------------------------------------------------------------
void TvModel::DrawFooter(TvBuilder& b) const {
    b.Fill(kMarginX, 950, kRightEdge - kMarginX, 2, TvColor::Line);
    const float y = 972.0f;

    struct Item {
        bool chip;
        std::string key;
        TvColor color;
        std::string label;
        bool optional;
    };
    std::vector<Item> items;
    auto hint = [&](const char* key, TvColor color, const std::string& label, bool optional = false) { items.push_back({ false, key, color, label, optional }); };
    auto chip = [&](const char* key, const std::string& label, bool optional = false) { items.push_back({ true, key, TvColor::Muted, label, optional }); };

    switch (static_cast<TvTab>(m_tab)) {
        case TvTab::Convert:
            if (m_snap.state == ConvertState::Converting) {
                hint("A", TvColor::Bad, "Stop");
            } else if (m_snap.state == ConvertState::Stopping) {
                hint("A", TvColor::Warn, "Stopping...");
            } else {
                hint("A", TvColor::Good, m_snap.pictureCount == 0 ? std::string("Convert")
                                          : (m_snap.pictureCount == 1 ? std::string("Convert the picture") : "Convert " + Int(m_snap.pictureCount) + " pictures"));
                if (m_snap.cardCount > 0 && m_snap.pictureCount > 0) hint("X", TvColor::ChartLine, "Convert again", true);
            }
            hint("Y", TvColor::Warn, "Screen Sleep");
            if (!Converting()) chip("Left Right", "Detail", true);
            chip("LB  RB", "Switch screen");
            break;
        case TvTab::Results:
            if (!m_cards.empty()) {
                hint("A", TvColor::Good, "Look closer");
                hint("X", TvColor::ChartLine, "Delete");
                chip("D-pad", "Choose", true);
            }
            chip("LB  RB", "Switch screen");
            break;
        case TvTab::Settings:
            chip("D-pad", "Choose and change", true);
            hint("A", TvColor::Good, "Apply");
            chip("LB  RB", "Switch screen");
            break;
        case TvTab::Phone:
            chip("LB  RB", "Switch screen");
            chip("View", "Back to Convert");
            break;
    }

    // everything has to fit between the margins: the hints marked optional go first, the last one first
    const float room = kRightEdge - kMarginX;
    auto total = [&]() {
        float t = 0.0f;
        for (const Item& it : items) t += it.chip ? ChipAdvance(it.key, it.label) : HintAdvance(it.label);
        return t - 36.0f;
    };
    while (total() > room) {
        size_t drop = items.size();
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].optional) drop = i;
        }
        if (drop == items.size()) break;
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    float x = kMarginX;
    for (const Item& it : items) {
        x = it.chip ? b.Chip(x, y, it.key, it.label) : b.Hint(x, y, it.key, it.color, it.label);
    }
    if (m_tab != static_cast<int>(TvTab::Phone) && !m_snap.lanUrl.empty()) {
        // the phone address at the right end, only when the whole address fits next to the hints
        const std::string url = "Phone: " + m_snap.lanUrl;
        const float need = TextWidth(url, TvFont::Small) * 1.1f + 20.0f;
        if (kRightEdge - (x - 36.0f) >= need + 40.0f) {
            b.Text(kRightEdge - need, y + 6, need, 40, url, TvFont::Small, TvColor::Muted, TvAlign::Right);
        }
    }
}

void TvModel::DrawToasts(TvBuilder& b) const {
    // Small notes in the bottom right corner (newest at the bottom). They sit over the least important part of every screen.
    const float w = 700.0f;
    const float x = kRightEdge - w;
    const float textW = w - 60.0f;
    const size_t perLine = static_cast<size_t>(textW / (TvFontSize(TvFont::Small) * 0.5f));   // letters on one line, about
    float bottom = kContentBottom - 8.0f;
    int shown = 0;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend() && shown < 3; ++it, ++shown) {
        TvColor c = TvColor::Accent;
        if (it->level == "good") c = TvColor::Good;
        else if (it->level == "warn") c = TvColor::Warn;
        else if (it->level == "error") c = TvColor::Bad;
        else if (it->level == "info") c = TvColor::ChartLine;
        // one, two or three lines; anything longer than three is cut
        const size_t lines = std::clamp<size_t>((it->text.size() + perLine - 1) / perLine, 1, 3);
        const float h = 30.0f + 32.0f * static_cast<float>(lines);
        const float y = bottom - h;
        b.Fill(x, y, w, h, TvColor::PanelRaised, 14);
        b.Outline(x, y, w, h, c, 2.0f, 14.0f);
        b.Fill(x, y + 10, 8, h - 20, c, 4);
        b.Text(x + 30, y + 14, textW, h - 20, Fit(it->text, textW * static_cast<float>(lines) - 24.0f, TvFont::Small), TvFont::Small, TvColor::Text, TvAlign::Left, lines > 1);
        bottom = y - 10.0f;
    }
}

void TvModel::DrawDialog(TvBuilder& b) const {
    b.Fill(0, 0, kTvWidth, kTvHeight, TvColor::Shade);
    const float w = 1100.0f;
    std::string body;
    // how tall the text will be: about 60 letters per line, plus one empty line between paragraphs (a little generous on purpose)
    size_t bodyLines = 0;
    for (size_t i = 0; i < m_dialog.lines.size(); ++i) {
        if (i > 0) { body += "\n\n"; bodyLines += 1; }
        body += m_dialog.lines[i];
        bodyLines += std::max<size_t>(1, (m_dialog.lines[i].size() + 59) / 60);
    }
    const float lineH = TvFontSize(TvFont::Body) * 1.4f;
    const float bodyH = static_cast<float>(bodyLines) * lineH;
    const float h = std::clamp(260.0f + bodyH, 340.0f, 800.0f);
    const float x = (kTvWidth - w) / 2.0f, y = (kTvHeight - h) / 2.0f;
    b.Fill(x, y, w, h, TvColor::Panel, 20);
    b.Outline(x, y, w, h, m_dialog.danger ? TvColor::Bad : TvColor::Accent, 3.0f, 20.0f);
    b.Text(x + 48, y + 36, w - 96, 100, m_dialog.title, TvFont::Heading, TvColor::Text, TvAlign::Left, true);
    b.Text(x + 48, y + 110, w - 96, h - 110 - 110, body, TvFont::Body, TvColor::Muted, TvAlign::Left, true);
    float hx = x + 48;
    const float hy = y + h - 80;
    hx = b.Hint(hx, hy, "A", m_dialog.danger ? TvColor::Bad : TvColor::Good, m_dialog.yesLabel);
    b.Hint(hx, hy, "B", TvColor::Muted, "Cancel");
}

} // namespace XboxOTFM
