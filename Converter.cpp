#include "Converter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace XboxOTFM {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kRecordFloats = 32;   // one candidate / best record (see Strokes.hlsl)
constexpr uint32_t kMaxSlices = 16;      // part buffer holds this many slices per candidate
constexpr double kTargetChunkSeconds = 0.15;

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

uint32_t SeedFor(uint32_t seed) {
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

Rgb8Image ToRgb8(const float* rgb, uint32_t size) {
    Rgb8Image img;
    img.w = size;
    img.h = size;
    img.px.resize(static_cast<size_t>(size) * size * 3);
    for (size_t i = 0; i < img.px.size(); ++i) {
        const float v = std::min(1.0f, std::max(0.0f, rgb[i]));
        img.px[i] = static_cast<uint8_t>(std::floor(v * 255.0f + 0.5f));
    }
    return img;
}

std::string Fmt(const char* format, double a = 0, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), format, a, b, c);
    return buf;
}

} // namespace

PresetInfo PresetFor(DetailPreset p) {
    switch (p) {
        case DetailPreset::Low: return { "Low", 500, 64 };
        case DetailPreset::Medium: return { "Medium", 1500, 96 };
        case DetailPreset::High: return { "High", 3000, 128 };
        default: return { "Ultra", 6000, 128 };
    }
}

const char* PresetName(uint32_t index) {
    return PresetFor(static_cast<DetailPreset>(std::min<uint32_t>(index, 3))).name;
}

namespace {
FlightWindow g_testFlights[3];
bool g_useTestFlights = false;
} // namespace

void SetFlightWindowsForTests(const FlightWindow* three) {
    g_useTestFlights = three != nullptr;
    if (three) for (int i = 0; i < 3; ++i) g_testFlights[i] = three[i];
}

const FlightWindow& FlightFor(uint32_t flight) {
    // The numbers come from tests on real photos (tests/exp_quality.cpp): a blurred aim made the final error clearly WORSE (Low: 18 % of the
    // plain-background error left with blur, 14 % without; High: 11.7 % against 8 %), so every flight aims at the sharp picture
    // (blurFraction 0). What makes the flights different is the stroke size, and the Detail flight turns its strokes along the edges.
    static const FlightWindow kFlights[3] = {
        //  size         aspect       softness     opacity      blur    edges  slices
        { 70.0f, 115.0f, 64.0f, 127.0f, 64.0f, 127.0f, 90.0f, 127.0f, 0.000f, false, 16 },   // Wash
        { 48.0f, 92.0f,  32.0f, 127.0f, 32.0f, 100.0f, 80.0f, 127.0f, 0.000f, false, 8 },    // Form
        { 20.0f, 80.0f,  0.0f,  127.0f, 0.0f,  48.0f,  90.0f, 127.0f, 0.000f, true,  4 },    // Detail
    };
    if (g_useTestFlights) return g_testFlights[flight > 2 ? 2 : flight];
    return kFlights[flight > 2 ? 2 : flight];
}

void SplitStrokes(uint32_t total, uint32_t out[3]) {
    out[0] = static_cast<uint32_t>(std::floor(0.2 * total + 0.5));
    out[1] = static_cast<uint32_t>(std::floor(0.3 * total + 0.5));
    if (out[0] + out[1] > total) out[1] = total - std::min(total, out[0]);
    out[2] = total - out[0] - out[1];
}

// =================================================================================================
// Memory
// =================================================================================================

uint64_t Converter::BytesPerPicture(const ConvertParams& p) {
    const uint64_t f2 = static_cast<uint64_t>(p.fit) * p.fit;
    const uint64_t tiles = static_cast<uint64_t>(p.fit / 8) * (p.fit / 8);
    uint64_t bytes = 5ull * f2 * 3 * 4;                                   // ref, target, scratch, canvas, paint
    bytes += f2 * 2 * 4 + tiles * 4;                                      // edges, tile sums
    bytes += static_cast<uint64_t>(p.candidates) * kRecordFloats * 4;     // candidates
    bytes += static_cast<uint64_t>(p.candidates) * kMaxSlices * 4 * 4;    // partial sums
    bytes += kRecordFloats * 4 + 8 * 4;                                   // best, stats
    bytes += static_cast<uint64_t>(StrokeCodec::CardDials(p.strokes)) * 4;
    return bytes + 4096;
}

uint32_t Converter::MaxPictures(const ConvertParams& p, uint64_t budgetBytes, uint32_t cap) {
    const uint64_t per = std::max<uint64_t>(1, BytesPerPicture(p));
    return static_cast<uint32_t>(std::max<uint64_t>(1, std::min<uint64_t>(cap, budgetBytes / per)));
}

Converter::Converter(GpuCore& gpu) : m_gpu(gpu) {}

Converter::~Converter() {
    Free();
}

void Converter::Free() {
    GpuBuffer* all[] = { &m_buf.ref, &m_buf.tgt, &m_buf.tmp, &m_buf.canvas, &m_buf.paint, &m_buf.edge,
                         &m_buf.tiles, &m_buf.cand, &m_buf.part, &m_buf.best, &m_buf.cards, &m_buf.stats };
    for (GpuBuffer* b : all) m_gpu.DestroyBuffer(*b);
    m_layout = Layout();
    m_valid = false;
}

