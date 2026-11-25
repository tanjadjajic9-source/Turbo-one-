// Strokes.hlsl - Xbox Series S/X D3D12 compute programs of the Image Trainer stroke converter.
//
// Every program below is compiled to its own .cso by fxc (profile cs_5_1, entry point = file name).
// The C++ side (GpuCore.cpp / Converter.cpp) binds ALL programs through ONE root signature:
//
//   root[0]      26 x 32-bit constants -> cbuffer StrokeConstants (b0)   (must match StrokeConstants in GpuCore.h)
//   root[1..4]   root SRVs             -> t0, t1, t2, t3
//   root[5..7]   root UAVs             -> u0, u1, u2
//
// Pictures are float RGB, 0..1, interleaved (r, g, b per pixel, row after row), one picture after the other.
// A "card" is 3 + 12 * N numbers (header: background red, green, blue; then 12 dials per stroke), every one a whole number 0..127,
// stored as float. The dial table (the decode contract) is implemented ONCE below, in MakeGeom() and Alpha(); StrokeCodec.cpp is the
// same maths on the processor and the self-test compares the two.
//
// Rules every program follows (so a bad launch can never touch memory outside its buffers):
//   * thread-group sizes are 64 or 8 x 8 threads (Xbox-friendly, no TDR-prone giant groups)
//   * every thread bounds-checks itself against the constants before reading or writing
//   * no groupshared memory and no barriers at all (fxc once crashed on the programs that had them)
//   * a launch never binds the same buffer as input and as output
//   * paint mixes as  new = under * (1 - a) + paint * a  with 0 <= a <= 1, so colours never burn out

cbuffer StrokeConstants : register(b0) {
    uint  gFirst;        //  0  first picture of this launch (the launch handles pictures gFirst .. gFirst + gBatch - 1)
    uint  gBatch;        //  1  pictures in this launch
    uint  gSize;         //  2  picture size in pixels (width = height)
    uint  gCands;        //  3  candidate strokes per picture (K)
    uint  gSlices;       //  4  slices per candidate (S): how many threads share one candidate's pixels
    uint  gStroke;       //  5  number of the stroke being made (PaintCard: the first stroke to paint)
    uint  gSeed;         //  6  seed of this run (every random choice is a hash of it, so a run can be repeated exactly)
    uint  gRound;        //  7  tuning round (0 = the first look)
    uint  gMode;         //  8  sub-stage inside one program
    uint  gCardFloats;   //  9  numbers per card (3 + 12 * strokes the card can hold)
    uint  gStrokeEnd;    // 10  PaintCard: one past the last stroke to paint
    uint  gOutSize;      // 11  PaintCard: size of the output picture in pixels
    uint  gRowFirst;     // 12  PaintCard: first output row of this launch
    uint  gRowCount;     // 13  PaintCard: output rows in this launch
    uint  gTilesPerRow;  // 14  ErrorMap / Scout: tiles of 8 x 8 pixels per picture row
    float gSizeLo;       // 15  Scout: lowest Size dial it may start from
    float gSizeHi;       // 16  Scout: highest Size dial
    float gAspectLo;     // 17  Scout: lowest Aspect dial
    float gAspectHi;     // 18  Scout: highest Aspect dial
    float gSoftLo;       // 19  Scout: lowest Softness dial
    float gSoftHi;       // 20  Scout: highest Softness dial
    float gOpacityLo;    // 21  Scout: lowest Opacity dial
    float gOpacityHi;    // 22  Scout: highest Opacity dial
    float gSigma;        // 23  Blur: width of the blur in pixels
    float gEdgeFollow;   // 24  Scout: 1 = the angle follows the edge direction of the picture (Detail), 0 = any angle
    float gStepScale;    // 25  Nudge: size of this round's steps (1 = the first round, then smaller)
};

StructuredBuffer<float>   gInA     : register(t0);
StructuredBuffer<float>   gInB     : register(t1);
StructuredBuffer<float>   gInC_Buf : register(t2);
StructuredBuffer<float>   gInD     : register(t3);
RWStructuredBuffer<float> gOutA    : register(u0);
RWStructuredBuffer<float> gOutB    : register(u1);
RWStructuredBuffer<float> gOutBufC : register(u2);

static const float PI = 3.14159265359f;

// Linear (1-D) dispatches are folded into a 2-D grid so that very large lists never need more than
// 65535 thread groups in X.  The C++ side always launches at most 32768 groups of 64 threads in X.
static const uint kLinearRowThreads = 2097152u;   // 32768 groups * 64 threads

