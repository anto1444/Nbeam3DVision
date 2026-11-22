/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "Common.h"
#include "UI.h"
#include "Charts.h"
#include "RenderContext.h"
#include "PhaseLock.h"
#include "ShaderManager.h"
#include "EmitterManager.h"
#include "StereoConvert.h"
#include "DepthAI.h"
#include <chrono>
#include <future>
#include "StereoDetect.h"
#include <deque>

namespace Nbeam
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
    bool DeviceLost() const { return m_deviceLost; } // the graphics card was lost: the renderer must be restarted
    // the input textures exist (false while they are rebuilt, or when a live rebuild failed: nothing may be captured or drawn then)
    bool InputsReady() const { return m_inputsOk && !m_renderContext.inputTextures.empty() && !m_renderContext.inputSlots.empty(); }

    const winrt::com_ptr<ID3D11Texture2D>& GetNextInput() const;
    bool                                   NewInputRequired() const;
    void                                   BufferReset() { if(m_ringPrimed || !m_ringQueue.empty()) DestroyRing(); } // buffer not in use: nothing stale is kept
    float                                  PhaseShortCycleGain() const; // brightness of a cycle one refresh shorter (Simple BFI)
    void                                   AdaptQueue();        // Auto queue length (Queued Frames = Auto): one frame more when frames are late, one less after a long calm
    void                                   ApplyQueueNow();     // the queue length in use is applied live (frame latency) and announced to the glasses timing
    int                                    QueueFramesNow() const;
    void                                   AdaptFrameBuffer(); // adapted buffer size (Frame buffer), called every loop of the render thread
    int                                    FrameBufferLimit() const; // ring size: 5 frames, or more with the map buffer (as many frames as fit in about 512 MB, up to 30)
    bool                                   MapSync() const; // 2D to 3D (AI) with "Map buffer": each image waits for its own depth map
    bool                                   BufferActive() const; // anti-stutter buffer in use (Options::frameBuffer)
    void                                   BufferedStep(const std::shared_ptr<CaptureBase>& capture); // replaces PollCapture while the buffer is active
    void                                   RollInput(bool newFrame);
    bool                                   ConvertStereoInput(bool newFrame); // SBS/TB/Checkerboard/Interlaced -> one eye, call after capture Poll and before RollInput; returns newFrame
    void                                   PhaseSample(double ageMs); // BFI phase lock: age of the new game frame when the cycle took it


    // SubFrames modes (see SubFrameModes.h). Auto: UpdateSubFramesMode() every vsync measures the game frame rate and changes
    // SubFrames live.
    void UpdateSubFramesMode();
    // live changes asked by the UI (see Options::liveReload / liveLatency): called by the render thread between two frames
    void ApplyLiveChanges();
    bool EffectsHidden() const { return m_options.effectsSuspended; } // Show / Hide effects: no effects, no 3D, emitter idle
    bool EffectsOff() const { return m_options.effectsSuspended || m_options.processResizing || m_options.processUnfocused; } // HUD visible and effects suspended: no shader, no 3D decoding (the emitters keep running)
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
    // adapted buffer size (Frame buffer: 1 up to the value chosen, one step per 2 seconds)
    int                                          m_fbMode { 0 };
    double                                       m_fbSliceMax { 0.0 };
    double                                       m_fbSlices[2] { 0.0, 0.0 };
    std::chrono::steady_clock::time_point        m_fbLastChange {};
    std::chrono::steady_clock::time_point        m_fbSliceStart {};
    std::chrono::steady_clock::time_point        m_fbLowSince {};
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
    double             m_syncAutoT0 { 0 };  // 3D Sync Auto: start of the 3 s measure of the game frame rate
    unsigned long long m_syncAutoN0 { 0 };
    double             m_capT0 { 0 };       // 3D frame rate cap: start of the 1 s measure
    unsigned long long m_capN0 { 0 };
    // Auto 3D format: the captured frame is read back every 2 s (copy now, read at the next call: no GPU stall)
    winrt::com_ptr<ID3D11Texture2D> m_autoStaging;
    bool               m_autoPending { false };
    bool               m_autoMapped { false }; // the CPU copy stays mapped while the analysis thread reads it (unmapped once it is done)
    winrt::com_ptr<ID3D11Texture2D> m_autoMid; // GPU copy of the 3D source (the readback copies it one picture later, never straight from the capture texture)
    bool               m_autoMidReady { false };
    std::future<StereoDetectResult> m_autoFut;
    double             m_autoT0 { 0.0 };
    HWND               m_autoWin { nullptr }; // captured window the Auto source works for
    bool               m_autoFirst { true };  // no detection yet for this window: the first one is taken at once
    ULONGLONG          m_autoLastCall { 0 };
    int                m_autoCand { 0 };      // format seen by the last detection, waiting for a second one before it replaces the current one
    void               DetectAutoFormat(ID3D11Texture2D* picture); // Auto source: picture = the capture that just arrived (nullptr: only follows the analysis in progress)
    int                m_capOk { 0 };       // consecutive measures under the cap (3D comes back after 2)
    int                m_capHigh { 0 };
    int                m_clearSlot { -1 };  // rate whose Clear Frames the BFI parameter shows
    bool            m_bypass { false };            // low fps with "Disable BFI": the image is shown as it is
    float           m_uniformGain { 1.0f };        // Auto + Uniform brightness: picture brightness multiplier (see UpdateSubFramesMode)

    int  m_appliedSubFrames { 0 };
    void ApplySubFramesLive();
    // BFI phase lock (BFI sync 1): ages of the game frames when the cycle took them, and the shift asked for the cycle
    bool                PhaseLockActive() const;
    PhaseLock           m_phase;               // the logic (PhaseLock.h)
    void                PhaseApply();          // mirrors it to the options (HUD) and to the render context (the cycle being shown)
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
    // presentation statistics (HUD: "Direct presentation"): how Windows shows the overlay, and the frames shown late or dropped
    void                                    UpdatePresentStats();
    winrt::com_ptr<IDXGISwapChainMedia>     m_swapMedia;
    bool                                    m_psValid { false };
    UINT                                    m_psPresentCount { 0 }, m_psRefreshCount { 0 };
    long long                               m_psGlitches { 0 };
    long long                               m_psGlitchBig { 0 };      // of those, frames that lost 3 refreshes or more (a single lost refresh is not counted: nothing can avoid it)
    long long                               m_psGlitchEvents { 0 };   // frames that did not come at one per refresh (Auto queue)
    int                                     m_queueEnv { 3 };         // queue length the glasses timing knows
    long long                               m_qEventsSeen { 0 };
    std::chrono::steady_clock::time_point   m_qStart {}, m_qLastChange {}, m_qCalmSince {};
    long long                               m_qBigSeen { 0 };
    std::deque<std::chrono::steady_clock::time_point> m_qSmall; // recent single lost refreshes (3 within 20 s count as a problem)
    std::deque<std::chrono::steady_clock::time_point> m_qRises; // recent rises of the Auto queue (a game that keeps needing more keeps it longer)
    double                                  m_psStart { 0.0 };
    bool                                    m_tearing { false };      // swap chain created with tearing allowed (Options::Tearing)
    winrt::com_ptr<IDXGIOutput>             m_tearOutput;             // output waited on for the tearing line
    // beam racing (tearing + Options::beamRacing): scanout position read through D3DKMTGetScanLine
    unsigned                                m_kmtAdapter { 0 };
    unsigned                                m_kmtSource { 0 };
    unsigned                                m_kmtLines { 0 };         // visible lines of the output
    winrt::com_ptr<ID3D11Query>             m_gpuDone;                // the frame is drawn: its flip is not delayed by the GPU
    float                                   m_tearSafe { 0.0f };      // where the tear may land in the frame just rendered (0 = vblank only)
    bool                                    m_beamOk { false };
    std::atomic<bool>                       m_deviceLost { false };
    bool                                    m_inputsOk { false };
    void                                    CheckPresent(HRESULT hr);       // beam racing allowed for this frame (never in 3D: classic vblank timing for the glasses)
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
    // HUD drawn apart (30 times a second, full rate while dragged) into its own texture, then laid over every frame at a small, constant cost: the HUD
    // no longer makes the frame time vary, which kept 3D / tearing in step while it is shown
    winrt::com_ptr<ID3D11Texture2D>          m_hudTex;
    winrt::com_ptr<ID3D11RenderTargetView>   m_hudRtv;
    winrt::com_ptr<ID3D11ShaderResourceView> m_hudSrv;
    winrt::com_ptr<ID3D11VertexShader>       m_hudVs;
    winrt::com_ptr<ID3D11PixelShader>        m_hudPs;
    winrt::com_ptr<ID3D11BlendState>         m_hudBlend;
    UINT                                     m_hudW { 0 }, m_hudH { 0 };
    // tracer of the HUD clicks (Nbeam_ls.log)
    struct ClickWindow
    {
        int    frames { 0 };
        int    late { 0 };
        double maxGap { 0.0 };
        double maxHud { 0.0 };
        double maxPresent { 0.0 };
    };
    void        ClickTrace(double presentMs, double nowMs);
    ClickWindow m_trCur, m_trRef;
    double      m_trLastPresent { 0.0 };
    double      m_trStart { 0.0 };
    double      m_trHudMs { 0.0 };
    ULONGLONG   m_trClick { 0 };
    long long   m_trGlitch0 { 0 };
    int         m_minLate { 0 };     // late frames of the current minute (log)
    double      m_minMaxGap { 0.0 };
    double      m_minStart { 0.0 };
    RECT                                     m_hudArea {};     // area of the HUD texture that holds something (the overlay draws there only)
    bool                                     m_hudEmpty { false };
    bool                                     m_hudFailed { false };
    void                                     RenderHud();
    void                                    ApplyCaptureTone();
#if _DEBUG
    winrt::com_ptr<ID3D11Debug> m_debug { nullptr };
#endif
};
} // namespace Nbeam