HRESULT Converter::EnsureBuffers(const ConvertParams& p, uint32_t pictures) {
    Layout want;
    want.fit = p.fit;
    want.strokes = p.strokes;
    want.cands = p.candidates;
    want.capacity = pictures;
    if (m_valid && m_layout == want) return S_OK;
    Free();
    const uint64_t c = pictures;
    const uint64_t f2 = static_cast<uint64_t>(p.fit) * p.fit;
    const uint64_t tiles = static_cast<uint64_t>(p.fit / 8) * (p.fit / 8);
    const uint64_t cardFloats = StrokeCodec::CardDials(p.strokes);
    struct Item { GpuBuffer* b; uint64_t floats; };
    const Item items[] = {
        { &m_buf.ref, c * f2 * 3 }, { &m_buf.tgt, c * f2 * 3 }, { &m_buf.tmp, c * f2 * 3 }, { &m_buf.canvas, c * f2 * 3 },
        { &m_buf.paint, c * f2 * 3 }, { &m_buf.edge, c * f2 * 2 }, { &m_buf.tiles, c * tiles },
        { &m_buf.cand, c * p.candidates * kRecordFloats }, { &m_buf.part, c * p.candidates * kMaxSlices * 4 },
        { &m_buf.best, c * kRecordFloats }, { &m_buf.cards, c * cardFloats }, { &m_buf.stats, c * 8 },
    };
    for (const Item& it : items) {
        const HRESULT hr = m_gpu.CreateBuffer(it.floats * 4, *it.b);
        if (FAILED(hr)) {
            Free();
            return hr;
        }
    }
    m_layout = want;
    m_valid = true;
    return S_OK;
}

// =================================================================================================
// Launch helpers
// =================================================================================================

HRESULT Converter::Launch(GpuDispatch& d) {
    ++m_launches;
    return m_gpu.Dispatch(d);
}

template <class Fn>
HRESULT Converter::ForChunks(uint32_t count, double workPerPicture, Fn fn) {
    uint32_t per = count;
    if (workPerPicture > 1.0) per = static_cast<uint32_t>(std::max(1.0, std::min<double>(count, m_chunkWork / workPerPicture)));
    for (uint32_t first = 0; first < count; first += per) {
        const uint32_t n = std::min(per, count - first);
        const HRESULT hr = fn(first, n);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

StrokeConstants Converter::BaseConstants(uint32_t first, uint32_t n) const {
    const FlightWindow& w = FlightFor(m_flight);
    StrokeConstants cb;
    cb.gFirst = first;
    cb.gBatch = n;
    cb.gSize = m_params.fit;
    cb.gCands = m_params.candidates;
    cb.gSlices = w.slices;
    cb.gSeed = m_seedRun;
    cb.gCardFloats = StrokeCodec::CardDials(m_params.strokes);
    cb.gOutSize = m_params.fit;
    cb.gRowFirst = 0;
    cb.gRowCount = m_params.fit;
    cb.gTilesPerRow = m_params.fit / 8;
    cb.gSizeLo = w.sizeLo;
    cb.gSizeHi = w.sizeHi;
    cb.gAspectLo = w.aspectLo;
    cb.gAspectHi = w.aspectHi;
    cb.gSoftLo = w.softLo;
    cb.gSoftHi = w.softHi;
    cb.gOpacityLo = w.opacityLo;
    cb.gOpacityHi = w.opacityHi;
    cb.gSigma = w.blurFraction * static_cast<float>(m_params.fit);
    cb.gEdgeFollow = w.followEdges ? 1.0f : 0.0f;
    cb.gStepScale = 1.0f;
    return cb;
}

GpuDispatch Converter::Make(GpuKernel kernel, uint32_t first, uint32_t n, uint32_t mode) const {
    GpuDispatch d;
    d.kernel = kernel;
    d.cb = BaseConstants(first, n);
    d.cb.gMode = mode;
    return d;
}

// Upper bound of the pixels one candidate can cover in this flight (the Tuner may grow a stroke a little past the window).
double Converter::WorkPerCandidatePixels(uint32_t flight) const {
    const FlightWindow& w = FlightFor(flight);
    const float dial = std::min(127.0f, w.sizeHi + 8.0f);
    const float radius = 0.004f * std::pow(125.0f, dial / 127.0f) * static_cast<float>(m_params.fit);
    const float side = std::min(static_cast<float>(m_params.fit), 2.0f * radius + 2.0f);
    return static_cast<double>(side) * side;
}

// =================================================================================================
// Setup
// =================================================================================================

HRESULT Converter::SetupGroup(uint32_t count) {
    const uint32_t F = m_params.fit;
    const double f2 = static_cast<double>(F) * F;
    m_flight = 0;
    HRESULT hr = ForChunks(count, f2 * 60.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::EdgeMap, first, n, 0);
        d.srv[0].buf = &m_buf.ref;
        d.uav[0].buf = &m_buf.edge;
        GpuCore::SetImageGrid(d, F, F, n);
        d.work = f2 * 60.0 * n;
        return Launch(d);
    });
    if (FAILED(hr)) return hr;
    hr = ForChunks(count, f2 * 10.0, [&](uint32_t first, uint32_t n) {   // average colour -> the 3 header dials
        GpuDispatch d = Make(GpuKernel::Background, first, n, 0);
        d.srv[0].buf = &m_buf.ref;
        d.uav[0].buf = &m_buf.cards;
        d.gx = 1;
        d.gy = 1;
        d.gz = n;
        d.work = f2 * 10.0 * n;
        return Launch(d);
    });
    if (FAILED(hr)) return hr;
    return ForChunks(count, f2 * 8.0, [&](uint32_t first, uint32_t n) {   // paint the canvas with the background
        GpuDispatch d = Make(GpuKernel::Background, first, n, 1);
        d.srv[0].buf = &m_buf.cards;
        d.uav[0].buf = &m_buf.canvas;
        GpuCore::SetImageGrid(d, F, F, n);
        d.work = f2 * 8.0 * n;
        return Launch(d);
    });
}