// One candidate / best stroke record: 12 dials, then the score, a flag, and the decoded shape.
//   [0..11] dials   [12] change in error (negative = better)   [13] 1 = the stroke covers something
//   [14] centre x   [15] centre y   [16] long half-axis   [17] short half-axis   [18] cos(angle)   [19] sin(angle)
//   [20] edge band  [21] opacity    [22] first column     [23] end column       [24] first row    [25] end row
static const uint kRecord = 32u;

// A stroke has to lower the squared error by at least this much (picture values 0..1, summed over the pixels and colours it covers) to be
// kept. Below it the "gain" is only rounding noise in the sums (a flat picture would otherwise get strokes that change nothing).
static const float kMinGain = 1.0e-5f;

inline uint LinearIndex(uint3 tid) {
    return tid.x + tid.y * kLinearRowThreads;
}

// ---------------------------------------------------------------------------------------------
// Random numbers: a hash of (seed, a, b, c, d). The same inputs always give the same number in 0 .. 1.
// ---------------------------------------------------------------------------------------------
inline uint HashU(uint x) {
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}

inline float Rand01(uint a, uint b, uint c, uint d) {
    uint h = HashU(a * 2654435761u + HashU(b * 2246822519u + HashU(c * 3266489917u + HashU(d + gSeed))));
    return (float)(h >> 8u) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------------------------
// The dial table. A stroke is an oval centred on (X, Y); its long half is Size, its short half is Size * Aspect, turned by Angle.
// ---------------------------------------------------------------------------------------------
struct Geom {
    float cx;
    float cy;
    float R;
    float r2;
    float c;
    float s;
    float band;
    float opacity;
    uint x0;
    uint x1;
    uint y0;
    uint y1;
};

inline Geom MakeGeom(float xc, float xf, float yc, float yf, float sizeD, float aspD, float angD, float softD, float opD, uint size) {
    Geom g;
    float side = (float)size;
    g.cx = ((xc * 128.0f + xf) / 16383.0f) * side;
    g.cy = ((yc * 128.0f + yf) / 16383.0f) * side;
    g.R = 0.004f * pow(125.0f, sizeD / 127.0f) * side;
    g.r2 = g.R * pow(0.05f, (127.0f - aspD) / 127.0f);
    float th = angD * (PI / 128.0f);
    g.c = cos(th);
    g.s = sin(th);
    g.band = min(1.0f, max(softD / 127.0f, 1.0f / g.r2));   // soft band, never narrower than about one pixel
    g.opacity = opD / 127.0f;
    float rc = g.R * g.c;
    float rs = g.R * g.s;
    float qc = g.r2 * g.c;
    float qs = g.r2 * g.s;
    float halfW = sqrt(rc * rc + qs * qs) + 1.0f;
    float halfH = sqrt(rs * rs + qc * qc) + 1.0f;
    g.x0 = (uint)max(0.0f, floor(g.cx - halfW));
    g.x1 = (uint)min(side, ceil(g.cx + halfW));
    g.y0 = (uint)max(0.0f, floor(g.cy - halfH));
    g.y1 = (uint)min(side, ceil(g.cy + halfH));
    return g;
}

// How much paint covers the pixel (px, py), 0 .. 1 (opacity already included). Full strength in the middle, fading to nothing at the rim.
inline float Alpha(Geom g, uint px, uint py) {
    float dx = (float)px + 0.5f - g.cx;
    float dy = (float)py + 0.5f - g.cy;
    float u = dx * g.c + dy * g.s;
    float v = dy * g.c - dx * g.s;
    float a = u / g.R;
    float b = v / g.r2;
    float rho = sqrt(a * a + b * b);
    return g.opacity * saturate((1.0f - rho) / g.band);
}

// Writes one candidate record (dials, shape and a cleared score) at float offset `rec` of gOutA.
inline void StoreCandidate(uint rec, float xc, float xf, float yc, float yf, float sizeD, float aspD, float angD, float softD,
                           float opD, float rD, float gD, float bD) {
    Geom g = MakeGeom(xc, xf, yc, yf, sizeD, aspD, angD, softD, opD, gSize);
    gOutA[rec + 0] = xc;
    gOutA[rec + 1] = xf;
    gOutA[rec + 2] = yc;
    gOutA[rec + 3] = yf;
    gOutA[rec + 4] = sizeD;
    gOutA[rec + 5] = aspD;
    gOutA[rec + 6] = angD;
    gOutA[rec + 7] = softD;
    gOutA[rec + 8] = opD;
    gOutA[rec + 9] = rD;
    gOutA[rec + 10] = gD;
    gOutA[rec + 11] = bD;
    gOutA[rec + 12] = 0.0f;
    gOutA[rec + 13] = 0.0f;
    gOutA[rec + 14] = g.cx;
    gOutA[rec + 15] = g.cy;
    gOutA[rec + 16] = g.R;
    gOutA[rec + 17] = g.r2;
    gOutA[rec + 18] = g.c;
    gOutA[rec + 19] = g.s;
    gOutA[rec + 20] = g.band;
    gOutA[rec + 21] = g.opacity;
    gOutA[rec + 22] = (float)g.x0;
    gOutA[rec + 23] = (float)g.x1;
    gOutA[rec + 24] = (float)g.y0;
    gOutA[rec + 25] = (float)g.y1;
}

// The shape of a stored record, read back from the SRV `gInA` (Evaluate modes 0 and 2) .
inline Geom ReadGeomA(uint rec) {
    Geom g;
    g.cx = gInA[rec + 14];
    g.cy = gInA[rec + 15];
    g.R = gInA[rec + 16];
    g.r2 = gInA[rec + 17];
    g.c = gInA[rec + 18];
    g.s = gInA[rec + 19];
    g.band = gInA[rec + 20];
    g.opacity = gInA[rec + 21];
    g.x0 = (uint)gInA[rec + 22];
    g.x1 = (uint)gInA[rec + 23];
    g.y0 = (uint)gInA[rec + 24];
    g.y1 = (uint)gInA[rec + 25];
    return g;
}

// ---------------------------------------------------------------------------------------------
// 1. CSBlur  (setup, once per flight)   target = a blurred copy of the picture
//    gMode 0: horizontal blur  t0 = picture  -> u0 = scratch      gMode 1: vertical blur  t0 = scratch -> u0 = target
//    gMode 2: plain copy       t0 = picture  -> u0 = target       (gSigma is the width of the blur in pixels)
//    Threads: 8 x 8 over the picture, depth = pictures.
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSBlur(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= gSize || y >= gSize || tid.z >= gBatch) return;
    uint plane = gSize * gSize * 3u;
    uint base = (gFirst + tid.z) * plane;
    uint pix = base + (y * gSize + x) * 3u;
    if (gMode == 2u) {
        gOutA[pix + 0] = gInA[pix + 0];
        gOutA[pix + 1] = gInA[pix + 1];
        gOutA[pix + 2] = gInA[pix + 2];
        return;
    }
    int radius = min(16, (int)ceil(3.0f * gSigma));
    float inv = -1.0f / (2.0f * gSigma * gSigma);
    float sum = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    int last = (int)gSize - 1;
    for (int i = -radius; i <= radius; ++i) {
        float w = exp((float)(i * i) * inv);
        int sx = (int)x;
        int sy = (int)y;
        if (gMode == 0u) sx = clamp(sx + i, 0, last); else sy = clamp(sy + i, 0, last);
        uint q = base + ((uint)sy * gSize + (uint)sx) * 3u;
        sum += w;
        r += w * gInA[q + 0];
        g += w * gInA[q + 1];
        b += w * gInA[q + 2];
    }
    gOutA[pix + 0] = r / sum;
    gOutA[pix + 1] = g / sum;
    gOutA[pix + 2] = b / sum;
}

