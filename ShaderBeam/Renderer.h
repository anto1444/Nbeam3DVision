/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "Common.h"
#include "UI.h"
#include "Charts.h"
#include "RenderContext.h"
#include "ShaderManager.h"
#include "EmitterManager.h"
#include "StereoConvert.h"
#include "DepthAI.h"
#include <chrono>
#include <deque>

namespace ShaderBeam
{

class CaptureBase;

class Renderer
{
public:
    Renderer(Options& options, UI& ui, Watcher& watcher, ShaderManager& shaderManager);

    void Start(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context);
    void Stop();
    void Render(bool present, bool ui = true);
    void Present(bool vsync);

    const winrt::com_ptr<ID3D11Texture2D>& GetNextInput() const;
    bool                                   NewInputRequired() const;
    void                                   BufferReset() { if(m_ringPrimed || !m_ringQueue.empty()) DestroyRing(); } // buffer not in use: nothing stale is kept
    void                                   AdaptFrameBuffer(); // Auto buffer size (Options::frameBuffer = -1), called every loop of the render thread
    int                                    FrameBufferLimit() const; // ring size: 5 frames, or more with the map buffer (as many frames as fit in about 512 MB, up to 30)
    bool                                   MapSync() const; // 2D to 3D (AI) with "Map buffer": each image waits for its own depth map
    bool                                   BufferActive() const; // anti-stutter buffer in use (Options::frameBuffer)
    void                                   BufferedStep(const std::shared_ptr<CaptureBase>& capture); // replaces PollCapture while the buffer is active
    void                                   RollInput(bool newFrame);
    void                                   ConvertStereoInput(bool newFrame); // SBS/TB/Checkerboard -> one eye, call after capture Poll and before RollInput


    // SubFrames modes (see SubFrameModes.h). Auto: UpdateSubFramesMode() every vsync measures the game frame rate and changes
    // SubFrames live.
    void UpdateSubFramesMode();
    // live changes asked by the UI (see Options::liveReload / liveLatency): called by the render thread between two frames
    void ApplyLiveChanges();
    bool DesktopEffectsOff() const { return m_options.desktopEffectsOff && !m_options.captureWindow; } // tray option: desktop capture, no effects, no 3D, emitter idle
    bool EffectsOff() const { return (m_options.effectsSuspended && m_options.ui) || (m_options.desktopEffectsOff && !m_options.captureWindow) || m_options.processResizing || m_options.processUnfocused; } // HUD visible and effects suspended: no shader, no 3D decoding (the emitters keep running)
    bool Bypassing() const { return m_bypass || EffectsOff(); } // low fps with "Disable BFI": poll the capture every vsync, no shader

    void Benchmark(const std::shared_ptr<CaptureBase>& capture);

private:
    Options&       m_options; // non-const: the emitter manager writes auto-config results back and reads live emitter settings
    UI&            m_ui;
    Watcher&       m_watcher;
    Charts         m_charts;
    RenderContext  m_renderContext;
    ShaderManager& m_shaderManager;
    EmitterManager m_emitters;

    // 3D source decoding (only when StereoSource != FrameSequential)
    StereoConverter                                       m_stereo;
    bool                                                  m_stereoActive { false };
    bool                                                  m_stereoHaveRaw { false };
    winrt::com_ptr<ID3D11Texture2D>                       m_stereoRaw;
    winrt::com_ptr<ID3D11ShaderResourceView>              m_stereoRawView;
    std::vector<winrt::com_ptr<ID3D11RenderTargetView>>   m_stereoTargets;
    Eye                                                   StereoImageEye();

    // 2D to 3D (AI): depth model on a worker thread, small downscaled frames in, depth map out
    DepthEstimator                                        m_depth;
    winrt::com_ptr<ID3D11Texture2D>                       m_depthSmall;   // frame reduced to the model size
    winrt::com_ptr<ID3D11RenderTargetView>                m_depthSmallRtv;
    winrt::com_ptr<ID3D11Texture2D>                       m_depthStaging; // CPU readback of m_depthSmall
    winrt::com_ptr<ID3D11Texture2D>                       m_depthGpu;     // latest depth map (R16_UNORM)
    winrt::com_ptr<ID3D11ShaderResourceView>              m_depthGpuSrv;
    std::vector<uint16_t>                                 m_depthBuf;
    int                                                   m_depthW { 0 };
    int                                                   m_depthH { 0 };
    int                                                   m_depthModelWidth { 0 };
    bool                                                  m_depthUseGpu { true };
    bool                                                  m_depthCopyPending { false };
    unsigned                                              m_depthTick { 0 };
    std::chrono::steady_clock::time_point                 m_depthLastStart {};
    std::chrono::steady_clock::time_point                 m_depthSubmitStamp {}; // when the frame of the map in progress was taken
    double                                                m_depthLatencyMs { 0.0 }; // frame taken -> depth map ready (smoothed)
    void                                                  CreateDepthResources();
    void                                                  DestroyDepthResources();
    void                                                  UpdateDepth(bool newFrame);
    int                                                   GetNextSlotForStereo() const;