HRESULT Converter::SetupFlight(uint32_t flight, uint32_t count) {
    m_flight = flight;
    const uint32_t F = m_params.fit;
    const double f2 = static_cast<double>(F) * F;
    const float sigma = FlightFor(flight).blurFraction * static_cast<float>(F);
    if (sigma < 0.35f) {
        return ForChunks(count, f2 * 8.0, [&](uint32_t first, uint32_t n) {
            GpuDispatch d = Make(GpuKernel::Blur, first, n, 2);
            d.srv[0].buf = &m_buf.ref;
            d.uav[0].buf = &m_buf.tgt;
            GpuCore::SetImageGrid(d, F, F, n);
            d.work = f2 * 8.0 * n;
            return Launch(d);
        });
    }
    const double taps = 2.0 * std::min(16.0, std::ceil(3.0 * sigma)) + 1.0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const HRESULT hr = ForChunks(count, f2 * taps * 24.0, [&](uint32_t first, uint32_t n) {
            GpuDispatch d = Make(GpuKernel::Blur, first, n, pass);
            d.srv[0].buf = pass == 0 ? &m_buf.ref : &m_buf.tmp;
            d.uav[0].buf = pass == 0 ? &m_buf.tmp : &m_buf.tgt;
            GpuCore::SetImageGrid(d, F, F, n);
            d.work = f2 * taps * 24.0 * n;
            return Launch(d);
        });
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

// =================================================================================================
// One stroke
// =================================================================================================

HRESULT Converter::RecordEvaluate(uint32_t flight, uint32_t count, uint32_t round, uint32_t stroke) {
    const uint32_t K = m_params.candidates;
    const uint32_t S = FlightFor(flight).slices;
    const double pixels = WorkPerCandidatePixels(flight);
    for (uint32_t mode = 0; mode < 4; ++mode) {
        const bool partial = (mode == 0 || mode == 2);
        const double perPicture = partial ? static_cast<double>(K) * pixels * 45.0 : static_cast<double>(K) * S * 8.0;
        const HRESULT hr = ForChunks(count, perPicture, [&](uint32_t first, uint32_t n) {
            GpuDispatch d = Make(GpuKernel::Evaluate, first, n, mode);
            d.cb.gStroke = stroke;
            d.cb.gRound = round;
            if (partial) {
                d.srv[0].buf = &m_buf.cand;
                d.srv[1].buf = &m_buf.canvas;
                d.srv[2].buf = &m_buf.tgt;
                d.uav[0].buf = &m_buf.part;
                GpuCore::SetLinearGrid(d, static_cast<uint64_t>(n) * K * S);
            } else {
                d.srv[0].buf = &m_buf.part;
                d.uav[0].buf = &m_buf.cand;
                GpuCore::SetLinearGrid(d, static_cast<uint64_t>(n) * K);
            }
            d.work = perPicture * n;
            return Launch(d);
        });
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT Converter::RecordScout(uint32_t flight, uint32_t stroke, uint32_t count) {
    (void)flight;
    const uint32_t F = m_params.fit;
    const uint32_t K = m_params.candidates;
    const uint32_t T = F / 8;
    HRESULT hr = S_OK;

    // Scout: where is the canvas still wrong, and which strokes might fix it?
    hr = ForChunks(count, static_cast<double>(T) * T * 64.0 * 12.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::ErrorMap, first, n, 0);
        d.srv[0].buf = &m_buf.canvas;
        d.srv[1].buf = &m_buf.tgt;
        d.uav[0].buf = &m_buf.tiles;
        GpuCore::SetLinearGrid(d, static_cast<uint64_t>(n) * T * T);
        d.work = static_cast<double>(T) * T * 64.0 * 12.0 * n;
        return Launch(d);
    });
    if (FAILED(hr)) return hr;
    hr = ForChunks(count, static_cast<double>(K) * (T * T * 6.0 + 300.0), [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::Scout, first, n, 0);
        d.cb.gStroke = stroke;
        d.srv[0].buf = &m_buf.tiles;
        d.srv[1].buf = &m_buf.edge;
        d.uav[0].buf = &m_buf.cand;
        GpuCore::SetLinearGrid(d, static_cast<uint64_t>(n) * K);
        d.work = static_cast<double>(K) * (T * T * 6.0 + 300.0) * n;
        return Launch(d);
    });
    if (FAILED(hr)) return hr;
    return S_OK;
}

HRESULT Converter::RecordPick(uint32_t count) {
    const double K = m_params.candidates;
    return ForChunks(count, K * 8.0 + 100.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::Pick, first, n, 0);
        d.srv[0].buf = &m_buf.cand;
        d.uav[0].buf = &m_buf.best;
        GpuCore::SetLinearGrid(d, n);
        d.work = (K * 8.0 + 100.0) * n;
        return Launch(d);
    });
}

HRESULT Converter::RecordNudge(uint32_t round, uint32_t stroke, uint32_t count) {
    const uint32_t K = m_params.candidates;
    return ForChunks(count, static_cast<double>(K) * 300.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::Nudge, first, n, 0);
        d.cb.gStroke = stroke;
        d.cb.gRound = round;
        d.cb.gStepScale = 1.0f / static_cast<float>(1u << (round - 1));
        d.srv[0].buf = &m_buf.best;
        d.uav[0].buf = &m_buf.cand;
        GpuCore::SetLinearGrid(d, static_cast<uint64_t>(n) * K);
        d.work = static_cast<double>(K) * 300.0 * n;
        return Launch(d);
    });
}

HRESULT Converter::RecordStroke(uint32_t flight, uint32_t stroke, uint32_t count) {
    const uint32_t F = m_params.fit;
    const double f2 = static_cast<double>(F) * F;
    HRESULT hr = RecordScout(flight, stroke, count);
    if (FAILED(hr)) return hr;

    // Artist + Pick, then the Tuner's rounds (Nudge, Artist, Pick)
    for (uint32_t round = 0; round <= m_params.rounds; ++round) {
        if (round > 0) {
            hr = RecordNudge(round, stroke, count);
            if (FAILED(hr)) return hr;
        }
        hr = RecordEvaluate(flight, count, round, stroke);
        if (FAILED(hr)) return hr;
        hr = RecordPick(count);
        if (FAILED(hr)) return hr;
    }

    // Commit: paint the winner, write its dials into the card
    return ForChunks(count, f2 * 14.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::Commit, first, n, 0);
        d.cb.gStroke = stroke;
        d.srv[0].buf = &m_buf.best;
        d.uav[0].buf = &m_buf.canvas;
        d.uav[1].buf = &m_buf.cards;
        GpuCore::SetImageGrid(d, F, F, n);
        d.work = f2 * 14.0 * n;
        return Launch(d);
    });
}

