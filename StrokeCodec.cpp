#include "StrokeCodec.h"

#include <algorithm>
#include <cmath>

namespace XboxOTFM {
namespace StrokeCodec {

namespace {
constexpr float kPi = 3.14159265359f;
inline float Saturate(float v) { return std::min(1.0f, std::max(0.0f, v)); }
} // namespace

// The maths below is written to mirror MakeGeom() and Alpha() in Strokes.hlsl line for line.
Geom MakeGeom(const float* d, uint32_t size) {
    Geom g;
    const float side = static_cast<float>(size);
    g.cx = ((d[kXCoarse] * 128.0f + d[kXFine]) / 16383.0f) * side;
    g.cy = ((d[kYCoarse] * 128.0f + d[kYFine]) / 16383.0f) * side;
    g.R = 0.004f * std::pow(125.0f, d[kSize] / 127.0f) * side;
    g.r2 = g.R * std::pow(0.05f, (127.0f - d[kAspect]) / 127.0f);
    const float th = d[kAngle] * (kPi / 128.0f);
    g.c = std::cos(th);
    g.s = std::sin(th);
    g.band = std::min(1.0f, std::max(d[kSoftness] / 127.0f, 1.0f / g.r2));
    g.opacity = d[kOpacity] / 127.0f;
    const float rc = g.R * g.c, rs = g.R * g.s, qc = g.r2 * g.c, qs = g.r2 * g.s;
    const float halfW = std::sqrt(rc * rc + qs * qs) + 1.0f;
    const float halfH = std::sqrt(rs * rs + qc * qc) + 1.0f;
    g.x0 = static_cast<uint32_t>(std::max(0.0f, std::floor(g.cx - halfW)));
    g.x1 = static_cast<uint32_t>(std::min(side, std::ceil(g.cx + halfW)));
    g.y0 = static_cast<uint32_t>(std::max(0.0f, std::floor(g.cy - halfH)));
    g.y1 = static_cast<uint32_t>(std::min(side, std::ceil(g.cy + halfH)));
    return g;
}

float Alpha(const Geom& g, uint32_t px, uint32_t py) {
    const float dx = static_cast<float>(px) + 0.5f - g.cx;
    const float dy = static_cast<float>(py) + 0.5f - g.cy;
    const float u = dx * g.c + dy * g.s;
    const float v = dy * g.c - dx * g.s;
    const float a = u / g.R;
    const float b = v / g.r2;
    const float rho = std::sqrt(a * a + b * b);
    return g.opacity * Saturate((1.0f - rho) / g.band);
}

void PaintCard(const float* card, uint32_t strokes, uint32_t first, uint32_t last, uint32_t size, bool fromBackground, float* rgb) {
    const size_t pixels = static_cast<size_t>(size) * size;
    if (fromBackground) {
        for (size_t i = 0; i < pixels; ++i) {
            rgb[i * 3 + 0] = card[0] / 127.0f;
            rgb[i * 3 + 1] = card[1] / 127.0f;
            rgb[i * 3 + 2] = card[2] / 127.0f;
        }
    }
    last = std::min(last, strokes);
    for (uint32_t n = first; n < last; ++n) {
        const float* d = card + kHeaderDials + static_cast<size_t>(n) * kStrokeDials;
        if (!(d[kOpacity] > 0.5f)) continue;
        const Geom g = MakeGeom(d, size);
        const float cr = d[kRed] / 127.0f, cg = d[kGreen] / 127.0f, cb = d[kBlue] / 127.0f;
        for (uint32_t y = g.y0; y < g.y1; ++y) {
            for (uint32_t x = g.x0; x < g.x1; ++x) {
                const float a = Alpha(g, x, y);
                if (a > 0.0f) {
                    float* p = rgb + (static_cast<size_t>(y) * size + x) * 3;
                    p[0] = p[0] * (1.0f - a) + cr * a;
                    p[1] = p[1] * (1.0f - a) + cg * a;
                    p[2] = p[2] * (1.0f - a) + cb * a;
                }
            }
        }
    }
}

bool PaintCardBytes(const uint8_t* dials, size_t dialCount, uint32_t size, Rgb8Image& out, uint32_t lastStroke) {
    out = Rgb8Image();
    if (size == 0 || size > 4096 || dialCount < kHeaderDials || (dialCount - kHeaderDials) % kStrokeDials != 0) return false;
    const uint32_t strokes = static_cast<uint32_t>((dialCount - kHeaderDials) / kStrokeDials);
    const std::vector<float> card = DialsToFloat(dials, dialCount);
    std::vector<float> rgb(static_cast<size_t>(size) * size * 3);
    PaintCard(card.data(), strokes, 0, lastStroke, size, true, rgb.data());
    out.w = size;
    out.h = size;
    out.px.resize(rgb.size());
    for (size_t i = 0; i < rgb.size(); ++i) {
        out.px[i] = static_cast<uint8_t>(std::floor(Saturate(rgb[i]) * 255.0f + 0.5f));
    }
    return true;
}

Score ScoreCandidate(float* d, uint32_t size, const float* canvas, const float* target) {
    Score sc;
    const Geom g = MakeGeom(d, size);
    for (uint32_t y = g.y0; y < g.y1; ++y) {
        for (uint32_t x = g.x0; x < g.x1; ++x) {
            const float a = Alpha(g, x, y);
            if (a <= 0.0f) continue;
            const size_t p = (static_cast<size_t>(y) * size + x) * 3;
            sc.den += static_cast<double>(a) * a;
            for (int c = 0; c < 3; ++c) {
                sc.num[c] += static_cast<double>(a) * (target[p + c] - canvas[p + c] * (1.0f - a));
            }
        }
    }
    if (sc.den > 1e-9) {
        sc.covers = true;
        for (int c = 0; c < 3; ++c) {
            const double col = std::min(1.0, std::max(0.0, sc.num[c] / sc.den));
            d[kRed + c] = std::min(127.0f, std::max(0.0f, std::floor(static_cast<float>(col) * 127.0f + 0.5f)));
            sc.colour[c] = d[kRed + c];
        }
        sc.delta = DeltaWithColour(d, size, canvas, target);
    }
    return sc;
}

float DeltaWithColour(const float* d, uint32_t size, const float* canvas, const float* target) {
    const Geom g = MakeGeom(d, size);
    const float q[3] = { d[kRed] / 127.0f, d[kGreen] / 127.0f, d[kBlue] / 127.0f };
    double delta = 0.0;
    for (uint32_t y = g.y0; y < g.y1; ++y) {
        for (uint32_t x = g.x0; x < g.x1; ++x) {
            const float a = Alpha(g, x, y);
            if (a <= 0.0f) continue;
            const size_t p = (static_cast<size_t>(y) * size + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const float u = canvas[p + c];
                const float e = u * (1.0f - a) + q[c] * a - target[p + c];
                const float o = u - target[p + c];
                delta += static_cast<double>(e * e) - static_cast<double>(o * o);
            }
        }
    }
    return static_cast<float>(delta);
}

namespace {
float LumaAt(const float* rgb, uint32_t size, int x, int y) {
    const int last = static_cast<int>(size) - 1;
    const int cx = std::min(last, std::max(0, x));
    const int cy = std::min(last, std::max(0, y));
    const size_t q = (static_cast<size_t>(cy) * size + static_cast<size_t>(cx)) * 3;
    return 0.299f * rgb[q + 0] + 0.587f * rgb[q + 1] + 0.114f * rgb[q + 2];
}
} // namespace

// Mirrors CSEdgeMap in Strokes.hlsl line for line.
void EdgeMapReference(const float* rgb, uint32_t size, float* out) {
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const int ix = static_cast<int>(x);
            const int iy = static_cast<int>(y);
            const float gx = (LumaAt(rgb, size, ix + 1, iy - 1) + 2.0f * LumaAt(rgb, size, ix + 1, iy) + LumaAt(rgb, size, ix + 1, iy + 1)) -
                             (LumaAt(rgb, size, ix - 1, iy - 1) + 2.0f * LumaAt(rgb, size, ix - 1, iy) + LumaAt(rgb, size, ix - 1, iy + 1));
            const float gy = (LumaAt(rgb, size, ix - 1, iy + 1) + 2.0f * LumaAt(rgb, size, ix, iy + 1) + LumaAt(rgb, size, ix + 1, iy + 1)) -
                             (LumaAt(rgb, size, ix - 1, iy - 1) + 2.0f * LumaAt(rgb, size, ix, iy - 1) + LumaAt(rgb, size, ix + 1, iy - 1));
            const float mag = std::sqrt(gx * gx + gy * gy) * 0.25f;
            float th = 0.0f;
            if (mag > 1e-6f) th = std::atan2(gx, -gy);
            if (th < 0.0f) th += kPi;
            if (th >= kPi) th -= kPi;
            const size_t o = (static_cast<size_t>(y) * size + x) * 2;
            out[o + 0] = mag;
            out[o + 1] = std::min(127.99f, th * (128.0f / kPi));
        }
    }
}