// ---------------------------------------------------------------------------------------------
// 2. CSBackground  (setup, once per picture)
//    gMode 0: average colour of the picture -> the 3 header dials of the card.   t0 = picture, u0 = cards.  One thread per picture.
//    gMode 1: fill the canvas with the header colour.                           t0 = cards,   u0 = canvas. 8 x 8 threads.
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSBackground(uint3 tid : SV_DispatchThreadID) {
    if (gMode == 0u) {
        // thread (x, 0, z) of the first group column handles picture z; every other thread does nothing
        if (tid.x != 0u || tid.y != 0u || tid.z >= gBatch) return;
        uint b = gFirst + tid.z;
        uint pixels = gSize * gSize;
        uint base = b * pixels * 3u;
        float r = 0.0f;
        float g = 0.0f;
        float bl = 0.0f;
        for (uint i = 0; i < pixels; ++i) {
            r += gInA[base + i * 3u + 0];
            g += gInA[base + i * 3u + 1];
            bl += gInA[base + i * 3u + 2];
        }
        float inv = 1.0f / (float)pixels;
        uint card = b * gCardFloats;
        gOutA[card + 0] = clamp(floor(r * inv * 127.0f + 0.5f), 0.0f, 127.0f);
        gOutA[card + 1] = clamp(floor(g * inv * 127.0f + 0.5f), 0.0f, 127.0f);
        gOutA[card + 2] = clamp(floor(bl * inv * 127.0f + 0.5f), 0.0f, 127.0f);
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    if (x >= gSize || y >= gSize || tid.z >= gBatch) return;
    uint b = gFirst + tid.z;
    uint card = b * gCardFloats;
    uint pix = (b * gSize * gSize + y * gSize + x) * 3u;
    gOutA[pix + 0] = gInA[card + 0] / 127.0f;
    gOutA[pix + 1] = gInA[card + 1] / 127.0f;
    gOutA[pix + 2] = gInA[card + 2] / 127.0f;
}

// ---------------------------------------------------------------------------------------------
// 3. CSEdgeMap  (setup, once per picture)   t0 = picture, u0 = edges [pictures, size * size, 2]
//    number 0 = how strong the edge is (0 .. about 1), number 1 = the direction ALONG the edge as an Angle dial (0 .. 127.99).
//    Threads: 8 x 8 over the picture, depth = pictures.
// ---------------------------------------------------------------------------------------------
inline float Luma(uint base, int x, int y) {
    int last = (int)gSize - 1;
    uint q = base + ((uint)clamp(y, 0, last) * gSize + (uint)clamp(x, 0, last)) * 3u;
    return 0.299f * gInA[q + 0] + 0.587f * gInA[q + 1] + 0.114f * gInA[q + 2];
}

[numthreads(8, 8, 1)]
void CSEdgeMap(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= gSize || y >= gSize || tid.z >= gBatch) return;
    uint b = gFirst + tid.z;
    uint base = b * gSize * gSize * 3u;
    int ix = (int)x;
    int iy = (int)y;
    float gx = (Luma(base, ix + 1, iy - 1) + 2.0f * Luma(base, ix + 1, iy) + Luma(base, ix + 1, iy + 1)) -
               (Luma(base, ix - 1, iy - 1) + 2.0f * Luma(base, ix - 1, iy) + Luma(base, ix - 1, iy + 1));
    float gy = (Luma(base, ix - 1, iy + 1) + 2.0f * Luma(base, ix, iy + 1) + Luma(base, ix + 1, iy + 1)) -
               (Luma(base, ix - 1, iy - 1) + 2.0f * Luma(base, ix, iy - 1) + Luma(base, ix + 1, iy - 1));
    float mag = sqrt(gx * gx + gy * gy) * 0.25f;
    float th = 0.0f;
    if (mag > 1e-6f) th = atan2(gx, -gy);   // the edge runs at a right angle to the change
    if (th < 0.0f) th += PI;
    if (th >= PI) th -= PI;
    uint o = (b * gSize * gSize + y * gSize + x) * 2u;
    gOutA[o + 0] = mag;
    gOutA[o + 1] = min(127.99f, th * (128.0f / PI));
}

