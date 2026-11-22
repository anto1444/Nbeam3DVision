/*
Nbeam 3D Vision: shader effect overlay -- stereo emitter settings
Plain data only (no Windows / D3D dependency) so it can be shared by Options, ini, UI and the emitters.
Parameter names are the usual ones of active 3D emitter software (emitter, ComPort, NVIDIA groups).
MIT License
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace Nbeam
{

enum class Eye : int { Left = 0, Right = 1 };

// Layout of the captured 3D content (what Nbeam receives from the capture)
enum class StereoSource : int
{
    Off             = 0, // 3D disabled: no decoding, emitters off
    SideBySide      = 1, // left | right, each half squeezed horizontally
    TopBottom       = 2, // left over right, each half squeezed vertically
    Checkerboard    = 3, // left on (x+y) even pixels, right on odd pixels
    AiDepth         = 4, // plain 2D picture: an AI depth map is estimated and both eyes are synthesized from it
    InterlacedH     = 5, // left eye on even rows, right eye on odd rows (row interleaved)
    InterlacedV     = 6  // left eye on even columns, right eye on odd columns (column interleaved)
};

// Watchdog of Window.cpp (runtime only): the renderer notes each picture presented and each graphics card loss; Nbeam restarts completely when
// the graphics card keeps being lost or nothing is shown any more
inline std::atomic<unsigned long long> g_lastPresentOk { 0 }; // GetTickCount64() of the last Present that worked (0 = renderer not started)
inline std::atomic<bool>               g_autoNoReadback { false }; // Auto source picture analysis switched off (the graphics card was lost with it)
inline std::atomic<int>                g_deviceLostTotal { 0 }; // graphics card losses since Nbeam started

// ComPort emitter settings (Arduino sketches compatible with WibbleWobble)
struct ComPortConfig
{
    int comPortNo { -1 }; // -1 = None (emitter disabled)
};

// NVIDIA 3D Vision emitter settings
struct NvidiaConfig
{
    mutable float openDurationUs { 0 }; // (timing of the glasses: kept for each emitter rate, see StereoEmitterSettings::ActivateTiming)
    // Offset correction: the value set by the user (openBase) and the value the correction put in its place (openNudged), 0 = none. Only the first is ever saved.
    mutable float openBase { 0 }, openNudged { 0 };
    mutable bool  openApplyDeferred { false }; // the emitter takes the corrected value later, in the first black frame (see EmitterManager::AfterPresent)
    float Saved() const { return openNudged > 0.0f && openDurationUs == openNudged ? openBase : openDurationUs; }
};

// "2D to 3D (AI)" settings
struct AiDepthConfig
{
    int   depthFps { 30 };       // maximum depth maps computed per second (1..80)
    int   modelWidth { 308 };    // width (pixels, multiple of 14) of the picture given to the depth model
    float strength { 3.0f };     // maximum parallax, % of the screen width
    float convergence { 50.0f }; // depth (0 = far .. 100 = near) that stays on the screen plane
    float smoothing { 50.0f };   // temporal smoothing of the depth map, %
    float antiHalo { 60.0f };    // keeps foreground depth from bleeding onto the background around objects, %
    bool  useGpu { true };       // DirectML (GPU); falls back to the CPU
    bool  showDepth { false };   // show the depth map instead of the picture
    bool  mapBuffer { true };    // map buffer: the image is held back until its own depth map is ready (needs a captured program)
};

struct StereoEmitterSettings
{
    // 3D rendering: how the captured frame is turned into one image per eye. The eye changes once per content
    // frame, i.e. every SubFrames output frames (eye rate = display Hz / SubFrames).
    StereoSource  sourceFormat { StereoSource::Off };
    bool          autoSource { false }; // "Auto source": the format (SBS / TB / Checkerboard) follows the picture, 3D starts when a 3D source is found
    // the layout actually decoded: the detected one in Auto mode (Off = not found yet, 2D picture)

    // common emitter settings
    bool          syncAuto { true };      // Auto: the rate is picked among the display Hz / n between 80 and 165 Hz (SubFrames follows)
    double        syncRateHz { 120.0 };   // manual rate in Hz (6 decimals, picked among display Hz / n) (SubFrames follows: display Hz / rate)
    mutable int   syncOffsetUs { 0 };     // from vsync; negative = signal before vsync
    float         syncAutoSec { 2.0f };   // Auto: how often the game frame rate is measured to pick the emitter rate (0.5 to 2 s)
    mutable bool  fpsCapped { false };    // runtime only: the game runs too fast for stable 3D, 3D and emitter are off until it comes back under the limit
    mutable bool  fullSbsBad { false };   // runtime only: Full SBS but the captured window is not twice as wide as the screen (3D and emitter off, shown as impossible)
    mutable float cappedFps { 0 };        // runtime only: the game frame rate that caused it
    mutable float fpsLimit { 0 };         // runtime only: the limit (half the highest emitter rate + 5)
    mutable int   autoDivisor { 0 };      // runtime only: Auto sync, the n of the rate picked from the game frame rate (0 = the highest rate)

    // The timing of the glasses (sync offset and open duration) can differ from one emitter rate to another: it is kept for each rate
    // (rounded to 1 Hz) and the right one is loaded when the rate in use changes, so switching rate never leaves the glasses on the
    // timing of another one. A rate seen for the first time starts from the timing in use.
    struct RateTiming
    {
        int   hz { 0 };
        int   offsetUs { 0 };
        float openUs { 0 };
        int   clearFrames { -1 }; // Clear Frames of the BFI for this rate (-1 = not known yet: the value in use is taken)
        int   swap { -1 };        // Swap Eyes for this rate (0 / 1, -1 = not known yet: the value in use is taken)
    };
    static constexpr int kTimingSlots = 16;
    mutable RateTiming   timings[kTimingSlots];
    mutable int          timingCount { 0 };
    mutable int          timingActive { -1 };
    void ActivateTiming(int hz) const
    {
        if(hz <= 0 || (timingActive >= 0 && timings[timingActive].hz == hz))
            return;
        if(timingActive >= 0)
        {
            timings[timingActive].offsetUs = syncOffsetUs;
            timings[timingActive].openUs   = nvidia.Saved();
            timings[timingActive].swap     = swapEyes ? 1 : 0;
        }
        int idx = -1;
        for(int i = 0; i < timingCount; i++)
            if(timings[i].hz == hz)
                idx = i;
        if(idx < 0)
        {
            idx = timingCount < kTimingSlots ? timingCount++ : (timingActive + 1) % kTimingSlots;
            timings[idx].hz       = hz;
            timings[idx].offsetUs = syncOffsetUs;
            timings[idx].openUs   = nvidia.Saved();
            timings[idx].clearFrames = -1;
            timings[idx].swap        = swapEyes ? 1 : 0;
        }
        else
        {
            syncOffsetUs            = timings[idx].offsetUs;
            nvidia.openDurationUs   = timings[idx].openUs;
            nvidia.openNudged       = 0.0f; // (a value of the user: the correction starts again from it)
            if(timings[idx].swap >= 0)
                swapEyes = timings[idx].swap == 1; // (a rate saved before this existed: the value in use is kept)
            else
                timings[idx].swap = swapEyes ? 1 : 0;
        }
        timingActive = idx;
    }
    bool          fullSbs { false };      // experimental, SBS only: the game window is twice the screen width, each eye shown whole (not squeezed)
    int           desyncMethod { 0 };             // Offset correction method: 0 = Open Duration -1 / +1 us (old), 1 = the signal that went out too early after a missed refresh is sent again at its right time
    float         expPeriodMarginPct { 0.0f };    // experiment (profile key expPeriodMarginPct=0..100): the free-running period of the NVIDIA emitter is this much longer than the eye period
    bool          offsetCorrection { false }; // a medium or strong micro-desync nudges the Open Duration by 1 us (the emitter then restarts its offset)
    mutable bool  swapEyes { false };     // reverses the eye order (mutable: the rate in use loads its own, see ActivateTiming)
    bool          scanoutSync { false };  // the glasses follow the real display refreshes (DXGI frame statistics) instead of the moment Present returned
    mutable int   scanoutSyncState { 0 }; // runtime only: 1 = working, -1 = statistics not available (the classic timing is used)
    mutable int   scanoutResyncs { 0 };   // runtime only: missed refreshes caught and corrected

    ComPortConfig    comPort;
    AiDepthConfig    ai;
    NvidiaConfig     nvidia;

    // the emitters run whenever 3D rendering is not Off
    bool hold { false };          // runtime only: 3D is held off for a moment (refresh rate change of the display)
    mutable bool rateBlocked { false }; // runtime only: Auto found no rate between 80 and 165 Hz for this display: emitter off
        bool AnyEnabled() const { return sourceFormat != StereoSource::Off && !hold && !rateBlocked && !fpsCapped && !fullSbsBad; }
    // eye switch rate of the emitter for the running output
    double EffectiveSyncHz(double displayHz, int subFrames) const
    {
        if(syncAuto)
            return (subFrames > 0 && displayHz > 0) ? displayHz / subFrames : 0.0;
        return syncRateHz;
    }
};

// implemented in EmitterComPort.cpp (Windows) -- numbers of COM ports present, ascending
std::vector<int> EnumerateComPorts();

} // namespace Nbeam
