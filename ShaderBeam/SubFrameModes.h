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

// Low frame rate rule (Auto): below the threshold (48) the BFI would flicker too slowly. Double: every game frame is lit twice
// (47 fps -> 94 Hz; under half the threshold the flashes stay at the threshold rate, 20 fps -> 48 Hz). Disable: no BFI.
struct LowFpsSettings
{
    float threshold { 48.0f }; // fps
    int   mode { 0 };          // below the threshold: 0 = double, 1 = disable BFI
};

// Auto settings
struct AutoSettings
{
    float intervalSec { 1.0f }; // how often the game frame rate is measured (0 = as fast as practical, every 0.25 s)
};

class LowFpsGate
{
public:
    void Reset() { m_low = false; }

    // call with each new frame rate measurement; hysteresis avoids flapping around the threshold
    void Update(double fps, const LowFpsSettings& s)
    {
        const double t = Threshold(s);
        if(!m_low && fps < t * 0.98)
            m_low = true;
        else if(m_low && fps > t * 1.04)
            m_low = false;
    }

    bool Low() const { return m_low; }
    bool Bypass(const LowFpsSettings& s) const { return m_low && s.mode == 1; }

    // flash rate (Hz) the display should have for a game running at fps
    double TargetHz(double fps, const LowFpsSettings& s) const
    {
        if(!m_low)
            return fps;
        if(s.mode == 1)
            return fps;
        const double t = Threshold(s);
        return fps >= t * 0.5 ? fps * 2.0 : t;
    }

    static double Threshold(const LowFpsSettings& s) { return std::clamp((double)s.threshold, 10.0, 240.0); }

private:
    bool m_low { false };
};

// Auto: measures the game frame rate and returns the SubFrames to use.
class AutoSubFrames
{
public:
    static constexpr int    kMax        = 16;
    static constexpr double kMinWindowMs = 250.0; // shortest measurement window (interval 0)

    void Reset()
    {
        m_t0  = 0;
        m_n0  = 0;
        m_fps = 0;
        m_gate.Reset();
    }

    // game frame rate measured over the last window (0 until known)
    float GameFps() const { return (float)m_fps; }
    bool  LowFps() const { return m_gate.Low(); }
    bool  Bypass(const LowFpsSettings& s) const { return m_gate.Bypass(s); }
    // flash rate the display is set to for the last measurement
    float TargetHz(const LowFpsSettings& s) const { return (float)m_gate.TargetHz(m_fps, s); }

    // call every vsync; returns the SubFrames to use (== current when nothing changes). The rate is measured every intervalSec
    // seconds (0 = every 0.25 s) and the result applies right away; the ratio has to be clearly past the middle between two integers
    // (hysteresis).
    int Update(double nowMs, unsigned long long totalFrames, double displayHz, int current, const LowFpsSettings& low, float intervalSec)
    {
        if(m_t0 == 0)
        {
            m_t0 = nowMs;
            m_n0 = totalFrames;
            return current;
        }
        const double window = (std::max)(kMinWindowMs, (double)std::clamp(intervalSec, 0.0f, 5.0f) * 1000.0);
        const double dt     = nowMs - m_t0;
        if(dt < window)
            return current;
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
            return current; // idle window / paused game: keep what we have
        m_gate.Update(fps, low);
        if(m_gate.Bypass(low))
            return current; // BFI disabled while the game is slow: SubFrames is not used
        const double ratio = displayHz / m_gate.TargetHz(fps, low);
        const int    cand  = std::clamp((int)std::lround(ratio), 1, kMax);
        if(cand == current || std::fabs(ratio - current) < 0.6)
            return current;
        return cand;
    }

private:
    double             m_t0 { 0 };
    unsigned long long m_n0 { 0 };
    double             m_fps { 0 };
    LowFpsGate         m_gate;
};

} // namespace ShaderBeam