void ErrorTilesReference(const float* canvas, const float* target, uint32_t size, float* tiles) {
    const uint32_t T = size / 8;
    for (uint32_t ty = 0; ty < T; ++ty) {
        for (uint32_t tx = 0; tx < T; ++tx) {
            double sum = 0.0;
            for (uint32_t j = 0; j < 8; ++j) {
                for (uint32_t i = 0; i < 8; ++i) {
                    const size_t q = (static_cast<size_t>(ty * 8 + j) * size + (tx * 8 + i)) * 3;
                    for (int c = 0; c < 3; ++c) {
                        const double d = static_cast<double>(canvas[q + c]) - target[q + c];
                        sum += d * d;
                    }
                }
            }
            tiles[ty * T + tx] = static_cast<float>(sum);
        }
    }
}

double RmsError255(const float* a, const float* b, size_t count) {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        sum += d * d;
    }
    return 255.0 * std::sqrt(sum / static_cast<double>(count));
}

std::vector<float> DialsToFloat(const uint8_t* dials, size_t count) {
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i) out[i] = static_cast<float>(std::min<uint8_t>(dials[i], 127));
    return out;
}

std::vector<uint8_t> DialsToBytes(const float* dials, size_t count) {
    std::vector<uint8_t> out(count);
    for (size_t i = 0; i < count; ++i) {
        const float v = std::floor(dials[i] + 0.5f);
        out[i] = static_cast<uint8_t>(std::min(127.0f, std::max(0.0f, std::isfinite(v) ? v : 0.0f)));
    }
    return out;
}