// ---------------------------------------------------------------------------------------------
// 4. CSErrorMap  (Scout, every stroke)   t0 = canvas, t1 = target, u0 = tile sums [pictures, tiles, tiles]
//    How wrong each 8 x 8 tile of the canvas still is (sum of squared differences). One thread per tile.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSErrorMap(uint3 tid : SV_DispatchThreadID) {
    uint tiles = gTilesPerRow;
    uint perPicture = tiles * tiles;
    uint idx = LinearIndex(tid);
    if (idx >= gBatch * perPicture) return;
    uint bl = idx / perPicture;
    uint t = idx % perPicture;
    uint b = gFirst + bl;
    uint tx = t % tiles;
    uint ty = t / tiles;
    uint base = b * gSize * gSize * 3u;
    float sum = 0.0f;
    for (uint j = 0; j < 8u; ++j) {
        for (uint i = 0; i < 8u; ++i) {
            uint q = base + ((ty * 8u + j) * gSize + (tx * 8u + i)) * 3u;
            float d0 = gInA[q + 0] - gInB[q + 0];
            float d1 = gInA[q + 1] - gInB[q + 1];
            float d2 = gInA[q + 2] - gInB[q + 2];
            sum += d0 * d0 + d1 * d1 + d2 * d2;
        }
    }
    gOutA[b * perPicture + t] = sum;
}

