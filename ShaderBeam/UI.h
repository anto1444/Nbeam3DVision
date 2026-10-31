/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "Common.h"
#include <atomic>
#include <mutex>
#include "ShaderManager.h"
#include "EmitterUI.h"

struct ImFont;
struct ImGuiStyle;

namespace ShaderBeam
{

class UI
{
public:
    UI(Options& options, ShaderManager& shaderManager);

    void Start(HWND window, float scale, winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context);
    bool Input(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool MouseRequired();
    void Render();
    void Stop();

    void SetError(const char* message);
    void SetBenchmark(const BenchmarkResult& result);
    void AddWindow(HWND hWnd);
    bool RenderRequired() const;
    // automatic save of the active profile: asked by the HUD (render thread) or a hotkey, done once a second by the main thread
    void RequestSave() { m_saveRequested = true; }
    bool TakeSaveRequest() { return m_saveRequested.exchange(false); }
    std::atomic<bool> m_saveRequested { false };
    // ImGui is drawn by the render thread and fed with the mouse / keyboard by the main thread: one at a time. The main thread only
    // waits a little (never blocks Nbeam); the render thread holds it for the whole HUD frame.
    std::timed_mutex m_imguiMutex;
    void             CancelHotkeyCapture(); // a shortcut capture left open (HUD hidden, other tab): the hotkeys work again
    bool Toggle();
    // Window mode: the HUD fills a normal window of its own (settings made there while the effects are off)
    void SetWindowMode(bool on) { m_windowMode = on; }
    bool WindowMode() const { return m_windowMode; }
    float& HudScale() { return m_windowMode ? m_options.hud.scaleWin : m_options.hud.scale; } // separate sizes: window / HUD over the screen

    float m_inputFPS { 0 };
    float m_outputFPS { 0 };
    float m_captureLag { 0 };
    float m_gameFps { 0 };      // Auto: measured game frame rate
    bool  m_lowFps { false };   // Auto: game below the low fps threshold
    bool  m_lowBypass { false };// ... and BFI is disabled because of it
    float m_flickerHz { 0 };    // Auto: current flash rate of the BFI

    std::vector<AdapterInfo> m_adapters;
    std::vector<DisplayInfo> m_displays;
    std::vector<ShaderInfo>  m_shaders;
    std::vector<CaptureInfo> m_captures;
    std::vector<std::string> m_monitorTypes;
    std::vector<InputInfo>   m_inputs;
    std::vector<std::string> m_queuedFrames;
    std::vector<std::string> m_splitScreens;
    std::vector<std::string> m_emitterStatus; // set by Renderer after the emitters started
    bool                     m_appearanceDirty { false };
    std::string              m_aiStatus;      // "2D to 3D (AI)": state of the depth model (set by Renderer)
    float                    m_aiInferMs { 0 };
    float                    m_aiFps { 0 };
    float                    m_aiLatencyMs { 0 };

private:
    void SetStyle(ImGuiStyle& style);
    void RenderBanner();
    void RenderHudHeader(float height); // OSD style status banner of the HUD
public:
    void UpdateCaptureTone(bool refreshHdr = false); // black / grey level of the capture; neutral unless Windows HDR is on
    void ShowBanner(); // "3D emitter on" banner for 3 seconds (when 3D is active)
    void ShowNote(const std::string& text); // one-line banner for 3 seconds
    bool HudMoving() const; // a HUD window is being dragged
    bool HudBounds(RECT& r) const; // area covered by the HUD windows drawn last (pixels); false = nothing drawn
    void ShowAdjustBanner(int kind); // banner with the current values, 3 seconds from the last change (kind = which one changed)
private:
    bool BannerActive() const;
    void SetApplyRequired();
    void ApplyPendingChanges();
    void ClearPendingChanges();
    void RefreshHotkeyTexts();
    void RenderHotkeys();
    void ScanWindows();

    struct
    {
        int  captureAdapterNo;
        int  shaderAdapterNo;
        int  captureDisplayNo;
        HWND captureWindow;
        int  shaderDisplayNo;
        bool outputOnProcess;
        int  subFramesMode;
        bool hardwareSrgb;
        int  captureMethod;
        int  splitScreen;
        int  monitorType;
        int  maxQueuedFrames;
        bool useHdr;
    } m_pending;

    bool            m_windowMode { false }; // drawn in the window of the window mode (not over the screen)
    HWND            m_hwnd { 0 };           // window ImGui is attached to
    ImFont*         m_smallFont;
    ImFont*         m_largeFont;
    Options&        m_options;
    ShaderManager&  m_shaderManager;
    bool            m_ready { false };
    bool            m_applyRequired { false };
    bool            m_emitterRestartRequired { false };
    bool            m_liveReloadRequired { false };
    StereoSource    m_lastSource { StereoSource::Off }; // 3D source format the renderer is running with
    volatile unsigned long long m_adjustUntil { 0 }; // adjustment hotkeys: banner shown until this tick count
    volatile int       m_adjustKind { -1 };
    std::string        m_noteText;
    std::mutex         m_noteMutex; // m_noteText: written by the main thread (hotkeys), read by the render thread
    volatile unsigned long long m_noteUntil { 0 };
    unsigned long long m_bannerUntil { 0 }; // tick count until which the "3D emitter on" banner is shown
    EmitterPanelContext m_emitterPanel;
    float           m_fontSize { 0 };
    winrt::com_ptr<ID3D11ShaderResourceView> m_logo; // Nbeam logo for the title bar
    winrt::com_ptr<ID3D11ShaderResourceView> m_logoSB; // ShaderBeam logo (Render / Shaders category)
    const char*     m_captureGPUs { nullptr };
    const char*     m_renderGPUs { nullptr };
    BenchmarkResult m_benchmarkResult {};
    bool            m_hasBenchmark { false };
    bool            m_hasError { false };
    std::string     m_errorMessage;
};
} // namespace ShaderBeam