HRESULT Converter::MeasureGroup(uint32_t count, std::vector<float>& stats) {
    const double f2 = static_cast<double>(m_params.fit) * m_params.fit;
    const HRESULT hr = ForChunks(count, f2 * 3.0 * 10.0, [&](uint32_t first, uint32_t n) {
        GpuDispatch d = Make(GpuKernel::Measure, first, n, 0);
        d.srv[0].buf = &m_buf.canvas;
        d.srv[1].buf = &m_buf.ref;
        d.srv[2].buf = &m_buf.tgt;
        d.uav[0].buf = &m_buf.stats;
        GpuCore::SetLinearGrid(d, n);
        d.work = f2 * 3.0 * 10.0 * n;
        return Launch(d);
    });
    if (FAILED(hr)) return hr;
    stats.assign(static_cast<size_t>(count) * 8, 0.0f);
    return m_gpu.Download(m_buf.stats, 0, stats.data(), stats.size() * sizeof(float));
}

// Paints the first `strokeEnd` strokes of the first `count` cards on the chip at `outSize` pixels into m_buf.paint, cut into rows so
// that no launch gets too long.
HRESULT Converter::PaintCards(uint32_t count, uint32_t outSize, uint32_t strokeEnd) {
    if (outSize == 0 || outSize > m_params.fit) return E_INVALIDARG;
    const double perRow = static_cast<double>(outSize) * std::max<uint32_t>(1, strokeEnd) * 150.0;
    return ForChunks(count, perRow * outSize, [&](uint32_t first, uint32_t n) {
        const uint32_t rows = static_cast<uint32_t>(std::max(1.0, std::min<double>(outSize, m_chunkWork / (perRow * n))));
        for (uint32_t row = 0; row < outSize; row += rows) {
            const uint32_t take = std::min(rows, outSize - row);
            GpuDispatch d = Make(GpuKernel::PaintCard, first, n, 0);
            d.cb.gStroke = 0;
            d.cb.gStrokeEnd = strokeEnd;
            d.cb.gOutSize = outSize;
            d.cb.gRowFirst = row;
            d.cb.gRowCount = take;
            d.srv[0].buf = &m_buf.cards;
            d.uav[0].buf = &m_buf.paint;
            GpuCore::SetImageGrid(d, outSize, take, n);
            d.work = perRow * take * n;
            const HRESULT hr = Launch(d);
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    });
}

bool Converter::ReadCanvas(uint32_t index, Rgb8Image& out) {
    out = Rgb8Image();
    if (!m_valid || index >= m_layout.capacity) return false;
    const uint32_t F = m_params.fit;
    const size_t floats = static_cast<size_t>(F) * F * 3;
    std::vector<float> rgb(floats);
    if (FAILED(m_gpu.Download(m_buf.canvas, static_cast<uint64_t>(index) * floats * 4, rgb.data(), floats * 4))) return false;
    out = ToRgb8(rgb.data(), F);
    return true;
}

// =================================================================================================
// A group of pictures
// =================================================================================================

HRESULT Converter::Convert(const std::vector<PictureIn>& pictures, const ConvertParams& params, std::vector<PictureOut>& out,
                           const ProgressFn& progress) {
    out.clear();
    const uint32_t count = static_cast<uint32_t>(pictures.size());
    if (count == 0) return S_OK;
    if (!m_gpu.Ready()) return E_NOT_VALID_STATE;
    if (params.fit < 16 || params.fit > 256 || params.fit % 8 != 0 || params.strokes < 3 || params.strokes > 200000 ||
        params.candidates < 4 || params.candidates > 256 || params.rounds > 8) {
        return E_INVALIDARG;
    }
    m_params = params;
    m_seedRun = SeedFor(params.seed);
    const uint32_t F = params.fit;
    const size_t plane = static_cast<size_t>(F) * F * 3;

    HRESULT hr = EnsureBuffers(params, count);
    if (FAILED(hr)) return hr;

    // the pictures, fitted to the working size, as floats 0..1
    std::vector<float> refs(plane * count);
    for (uint32_t i = 0; i < count; ++i) {
        const Rgb8Image& src = pictures[i].image;
        if (!src.Valid()) return E_INVALIDARG;
        Rgb8Image fitted;
        if (src.w == F && src.h == F) fitted = src;
        else fitted = ResizeRgb(src.px.data(), src.w, src.h, F, F);
        if (!fitted.Valid()) return E_INVALIDARG;
        for (size_t k = 0; k < plane; ++k) refs[i * plane + k] = static_cast<float>(fitted.px[k]) / 255.0f;
    }
    hr = m_gpu.Upload(m_buf.ref, 0, refs.data(), refs.size() * sizeof(float));
    if (FAILED(hr)) { Free(); return hr; }

    hr = SetupGroup(count);
    if (FAILED(hr)) { m_gpu.Abort(); Free(); return hr; }

    uint32_t perFlight[3];
    SplitStrokes(params.strokes, perFlight);
    const auto start = Clock::now();
    uint32_t done = 0;
    uint32_t chunk = params.chunk ? params.chunk : 4;
    std::vector<float> stats;
    for (uint32_t flight = 0; flight < 3; ++flight) {
        if (perFlight[flight] == 0) continue;
        hr = SetupFlight(flight, count);
        if (FAILED(hr)) { m_gpu.Abort(); Free(); return hr; }
        const uint32_t end = done + perFlight[flight];
        while (done < end) {
            const uint32_t n = std::min(chunk, end - done);
            const auto t0 = Clock::now();
            for (uint32_t i = 0; i < n && SUCCEEDED(hr); ++i) {
                hr = RecordStroke(flight, done + i, count);
                if (SUCCEEDED(hr) && TraceForTests) {
                    std::vector<float> st;
                    hr = MeasureGroup(count, st);
                    if (SUCCEEDED(hr)) {
                        for (uint32_t p = 0; p < count; ++p) TraceForTests->push_back(static_cast<double>(st[p * 8 + 1]));
                    }
                }
            }
            if (SUCCEEDED(hr)) hr = MeasureGroup(count, stats);
            if (FAILED(hr)) { m_gpu.Abort(); Free(); return hr; }
            done += n;
            if (params.chunk == 0) {
                const double perStroke = std::max(1e-6, Seconds(t0, Clock::now()) / n);
                const uint32_t want = static_cast<uint32_t>(std::max(1.0, std::min(256.0, kTargetChunkSeconds / perStroke)));
                chunk = std::min(want, chunk * 2);
            }
            if (progress) {
                ConvertProgress p;
                p.flight = flight;
                p.strokesDone = done;
                p.strokesTotal = params.strokes;
                p.pictures = count;
                double sum = 0.0;
                for (uint32_t i = 0; i < count; ++i) sum += stats[i * 8];
                p.errorNow = sum / count;
                p.seconds = Seconds(start, Clock::now());
                p.strokesPerSecond = p.seconds > 0 ? static_cast<double>(done) * count / p.seconds : 0.0;
                if (!progress(p)) return E_ABORT;
            }
        }
    }

    // the cards and the finished canvases
    const size_t cardFloats = StrokeCodec::CardDials(params.strokes);
    std::vector<float> cards(cardFloats * count);
    hr = m_gpu.Download(m_buf.cards, 0, cards.data(), cards.size() * sizeof(float));
    if (FAILED(hr)) { Free(); return hr; }
    std::vector<float> canvas(plane * count);
    hr = m_gpu.Download(m_buf.canvas, 0, canvas.data(), canvas.size() * sizeof(float));
    if (FAILED(hr)) { Free(); return hr; }
    if (stats.size() < static_cast<size_t>(count) * 8) { stats.assign(static_cast<size_t>(count) * 8, 0.0f); }

    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        PictureOut& o = out[i];
        o.id = pictures[i].id;
        o.strokes = params.strokes;
        for (int f = 0; f < 3; ++f) o.perFlight[f] = perFlight[f];
        o.fit = F;
        o.errorRms = stats[i * 8];
        o.dials = StrokeCodec::DialsToBytes(cards.data() + i * cardFloats, cardFloats);
        for (uint32_t s = 0; s < params.strokes; ++s) {
            if (o.dials[StrokeCodec::kHeaderDials + static_cast<size_t>(s) * StrokeCodec::kStrokeDials + StrokeCodec::kOpacity] == 0) ++o.emptyStrokes;
        }
        o.canvas = ToRgb8(canvas.data() + i * plane, F);
    }
    return S_OK;
}