// ---------------------------------------------------------------------------------------------
// 5. CSScout  (Scout, every stroke)   t0 = tile sums, t1 = edges, u0 = candidates [pictures, K, 32]
//    Makes K candidate strokes per picture: centres where the canvas is still wrong (a quarter of them in the worst tile, the rest
//    drawn in proportion to each tile's error), shape dials from the window of the current flight (the Detail flight turns the
//    stroke along the edge). One thread per candidate. The colour is not decided here: Evaluate solves it.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSScout(uint3 tid : SV_DispatchThreadID) {
    uint idx = LinearIndex(tid);
    if (idx >= gBatch * gCands) return;
    uint k = idx % gCands;
    uint bl = idx / gCands;
    uint b = gFirst + bl;
    uint tiles = gTilesPerRow;
    uint count = tiles * tiles;
    uint tbase = b * count;

    float total = 0.0f;
    float worst = -1.0f;
    uint worstTile = 0u;
    for (uint t = 0; t < count; ++t) {
        float v = gInA[tbase + t];
        total += v;
        if (v > worst) {
            worst = v;
            worstTile = t;
        }
    }
    uint tile = worstTile;
    if (total <= 1e-12f) {
        tile = min(count - 1u, (uint)(Rand01(b, gStroke, k, 1u) * (float)count));
    } else if ((k & 3u) != 0u) {
        float pick = Rand01(b, gStroke, k, 2u) * total;
        float acc = 0.0f;
        tile = count - 1u;
        for (uint t2 = 0; t2 < count; ++t2) {
            acc += gInA[tbase + t2];
            if (pick < acc) {
                tile = t2;
                break;
            }
        }
    }
    float side = (float)gSize;
    float px = (float)((tile % tiles) * 8u) + Rand01(b, gStroke, k, 3u) * 8.0f;
    float py = (float)((tile / tiles) * 8u) + Rand01(b, gStroke, k, 4u) * 8.0f;
    uint x14 = (uint)clamp(floor(px / side * 16383.0f + 0.5f), 0.0f, 16383.0f);
    uint y14 = (uint)clamp(floor(py / side * 16383.0f + 0.5f), 0.0f, 16383.0f);

    float sizeD = clamp(floor(gSizeLo + Rand01(b, gStroke, k, 5u) * (gSizeHi - gSizeLo) + 0.5f), 0.0f, 127.0f);
    float aspD = clamp(floor(gAspectLo + Rand01(b, gStroke, k, 6u) * (gAspectHi - gAspectLo) + 0.5f), 0.0f, 127.0f);
    float softD = clamp(floor(gSoftLo + Rand01(b, gStroke, k, 7u) * (gSoftHi - gSoftLo) + 0.5f), 0.0f, 127.0f);
    float opD = clamp(floor(gOpacityLo + Rand01(b, gStroke, k, 8u) * (gOpacityHi - gOpacityLo) + 0.5f), 0.0f, 127.0f);
    float angD = min(127.0f, floor(Rand01(b, gStroke, k, 9u) * 128.0f));
    if (gEdgeFollow > 0.5f) {
        uint ex = min(gSize - 1u, (uint)px);
        uint ey = min(gSize - 1u, (uint)py);
        uint e = (b * gSize * gSize + ey * gSize + ex) * 2u;
        if (gInB[e + 0] > 0.03f) {
            int ai = (int)floor(gInB[e + 1] + (Rand01(b, gStroke, k, 10u) - 0.5f) * 6.0f + 0.5f);
            ai = ((ai % 128) + 128) % 128;
            angD = (float)ai;
        }
    }
    StoreCandidate((b * gCands + k) * kRecord, (float)(x14 >> 7u), (float)(x14 & 127u), (float)(y14 >> 7u), (float)(y14 & 127u),
                   sizeD, aspD, angD, softD, opD, 0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------------------------
// 6. CSEvaluate  (Artist, every stroke and tuning round)   scores every candidate against the canvas it would be painted on.
//    A candidate's pixels are shared between gSlices threads (rows y0 + s, y0 + s + S, ...), so even one picture keeps the chip busy.
//    gMode 0: partial sums for the colour   t0 = candidates, t1 = canvas, t2 = target  -> u0 = partials [pictures, K, S, 4]
//    gMode 1: solve the colour (closed form, per channel c = sum a (t - under (1 - a)) / sum a^2), snap it to the 0..127 grid
//             t0 = partials -> u0 = candidates (dials 9..11, flag 13)                         threads: pictures * K
//    gMode 2: partial sums of the change in error with that colour   (same buffers as mode 0)
//    gMode 3: add the partials up -> u0 = candidates (number 12 = change in error)           threads: pictures * K
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSEvaluate(uint3 tid : SV_DispatchThreadID) {
    uint idx = LinearIndex(tid);
    if (gMode == 0u || gMode == 2u) {
        if (idx >= gBatch * gCands * gSlices) return;
        uint s = idx % gSlices;
        uint k = (idx / gSlices) % gCands;
        uint b = gFirst + idx / (gSlices * gCands);
        uint rec = (b * gCands + k) * kRecord;
        uint outp = ((b * gCands + k) * gSlices + s) * 4u;
        float n0 = 0.0f;
        float n1 = 0.0f;
        float n2 = 0.0f;
        float den = 0.0f;
        float delta = 0.0f;
        if (gMode == 2u && gInA[rec + 13] < 0.5f) {
            gOutA[outp + 0] = 0.0f;
            return;
        }
        Geom g = ReadGeomA(rec);
        float q0 = gInA[rec + 9] / 127.0f;
        float q1 = gInA[rec + 10] / 127.0f;
        float q2 = gInA[rec + 11] / 127.0f;
        uint base = b * gSize * gSize * 3u;
        for (uint y = g.y0 + s; y < g.y1; y += gSlices) {
            for (uint x = g.x0; x < g.x1; ++x) {
                float a = Alpha(g, x, y);
                if (a > 0.0f) {
                    uint p = base + (y * gSize + x) * 3u;
                    float uu0 = gInB[p + 0];
                    float uu1 = gInB[p + 1];
                    float uu2 = gInB[p + 2];
                    float tt0 = gInC_Buf[p + 0];
                    float tt1 = gInC_Buf[p + 1];
                    float tt2 = gInC_Buf[p + 2];
                    if (gMode == 0u) {
                        den += a * a;
                        n0 += a * (tt0 - uu0 * (1.0f - a));
                        n1 += a * (tt1 - uu1 * (1.0f - a));
                        n2 += a * (tt2 - uu2 * (1.0f - a));
                    } else {
                        float e0 = uu0 * (1.0f - a) + q0 * a - tt0;
                        float e1 = uu1 * (1.0f - a) + q1 * a - tt1;
                        float e2 = uu2 * (1.0f - a) + q2 * a - tt2;
                        float oo0 = uu0 - tt0;
                        float oo1 = uu1 - tt1;
                        float oo2 = uu2 - tt2;
                        delta += (e0 * e0 + e1 * e1 + e2 * e2) - (oo0 * oo0 + oo1 * oo1 + oo2 * oo2);
                    }
                }
            }
        }
        if (gMode == 0u) {
            gOutA[outp + 0] = n0;
            gOutA[outp + 1] = n1;
            gOutA[outp + 2] = n2;
            gOutA[outp + 3] = den;
        } else {
            gOutA[outp + 0] = delta;
        }
        return;
    }
    if (idx >= gBatch * gCands) return;
    uint k2 = idx % gCands;
    uint b2 = gFirst + idx / gCands;
    uint rec2 = (b2 * gCands + k2) * kRecord;
    uint pbase = (b2 * gCands + k2) * gSlices * 4u;
    if (gMode == 1u) {
        float m0 = 0.0f;
        float m1 = 0.0f;
        float m2 = 0.0f;
        float md = 0.0f;
        for (uint s2 = 0; s2 < gSlices; ++s2) {
            m0 += gInA[pbase + s2 * 4u + 0];
            m1 += gInA[pbase + s2 * 4u + 1];
            m2 += gInA[pbase + s2 * 4u + 2];
            md += gInA[pbase + s2 * 4u + 3];
        }
        if (md > 1e-9f) {
            gOutA[rec2 + 9] = clamp(floor(clamp(m0 / md, 0.0f, 1.0f) * 127.0f + 0.5f), 0.0f, 127.0f);
            gOutA[rec2 + 10] = clamp(floor(clamp(m1 / md, 0.0f, 1.0f) * 127.0f + 0.5f), 0.0f, 127.0f);
            gOutA[rec2 + 11] = clamp(floor(clamp(m2 / md, 0.0f, 1.0f) * 127.0f + 0.5f), 0.0f, 127.0f);
            gOutA[rec2 + 13] = 1.0f;
        } else {
            gOutA[rec2 + 13] = 0.0f;
        }
        return;
    }
    // gMode 3
    float total = 0.0f;
    for (uint s3 = 0; s3 < gSlices; ++s3) {
        total += gInA[pbase + s3 * 4u];
    }
    if (!(total < 1.0e29f && total > -1.0e29f)) total = 1.0e30f;   // not a number: never the best
    gOutA[rec2 + 12] = total;
}

// ---------------------------------------------------------------------------------------------
// 7. CSPick  (after Scout and after every tuning round)   t0 = candidates, u0 = best [pictures, 32]
//    The candidate with the lowest (most negative) change in error becomes the best stroke so far; on a tie the earlier one stays.
//    One thread per picture.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSPick(uint3 tid : SV_DispatchThreadID) {
    uint idx = LinearIndex(tid);
    if (idx >= gBatch) return;
    uint b = gFirst + idx;
    uint pick = 0u;
    float lowest = gInA[(b * gCands) * kRecord + 12];
    for (uint k = 1; k < gCands; ++k) {
        float d = gInA[(b * gCands + k) * kRecord + 12];
        if (d < lowest) {
            lowest = d;
            pick = k;
        }
    }
    uint from = (b * gCands + pick) * kRecord;
    for (uint i = 0; i < 26u; ++i) {
        gOutA[b * kRecord + i] = gInA[from + i];
    }
}

