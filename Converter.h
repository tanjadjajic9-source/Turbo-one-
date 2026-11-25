#pragma once
// The converter: turns pictures into cards of dials, on the graphics chip. No learning, no randomness that cannot be repeated.
//
// For every picture it starts from the average colour (the 3 header dials) and paints strokes one after the other, in three flights:
//   Wash   (20 % of the strokes)  big soft strokes
//   Form   (30 %)                 medium strokes
//   Detail (50 %)                 small crisp strokes, turned along the edges
// Every flight aims at the sharp picture itself: tests on real photos showed that aiming the first two flights at blurred copies left
// clearly MORE error at the end (see FlightFor in Converter.cpp). What differs between the flights is the stroke size window.
// Every stroke is made in four moves: Scout (K candidate strokes where the canvas is still wrong), Artist (every candidate gets its best
// colour in closed form and is scored: the change in error), Pick (the best), Tuner (R rounds: nudge one dial of the best stroke, score,
// pick again), then Commit (paint it, write its 12 dials into the card). A stroke that would not lower the error is written as an empty
// stroke. Every candidate is snapped to the 0..127 grid BEFORE it is scored, so what is measured is what is stored.
//
// The chip programs are in Strokes.hlsl; GpuCore.h has the chip plumbing; StrokeCodec.h the dial table and a processor painter.

#include "GpuCore.h"
#include "ImageUtil.h"
#include "StrokeCodec.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace XboxOTFM {

enum class DetailPreset : uint32_t { Low = 0, Medium = 1, High = 2, Ultra = 3 };

struct PresetInfo {
    const char* name;
    uint32_t strokes; // strokes on the card
    uint32_t fit;     // pictures are fitted to this many pixels on a side while converting
};
PresetInfo PresetFor(DetailPreset p);
const char* PresetName(uint32_t index);

// Where Scout starts its search in each flight (dial units, 0..127). The Tuner may nudge outside these windows.
struct FlightWindow {
    float sizeLo, sizeHi;
    float aspectLo, aspectHi;
    float softLo, softHi;
    float opacityLo, opacityHi;
    float blurFraction;   // blur of the target, as a share of the picture side (0 = the sharp picture)
    bool followEdges;     // the angle follows the edge direction
    uint32_t slices;      // threads that share one candidate's pixels
};
const FlightWindow& FlightFor(uint32_t flight);
// For the developer tests: replace the three flight windows (nullptr brings the built-in ones back).
void SetFlightWindowsForTests(const FlightWindow* three);
// 20 % / 30 % / 50 % of `total`, as whole strokes (they add up to `total`).
void SplitStrokes(uint32_t total, uint32_t out[3]);

struct ConvertParams {
    uint32_t strokes = 1500;   // strokes per card
    uint32_t fit = 96;         // 64, 96 or 128 (a multiple of 8)
    uint32_t seed = 1;         // every random choice is a hash of this: the same seed gives the same cards
    uint32_t candidates = 64;  // K: candidate strokes per stroke
    uint32_t rounds = 3;       // R: tuning rounds per stroke
    uint32_t chunk = 0;        // strokes between two checks (0 = chosen by speed)
};

struct PictureIn {
    uint64_t id = 0;
    Rgb8Image image;           // any size; it is fitted to ConvertParams::fit
};

struct PictureOut {
    uint64_t id = 0;
    std::vector<uint8_t> dials;       // 3 + 12 * strokes numbers 0..127, one byte each
    uint32_t strokes = 0;
    uint32_t perFlight[3] = { 0, 0, 0 };
    uint32_t emptyStrokes = 0;        // strokes that could not lower the error (written empty)
    double errorRms = 0.0;            // leftover error against the fitted picture, root-mean-square on the 0..255 scale
    uint32_t fit = 0;
    Rgb8Image canvas;                 // what the chip painted (fit x fit), for the live view and the tests
};

