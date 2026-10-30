/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"

#include "ShaderBeam.h"
#include "Helpers.h"
#include "CaptureWGC.h"

namespace ShaderBeam
{

ShaderBeam::ShaderBeam() :
    m_options(), m_ui(m_options, m_shaderManager), m_watcher(m_ui), m_renderer(m_options, m_ui, m_watcher, m_shaderManager), m_renderThread(m_options, m_ui, m_renderer)
{ }

void ShaderBeam::Create(HWND window)
{
    m_options.outputWindow = window;

    m_ui.m_adapters = GetAdapters();
    m_ui.m_displays = GetDisplays();
    m_ui.m_shaders  = m_shaderManager.GetShaders();

    auto wgc = std::make_shared<CaptureWGC>(m_watcher, m_options);
    if(wgc->IsSupported())
        m_ui.m_captures.emplace_back((int)m_ui.m_captures.size(), wgc->m_name, wgc);
    if(m_ui.m_captures.empty())
    {
        MessageBoxW(nullptr, L"Windows Graphics Capture is not available on this system (Windows 10 version 1903 or newer is required).", L"Nbeam 3D Vision", MB_ICONERROR);
        ExitProcess(1);
    }

    m_ui.m_monitorTypes.push_back("LCD");
    m_ui.m_monitorTypes.push_back("OLED");

    m_ui.m_queuedFrames.push_back("3 (default)"); // index 0 = driver default (shown at the place of 3)
    for(int i = 1; i <= 14; i++)
        m_ui.m_queuedFrames.push_back(std::to_string(i));
    m_ui.m_queuedFrames.push_back("0 (tearing allowed)"); // index Options::QUEUE_TEARING

    m_ui.m_splitScreens.push_back("None");
    m_ui.m_splitScreens.push_back("Vertical");
    m_ui.m_splitScreens.push_back("Horizontal");

    timeBeginPeriod(1);
    Helpers::InitQPC();

    m_options.InitProfile();
    // "Default profile": Nbeam always starts on it (not on the profile of a program that was running when it was closed)
    if(!m_options.defaultProfile.empty() && std::filesystem::exists(Options::ProfilesDir() / m_options.defaultProfile))
    {
        m_options.profileFile = m_options.defaultProfile;
        m_options.RememberProfile();
    }
    m_options.Load(m_shaderManager);

    // the screens are identified only now: the settings (detections, remembered screens) are loaded, a save before would erase them
    m_options.RefreshMonitors(true);
    m_ui.m_displays = GetDisplays();

    DefaultOptions();

    for(int id = 0; id < HOTKEY_COUNT; id++)
        m_options.RegisterHotkey(id);
}

void ShaderBeam::Destroy()
{
    for(int id = 0; id < HOTKEY_COUNT; id++)
        m_options.UnregisterHotkey(id);

    timeEndPeriod(1);
}

void ShaderBeam::RunBenchmark()
{
    m_renderThread.Benchmark();
}

void ShaderBeam::UpdateVsyncRate()
{
    // get VSync duration from DWM (should match fastest display)
    DWM_TIMING_INFO dwmInfo {};
    dwmInfo.cbSize = sizeof(dwmInfo);
    DwmGetCompositionTimingInfo(NULL, &dwmInfo);
    m_options.vsyncDuration = Helpers::QPCToDeltaMs(dwmInfo.qpcRefreshPeriod);
    m_options.vsyncRate     = 1000.0f / m_options.vsyncDuration;

    // DWM runs at the fastest screen (e.g. 480 Hz) but the output screen can be slower (e.g. 120 Hz): the rate of the output screen wins,
    // otherwise the rendered / content FPS and the SubFrames are wrong
    HMONITOR mon = m_options.OutputMonitor(m_ui.m_displays);
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof(mi);
    if(mon && GetMonitorInfoW(mon, &mi))
    {
        DEVMODEW dm {};
        dm.dmSize = sizeof(dm);
        if(EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        {
            const float hz = (float)dm.dmDisplayFrequency;
            if(fabsf(m_options.vsyncRate - hz) / hz > 0.02f)
            {
                m_options.vsyncRate     = hz;
                m_options.vsyncDuration = 1000.0f / hz;
            }
        }
    }
}

void ShaderBeam::SwitchProfile()
{
    if(m_options.pendingProfile.empty())
    {
        ApplyLinkDisplay();
        return;
    }
    // Stop() has just saved the previous profile; load the new one on top of the current values
    m_options.profileFile    = m_options.pendingProfile;
    m_options.pendingProfile.clear();
    // any profile but the default one shows the effects again (they may have been hidden with Show / Hide)
    if(m_options.defaultProfile.empty() || _stricmp(m_options.defaultProfile.c_str(), m_options.profileFile.c_str()) != 0)
        m_options.effectsSuspended = false;
    {
        // hotkeys are global: a profile never changes them
        Options::Hotkey keep[HOTKEY_COUNT];
        std::copy(std::begin(m_options.hotkeys), std::end(m_options.hotkeys), keep);
        m_options.Load(m_shaderManager);
        std::copy(std::begin(keep), std::end(keep), m_options.hotkeys);
    }
    m_options.RememberProfile();
    DefaultOptions();
    ApplyLinkDisplay();
}

// The screen of the active profile detection (one-shot: the HUD can still change the display afterwards)
void ShaderBeam::ApplyLinkDisplay()
{
    const int n = (int)m_ui.m_displays.size();
    if(m_options.linkShaderDisplayNo >= 0 && m_options.linkShaderDisplayNo < n)
        m_options.shaderDisplayNo = m_options.linkShaderDisplayNo;
    if(m_options.linkCaptureDisplayNo >= 0 && m_options.linkCaptureDisplayNo < n)
        m_options.captureDisplayNo = m_options.linkCaptureDisplayNo;
    m_options.linkShaderDisplayNo  = -1;
    m_options.linkCaptureDisplayNo = -1;
}

void ShaderBeam::DefaultOptions()
{
    UpdateVsyncRate();

    // Manual SubFrames value: a profile that never had one (or the old "0 = auto") starts at display Hz / 60
    m_options.subFramesSetting = m_options.ResolveSubFrames();
    m_options.subFrames        = m_options.subFramesSetting;

    if(m_options.captureAdapterNo >= m_ui.m_adapters.size())
        m_options.captureAdapterNo = 0;
    if(m_options.shaderAdapterNo >= m_ui.m_adapters.size())
        m_options.shaderAdapterNo = 0;
    if(m_options.captureDisplayNo >= m_ui.m_displays.size())
        m_options.captureDisplayNo = 0;
    // the screens saved in the profile (by their identification) win over the display numbers
    if(!m_options.shaderScreenId.empty())
    {
        const int n = m_options.DisplayNoForScreen(m_options.shaderScreenId, m_ui.m_displays);
        if(n >= 0)
        {
            m_options.shaderDisplayNo = n;
            m_options.shaderScreenId.clear();
        }
    }
    if(!m_options.captureScreenId.empty())
    {
        const int n = m_options.DisplayNoForScreen(m_options.captureScreenId, m_ui.m_displays);
        if(n >= 0)
        {
            m_options.captureDisplayNo = n;
            m_options.captureScreenId.clear();
        }
    }
    if(m_options.shaderDisplayNo >= m_ui.m_displays.size())
        m_options.shaderDisplayNo = 0;
    if(m_options.monitorType >= m_ui.m_monitorTypes.size())
        m_options.monitorType = 0;
    if(m_options.captureMethod >= m_ui.m_captures.size())
        m_options.captureMethod = 0;
    if(m_options.splitScreen >= m_ui.m_splitScreens.size())
        m_options.splitScreen = 0;
    if(m_options.shaderProfileNo >= m_ui.m_shaders.size())
        m_options.shaderProfileNo = 0;
}

void ShaderBeam::Start()
{
    // no display (a TV switched off, a screen unplugged) or no graphics card listed: wait and try again every 2 s
    if(m_ui.m_displays.empty())
        m_ui.m_displays = GetDisplays();
    if(m_ui.m_displays.empty() || m_ui.m_adapters.empty())
    {
        SetTimer(m_options.outputWindow, TIMER_RETRY_START, 2000, nullptr);
        return;
    }
    if(m_options.captureDisplayNo >= m_ui.m_displays.size())
        m_options.captureDisplayNo = 0;
    if(m_options.shaderDisplayNo >= m_ui.m_displays.size())
        m_options.shaderDisplayNo = 0;
    if(m_options.captureAdapterNo >= m_ui.m_adapters.size())
        m_options.captureAdapterNo = 0;
    if(m_options.shaderAdapterNo >= m_ui.m_adapters.size())
        m_options.shaderAdapterNo = 0;
    m_options.crossAdapter     = m_options.shaderAdapterNo != m_options.captureAdapterNo;
    const auto& captureAdapter = m_ui.m_adapters.at(m_options.captureAdapterNo);
    const auto& shaderAdapter  = m_ui.m_adapters.at(m_options.shaderAdapterNo);
    winrt::com_ptr<ID3D11Device> captureDevice, shaderDevice;
    try
    {
        captureDevice = Helpers::CreateD3DDevice(captureAdapter.adapter.get());
        shaderDevice  = m_options.crossAdapter ? Helpers::CreateD3DDevice(shaderAdapter.adapter.get()) : captureDevice;
    }
    catch(...)
    {
        // graphics card not available yet (driver being updated or restarting after a crash): try again in 2 s
        SetTimer(m_options.outputWindow, TIMER_RETRY_START, 2000, nullptr);
        return;
    }

    UpdateVsyncRate();

    // a captured process needs a capture method that can capture a window
    if(m_options.captureWindow && !m_ui.m_captures[m_options.captureMethod].api->SupportsWindowCapture())
    {
        for(const auto& c : m_ui.m_captures)
            if(c.api->SupportsWindowCapture())
            {
                m_options.captureMethod = c.no;
                break;
            }
    }

    const auto& captureDisplay = m_ui.m_displays.at(m_options.captureDisplayNo);
    const auto& shaderDisplay  = m_ui.m_displays.at(m_options.shaderDisplayNo);

    m_options.captureMonitor = captureDisplay.monitor;
    m_options.outputWidth    = shaderDisplay.width;
    m_options.outputHeight   = shaderDisplay.height;
    {
        // output "Process": the output window covers the window of the captured process and has its size
        RECT pr {};
        m_options.processOutput = m_options.captureWindow && m_ui.m_captures[m_options.captureMethod].api->SupportsWindowCapture() && m_options.ProcessRect(pr);
        if(m_options.processOutput)
        {
            m_options.outputWidth  = (unsigned)(pr.right - pr.left);
            m_options.outputHeight = (unsigned)(pr.bottom - pr.top);
            SetWindowPos(m_options.outputWindow, HWND_TOPMOST, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    }
    m_options.swapChainBuffers = m_options.Tearing() ? 2 : (m_options.maxQueuedFrames ? m_options.maxQueuedFrames + 1 : 3);
    m_options.format           = m_options.useHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    if(m_options.useHdr)
        m_options.hardwareSrgb = false;

    winrt::com_ptr<ID3D11DeviceContext> deviceContext;
    shaderDevice->GetImmediateContext(deviceContext.put());
    const auto& capture = m_ui.m_captures[m_options.captureMethod].api;
    if(m_options.captureWindow && !capture->SupportsWindowCapture())
        m_options.captureWindow = NULL;
    // Manual: SubFrames is the typed value. Auto keeps the last detected value as its starting point.
    if(m_options.EffectiveSubFramesMode() == SF_MANUAL)
        m_options.subFrames = m_options.ManualSubFrames();

    try
    {
        float dpi = (float)GetDpiForWindow(m_options.outputWindow);
        m_ui.Start(m_options.outputWindow, dpi / USER_DEFAULT_SCREEN_DPI, shaderDevice, deviceContext);
    }
    catch(std::exception& ex)
    {
        MessageBoxA(m_options.outputWindow, ex.what(), SHADERBEAM_TITLE, MB_ICONERROR | MB_OK);
        PostMessage(m_options.outputWindow, WM_DESTROY, 0, 0);
        return;
    }
    catch(...)
    {
        // a Windows (WinRT / COM) error, or the graphics card lost while starting: try again a little later instead of closing
        SetTimer(m_options.outputWindow, TIMER_RETRY_START, 2000, nullptr);
        return;
    }

    try
    {
        m_renderer.Start(shaderDevice, deviceContext);
    }
    catch(std::exception& ex)
    {
        MessageBoxA(m_options.outputWindow, ex.what(), SHADERBEAM_TITLE, MB_ICONERROR | MB_OK);
        PostMessage(m_options.outputWindow, WM_DESTROY, 0, 0);
        return;
    }
    catch(...)
    {
        // a Windows (WinRT / COM) error, or the graphics card lost while starting: try again a little later instead of closing
        SetTimer(m_options.outputWindow, TIMER_RETRY_START, 2000, nullptr);
        return;
    }

    m_watcher.Start();

    try
    {
        capture->Start(captureDevice.as<IDXGIDevice>(), deviceContext);
    }
    catch(std::exception& ex)
    {
        m_ui.SetError(ex.what());
    }
    catch(winrt::hresult_error const& ex)
    {
        // Windows Graphics Capture refused (the game window closed or changed at that very moment...): shown, Nbeam keeps running
        const std::string msg = winrt::to_string(ex.message());
        m_ui.SetError(msg.c_str());
    }
    catch(...)
    {
        m_ui.SetError("Capture could not start");
    }

    m_renderThread.Start(capture);
    // high priority while Nbeam renders: a game that loads or compiles shaders no longer takes its CPU time (graphics card
    // priority is already at its highest, see gpuThreadPriority)
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    m_active = true;
}

void ShaderBeam::Stop()
{
    m_options.Save(m_shaderManager);
    if(!m_active)
        return; // the start did not happen (no display, start error): nothing to stop

    m_renderThread.Stop();
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    m_ui.m_captures[m_options.captureMethod].api->Stop();
    m_watcher.Stop();
    m_renderer.Stop();
    m_ui.Stop();

    m_active = false;
}

std::vector<winrt::com_ptr<IDXGIAdapter2>> ShaderBeam::EnumerateAdapters()
{
    winrt::com_ptr<IDXGIAdapter1>              pAdapter;
    std::vector<winrt::com_ptr<IDXGIAdapter2>> vAdapters;
    IDXGIFactory1*                             pFactory = NULL;

    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory)))
    {
        return vAdapters;
    }
    for(UINT i = 0; pFactory->EnumAdapters1(i, pAdapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        vAdapters.push_back(pAdapter.as<IDXGIAdapter2>());
    }
    if(pFactory)
    {
        pFactory->Release();
    }
    return vAdapters;
}

std::vector<AdapterInfo> ShaderBeam::GetAdapters()
{
    const auto&              dxgiAdapters = EnumerateAdapters();
    std::vector<AdapterInfo> adapters;
    int                      no = 0;
    for(auto adapter : dxgiAdapters)
    {
        DXGI_ADAPTER_DESC2 desc;
        if(SUCCEEDED(adapter->GetDesc2(&desc)))
        {
            if((desc.VendorId == 0x1414) && (desc.DeviceId == 0x8c))
                continue; // Microsoft Basic Render Driver

            const char* preempt = "";
            switch(desc.GraphicsPreemptionGranularity)
            {
            case DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY:
                preempt = " (D)";
                break;
            case DXGI_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY:
                preempt = " (P)";
                break;
            case DXGI_GRAPHICS_PREEMPTION_TRIANGLE_BOUNDARY:
                preempt = " (T)";
                break;
            case DXGI_GRAPHICS_PREEMPTION_PIXEL_BOUNDARY:
                preempt = " (PX)";
                break;
            case DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY:
                preempt = " (I)";
                break;
            }
            auto name = std::string("#") + std::to_string(no + 1) + ": " + Helpers::WCharToString(desc.Description) + std::string(preempt);
            adapters.emplace_back(no++, name, adapter, desc.AdapterLuid);
        }
    }
    return adapters;
}

static BOOL CALLBACK EnumDisplayMonitorsProc(_In_ HMONITOR hMonitor, _In_ HDC hDC, _In_ LPRECT lpRect, _In_ LPARAM lParam)
{
    std::vector<DisplayInfo>* displays = (std::vector<DisplayInfo>*)lParam;

    MONITORINFOEX info;
    info.cbSize = sizeof(info);
    GetMonitorInfo(hMonitor, &info);
    unsigned w    = info.rcMonitor.right - info.rcMonitor.left;
    unsigned h    = info.rcMonitor.bottom - info.rcMonitor.top;
    unsigned no   = (unsigned)displays->size();
    auto     name = Helpers::WCharToString(info.szDevice); // GDI name for now (replaced by the screen name in GetDisplays)
    displays->emplace_back(no, w, h, name, hMonitor);
    return true;
}

std::vector<DisplayInfo> ShaderBeam::GetDisplays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(NULL, NULL, EnumDisplayMonitorsProc, (LPARAM)&displays);

    // names of the screens (with their refresh rate) instead of "Desktop N"; the ids are kept for the profiles
    const auto known = m_options.MonitorsSnapshot();
    m_options.displayIds.assign(displays.size(), std::string());
    std::vector<std::string> labels(displays.size());
    for(size_t i = 0; i < displays.size(); i++)
    {
        const std::string gdi = displays[i].name;
        std::string       base = "Desktop " + std::to_string(i + 1);
        int               hz   = 0;
        for(const auto& k : known)
            if(k.connected && k.gdi == gdi)
            {
                m_options.displayIds[i] = k.id;
                if(!k.name.empty())
                    base = k.name;
                hz = k.hz;
            }
        labels[i] = base;
        displays[i].name = base;
        if(hz > 0)
            displays[i].name += " (" + std::to_string(hz) + " Hz)";
    }
    for(size_t i = 0; i < displays.size(); i++)
    {
        int same = 0, rank = 0;
        for(size_t j = 0; j < displays.size(); j++)
            if(labels[j] == labels[i])
            {
                same++;
                if(j < i)
                    rank++;
            }
        if(same > 1)
            displays[i].name += " #" + std::to_string(rank + 1);
    }
    return displays;
}

} // namespace ShaderBeam