// ---------------------------------------------------------------------------------------------
// 8. CSNudge  (Tuner, every tuning round)   t0 = best, u0 = candidates
//    Candidate 0 is the best stroke unchanged; every other candidate moves ONE shape dial (X, Y, Size, Aspect, Angle, Softness or
//    Opacity) by a random step that shrinks round by round. The colour is never nudged: Evaluate solves it again. One thread per candidate.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSNudge(uint3 tid : SV_DispatchThreadID) {
    uint idx = LinearIndex(tid);
    if (idx >= gBatch * gCands) return;
    uint k = idx % gCands;
    uint b = gFirst + idx / gCands;
    uint br = b * kRecord;
    int x14 = (int)(gInA[br + 0] * 128.0f + gInA[br + 1] + 0.5f);
    int y14 = (int)(gInA[br + 2] * 128.0f + gInA[br + 3] + 0.5f);
    int sizeD = (int)(gInA[br + 4] + 0.5f);
    int aspD = (int)(gInA[br + 5] + 0.5f);
    int angD = (int)(gInA[br + 6] + 0.5f);
    int softD = (int)(gInA[br + 7] + 0.5f);
    int opD = (int)(gInA[br + 8] + 0.5f);
    if (k > 0u) {
        uint which = min(6u, (uint)(Rand01(b, gStroke, k, 20u + gRound * 16u) * 7.0f));
        float dir = Rand01(b, gStroke, k, 21u + gRound * 16u) < 0.5f ? -1.0f : 1.0f;
        float amount = (0.5f + 0.5f * Rand01(b, gStroke, k, 22u + gRound * 16u)) * dir * gStepScale;
        // a step is at least one dial unit, whatever the round
        float posStep = 2.0f / (float)gSize * 16383.0f;
        if (which == 0u) {
            x14 = clamp(x14 + (int)floor(amount * posStep + 0.5f), 0, 16383);
        } else if (which == 1u) {
            y14 = clamp(y14 + (int)floor(amount * posStep + 0.5f), 0, 16383);
        } else {
            float base = 6.0f;
            if (which == 3u) base = 8.0f;
            if (which == 4u) base = 6.0f;
            if (which >= 5u) base = 12.0f;
            int move = (int)floor(amount * base + 0.5f);
            if (move == 0) move = dir < 0.0f ? -1 : 1;
            if (which == 2u) sizeD = clamp(sizeD + move, 0, 127);
            if (which == 3u) aspD = clamp(aspD + move, 0, 127);
            if (which == 4u) angD = ((angD + move) % 128 + 128) % 128;
            if (which == 5u) softD = clamp(softD + move, 0, 127);
            if (which == 6u) opD = clamp(opD + move, 0, 127);
        }
    }
    StoreCandidate((b * gCands + k) * kRecord, (float)(x14 >> 7), (float)(x14 & 127), (float)(y14 >> 7), (float)(y14 & 127),
                   (float)sizeD, (float)aspD, (float)angD, (float)softD, (float)opD,
                   gInA[br + 9], gInA[br + 10], gInA[br + 11]);
}

