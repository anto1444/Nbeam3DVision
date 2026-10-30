/*
ShaderBeam: shader effect overlay -- stereo emitter interface
MIT License
*/

#pragma once

#include "EmitterSettings.h"
#include <memory>
#include <string>

namespace ShaderBeam
{

// Everything an emitter may need to know about the running output
struct EmitterEnv
{
    HWND           outputWindow { nullptr };
    unsigned       width { 0 };
    unsigned       height { 0 };
    double         vsyncRateHz { 60.0 }; // measured display refresh
    int            subFrames { 1 };      // output frames per content frame
    IDXGISwapChain* swapChain { nullptr };
    LUID           adapterLuid {};
    int            queuedFrames { 1 };   // presentation queue of the swap chain (0 = tearing): estimate of the Present -> screen delay

    // Sync rate in Hz: rate at which the glasses switch eye. 0 = auto = display Hz / SubFrames
    double SyncRateHz(const StereoEmitterSettings& s) const { return s.EffectiveSyncHz(vsyncRateHz, subFrames > 0 ? subFrames : 1); }
};

// Handed to on-screen emitters right before Present
struct EmitterFrame
{
    Eye                     eye { Eye::Left };
    int                     frameNo { 0 };
    int                     subFrameNo { 0 };
    ID3D11DeviceContext*    deviceContext { nullptr };
    ID3D11RenderTargetView* outputView { nullptr };
    unsigned                width { 0 };
    unsigned                height { 0 };
};

class IEmitter
{
public:
    virtual ~IEmitter() = default;
    virtual const char* Name() const = 0;
    // `settings` is non-const: auto-config writes the resolved timings back so the UI can show them.
    // On failure return false and set `status` (shown in the UI).
    virtual bool Init(StereoEmitterSettings& settings, const EmitterEnv& env, std::string& status) = 0;
    virtual void Deinit() = 0;
    virtual bool IsOnScreen() const { return false; }
    // on-screen emitters draw here (render thread, right before Present)
    virtual void DrawOverlay(const EmitterFrame&) { }
    // hardware emitters signal here (emitter sync thread, already offset in time). Must be fast.
    virtual void Sync(Eye) { }
    // environment changed live (SubFrames); emitters caching the sync rate must refresh
    virtual void OnEnvChanged(const EmitterEnv&) { }
};

std::unique_ptr<IEmitter> CreateComPortEmitter();
std::unique_ptr<IEmitter> CreateNvidiaEmitter();

} // namespace ShaderBeam
