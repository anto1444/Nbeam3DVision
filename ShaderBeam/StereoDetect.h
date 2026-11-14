// Automatic 3D format detection (SBS / TB / Checkerboard / interlaced) from a captured frame.
// Header only, no D3D: the caller gives a pixel reader, so it can be tested on its own.
//
// Principle (colours compared, pure black / pure white / flat pixels ignored):
//  - SBS: a pixel of the left half is compared with the pixels of the right half on the same row (a small horizontal search, the parallax),
//    and with two "wrong" places (same row far away horizontally, another row): a real SBS matches much better in the right place.
//  - TB: the same, top against bottom.
//  - Checkerboard: neighbours left/right/up/down belong to the other eye, the diagonal ones to the same eye: in a checkerboard the
//    straight neighbours differ more than the diagonal ones (in a normal picture it is the other way round, the diagonal ones are farther).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ShaderBeam
{
struct StereoDetectResult
{
    int    samples { 0 };    // valid samples (coloured, textured pixels): too few = the picture is not readable (black, menu...)
    double sbs { 1.0 };      // match in the right place / match in the wrong places (low = SBS)
    double tb { 1.0 };       // same for top / bottom
    double checker { 0.0 };  // straight neighbour difference / diagonal neighbour difference (high = checkerboard)
    double rowRatio { 0.0 }; // vertical neighbour difference / 2-row difference (high = row interlaced)
    double colRatio { 0.0 }; // horizontal neighbour difference / 2-column difference (high = column interlaced)
    int    verdict { 0 };    // 0 = unclear, 1 = SBS, 2 = TB, 3 = checkerboard, 5 = interlaced rows, 6 = interlaced columns (= StereoSource values)
};

namespace StereoDetectDetail
{
struct Rgb
{
    float c[3];
};

inline float Diff(const Rgb& a, const Rgb& b)
{
    return (std::fabs(a.c[0] - b.c[0]) + std::fabs(a.c[1] - b.c[1]) + std::fabs(a.c[2] - b.c[2])) * (1.0f / 3.0f);
}

inline bool BlackOrWhite(const Rgb& a)
{
    const float mx = (std::max)({ a.c[0], a.c[1], a.c[2] });
    const float mn = (std::min)({ a.c[0], a.c[1], a.c[2] });
    return mx <= 0.04f || mn >= 0.96f;
}
} // namespace StereoDetectDetail

// Px: callable (int x, int y) -> StereoDetectDetail::Rgb, values 0..1, only called inside [0,W) x [0,H)
template <class Px>
StereoDetectResult DetectStereo(int W, int H, Px&& px)
{
    using namespace StereoDetectDetail;
    StereoDetectResult r;
    if(W < 256 || H < 128)
        return r;

    const int D  = W / 100 + 4; // search range of the parallax (SBS: in the squeezed half, so the same value is generous)
    const int DT = W / 60 + 4;  // TB: the parallax is the full one
    const int nx = 128, ny = 72;

    double sbsAl = 0, sbsBx = 0, sbsBy = 0, tbAl = 0, tbBx = 0, tbBy = 0;
    int    nSbs = 0, nTb = 0;
    double hSum = 0, gSum = 0;
    int    nCb = 0;
    double r1 = 0, r2 = 0, c1 = 0, c2 = 0;

    auto minDiffRow = [&](const Rgb& a, int cx, int y, int range, int lo, int hi) {
        float best = 1e9f;
        for(int d = -range; d <= range; d++)
        {
            const int x = cx + d;
            if(x < lo || x >= hi)
                continue;
            best = (std::min)(best, Diff(a, px(x, y)));
        }
        return best;
    };

    for(int j = 0; j < ny; j++)
    {
        const int y = 2 + (int)((long long)j * (H - 6) / ny);
        for(int i = 0; i < nx; i++)
        {
            const int x = 2 + (int)((long long)i * (W - 6) / nx);
            const Rgb a = px(x, y);
            if(BlackOrWhite(a))
                continue;
            // textured: some colour change around (flat areas tell nothing)
            const Rgb ar = px((std::min)(x + 3, W - 1), y), ad = px(x, (std::min)(y + 3, H - 1));
            if(Diff(a, ar) + Diff(a, ad) < 0.02f)
                continue;
            r.samples++;

            // ---- checkerboard: straight neighbours (other eye) against diagonal ones (same eye) ----
            {
                const Rgb right = px(x + 1, y), down = px(x, y + 1), diag = px(x + 1, y + 1);
                const Rgb leftDown = px(x - 1, y + 1);
                hSum += 0.5 * (Diff(a, right) + Diff(a, down));
                gSum += 0.5 * (Diff(a, diag) + Diff(a, leftDown));
                r1 += Diff(a, px(x, (std::min)(y + 1, H - 1)));
                r2 += Diff(a, px(x, (std::min)(y + 2, H - 1)));
                c1 += Diff(a, px((std::min)(x + 1, W - 1), y));
                c2 += Diff(a, px((std::min)(x + 2, W - 1), y));
                nCb++;
            }

            // ---- SBS: this pixel in the left half against the right half ----
            if(x >= D && x < W / 2 - D)
            {
                const int cx = x + W / 2;
                const int bx = W / 2 + (x + W / 5) % (W / 2); // same row, far away horizontally (in the right half)
                const int yb = (y + H / 5) % H;                // another row
                sbsAl += minDiffRow(a, cx, y, D, W / 2, W);
                sbsBx += minDiffRow(a, bx, y, D, W / 2, W);
                sbsBy += minDiffRow(a, cx, yb, D, W / 2, W);
                nSbs++;
            }
            // ---- TB: this pixel in the top half against the bottom half ----
            if(y < H / 2 - 1 && x >= DT && x < W - DT)
            {
                const int cy = y + H / 2;
                const int xb = DT + (x + W / 5) % (W - 2 * DT);   // same row, far away horizontally
                const int yb = H / 2 + (y + H / 5) % (H / 2);      // another row of the bottom half
                if(cy < H && yb < H)
                {
                    tbAl += minDiffRow(a, x, cy, DT, 0, W);
                    tbBx += minDiffRow(a, xb, cy, DT, 0, W);
                    tbBy += minDiffRow(a, x, yb, DT, 0, W);
                    nTb++;
                }
            }
        }
    }

    if(nSbs > 0)
        r.sbs = sbsAl / (std::max)(1e-6, (std::min)(sbsBx, sbsBy));
    if(nTb > 0)
        r.tb = tbAl / (std::max)(1e-6, (std::min)(tbBx, tbBy));
    if(nCb > 0 && gSum > 1e-6)
        r.checker = hSum / gSum;
    if(r2 > 1e-6)
        r.rowRatio = r1 / r2;
    if(c2 > 1e-6)
        r.colRatio = c1 / c2;

    // the picture must have enough readable pixels
    if(r.samples < 400 || nSbs < 150 || nTb < 150)
        return r;

    const bool sbs  = r.sbs < 0.50;
    const bool tb   = r.tb < 0.50;
    const bool cb   = r.checker > 1.05;
    if(sbs && !tb)
        r.verdict = 1;
    else if(tb && !sbs)
        r.verdict = 2;
    else if(sbs && tb)
        r.verdict = (r.sbs + 0.15 < r.tb) ? 1 : (r.tb + 0.15 < r.sbs) ? 2 : 0;
    else if(r.rowRatio > 1.0 && r.colRatio < 0.9)
        r.verdict = 5;
    else if(r.colRatio > 1.0 && r.rowRatio < 0.9)
        r.verdict = 6;
    else if(cb)
        r.verdict = 3;
    return r;
}
} // namespace ShaderBeam
