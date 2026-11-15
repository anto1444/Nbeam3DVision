/*
Nbeam 3D Vision -- SubFrames modes (pure logic, no D3D)
MIT License

SubFrames can be chosen in two ways:
 - Manual   : the user types the number of display frames per content frame (original ShaderBeam behaviour);
 - Auto     : the game frame rate is measured on the captured process and SubFrames = round(display Hz / game fps),
              e.g. 120 fps on a 480 Hz display -> 4, 240 fps -> 2. The measurement is repeated every "detection
              interval" (0 to 5 seconds, AutoSettings below).
3D (stereo emitter) is only available in Manual mode.
*/

#pragma once

#include <algorithm>
#include <cmath>

namespace ShaderBeam
{

enum SubFramesMode
{
    SF_MANUAL   = 0,
    SF_AUTO     = 1,
};

// SubFrames threshold (Auto and Manual, desktop included): above it the BFI would flicker too slowly (on a 480 Hz display 10 = 48 Hz).
// Double: the BFI flashes twice as fast (SubFrames halved; far above the threshold the flashes stay at the threshold rate).
// Disable: no BFI. Not used while the SubFrames follow the 3D emitter.
struct LowFpsSettings
{
    int threshold { 10 }; // SubFrames
    int mode { 0 };       // above the threshold: 0 = double, 1 = disable BFI
};

struct LowFpsResult
{
    int  subFrames; // SubFrames to run
    bool low;       // the requested SubFrames is above the threshold
    bool bypass;    // ... and the BFI is disabled because of it
};

inline int LowThreshold(const LowFpsSettings& s) { return std::clamp(s.threshold, 2, 20); }

inline LowFpsResult ApplyLowRule(int requested, const LowFpsSettings& s)
{
    const int t = LowThreshold(s);
    if(requested <= t)
        return { requested, false, false };
    if(s.mode == 1)
        return { requested, true, true };
    return { requested <= 2 * t ? (std::max)(1, (requested + 1) / 2) : t, true, false };
}

// Auto settings
struct AutoSettings
{
    float intervalSec { 1.0f }; // how often the game frame rate is measured (0.1 to 2 s)
};

// Auto: measures the game frame rate and returns the SubFrames it asks for.
class AutoSubFrames
{
public:
    static constexpr int    kMax        = 16;
    static constexpr double kMinWindowMs = 100.0; // shortest measurement window (interval 0.1 s)

    void Reset()
    {
        m_t0  = 0;
        m_n0  = 0;
        m_fps = 0;
        m_req = 0;
    }

    // game frame rate measured over the last window (0 until known)
    float GameFps() const { return (float)m_fps; }

    // call every vsync; returns the SubFrames to use (== current when nothing changes). The rate is measured every intervalSec
    // seconds (0 = every 0.25 s) and the result applies right away; the ratio has to be clearly past the middle between two integers
    // (hysteresis).
    int Update(double nowMs, unsigned long long totalFrames, double displayHz, int current, float intervalSec)
    {
        if(m_req == 0)
            m_req = current;
        if(m_t0 == 0)
        {
            m_t0 = nowMs;
            m_n0 = totalFrames;
            return m_req;
        }
        const double window = (std::max)(kMinWindowMs, (double)std::clamp(intervalSec, 0.0f, 5.0f) * 1000.0);
        const double dt     = nowMs - m_t0;
        if(dt < window)
            return m_req;
        double fps = (double)(totalFrames - m_n0) * 1000.0 / dt;
        m_t0       = nowMs;
        m_n0       = totalFrames;
        // short windows hold only a few frames and are noisy: smooth them (a window of 1 s or more is used as it is,
        // a big change of frame rate is taken over right away)
        const double a = (std::min)(1.0, dt / 1000.0);
        if(a < 1.0 && m_fps >= 5.0 && std::fabs(fps - m_fps) < 0.25 * m_fps)
            fps = m_fps * (1.0 - a) + fps * a;
        m_fps = fps;
        if(fps < 5.0 || displayHz < 1.0)
            return m_req; // idle window / paused game: keep what we have
        const double ratio = displayHz / fps;
        const int    cand  = std::clamp((int)std::lround(ratio), 1, kMax);
        if(cand != m_req && std::fabs(ratio - m_req) >= 0.6)
            m_req = cand;
        return m_req; // the SubFrames the game rate asks for (ApplyLowRule gives what is run)
    }

private:
    double             m_t0 { 0 };
    unsigned long long m_n0 { 0 };
    double             m_fps { 0 };
    int                m_req { 0 }; // the requested SubFrames (before the threshold rule)
};

} // namespace ShaderBeam
