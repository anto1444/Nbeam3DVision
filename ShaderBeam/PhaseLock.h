/*
Nbeam 3D Vision: shader effect overlay
MIT License

BFI phase lock, the pure logic (no Windows, no GPU): the real code of Renderer and of the test bench (Tests/test_phase_lock.cpp).
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace Nbeam
{

// Light of a cycle one refresh shorter (its last subframe dropped), so the brightness stays even: a cycle of N refreshes gives
// E(N) = clear + dark x (N - clear); the shortened one must give E(N) x (N - 1) / N over its N - 1 refreshes. Never above 1 (the light can
// only be lowered). `dark` is the light left in a dark frame: 1 - Gain vs Blur in linear light, but the BFI multiplies the picture as it is
// captured, so for a gamma encoded picture (8 bit, no sRGB hardware) the light left is (1 - Gain vs Blur) ^ 2.2.
inline float PhaseShortCycleGainFor(int N, int clearFrames, float darkGain, bool linear)
{
    if(N < 2 || clearFrames <= 0)
        return 1.0f; // not the BFI shader
    const float left   = 1.0f - std::clamp(darkGain, 0.0f, 1.0f);
    const float dark   = linear ? left : powf(left, 2.2f);
    auto        energy = [&](int n) { const int c = (std::min)(clearFrames, n); return (float)c + (float)(n - c) * dark; };
    const float full   = energy(N);
    const float shortE = energy(N - 1);
    if(shortE <= 0.0f)
        return 1.0f;
    return std::clamp(full * (float)(N - 1) / (float)N / shortE, 0.0f, 1.0f);
}

// The multiplier to apply to the picture (as captured) in a shortened cycle for the light `gain` (linear light): the gamma encoded picture
// takes the power 1 / 2.2.
inline float PhaseGainOnPicture(float gain, bool linear) { return linear ? gain : powf(gain, 1.0f / 2.2f); }

// A game frame is only taken at the start of a BFI cycle (subframe 0): see Renderer.cpp for the whole idea. Sample() is called with the
// age of each new game frame when the cycle took it; the cycle is moved with `skip` shortened cycles.
struct PhaseLock
{
    std::vector<double> ages;
    double              windowStart { 0.0 };
    double              ignoreUntil { 0.0 };
    double              guard { -1.0 }; // shortest wait seen for a frame that was still taken (ms): a frame needs about that much to be ready
    bool                first { true }; // first measure not made yet
    double              p { 0.0 };      // refresh period and SubFrames the measures are for
    int                 n { 0 };
    int                 skip { 0 };         // cycles still to shorten by one refresh (each one moves the cycle one refresh earlier)
    bool                shortCycle { false }; // the cycle being shown is one refresh shorter
    double              lastSample { 0.0 };   // time of the last frame measured (a long gap = pause: the measure starts again)
    int                 state { 0 };          // 0 = off, 1 = measuring, 2 = locked, 3 = game frames at an unsteady moment of the cycle
    float               waitMs { 0.0f };      // median wait of the game frames before they are shown (ms)
    int                 corrections { 0 };    // cycle shifts made

    void Reset(int st)
    {
        lastSample  = 0.0;
        ages.clear();
        windowStart = 0.0;
        ignoreUntil = 0.0;
        guard       = -1.0;
        first       = true;
        skip        = 0; // (a shortened cycle already started ends normally)
        state       = st;
    }

    // not active: everything stops
    void Stop()
    {
        if(state != 0 || skip)
            Reset(0);
    }

    // the cycle that starts: true when it is one refresh shorter
    bool BeginCycle()
    {
        shortCycle = false;
        if(skip > 0)
        {
            skip--;
            shortCycle = true;
        }
        return shortCycle;
    }

    // now: ms (any origin); ageMs: age of the new game frame; P: refresh period (ms); N: SubFrames; intervalSec: "Measure interval"
    void Sample(double now, double ageMs, double P, int N, double intervalSec)
    {
        const double C = P * N; // one BFI cycle
    if(lastSample > 0.0 && now - lastSample > 500.0 && state != 0)
    {
        // back after a pause (the game paused): the position is kept, the measure starts again
        ages.clear();
        ignoreUntil = now + 300.0;
        windowStart = ignoreUntil;
        skip = 0;
    }
    lastSample = now;
    if(state == 0 || N != n || fabs(P - p) > 0.05)
    {
        // (re)start: nothing known yet
        Reset(1);
        n           = N;
        p           = P;
        windowStart = now;
        ignoreUntil = now + 300.0;
    }
    if(skip || shortCycle || now < ignoreUntil || ageMs < 0.0 || ageMs > 1000.0)
        return; // a move on its way or just made, or a frame without a usable time
    ages.push_back(ageMs);
    if(!first && state == 2 && ageMs > C - P * 0.5) // (only while locked on a steady rate)
    {
        // a frame just missed its cycle (the game drifted to the edge, slower than the cycle): one refresh later at once, without
        // waiting for the end of the measure (otherwise the frames would go on alternating taken / missed: doubled and skipped frames)
        // (N - 1 cycles one refresh shorter = one refresh later; the first one already takes the frames away from the edge)
        skip                = N - 1;
        corrections = corrections + 1;
        ages.clear();
        ignoreUntil = now + (N + 3) * C;
        windowStart = ignoreUntil;
        return;
    }
    const double window = std::clamp(intervalSec, 0.1, 2.0) * 1000.0; // "Measure interval"
    if(now - windowStart < window)
        return;
    std::vector<double> a;
    a.swap(ages);
    windowStart = now;
    if(a.size() < 8)
        return; // too few game frames (pause, loading...)
    first = false;
    std::sort(a.begin(), a.end());
    // a frame needs a little time to be ready after Windows made it: the shortest waits seen for frames that were taken tell how much
    const double low = a[a.size() / 50];
    guard     = guard < 0.0 ? low : (std::min)(guard, low);
    const double g   = std::clamp(guard, 0.2, P * 0.75);
    const double m   = (std::max)(0.4, P * 0.15); // margin for the variations not seen yet
    // cost of each shift: mean wait + a heavy price for the frames at the edge (they would be taken one cycle, missed the next)
    double bestCost = 0.0, cost0 = 0.0, risky0 = 0.0, bestRisky = 0.0;
    int    best     = 0;
    for(int k = 0; k < N; k++)
    {
        double sum = 0.0;
        int    risky = 0;
        for(const double age : a)
        {
            double x = fmod(age, C) - k * P; // wait if this frame is taken by the same cycle, once the cycle comes k refreshes earlier
            if(x < g - C)
                x += C;
            sum += x >= g ? x : x + C;
            if(fabs(x - g) < m || fabs(x + C - g) < m)
                risky++;
        }
        const double frac = (double)risky / a.size();
        const double cost = sum / a.size() + 2.0 * C * frac;
        if(k == 0)
        {
            cost0  = cost;
            risky0 = frac;
        }
        if(k == 0 || cost < bestCost)
        {
            bestCost  = cost;
            best      = k;
            bestRisky = frac;
        }
    }
    {
        // the wait now (median), for the HUD
        std::vector<double> w;
        w.reserve(a.size());
        for(const double age : a)
        {
            const double x = fmod(age, C);
            w.push_back(x >= g ? x : x + C);
        }
        std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
        waitMs = (float)w[w.size() / 2];
    }
    int shift = 0;
    if(best != 0 && bestCost < cost0 - P * 0.5)
        shift = best;
    // locked; "unsteady" when even the best position leaves many frames at the edge (game far from a steady rate)
    state = (shift ? bestRisky : risky0) > 0.10 ? 3 : 2;
    if(shift)
    {
        skip                = shift; // one refresh per cycle, over "shift" cycles
        corrections = corrections + 1;
        ignoreUntil = now + (shift + 3) * C; // the frames of the move itself are not measured
        windowStart = ignoreUntil;
    }
}
};

} // namespace Nbeam
