/*
ShaderBeam: shader effect overlay -- stereo emitter settings
Plain data only (no Windows / D3D dependency) so it can be shared by Options, ini, UI and the emitters.
Parameter names are the usual ones of active 3D emitter software (emitter, ComPort, NVIDIA groups).
MIT License
*/

#pragma once

#include <vector>

namespace ShaderBeam
{

enum class Eye : int { Left = 0, Right = 1 };

// Layout of the captured 3D content (what ShaderBeam receives from the capture)
enum class StereoSource : int
{
    Off             = 0, // 3D disabled: no decoding, emitters off
    SideBySide      = 1, // left | right, each half squeezed horizontally
    TopBottom       = 2, // left over right, each half squeezed vertically
    Checkerboard    = 3, // left on (x+y) even pixels, right on odd pixels
    AiDepth         = 4  // plain 2D picture: an AI depth map is estimated and both eyes are synthesized from it
};

// ComPort emitter settings (Arduino sketches compatible with WibbleWobble)
struct ComPortConfig
{
    int comPortNo { -1 }; // -1 = None (emitter disabled)
};

// NVIDIA 3D Vision emitter settings
struct NvidiaConfig
{
    float openDurationUs { 0 };
};

// "2D to 3D (AI)" settings
struct AiDepthConfig
{
    int   depthFps { 30 };       // maximum depth maps computed per second (1..80)
    int   modelWidth { 308 };    // width (pixels, multiple of 14) of the picture given to the depth model
    float strength { 3.0f };     // maximum parallax, % of the screen width
    float convergence { 50.0f }; // depth (0 = far .. 100 = near) that stays on the screen plane
    float smoothing { 50.0f };   // temporal smoothing of the depth map, %
    float antiHalo { 30.0f };    // keeps foreground depth from bleeding onto the background around objects, %
    bool  invert { false };      // swap near and far
    bool  useGpu { true };       // DirectML (GPU); falls back to the CPU
    bool  showDepth { false };   // show the depth map instead of the picture
};

struct StereoEmitterSettings
{
    // 3D rendering: how the captured frame is turned into one image per eye. The eye changes once per content
    // frame, i.e. every SubFrames output frames (eye rate = display Hz / SubFrames).
    StereoSource  sourceFormat { StereoSource::Off };

    // common emitter settings
    bool          syncAuto { true };      // Auto: the rate is picked among the display Hz / n between 80 and 165 Hz (SubFrames follows)
    float         syncRateHz { 120 };     // manual rate in Hz (SubFrames follows: display Hz / rate)
    int           syncOffsetUs { 0 };     // from vsync; negative = signal before vsync
    bool          swapEyes { false };     // reverses the eye order

    ComPortConfig    comPort;
    AiDepthConfig    ai;
    NvidiaConfig     nvidia;

    // the emitters run whenever 3D rendering is not Off
    bool hold { false };          // runtime only: 3D is held off for a moment (refresh rate change of the display)
    mutable bool rateBlocked { false }; // runtime only: Auto found no rate between 80 and 165 Hz for this display: emitter off
    bool AnyEnabled() const { return sourceFormat != StereoSource::Off && !hold && !rateBlocked; }
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

} // namespace ShaderBeam