bool DialsValid(const float* dials, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const float v = dials[i];
        if (!(v >= 0.0f && v <= 127.0f) || v != std::floor(v)) return false;
    }
    return true;
}

std::vector<float> AwkwardTestCard(uint32_t strokes) {
    std::vector<float> card(CardDials(strokes), 0.0f);
    card[0] = 40; card[1] = 70; card[2] = 100;                 // a bluish background
    uint32_t seed = 12345u;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 8) & 0xFFFFu) / 65536.0f; };
    for (uint32_t n = 0; n < strokes; ++n) {
        float* d = card.data() + kHeaderDials + static_cast<size_t>(n) * kStrokeDials;
        const uint32_t kind = n % 8u;
        const uint32_t x14 = static_cast<uint32_t>(next() * 16383.0f);
        const uint32_t y14 = static_cast<uint32_t>(next() * 16383.0f);
        d[kXCoarse] = static_cast<float>(x14 >> 7); d[kXFine] = static_cast<float>(x14 & 127u);
        d[kYCoarse] = static_cast<float>(y14 >> 7); d[kYFine] = static_cast<float>(y14 & 127u);
        d[kSize] = std::floor(40.0f + next() * 60.0f);
        d[kAspect] = std::floor(next() * 127.0f);
        d[kAngle] = std::floor(next() * 127.0f);
        d[kSoftness] = std::floor(next() * 127.0f);
        d[kOpacity] = std::floor(60.0f + next() * 67.0f);
        d[kRed] = std::floor(next() * 127.0f);
        d[kGreen] = std::floor(next() * 127.0f);
        d[kBlue] = std::floor(next() * 127.0f);
        switch (kind) {
            case 0: d[kAspect] = 0; d[kSoftness] = 0; break;                       // thinnest crisp line
            case 1: d[kSize] = 127; d[kAspect] = 127; d[kSoftness] = 127; break;   // huge soft round dab
            case 2: d[kSize] = 0; d[kAspect] = 127; break;                         // tiniest dab
            case 3: d[kXCoarse] = 127; d[kXFine] = 127; d[kSize] = 90; break;      // on the right edge: spills over
            case 4: d[kYCoarse] = 0; d[kYFine] = 0; d[kAngle] = 32; break;         // on the top edge, tilted 45 degrees
            case 5: d[kOpacity] = 0; break;                                        // an empty stroke
            case 6: d[kOpacity] = 127; d[kSoftness] = 0; d[kAspect] = 60; break;   // solid crisp oval
            default: break;
        }
    }
    return card;
}

std::vector<float> TestPicture(uint32_t size, uint32_t variant) {
    std::vector<float> img(static_cast<size_t>(size) * size * 3);
    const float s = static_cast<float>(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / s;
            const float v = (static_cast<float>(y) + 0.5f) / s;
            float r = 0.15f + 0.7f * u;
            float g = 0.25f + 0.5f * v;
            float b = 0.8f - 0.5f * u * v;
            if (variant & 1u) { r = 0.9f - 0.6f * v; g = 0.3f + 0.4f * u; b = 0.2f + 0.5f * (u + v) * 0.5f; }
            const float d1 = std::sqrt((u - 0.35f) * (u - 0.35f) + (v - 0.4f) * (v - 0.4f));
            if (d1 < 0.2f) { r = 0.95f; g = 0.75f; b = 0.1f; }                          // a yellow disc with a hard edge
            if (u > 0.6f && u < 0.85f && v > 0.55f && v < 0.9f) { r = 0.1f; g = 0.5f; b = 0.25f; } // a green block
            if (std::fabs(u - v) < 0.02f) { r = 0.05f; g = 0.05f; b = 0.05f; }          // a thin dark diagonal
            const size_t p = (static_cast<size_t>(y) * size + x) * 3;
            img[p + 0] = Saturate(r); img[p + 1] = Saturate(g); img[p + 2] = Saturate(b);
        }
    }
    return img;
}

} // namespace StrokeCodec
} // namespace XboxOTFM