    // anti-stutter buffer: captured game frames wait in a ring of textures and are shown at the content cadence
    std::vector<winrt::com_ptr<ID3D11Texture2D>> m_ring;
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> m_ringSrv; // views of the ring frames (the depth estimation reads the newest one)
    std::deque<int>                              m_ringQueue; // indices of the buffered frames, oldest first
    std::vector<int>                             m_ringFree;
    bool                                         m_ringPrimed { false };
    int                                          m_ringSkip { 0 };
    double                                       m_ringIntervalMs { 8.0 };
    // adaptive buffer (frameBuffer = -1): worst gap between two game frames over the last seconds decides the size
    std::chrono::steady_clock::time_point        m_fbSliceStart {};
    double                                       m_fbSliceMax { 0.0 };
    double                                       m_fbSlices[4] { 0.0, 0.0, 0.0, 0.0 };
    std::chrono::steady_clock::time_point        m_fbLowSince {};
    std::chrono::steady_clock::time_point        m_fbLastChange {};
    bool                                         m_fbUnderrun { false };
    double                                       m_fbMeanMs { 8.0 };
    std::chrono::steady_clock::time_point        m_ringLastArrival {};
    std::vector<std::chrono::steady_clock::time_point> m_ringTime; // capture time of each ring frame (2D to 3D (AI): pairs images and depth maps)
    bool                                         m_ringArrived { false };
    int                                          m_mapFrames { 0 }; // map buffer: extra game frames held so the depth maps match the images
    std::chrono::steady_clock::time_point        m_mapLowSince {};
    int                                          m_ringStepCount { 0 }; // content requests since the last game frame was taken (divisor pacing) // a new frame entered the ring since the last depth map was started
    void                                         EnsureRing();
    void                                         DestroyRing();

    // SubFrames modes
    int             m_mode { SF_MANUAL }; // effective mode for this run (Options::EffectiveSubFramesMode at Start)
    AutoSubFrames   m_auto;
    bool            m_bypass { false };            // low fps with "Disable BFI": the image is shown as it is

    int  m_appliedSubFrames { 0 };
    void ApplySubFramesLive();
    void Create();
    void StartEmitters();
    void ResetModeState();
    void ReloadLive();
    void CreateInputs();
    void Destroy();
    void DestroyInputs();
    void WaitTillIdle();
    int  GetNextSlot() const;

    winrt::com_ptr<IDXGISwapChain1>         m_swapChain { nullptr };
    bool                                    m_tearing { false };      // swap chain created with tearing allowed (Options::Tearing)
    winrt::com_ptr<IDXGIOutput>             m_tearOutput;             // output waited on for the tearing line
    // beam racing (tearing + Options::beamRacing): scanout position read through D3DKMTGetScanLine
    unsigned                                m_kmtAdapter { 0 };
    unsigned                                m_kmtSource { 0 };
    unsigned                                m_kmtLines { 0 };         // visible lines of the output
    winrt::com_ptr<ID3D11Query>             m_gpuDone;                // the frame is drawn: its flip is not delayed by the GPU
    float                                   m_tearSafe { 0.0f };      // where the tear may land in the frame just rendered (0 = vblank only)
    double                                  m_brLastF { 1.0 };
    bool                                    m_brLastVbl { true };
    bool                                    m_brNewRefresh { true };
    void                                    OpenScanline();
    void                                    CloseScanline();
    bool                                    BeamRacePresent();
    winrt::com_ptr<ID3D11RasterizerState>   m_rasterizerState { nullptr };
    winrt::com_ptr<ID3D11DepthStencilState> m_depthStencilState { nullptr };
    winrt::com_ptr<ID3D11RenderTargetView>  m_uiTargetView { nullptr };
    winrt::com_ptr<ID3D11VertexShader>      m_toneVs;    // capture black / grey level (Windows HDR)
    winrt::com_ptr<ID3D11PixelShader>       m_tonePs;      // black / grey level + brightness (reads a copy of the picture)
    winrt::com_ptr<ID3D11Buffer>            m_toneCb;
    winrt::com_ptr<ID3D11Texture2D>         m_toneCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> m_toneCopySrv;
    UINT                                    m_toneW { 0 }, m_toneH { 0 };
    void                                    ApplyCaptureTone();
#if _DEBUG
    winrt::com_ptr<ID3D11Debug> m_debug { nullptr };
#endif
};
} // namespace ShaderBeam