// =================================================================================================
// The start-up self-test: five checks. A chip that fails any of them is not trusted (the Convert page says so).
// =================================================================================================

HRESULT Converter::SelfTest(std::string& report) {
    report.clear();
    if (!m_gpu.Ready()) {
        report = "self-test: the graphics chip is not ready";
        return E_NOT_VALID_STATE;
    }
    const uint32_t F = 64;
    const uint32_t kStrokes = 24;
    ConvertParams p;
    p.strokes = kStrokes;
    p.fit = F;
    p.seed = 7;
    p.candidates = 64;
    p.rounds = 1;
    p.chunk = 1;
    m_params = p;
    m_seedRun = SeedFor(p.seed);
    m_flight = 1;
    const size_t plane = static_cast<size_t>(F) * F * 3;
    const uint32_t cardFloats = StrokeCodec::CardDials(kStrokes);
    bool allOk = true;
    char buf[320];
    auto fail = [&](const char* name, const std::string& why) {
        allOk = false;
        report += std::string("check ") + name + " FAILED: " + why + "\n";
    };
    auto pass = [&](const char* name, const std::string& what) { report += std::string("check ") + name + " ok: " + what + "\n"; };
    auto hrFail = [&](const char* name, HRESULT hr) {
        std::snprintf(buf, sizeof(buf), "the chip returned error 0x%08X", static_cast<unsigned>(hr));
        fail(name, buf);
        m_gpu.Abort();
        Free();
        return E_FAIL;
    };

    HRESULT hr = EnsureBuffers(p, 2);
    if (FAILED(hr)) return hrFail("setup", hr);

    // ---- 1. paint parity: an awkward card, chip against processor, at two output sizes
    {
        const std::vector<float> card = StrokeCodec::AwkwardTestCard(16);
        std::vector<float> host(static_cast<size_t>(cardFloats) * 2, 0.0f);
        std::copy(card.begin(), card.end(), host.begin());
        hr = m_gpu.Upload(m_buf.cards, 0, host.data(), host.size() * sizeof(float));
        double worst = 0.0;
        for (uint32_t outSize : { 64u, 40u }) {
            if (SUCCEEDED(hr)) hr = PaintCards(1, outSize, 16);
            std::vector<float> chip(static_cast<size_t>(outSize) * outSize * 3, 0.0f);
            if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.paint, 0, chip.data(), chip.size() * sizeof(float));
            if (FAILED(hr)) return hrFail("1 paint parity", hr);
            std::vector<float> cpu(chip.size(), 0.0f);
            StrokeCodec::PaintCard(card.data(), 16, 0, 16, outSize, true, cpu.data());
            for (size_t i = 0; i < chip.size(); ++i) {
                const double d = std::fabs(static_cast<double>(chip[i]) - cpu[i]);
                worst = std::max(worst, std::isfinite(d) ? d : 1e9);
            }
        }
        if (worst < 0.001) pass("1 paint parity", Fmt("largest pixel difference %.6f (limit 0.001)", worst));
        else fail("1 paint parity", Fmt("largest pixel difference %.6f is over the limit 0.001", worst));
    }

    // ---- 2 + 3. colour solve and scoring: Scout makes 64 candidates on a test canvas, the chip scores them, the processor checks every one
    {
        const std::vector<float> canvasImg = StrokeCodec::TestPicture(F, 1);
        const std::vector<float> targetImg = StrokeCodec::TestPicture(F, 0);
        hr = m_gpu.Upload(m_buf.canvas, 0, canvasImg.data(), plane * sizeof(float));
        if (SUCCEEDED(hr)) hr = m_gpu.Upload(m_buf.tgt, 0, targetImg.data(), plane * sizeof(float));
        if (SUCCEEDED(hr)) hr = RecordScout(1, 0, 1);
        if (SUCCEEDED(hr)) hr = RecordEvaluate(1, 1, 0, 0);
        const uint32_t K = p.candidates;
        std::vector<float> rec(static_cast<size_t>(K) * kRecordFloats, 0.0f);
        if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.cand, 0, rec.data(), rec.size() * sizeof(float));
        if (FAILED(hr)) return hrFail("2/3 scoring", hr);

        uint32_t badDials = 0, badGeom = 0, badFlag = 0, badColour = 0, badScore = 0, brute = 0, bruteBad = 0;
        double worstRel = 0.0;
        int worstColour = 0, worstBrute = 0;
        for (uint32_t k = 0; k < K; ++k) {
            const float* r = rec.data() + static_cast<size_t>(k) * kRecordFloats;
            if (!StrokeCodec::DialsValid(r, 12)) { ++badDials; continue; }
            // the shape the chip stored must be the shape the dial table says
            const StrokeCodec::Geom g = StrokeCodec::MakeGeom(r, F);
            const float want[8] = { g.cx, g.cy, g.R, g.r2, g.c, g.s, g.band, g.opacity };
            for (int i = 0; i < 8; ++i) {
                if (!(std::fabs(r[14 + i] - want[i]) <= 1e-3f * std::max(1.0f, std::fabs(want[i])))) { ++badGeom; break; }
            }
            if (static_cast<uint32_t>(r[22]) != g.x0 || static_cast<uint32_t>(r[23]) != g.x1 ||
                static_cast<uint32_t>(r[24]) != g.y0 || static_cast<uint32_t>(r[25]) != g.y1) ++badGeom;
            // the processor's own score
            float d[12];
            std::memcpy(d, r, sizeof(d));
            const StrokeCodec::Score sc = StrokeCodec::ScoreCandidate(d, F, canvasImg.data(), targetImg.data());
            if ((r[13] > 0.5f) != sc.covers) { ++badFlag; continue; }
            if (!sc.covers) continue;
            for (int c = 0; c < 3; ++c) {
                const int diff = static_cast<int>(std::fabs(r[9 + c] - d[9 + c]));
                worstColour = std::max(worstColour, diff);
                if (diff > 1) ++badColour;
            }
            // the chip's change in error against the processor's, for the colour the chip chose
            const float cpuDelta = StrokeCodec::DeltaWithColour(r, F, canvasImg.data(), targetImg.data());
            const double diff = std::fabs(static_cast<double>(r[12]) - cpuDelta);
            const double tol = 0.001 * std::fabs(static_cast<double>(cpuDelta)) + 2e-4;
            worstRel = std::max(worstRel, diff / std::max(std::fabs(static_cast<double>(cpuDelta)), 0.2));
            if (!(diff <= tol)) ++badScore;
            // brute force over all 128 levels of every colour channel (first eight strokes)
            if (brute < 8) {
                ++brute;
                double err[3][128] = {};
                for (uint32_t y = g.y0; y < g.y1; ++y) {
                    for (uint32_t x = g.x0; x < g.x1; ++x) {
                        const float a = StrokeCodec::Alpha(g, x, y);
                        if (a <= 0.0f) continue;
                        const size_t q = (static_cast<size_t>(y) * F + x) * 3;
                        for (int c = 0; c < 3; ++c) {
                            for (int lv = 0; lv < 128; ++lv) {
                                const double e = canvasImg[q + c] * (1.0 - a) + (static_cast<double>(lv) / 127.0) * a - targetImg[q + c];
                                err[c][lv] += e * e;
                            }
                        }
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    int best = 0;
                    for (int lv = 1; lv < 128; ++lv) if (err[c][lv] < err[c][best]) best = lv;
                    const int diffLevel = static_cast<int>(std::fabs(static_cast<float>(best) - r[9 + c]));
                    worstBrute = std::max(worstBrute, diffLevel);
                    if (diffLevel > 1) ++bruteBad;
                }
            }
        }
        if (bruteBad == 0 && brute >= 4) pass("2 colour solve", Fmt("%.0f strokes checked against all 128 levels, worst difference %.0f level(s) (limit 1)", brute, worstBrute));
        else fail("2 colour solve", Fmt("%.0f of the colours are more than 1 level from the brute-force answer (worst %.0f)", bruteBad, worstBrute));
        // the steering programs and the Tuner, on the same candidates: error map, pick of the best, one nudge
        uint32_t badTiles = 0, badPick = 0, badNudge = 0, nudgeMoved = 0;
        {
            const uint32_t T = F / 8;
            std::vector<float> tilesChip(static_cast<size_t>(T) * T, 0.0f), tilesCpu(tilesChip.size(), 0.0f);
            if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.tiles, 0, tilesChip.data(), tilesChip.size() * sizeof(float));
            StrokeCodec::ErrorTilesReference(canvasImg.data(), targetImg.data(), F, tilesCpu.data());
            for (size_t i = 0; i < tilesCpu.size(); ++i) {
                if (!(std::fabs(tilesChip[i] - tilesCpu[i]) <= 1e-4 * std::fabs(tilesCpu[i]) + 1e-5)) ++badTiles;
            }
            // Pick: the candidate with the lowest change in error, the earlier one on a tie, copied whole
            if (SUCCEEDED(hr)) hr = RecordPick(1);
            std::vector<float> best(kRecordFloats, 0.0f);
            if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.best, 0, best.data(), best.size() * sizeof(float));
            uint32_t arg = 0;
            for (uint32_t k = 1; k < K; ++k) {
                if (rec[static_cast<size_t>(k) * kRecordFloats + 12] < rec[static_cast<size_t>(arg) * kRecordFloats + 12]) arg = k;
            }
            for (uint32_t i = 0; i < 26; ++i) {
                if (std::memcmp(&best[i], &rec[static_cast<size_t>(arg) * kRecordFloats + i], sizeof(float)) != 0) ++badPick;
            }
            // Nudge: candidate 0 is the best unchanged, every other one moves at most ONE shape dial by a bounded step and keeps the colour
            if (SUCCEEDED(hr)) hr = RecordNudge(1, 0, 1);
            std::vector<float> nud(static_cast<size_t>(K) * kRecordFloats, 0.0f);
            if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.cand, 0, nud.data(), nud.size() * sizeof(float));
            if (FAILED(hr)) return hrFail("2/3 scoring", hr);
            auto split = [](const float* r, int out[7]) {
                out[0] = static_cast<int>(r[0] * 128.0f + r[1] + 0.5f);
                out[1] = static_cast<int>(r[2] * 128.0f + r[3] + 0.5f);
                out[2] = static_cast<int>(r[4] + 0.5f);
                out[3] = static_cast<int>(r[5] + 0.5f);
                out[4] = static_cast<int>(r[6] + 0.5f);
                out[5] = static_cast<int>(r[7] + 0.5f);
                out[6] = static_cast<int>(r[8] + 0.5f);
            };
            const int bound[7] = { 512, 512, 6, 8, 6, 12, 12 };   // the largest step of each dial in the first round
            int bd[7];
            split(best.data(), bd);
            for (uint32_t k = 0; k < K; ++k) {
                const float* r = nud.data() + static_cast<size_t>(k) * kRecordFloats;
                if (!StrokeCodec::DialsValid(r, 12)) { ++badNudge; continue; }
                const StrokeCodec::Geom g = StrokeCodec::MakeGeom(r, F);
                const float want[8] = { g.cx, g.cy, g.R, g.r2, g.c, g.s, g.band, g.opacity };
                for (int i = 0; i < 8; ++i) {
                    if (!(std::fabs(r[14 + i] - want[i]) <= 1e-3f * std::max(1.0f, std::fabs(want[i])))) { ++badNudge; break; }
                }
                if (r[9] != best[9] || r[10] != best[10] || r[11] != best[11] || r[12] != 0.0f || r[13] != 0.0f) ++badNudge;
                int nd[7];
                split(r, nd);
                int moved = 0;
                bool tooBig = false;
                for (int i = 0; i < 7; ++i) {
                    int diff = std::abs(nd[i] - bd[i]);
                    if (i == 4) diff = std::min(diff, 128 - diff);   // the angle wraps around
                    if (diff != 0) ++moved;
                    if (diff > bound[i]) tooBig = true;
                }
                if (tooBig || moved > 1 || (k == 0 && moved != 0)) ++badNudge;
                if (k > 0 && moved == 1) ++nudgeMoved;
            }
            if (nudgeMoved < 40) ++badNudge;   // 63 of 64 should have moved one dial (a few can be pinned at the end of a dial)
        }
        if (badDials == 0 && badGeom == 0 && badFlag == 0 && badColour == 0 && badScore == 0 && badTiles == 0 && badPick == 0 && badNudge == 0) {
            pass("3 scoring parity", Fmt("%.0f strokes scored on chip and processor, worst difference %.4f %% (limit 0.1 %%); error map, pick and nudge agree with the processor", K, worstRel * 100.0));
        } else {
            std::snprintf(buf, sizeof(buf), "%u bad dials, %u bad shapes, %u bad flags, %u bad colours, %u bad scores (worst %.3f %%), %u bad error tiles, %u bad pick numbers, %u bad nudges",
                          badDials, badGeom, badFlag, badColour, badScore, worstRel * 100.0, badTiles, badPick, badNudge);
            fail("3 scoring parity", buf);
        }
    }

    // ---- 4 + 5. honest descent, repeatable and consistent: the real pipeline, twice with the same seed
    {
        std::vector<PictureIn> pics(2);
        std::vector<double> baseline(2, 0.0);
        for (uint32_t i = 0; i < 2; ++i) {
            const std::vector<float> img = StrokeCodec::TestPicture(F, i);
            pics[i].id = i + 1;
            pics[i].image = ToRgb8(img.data(), F);
            double mean[3] = { 0, 0, 0 };
            for (size_t k = 0; k < img.size(); ++k) mean[k % 3] += pics[i].image.px[k] / 255.0;
            double sum = 0.0;
            double bg[3];
            for (int c = 0; c < 3; ++c) bg[c] = std::floor(mean[c] / (static_cast<double>(F) * F) * 127.0 + 0.5) / 127.0;
            for (size_t k = 0; k < img.size(); ++k) {
                const double d = bg[k % 3] - pics[i].image.px[k] / 255.0;
                sum += d * d;
            }
            baseline[i] = 255.0 * std::sqrt(sum / static_cast<double>(img.size()));
        }
        std::vector<double> trace;
        std::vector<PictureOut> a, b;
        TraceForTests = &trace;
        hr = Convert(pics, p, a);
        TraceForTests = nullptr;
        if (FAILED(hr)) return hrFail("4 honest descent", hr);

        uint32_t split[3];
        SplitStrokes(kStrokes, split);
        uint32_t rises = 0, checked = 0;
        double worstRise = 0.0;
        uint32_t flightStart[3] = { 0, split[0], split[0] + split[1] };
        for (uint32_t i = 0; i < 2; ++i) {
            for (uint32_t s = 1; s < kStrokes; ++s) {
                if (s == flightStart[1] || s == flightStart[2]) continue;   // the target changes between flights
                const double prev = trace[(s - 1) * 2 + i];
                const double cur = trace[static_cast<size_t>(s) * 2 + i];
                ++checked;
                const double rise = cur - prev;
                worstRise = std::max(worstRise, rise);
                if (!(rise <= 1e-5 * prev + 1e-3)) ++rises;
            }
        }
        bool endOk = true;
        double worstRatio = 0.0;
        for (uint32_t i = 0; i < 2; ++i) {
            const double ratio = a[i].errorRms / std::max(1e-6, baseline[i]);
            worstRatio = std::max(worstRatio, ratio);
            if (!(ratio <= 0.8)) endOk = false;
        }
        if (rises == 0 && endOk) {
            pass("4 honest descent", Fmt("error never rose over %.0f strokes, and ended at %.0f %% of the plain-background error (limit 80 %%)", checked, worstRatio * 100.0));
        } else {
            std::snprintf(buf, sizeof(buf), "%u of %u strokes made the error rise (worst %.4f); the final error is %.0f %% of the background error (limit 80 %%)",
                          rises, checked, worstRise, worstRatio * 100.0);
            fail("4 honest descent", buf);
        }

        hr = Convert(pics, p, b);
        if (FAILED(hr)) return hrFail("5 repeatable and consistent", hr);
        bool same = a.size() == b.size() && a.size() == 2;
        for (size_t i = 0; same && i < a.size(); ++i) same = a[i].dials == b[i].dials;
        std::vector<float> cards(static_cast<size_t>(cardFloats) * 2);
        std::vector<float> stats;
        hr = m_gpu.Download(m_buf.cards, 0, cards.data(), cards.size() * sizeof(float));
        if (SUCCEEDED(hr)) hr = MeasureGroup(2, stats);
        std::vector<float> chipCanvas(plane * 2), repaint(plane * 2);
        if (SUCCEEDED(hr)) hr = PaintCards(2, F, kStrokes);
        if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.paint, 0, repaint.data(), repaint.size() * sizeof(float));
        if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.canvas, 0, chipCanvas.data(), chipCanvas.size() * sizeof(float));
        if (FAILED(hr)) return hrFail("5 repeatable and consistent", hr);
        const bool dialsOk = StrokeCodec::DialsValid(cards.data(), cards.size());
        const bool noBad = stats.size() >= 16 && stats[2] == 0.0f && stats[10] == 0.0f;
        // the chip's own error measure against the processor's (the last flight aims at the picture itself, so both numbers are known)
        double worstMeasure = stats.size() >= 16 ? 0.0 : 1e9;
        for (uint32_t i = 0; i < 2 && stats.size() >= 16; ++i) {
            double sum = 0.0;
            for (size_t k = 0; k < plane; ++k) {
                const double d = static_cast<double>(chipCanvas[i * plane + k]) - pics[i].image.px[k] / 255.0;
                sum += d * d;
            }
            const double wantRms = 255.0 * std::sqrt(sum / static_cast<double>(plane));
            worstMeasure = std::max(worstMeasure, std::fabs(static_cast<double>(stats[i * 8 + 0]) - wantRms) / (wantRms + 0.05));
            worstMeasure = std::max(worstMeasure, std::fabs(static_cast<double>(stats[i * 8 + 1]) - sum) / (sum + 1e-3));
        }
        const bool measureOk = worstMeasure < 1e-3;
        // the edge map of the pictures and the background colour in the card header, against the processor
        std::vector<float> edgesChip(static_cast<size_t>(F) * F * 2 * 2, 0.0f);
        if (SUCCEEDED(hr)) hr = m_gpu.Download(m_buf.edge, 0, edgesChip.data(), edgesChip.size() * sizeof(float));
        if (FAILED(hr)) return hrFail("5 repeatable and consistent", hr);
        uint32_t badEdges = 0, badHeader = 0;
        for (uint32_t i = 0; i < 2; ++i) {
            std::vector<float> pic(plane), edgesCpu(static_cast<size_t>(F) * F * 2);
            double mean[3] = { 0, 0, 0 };
            for (size_t k = 0; k < plane; ++k) {
                pic[k] = pics[i].image.px[k] / 255.0f;
                mean[k % 3] += pic[k];
            }
            StrokeCodec::EdgeMapReference(pic.data(), F, edgesCpu.data());
            const float* ec = edgesChip.data() + static_cast<size_t>(i) * F * F * 2;
            for (size_t px = 0; px < static_cast<size_t>(F) * F; ++px) {
                if (!(std::fabs(ec[px * 2] - edgesCpu[px * 2]) <= 1e-4f)) { ++badEdges; continue; }
                if (edgesCpu[px * 2] > 0.02f) {
                    float dd = std::fabs(ec[px * 2 + 1] - edgesCpu[px * 2 + 1]);
                    dd = std::min(dd, 128.0f - dd);   // the direction wraps around (an edge has no front or back)
                    if (!(dd <= 0.02f)) ++badEdges;
                }
            }
            for (int c = 0; c < 3; ++c) {
                const double wantDial = std::floor(mean[c] / (static_cast<double>(F) * F) * 127.0 + 0.5);
                if (!(std::fabs(cards[static_cast<size_t>(i) * cardFloats + c] - wantDial) <= 1.0)) ++badHeader;
            }
        }
        double worst = 0.0;
        for (size_t k = 0; k < repaint.size(); ++k) {
            const double d = std::fabs(static_cast<double>(repaint[k]) - chipCanvas[k]);
            worst = std::max(worst, std::isfinite(d) ? d : 1e9);
        }
        if (same && dialsOk && noBad && measureOk && badEdges == 0 && badHeader == 0 && worst < 0.001) {
            pass("5 repeatable and consistent", Fmt("same seed gave identical dials; the repainted card differs from the converter's canvas by at most %.6f; all dials 0 to 127; no NaN; the chip's error measure, edge map and background colour agree with the processor's (error measure to %.4f %%)", worst, worstMeasure * 100.0));
        } else {
            std::snprintf(buf, sizeof(buf), "same dials twice: %s; all dials 0..127: %s; no NaN: %s; chip error measure matches: %s (%.3f %% off); %u bad edge numbers; %u bad background dials; repaint difference %.6f (limit 0.001)",
                          same ? "yes" : "NO", dialsOk ? "yes" : "NO", noBad ? "yes" : "NO", measureOk ? "yes" : "NO", worstMeasure * 100.0, badEdges, badHeader, worst);
            fail("5 repeatable and consistent", buf);
        }
    }

    Free();
    report += allOk ? "self-test PASSED\n" : "self-test FAILED\n";
    return allOk ? S_OK : E_FAIL;
}

} // namespace XboxOTFM