// ---------------------------------------------------------------------------------------------
// 9. CSCommit  (Commit, every stroke)   t0 = best, u0 = canvas, u1 = cards
//    Paints the best stroke onto the canvas and writes its 12 dials into the card as stroke number gStroke. A stroke that does not
//    lower the error (by at least kMinGain) is written as an empty stroke (opacity 0) instead, so the card never lies about the canvas.
//    8 x 8 threads over the picture, depth = pictures.
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSCommit(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= gSize || y >= gSize || tid.z >= gBatch) return;
    uint b = gFirst + tid.z;
    uint br = b * kRecord;
    bool apply = gInA[br + 12] < -kMinGain && gInA[br + 13] > 0.5f;
    if (x == 0u && y == 0u) {
        uint card = b * gCardFloats + 3u + gStroke * 12u;
        if (card + 12u <= (b + 1u) * gCardFloats) {
            for (uint i = 0; i < 12u; ++i) {
                gOutB[card + i] = apply ? gInA[br + i] : 0.0f;
            }
        }
    }
    if (!apply) return;
    uint x0 = (uint)gInA[br + 22];
    uint x1 = (uint)gInA[br + 23];
    uint y0 = (uint)gInA[br + 24];
    uint y1 = (uint)gInA[br + 25];
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return;
    Geom g = ReadGeomA(br);
    float a = Alpha(g, x, y);
    if (a <= 0.0f) return;
    uint p = (b * gSize * gSize + y * gSize + x) * 3u;
    gOutA[p + 0] = gOutA[p + 0] * (1.0f - a) + (gInA[br + 9] / 127.0f) * a;
    gOutA[p + 1] = gOutA[p + 1] * (1.0f - a) + (gInA[br + 10] / 127.0f) * a;
    gOutA[p + 2] = gOutA[p + 2] * (1.0f - a) + (gInA[br + 11] / 127.0f) * a;
}

