#pragma once
// The dial table of the Pure MIDI Dials card, in one place on the processor side. The converter, the preview, the replay, the TV and the
// (later) Painter all go through the decode functions here; Strokes.hlsl holds the very same maths for the graphics chip, and the start-up
// self-test compares the two. There is no Windows code in here, so everything is tested natively.
//
// A card is 3 header dials (background red, green, blue) followed by 12 dials per stroke. Every dial is a whole number 0..127.
// Stroke dial order: X coarse, X fine, Y coarse, Y fine, Size, Aspect, Angle, Softness, Opacity, Red, Green, Blue.

#include "ImageUtil.h"

#include <cstdint>
#include <string>
#include <vector>

namespace XboxOTFM {
namespace StrokeCodec {

constexpr uint32_t kHeaderDials = 3;
constexpr uint32_t kStrokeDials = 12;
constexpr uint32_t kMaxDial = 127;

enum Dial : uint32_t {
    kXCoarse = 0, kXFine, kYCoarse, kYFine, kSize, kAspect, kAngle, kSoftness, kOpacity, kRed, kGreen, kBlue
};

constexpr uint32_t CardDials(uint32_t strokes) { return kHeaderDials + kStrokeDials * strokes; }

// What the dials of one stroke mean, for a picture `size` pixels on a side (lengths are in those pixels).
struct Geom {
    float cx = 0, cy = 0;     // centre
    float R = 1, r2 = 1;      // long and short half axis
    float c = 1, s = 0;       // cosine and sine of the angle
    float band = 1;           // width of the soft rim, as a share of the radius (never narrower than about one pixel)
    float opacity = 0;        // 0 .. 1
    uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0; // the pixels the stroke can touch: columns x0 .. x1 - 1, rows y0 .. y1 - 1
};

// d12 = the 12 dials of a stroke (whole numbers stored as float).
Geom MakeGeom(const float* d12, uint32_t size);
// Paint cover at pixel (px, py), 0 .. 1, opacity included.
float Alpha(const Geom& g, uint32_t px, uint32_t py);

// Paints strokes [first, last) of a card onto `rgb` (size * size * 3 floats, 0..1) in card order. With `fromBackground` the picture first
// becomes the background colour of the card. `card` holds 3 + 12 * strokes numbers.
void PaintCard(const float* card, uint32_t strokes, uint32_t first, uint32_t last, uint32_t size, bool fromBackground, float* rgb);
// The same from the stored bytes, as an 8-bit picture.
bool PaintCardBytes(const uint8_t* dials, size_t dialCount, uint32_t size, Rgb8Image& out, uint32_t lastStroke = 0xFFFFFFFFu);

// Score of one candidate stroke on a canvas against a target (both size * size * 3 floats). The colour dials of d12 are replaced by the
// closed-form best colour, snapped to the 0..127 grid. delta = change in squared error (negative = the stroke helps).
struct Score {
    float delta = 0.0f;
    bool covers = false;
    float colour[3] = { 0, 0, 0 };    // dials 0..127
    double num[3] = { 0, 0, 0 };      // the sums behind the colour solve
    double den = 0.0;
};
Score ScoreCandidate(float* d12, uint32_t size, const float* canvas, const float* target);
// Change in error when the stroke d12 is painted with ITS OWN colour dials (nothing is solved).
float DeltaWithColour(const float* d12, uint32_t size, const float* canvas, const float* target);

// The Sobel edge map exactly as CSEdgeMap makes it: for every pixel two numbers, how strong the edge is (0 .. about 1) and the direction
// ALONG the edge as an Angle dial (0 .. 127.99). `out` holds size * size * 2 floats.
void EdgeMapReference(const float* rgb, uint32_t size, float* out);
// Sum of squared differences of every 8 x 8 tile, as CSErrorMap makes it: (size / 8)^2 numbers, row after row.
void ErrorTilesReference(const float* canvas, const float* target, uint32_t size, float* tiles);

// Error between two float pictures, root-mean-square on the 0..255 scale.
double RmsError255(const float* a, const float* b, size_t count);

// Bytes <-> float dials.
std::vector<float> DialsToFloat(const uint8_t* dials, size_t count);
std::vector<uint8_t> DialsToBytes(const float* dials, size_t count);
// True when every dial is a whole number in 0..127.
bool DialsValid(const float* dials, size_t count);

// A card that exercises every corner of the dial table (soft, tilted, thin, overlapping, spilling over the edge, tiny, huge, empty)
// for the self-test and the tests. Returns the card as floats, 3 + 12 * strokes numbers.
std::vector<float> AwkwardTestCard(uint32_t strokes);

// A smooth built-in test picture (gradients and a few shapes), float RGB 0..1, size * size * 3.
std::vector<float> TestPicture(uint32_t size, uint32_t variant);

} // namespace StrokeCodec
} // namespace XboxOTFM