struct ConvertProgress {
    uint32_t flight = 0;              // 0 Wash, 1 Form, 2 Detail
    uint32_t strokesDone = 0;         // per picture, in this group
    uint32_t strokesTotal = 0;
    uint32_t pictures = 0;            // in this group
    double errorNow = 0.0;            // average leftover error of the group (0..255 scale)
    double strokesPerSecond = 0.0;    // strokes per second, all pictures of the group together
    double seconds = 0.0;             // since the group started
};
// Return false to stop at once (the group is abandoned).
using ProgressFn = std::function<bool(const ConvertProgress&)>;

struct ConverterTestAccess;

class Converter {
    friend struct ConverterTestAccess;
public:
    explicit Converter(GpuCore& gpu);
    ~Converter();
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    // How many pictures fit in `budgetBytes` of chip memory at once (at least 1, at most `cap`).
    static uint32_t MaxPictures(const ConvertParams& params, uint64_t budgetBytes, uint32_t cap = 32);
    static uint64_t BytesPerPicture(const ConvertParams& params);

    // Converts the pictures as one group (they share the chip, so more pictures at once = faster per picture). Returns S_OK, E_ABORT when
    // the progress function asked to stop, or an error (the chip then needs a fresh Free()/start before the next try).
    HRESULT Convert(const std::vector<PictureIn>& pictures, const ConvertParams& params, std::vector<PictureOut>& out,
                    const ProgressFn& progress = ProgressFn());

    // The canvas of picture `index` of the running group as it is right now (only from inside the progress function).
    bool ReadCanvas(uint32_t index, Rgb8Image& out);

    // Chip memory of the current buffers.
    uint64_t MemoryBytes() const { return m_gpu.BytesInUse(); }
    void Free();

    // The start-up self-test (five checks). The report has one line per check. S_OK only when all five pass.
    HRESULT SelfTest(std::string& report);

    // For the developer tests.
    void SetChunkWorkForTests(double ops) { m_chunkWork = ops; }
    std::vector<double>* TraceForTests = nullptr;        // when set, the error against the target after every stroke is appended
    uint64_t LaunchCountForTests() const { return m_launches; }

private:
    struct Buffers {
        GpuBuffer ref, tgt, tmp, canvas, paint, edge, tiles, cand, part, best, cards, stats;
    };
    struct Layout {
        uint32_t fit = 0, strokes = 0, cands = 0, capacity = 0;
        bool operator==(const Layout& o) const { return fit == o.fit && strokes == o.strokes && cands == o.cands && capacity >= o.capacity; }
    };

    HRESULT EnsureBuffers(const ConvertParams& params, uint32_t pictures);
    HRESULT Launch(GpuDispatch& d);
    template <class Fn> HRESULT ForChunks(uint32_t count, double workPerPicture, Fn fn);
    StrokeConstants BaseConstants(uint32_t first, uint32_t n) const;
    GpuDispatch Make(GpuKernel kernel, uint32_t first, uint32_t n, uint32_t mode) const;

    HRESULT SetupGroup(uint32_t count);
    HRESULT SetupFlight(uint32_t flight, uint32_t count);
    HRESULT RecordScout(uint32_t flight, uint32_t stroke, uint32_t count);
    HRESULT RecordStroke(uint32_t flight, uint32_t stroke, uint32_t count);
    HRESULT RecordEvaluate(uint32_t flight, uint32_t count, uint32_t round, uint32_t stroke);
    HRESULT RecordPick(uint32_t count);
    HRESULT RecordNudge(uint32_t round, uint32_t stroke, uint32_t count);
    HRESULT MeasureGroup(uint32_t count, std::vector<float>& stats);
    HRESULT PaintCards(uint32_t count, uint32_t outSize, uint32_t strokeEnd);
    double WorkPerCandidatePixels(uint32_t flight) const;

    GpuCore& m_gpu;
    Buffers m_buf;
    Layout m_layout;
    ConvertParams m_params;
    uint32_t m_flight = 0;
    uint32_t m_seedRun = 1;
    double m_chunkWork = 1.2e9;
    uint64_t m_launches = 0;
    bool m_valid = false;
    bool m_selfTestMode = false;
};

} // namespace XboxOTFM