// ---------------------------------------------------------------------------------------------
// 10. CSPaintCard  (Preview, on demand)   t0 = cards, u0 = output pictures [pictures, gOutSize, gOutSize, 3]
//    Paints strokes gStroke .. gStrokeEnd - 1 of each card in card order, at any output size (sizes are shares of the picture side,
//    so a card paints the same at 64 px or at 1080p). gMode 0 starts from the background colour of the card, gMode 1 carries on from
//    what is already in the output (so a long card can be painted in several launches). The caller cuts big pictures into rows
//    (gRowFirst, gRowCount) so no launch gets too long. 8 x 8 threads over the rows, depth = pictures.
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSPaintCard(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = gRowFirst + tid.y;
    if (x >= gOutSize || tid.y >= gRowCount || y >= gOutSize || tid.z >= gBatch) return;
    uint b = gFirst + tid.z;
    uint card = b * gCardFloats;
    uint p = (b * gOutSize * gOutSize + y * gOutSize + x) * 3u;
    float r = 0.0f;
    float g = 0.0f;
    float bl = 0.0f;
    if (gMode == 0u) {
        r = gInA[card + 0] / 127.0f;
        g = gInA[card + 1] / 127.0f;
        bl = gInA[card + 2] / 127.0f;
    } else {
        r = gOutA[p + 0];
        g = gOutA[p + 1];
        bl = gOutA[p + 2];
    }
    for (uint n = gStroke; n < gStrokeEnd; ++n) {
        uint s = card + 3u + n * 12u;
        if (gInA[s + 8] > 0.5f) {
            Geom geo = MakeGeom(gInA[s + 0], gInA[s + 1], gInA[s + 2], gInA[s + 3], gInA[s + 4], gInA[s + 5], gInA[s + 6],
                                gInA[s + 7], gInA[s + 8], gOutSize);
            if (x >= geo.x0 && x < geo.x1 && y >= geo.y0 && y < geo.y1) {
                float a = Alpha(geo, x, y);
                if (a > 0.0f) {
                    r = r * (1.0f - a) + (gInA[s + 9] / 127.0f) * a;
                    g = g * (1.0f - a) + (gInA[s + 10] / 127.0f) * a;
                    bl = bl * (1.0f - a) + (gInA[s + 11] / 127.0f) * a;
                }
            }
        }
    }
    gOutA[p + 0] = r;
    gOutA[p + 1] = g;
    gOutA[p + 2] = bl;
}

// ---------------------------------------------------------------------------------------------
// 11. CSMeasure  (Measure, per picture, at the end of a flight and of the job)   t0 = canvas, t1 = picture, t2 = target, u0 = stats
//    stats [pictures, 8]: 0 = error against the picture (root-mean-square, 0 .. 255 scale), 1 = sum of squared differences to the
//    target, 2 = how many canvas numbers are not a number or outside 0 .. 1 (should be 0). One thread per picture.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSMeasure(uint3 tid : SV_DispatchThreadID) {
    uint idx = LinearIndex(tid);
    if (idx >= gBatch) return;
    uint b = gFirst + idx;
    uint count = gSize * gSize * 3u;
    uint base = b * count;
    float vsPicture = 0.0f;
    float vsTarget = 0.0f;
    float bad = 0.0f;
    for (uint i = 0; i < count; ++i) {
        float c = gInA[base + i];
        float d1 = c - gInB[base + i];
        float d2 = c - gInC_Buf[base + i];
        vsPicture += d1 * d1;
        vsTarget += d2 * d2;
        if (!(c >= -0.001f && c <= 1.001f)) bad += 1.0f;
    }
    gOutA[b * 8u + 0] = 255.0f * sqrt(vsPicture / (float)count);
    gOutA[b * 8u + 1] = vsTarget;
    gOutA[b * 8u + 2] = bad;
}
