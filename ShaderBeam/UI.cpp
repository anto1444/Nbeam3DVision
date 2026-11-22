/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include <cmath>

#include "UI.h"
#include "Helpers.h"
#include "CaptureBase.h"
#include "DepthAI.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends\imgui_impl_win32.h"
#include "backends\imgui_impl_dx11.h"

#include "HudFont.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include "UIWidgets.h"
#include "LogoShaderBeam.h"
#include "ReleaseNotes.h"
#include <shellapi.h>
#include "Lang.h"
#include "resource.h"
#include <dxgi1_6.h>

static float s_sdrWhiteScale = 1.0f; // Windows "SDR content brightness" of the display as an scRGB multiplier (1.0 = 80 nits)

// SDR white level Windows uses on the display with that GDI name (e.g. \\.\DISPLAY1), as a multiplier of 80 nits; 1 if unknown
static float QuerySdrWhiteScale(const wchar_t* gdiName)
{
    UINT32 np = 0, nm = 0;
    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
        return 1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return 1.0f;
    for(UINT32 i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn {};
        sn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size      = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id        = paths[i].sourceInfo.id;
        if(DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdiName) != 0)
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl {};
        wl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size      = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id        = paths[i].targetInfo.id;
        if(DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            return wl.SDRWhiteLevel / 1000.0f;
    }
    return 1.0f;
}

// Windows HDR state of the monitor of the window. The (slow) query is only done by RefreshHdrState(), called about once a second
// by the timer of the main thread (and once at the first use); the render thread only reads the stored result.
static std::atomic<bool> s_hdrOn { false };
static std::atomic<bool> s_hdrKnown { false };

static void RefreshHdrState(HWND hWnd)
{
    bool on         = false;
    float white     = 1.0f;
    try
    {
        HMONITOR                      mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
        winrt::com_ptr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())))
        {
            s_hdrOn = false;
            s_hdrKnown = true;
            return;
        }
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for(UINT a = 0; !on && factory->EnumAdapters1(a, adapter.put()) == S_OK; a++, adapter = nullptr)
        {
            winrt::com_ptr<IDXGIOutput> output;
            for(UINT o = 0; adapter->EnumOutputs(o, output.put()) == S_OK; o++, output = nullptr)
            {
                DXGI_OUTPUT_DESC desc {};
                output->GetDesc(&desc);
                if(desc.Monitor != mon)
                    continue;
                if(auto o6 = output.try_as<IDXGIOutput6>())
                {
                    DXGI_OUTPUT_DESC1 d1 {};
                    if(SUCCEEDED(o6->GetDesc1(&d1)))
                    {
                        on = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        if(on)
                            white = QuerySdrWhiteScale(desc.DeviceName);
                    }
                }
                break;
            }
        }
    }
    catch(...)
    { }
    s_sdrWhiteScale = white;
    s_hdrOn         = on;
    s_hdrKnown      = true;
}

// true when Windows HDR is on for the monitor of the window (stored result, cheap)
static bool WindowsHdrOn(HWND hWnd)
{
    if(!s_hdrKnown)
        RefreshHdrState(hWnd); // first use only
    return s_hdrOn;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// About: one line per row, each centred in the window (the version is the one of the release notes below)
const char* NBEAM_ABOUT = NBEAM_TITLE "\n"
                               "Version 3.1.0\n"
                               "Made by anto1444, 2026\n"
                               "\n"
                               "Based on ShaderBeam (c) 2025-2026 mausimus\n"
                               "Distributed under MIT License\n"
                               "\n"
                               "Includes Blur Busters CRT Beam Simulator\n"
                               "by Mark Rejhon @BlurBusters and\n"
                               "Timothy Lottes @NOTimothyLottes\n"
                               "https://blurbusters.com/crt";
const char* NBEAM_ABOUT_FR = NBEAM_TITLE "\n"
                                  "Version 3.1.0\n"
                                  "Fait par anto1444, 2026\n"
                                  "\n"
                                  "Bas\303\251 sur ShaderBeam (c) 2025-2026 mausimus\n"
                                  "Distribu\303\251 sous licence MIT\n"
                                  "\n"
                                  "Inclut le Blur Busters CRT Beam Simulator\n"
                                  "de Mark Rejhon @BlurBusters et\n"
                                  "Timothy Lottes @NOTimothyLottes\n"
                                  "https://blurbusters.com/crt";

static std::string s_hotkeyMessage;
static std::string s_hotkeyHelp;
static int         s_captureId = -1;

static int s_hotkeysDrawnFrame = -1;
static const char* s_hudSection = nullptr; // section of the HUD shown (log of the slow HUD frames) // last HUD frame in which the Hotkeys section was drawn

// the HUD and the effects shortcuts can never be empty (they must stay reachable)
static bool IsProtectedHotkey(int id)
{
    return id == HOTKEY_TOGGLEUI || id == HOTKEY_EFFECTS;
}

static void CancelCaptureNow(Nbeam::Options& o)
{
    if(s_captureId < 0 && !o.hotkeysSuspended)
        return;
    s_captureId        = -1;
    o.hotkeysSuspended = false;
    for(int id = 0; id < HOTKEY_COUNT; id++)
        o.UnregisterHotkey(id);
    for(int id = 0; id < HOTKEY_COUNT; id++)
        o.RegisterHotkey(id);
}

static std::string s_hotkeyError;

namespace Nbeam
{

// scroll position survives restarts (the ImGui context is recreated each time)
static std::vector<std::string> s_profiles;
static bool                     s_profilesDirty = true;
static char                     s_newProfile[64] = "";
static std::string              s_procSel; // the program (normalized .exe name) selected in the "Program settings" tab
static bool                     s_procFocus = false;
static void                     SortNoCase(std::vector<std::string>& v)
{
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
}
static float s_scrollY       = 0.0f;
static int   s_restoreFrames = 0;

// logos shown by the category icons (set every frame from the UI members)
static ID3D11ShaderResourceView* s_iconNbeam = nullptr;
static ID3D11ShaderResourceView* s_iconSB    = nullptr;
// HUD scale: base style and base font size (scale 1), applied scale
static ImGuiStyle s_baseStyle;
static float      s_baseFont     = 16.0f;
static float      s_startDpi     = 96.0f; // Windows scale (dpi) at the start: the interface is at its normal size for it
static float      s_appliedScale = 1.0f;

// the ShaderBeam logo (embedded RGBA, straight alpha)
static winrt::com_ptr<ID3D11ShaderResourceView> CreateNbeamLogoTexture(ID3D11Device* device)
{
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC td {};
    td.Width            = kLogoNbeamSize;
    td.Height           = kLogoNbeamSize;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd {};
    sd.pSysMem     = kLogoNbeamRgba;
    sd.SysMemPitch = kLogoNbeamSize * 4;
    winrt::com_ptr<ID3D11Texture2D> tex;
    if(SUCCEEDED(device->CreateTexture2D(&td, &sd, tex.put())))
        device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    return srv;
}

// The Nbeam logo (the exe icon) as a texture for the title bar.
static winrt::com_ptr<ID3D11ShaderResourceView> CreateLogoTexture(ID3D11Device* device)
{
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_NBEAM), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
    if(!icon)
        return srv;
    ICONINFO ii {};
    if(GetIconInfo(icon, &ii))
    {
        BITMAP bm {};
        if(ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
        {
            const int             w = bm.bmWidth, h = bm.bmHeight;
            std::vector<uint32_t> px((size_t)w * h), mask((size_t)w * h);
            BITMAPINFO            bi {};
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = w;
            bi.bmiHeader.biHeight      = -h;
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            HDC dc = GetDC(nullptr);
            GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
            bool hasAlpha = false;
            for(uint32_t v : px)
                if(v >> 24)
                {
                    hasAlpha = true;
                    break;
                }
            if(hasAlpha)
            {
                for(auto& v : px) // GDI gives premultiplied alpha, ImGui blends straight alpha
                {
                    const uint32_t a = v >> 24;
                    if(a && a < 255)
                    {
                        const uint32_t r = (std::min)(255u, ((v >> 16) & 255) * 255 / a);
                        const uint32_t g = (std::min)(255u, ((v >> 8) & 255) * 255 / a);
                        const uint32_t b = (std::min)(255u, (v & 255) * 255 / a);
                        v                = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
            else if(ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS))
            {
                for(size_t i = 0; i < px.size(); i++)
                    px[i] = (px[i] & 0x00FFFFFF) | ((mask[i] & 0x00FFFFFF) ? 0 : 0xFF000000);
            }
            ReleaseDC(nullptr, dc);

            D3D11_TEXTURE2D_DESC td {};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_IMMUTABLE;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd {};
            sd.pSysMem     = px.data();
            sd.SysMemPitch = w * 4;
            winrt::com_ptr<ID3D11Texture2D> tex;
            if(SUCCEEDED(device->CreateTexture2D(&td, &sd, tex.put())))
                device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
        }
        if(ii.hbmColor)
            DeleteObject(ii.hbmColor);
        if(ii.hbmMask)
            DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return srv;
}

// Auto: the interface follows the Windows display scale of the screen it is on (100 % in Windows = normal size, 150 % = 1.5 times...).
float UI::EffScale()
{
    if(m_options.hud.autoScale && m_hwnd)
    {
        const UINT dpi = GetDpiForWindow(m_hwnd);
        if(dpi)
            return std::clamp((float)dpi / s_startDpi, 0.6f, 3.0f);
    }
    return HudScale();
}

UI::UI(Options& options, ShaderManager& shaderManager) : m_options(options), m_shaderManager(shaderManager) { }

void UI::Start(HWND window, float scale, winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_hwnd = window;
    s_restoreFrames = 4;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device.get(), context.get());
    m_logo   = CreateLogoTexture(device.get());
    m_logoSB = CreateNbeamLogoTexture(device.get());

    ImGuiStyle& style      = ImGui::GetStyle();
    style.Alpha            = 1;
    style.DisabledAlpha    = 1;
    style.FrameRounding    = 0;
    style.FrameBorderSize  = 0;
    style.WindowBorderSize = 2;
    style.FramePadding     = ImVec2(8, 5);
    style.ItemSpacing      = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize    = 14;
    style.GrabMinSize      = 14;
    style.SeparatorTextBorderSize = 2;
    style.SeparatorTextPadding    = ImVec2(16, 4);
    style.WindowRounding   = 0;
    SetStyle(style);
    style.ScaleAllSizes(scale);

    m_fontSize = roundf(16.0f * scale);
    s_baseFont     = m_fontSize;
    s_startDpi     = (std::max)(96.0f * scale, 48.0f);
    s_baseStyle    = style; // sizes at scale 1 of the HUD (colours are taken from the live style)
    s_appliedScale = 1.0f;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = false; // the HUD (no title bar) is moved by its background when it is detached
    //io.IniFilename = NULL;

    // HUD typeface: Inter (open licence), a clean DIN-like sans close to the GeForce / RTX look. Medium for the text, Bold for titles.
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false; // the data is static
    m_smallFont = io.Fonts->AddFontFromMemoryTTF((void*)HudFontMedium_data, (int)HudFontMedium_size, m_fontSize, &fontCfg);
    m_largeFont = io.Fonts->AddFontFromMemoryTTF((void*)HudFontBold_data, (int)HudFontBold_size, m_fontSize * 3, &fontCfg);
    io.Fonts->Build();

    RefreshHotkeyTexts();
    m_lastSource = m_options.emitter.sourceFormat;
    ShowBanner();

    ClearPendingChanges();
    m_hasError     = false;
    m_hasBenchmark = false;

    ScanWindows();

    m_errorMessage.clear();
    m_ready = true;
}

bool UI::MouseRequired()
{
    std::unique_lock<std::timed_mutex> lock(m_imguiMutex, std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(50)))
        return false;
    return m_ready && m_options.ui && ImGui::GetIO().WantCaptureMouse;
}

// exact refresh rate of a monitor (59.940 / 239.757 Hz...), from the display configuration; 0 if unknown
double UI::ExactRefreshRate(HMONITOR monitor)
{
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof(mi);
    if(!monitor || !GetMonitorInfoW(monitor, &mi))
        return 0.0;
    UINT32 np = 0, nm = 0;
    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np)
        return 0.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return 0.0;
    for(UINT32 i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src {};
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id        = paths[i].sourceInfo.id;
        if(DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0)
            continue;
        const auto& r = paths[i].targetInfo.refreshRate;
        return r.Denominator ? (double)r.Numerator / (double)r.Denominator : 0.0;
    }
    return 0.0;
}

static void KeepMax(std::atomic<int>& v, int x); // (tracer of the HUD clicks)

bool UI::Input(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    try
    {
        // the render thread may be drawing the HUD right now: wait for it (a little, never forever)
        if(m_options.ui && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_LBUTTONDBLCLK))
        {
            // tracer of the HUD clicks (Renderer::ClickTrace)
            m_lastClickName = msg == WM_LBUTTONDOWN ? "left down" : msg == WM_LBUTTONUP ? "left up" : msg == WM_LBUTTONDBLCLK ? "double click" : msg == WM_RBUTTONDOWN ? "right down" : "right up";
            m_lastClick     = GetTickCount64();
        }
        // A message sent by ImGui itself while it handles one (ReleaseCapture when the button is released sends WM_CAPTURECHANGED at once, on
        // this same thread): the lock is already held by this thread, so it is handled directly. Waiting for the lock here blocked for 50 ms
        // while holding it, and the render thread with it (a flash of the BFI / 3D and the glasses out of step at each release of the button).
        static thread_local bool inside = false;
        if(inside)
            return m_ready && m_options.ui && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        const auto                         w0 = std::chrono::steady_clock::now();
        std::unique_lock<std::timed_mutex> lock(m_imguiMutex, std::defer_lock);
        if(!lock.try_lock_for(std::chrono::milliseconds(50)))
            return false;
        KeepMax(m_waitInputUs, (int)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - w0).count());
        struct Inside
        {
            Inside() { inside = true; }
            ~Inside() { inside = false; }
        } mark;
        return m_ready && m_options.ui && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
    catch(...)
    { }
    return false;
}

std::atomic<long> g_rawMouseX { 0 }, g_rawMouseY { 0 };

// "Draw a pointer" (Direct presentation, HUD hidden): the Windows pointer is hidden over Nbeam (WM_SETCURSOR), so Nbeam draws one with the
// picture (it follows the BFI and the 3D) while the mouse moves, and stops 1 s after the mouse stops. Windows cannot tell whether the game shows
// its pointer (the pointer is over Nbeam, not over the game), so it is looked at from the mouse: when the game captures it to turn the camera,
// the mouse moves (raw input) but the pointer does not follow (the game puts it back or locks it): nothing is drawn until the pointer follows
// the mouse again (a menu opened). Render thread only.
static bool OwnPointerWanted(const Options& o, bool windowMode)
{
    static POINT     s_last {}, s_winPos {};
    static ULONGLONG s_still = 0, s_winStart = 0, s_lastCall = 0; // since when the pointer has not moved / start of the 250 ms look / last call
    static long      s_winRawX = 0, s_winRawY = 0;
    static bool      s_have = false, s_captured = false;
    POINT            p {};
    if(!o.drawPointer || !o.DirectActive() || o.ui || windowMode || !GetCursorPos(&p))
    {
        s_have = false;
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    const long      rx = g_rawMouseX.exchange(0), ry = g_rawMouseY.exchange(0);
    if(!s_have || now - s_lastCall > 500) // not looked at for a while: start again
    {
        s_have     = true;
        s_captured = false;
        s_last     = p;
        s_still    = now;
        s_winStart = 0;
    }
    s_lastCall = now;
    if(p.x != s_last.x || p.y != s_last.y)
    {
        s_last  = p;
        s_still = now;
    }
    if(s_winStart == 0)
    {
        s_winStart = now;
        s_winPos   = p;
        s_winRawX = s_winRawY = 0;
    }
    s_winRawX += rx;
    s_winRawY += ry;
    if(now - s_winStart >= 250)
    {
        // net movement of the mouse over 250 ms and net movement of the pointer: a pointer that follows the mouse moves about as much
        const double n = std::sqrt((double)s_winRawX * s_winRawX + (double)s_winRawY * s_winRawY);
        const double d = std::sqrt((double)(p.x - s_winPos.x) * (p.x - s_winPos.x) + (double)(p.y - s_winPos.y) * (p.y - s_winPos.y));
        // (a pointer pushed against the edge of the screen does not move either: not a capture)
        const int  vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int  vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const bool atEdge = p.x <= vx + 2 || p.y <= vy + 2 || p.x >= vx + vw - 3 || p.y >= vy + vh - 3;
        if(n >= 40 && !atEdge)
        {
            const bool was = s_captured;
            if(d * 8 < n)
                s_captured = true;
            else if(d * 3 >= n)
                s_captured = false;
            if(was != s_captured)
                std::thread([c = s_captured, n, d]() { LsLog("own pointer: mouse %s (mouse %.0f, pointer %.0f px in 250 ms)", c ? "captured by the game" : "free", n, d); }).detach();
        }
        s_winStart = 0;
    }
    return !s_captured && now - s_still <= 1000;
}

bool UI::RenderRequired() const
{
    return m_ready && (m_options.ui || BannerActive() || OwnPointerWanted(m_options, m_windowMode));
}

// "Auto fps cap" choice (None / RTSS / Display Commander) of a program detection and of the global settings; a program that is not installed is
// greyed out. Returns true when the choice changed.
static bool FpsCapCombo(const char* id, int& mode, bool rtss, bool dc, bool mixed = false)
{
    bool        changed  = false;
    const char* names[4] = { Tr("None"), "RTSS", "Display Commander", "ReLimiter" };
    if(mode < 0 || mode > 3 || (mode == 1 && !rtss) || (mode == 2 && !dc))
        mode = 0;
    if(ImGui::BeginCombo(id, mixed ? Tr("(mixed)") : names[mode], 0))
    {
        for(int m = 0; m < 4; m++)
        {
            const bool avail = m == 0 || m == 3 || (m == 1 && rtss) || (m == 2 && dc); // (ReLimiter has no installation to look for: it is used for the games that have it)
            ImGui::BeginDisabled(!avail);
            if(ImGui::Selectable(names[m], mode == m) && avail)
            {
                mode    = m;
                changed = true;
            }
            ImGui::EndDisabled();
            if(!avail && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", m == 1 ? Tr("Not available: RivaTuner Statistics Server (RTSS) was not found on this PC. Install it (it comes with MSI Afterburner, or from guru3d.com), then restart Nbeam.")
                                               : Tr("Not available: Display Commander was not found on this PC (folder Display_Commander in %LocalAppData%\\Programs). Install it, then restart Nbeam."));
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Experimental. Caps the frame rate of this program automatically while it is detected. RTSS (RivaTuner Statistics Server): in 3D, half the 3D rate (e.g. 60 fps for a 120 Hz 3D); with BFI or MPRT sweep, the content rate (screen Hz / SubFrames); without effects there is no cap. It is followed live; Nbeam starts RTSS minimized if it is not running, and only changes the frame limit of the program in RTSS (the previous limit comes back when the program closes or Nbeam quits). Display Commander / ReLimiter (ReShade addons): Nbeam writes the \"Frame limit\" of this program (one per screen) in the DisplayCommander.ini / relimiter.ini next to the game, for every program at once, when Nbeam starts, when its screen changes and after the game closes, so it is ready before the game starts (the addon reads it only then; it does not follow the 3D / BFI changes). A game without that file is ignored."));
    return changed;
}

static void ShowHelpMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(450.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void ShowAlertMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.0f, 1.0f), "(!)");
    if(ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(450.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
// Vector icons of the categories (drawn with the draw list: they take the text colour and any scale).
// 0 Render (gauge), 1 Input / Output (arrows), 2 Shader (layers), 3 3D (cube), 4 Profiles (user), 5 Application (gear), 6 Appearance (eye), 7 Hotkeys (key)
static void DrawCategoryIcon(ImDrawList* dl, int id, ImVec2 c, float size, ImU32 col)
{
    const float r  = size * 0.5f;
    const float th = (std::max)(1.6f, size * 0.09f);
    auto        P  = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch(id)
    {
    case 0: // gauge
    {
        dl->PathArcTo(c, r * 0.92f, 2.5f, 6.78f, 32);
        dl->PathStroke(col, 0, th);
        for(int i = 0; i < 5; i++)
        {
            const float a = 2.5f + i * (4.28f / 4.0f);
            dl->AddLine(ImVec2(c.x + cosf(a) * r * 0.68f, c.y + sinf(a) * r * 0.68f), ImVec2(c.x + cosf(a) * r * 0.82f, c.y + sinf(a) * r * 0.82f), col, th * 0.8f);
        }
        dl->AddLine(c, P(0.5f, -0.5f), col, th * 1.2f);
        dl->AddCircleFilled(c, th * 1.4f, col);
        break;
    }
    case 1: // input / output arrows
        dl->AddLine(P(-0.9f, -0.4f), P(0.55f, -0.4f), col, th);
        dl->AddTriangleFilled(P(0.95f, -0.4f), P(0.45f, -0.78f), P(0.45f, -0.02f), col);
        dl->AddLine(P(0.9f, 0.4f), P(-0.55f, 0.4f), col, th);
        dl->AddTriangleFilled(P(-0.95f, 0.4f), P(-0.45f, 0.02f), P(-0.45f, 0.78f), col);
        break;
    case 2: // ShaderBeam logo
        if(s_iconSB)
        {
            dl->AddImage(ImTextureRef((ImTextureID)(uintptr_t)s_iconSB), P(-1.0f, -1.0f), P(1.0f, 1.0f));
            break;
        }
    {
        const ImVec2 top[4] = { P(0.0f, -0.85f), P(0.95f, -0.40f), P(0.0f, 0.05f), P(-0.95f, -0.40f) };
        dl->AddConvexPolyFilled(top, 4, col);
        const ImVec2 mid[3] = { P(-0.95f, 0.05f), P(0.0f, 0.5f), P(0.95f, 0.05f) };
        dl->AddPolyline(mid, 3, col, 0, th);
        const ImVec2 low[3] = { P(-0.95f, 0.5f), P(0.0f, 0.95f), P(0.95f, 0.5f) };
        dl->AddPolyline(low, 3, col, 0, th);
        break;
    }
    case 3: // Nbeam logo
        if(s_iconNbeam)
        {
            dl->AddImage(ImTextureRef((ImTextureID)(uintptr_t)s_iconNbeam), P(-1.0f, -1.0f), P(1.0f, 1.0f));
            break;
        }
    {
        const ImVec2 hex[6] = { P(0.0f, -0.95f), P(0.85f, -0.48f), P(0.85f, 0.48f), P(0.0f, 0.95f), P(-0.85f, 0.48f), P(-0.85f, -0.48f) };
        dl->AddPolyline(hex, 6, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.0f, 0.0f), P(-0.85f, -0.48f), col, th);
        dl->AddLine(P(0.0f, 0.0f), P(0.85f, -0.48f), col, th);
        dl->AddLine(P(0.0f, 0.0f), P(0.0f, 0.95f), col, th);
        break;
    }
    case 4: // user
    {
        dl->AddCircle(P(0.0f, -0.4f), r * 0.38f, col, 24, th);
        dl->PathArcTo(P(0.0f, 0.95f), r * 0.80f, 3.6f, 5.82f, 24);
        dl->PathStroke(col, 0, th);
        break;
    }
    case 5: // gear
    {
        for(int i = 0; i < 8; i++)
        {
            const float a = i * 0.7853982f;
            dl->AddLine(ImVec2(c.x + cosf(a) * r * 0.66f, c.y + sinf(a) * r * 0.66f), ImVec2(c.x + cosf(a) * r * 0.95f, c.y + sinf(a) * r * 0.95f), col, th * 1.8f);
        }
        dl->AddCircle(c, r * 0.64f, col, 28, th);
        dl->AddCircle(c, r * 0.26f, col, 20, th);
        break;
    }
    case 6: // eye
    {
        dl->PathLineTo(P(-1.0f, 0.0f));
        dl->PathBezierCubicCurveTo(P(-0.5f, -0.8f), P(0.5f, -0.8f), P(1.0f, 0.0f));
        dl->PathBezierCubicCurveTo(P(0.5f, 0.8f), P(-0.5f, 0.8f), P(-1.0f, 0.0f));
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddCircleFilled(c, r * 0.3f, col);
        break;
    }
    case 7: // keyboard key
    {
        dl->AddRect(P(-0.95f, -0.6f), P(0.95f, 0.6f), col, r * 0.18f, 0, th);
        for(int i = 0; i < 3; i++)
            dl->AddCircleFilled(P(-0.5f + i * 0.5f, -0.18f), th * 0.75f, col);
        dl->AddLine(P(-0.5f, 0.22f), P(0.5f, 0.22f), col, th);
        break;
    }
    }
}

// Status banner of the HUD (OSD style): Nbeam logo and name on an angled accent shape, then the output mode and, on the right,
// profile / 3D / HDR / FPS cells separated by thin lines, and the quit cross. Drawn at the current cursor, `height` tall.
void UI::RenderHudHeader(float height)
{
    const float  fs     = m_fontSize;
    const float  w      = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + height);
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    ImFont*      font   = ImGui::GetFont();
    const ImVec4 accent(m_options.hud.r, m_options.hud.g, m_options.hud.b, 1.0f);
    const ImU32  cAcc    = ImGui::GetColorU32(accent);
    const ImU32  cAccDim = ImGui::GetColorU32(ImVec4(accent.x * 0.55f, accent.y * 0.55f, accent.z * 0.55f, 1.0f));
    const float  winA    = ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w;
    const ImU32  cBg     = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f * winA + 0.1f));
    const ImU32  cWhite  = IM_COL32(245, 247, 245, 255);
    const ImU32  cDim    = IM_COL32(190, 196, 190, 255);
    const ImU32  cOnGreen = ImGui::GetColorU32(ImVec4((std::min)(accent.x * 1.25f + 0.03f, 1.0f), (std::min)(accent.y * 1.25f + 0.03f, 1.0f), (std::min)(accent.z * 1.25f + 0.03f, 1.0f), 1.0f));
    const float  lum     = 0.299f * accent.x + 0.587f * accent.y + 0.114f * accent.z;
    const ImU32  cOnAcc  = lum > 0.5f ? IM_COL32(8, 8, 8, 255) : IM_COL32(255, 255, 255, 255);
    auto         textW   = [&](float size, const char* t) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, t).x; };

    dl->AddRectFilled(p0, p1, cBg);

    // accent shape with the logo and the name
    const float logoSz  = height * 0.62f;
    const float titleSz = fs * 1.3f, verSz = fs * 0.8f;
    const char* title   = "NBEAM 3D VISION";
    const float logoX   = p0.x + fs * 0.9f;
    const float titleX  = logoX + logoSz + fs * 0.7f;
    const float shapeW  = (titleX - p0.x) + (std::max)(textW(titleSz, title), 1.0f) + height * 0.45f + fs * 0.8f;
    dl->AddQuadFilled(p0, ImVec2(p0.x + shapeW, p0.y), ImVec2(p0.x + shapeW - height * 0.45f, p1.y), ImVec2(p0.x, p1.y), cAcc);
    if(m_logo)
        dl->AddImage(ImTextureRef((ImTextureID)(uintptr_t)m_logo.get()), ImVec2(logoX, p0.y + (height - logoSz) * 0.5f), ImVec2(logoX + logoSz, p0.y + (height + logoSz) * 0.5f));
    dl->AddText(font, titleSz, ImVec2(titleX, p0.y + height * 0.5f - titleSz * 0.95f), cOnAcc, title);
    dl->AddText(font, verSz, ImVec2(titleX, p0.y + height * 0.5f + fs * 0.1f), cOnAcc, "v3.1.0");

    // output mode (like "3440 x 1440 | 240Hz")
    char mode[64];
    snprintf(mode, sizeof(mode), "%d x %d | %.0f Hz", m_options.outputWidth, m_options.outputHeight, m_options.vsyncRate);
    const float modeX = p0.x + shapeW + fs * 1.2f;
    dl->AddText(font, fs * 0.85f, ImVec2(modeX, p0.y + height * 0.5f - fs * 1.15f), cDim, Tr("Output"));
    dl->AddText(font, fs * 1.15f, ImVec2(modeX, p0.y + height * 0.5f + fs * 0.0f), cWhite, mode);
    float       modeEnd = modeX + (std::max)(textW(fs * 1.15f, mode), textW(fs * 0.85f, Tr("Output"))) + fs * 1.0f;

    // 3D, right next to the output mode, only when 3D is used: its refresh rate (half of the emitter rate)
    if(m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && m_options.StereoAllowed()) // (nothing shown when 3D is not used)
    {
        char         v3d[48];
        const double syncHz = m_options.emitter.EffectiveSyncHz(m_options.vsyncRate, m_options.subFrames);
        if(syncHz > 0.0)
            snprintf(v3d, sizeof(v3d), "%d x %d | %.0f Hz", m_options.outputWidth, m_options.outputHeight, syncHz / 2.0);
        else
            snprintf(v3d, sizeof(v3d), "%d x %d | ON", m_options.outputWidth, m_options.outputHeight);
        const float cw = (std::max)(textW(fs * 1.15f, v3d), textW(fs * 0.85f, "3D"));
        const float sx = modeEnd + fs * 0.4f;
        dl->AddLine(ImVec2(sx - fs * 0.5f, p0.y + height * 0.2f), ImVec2(sx - fs * 0.5f, p1.y - height * 0.2f), cAccDim, 2.0f);
        dl->AddText(font, fs * 0.85f, ImVec2(sx + fs * 0.4f, p0.y + height * 0.5f - fs * 1.15f), cDim, "3D");
        dl->AddText(font, fs * 1.15f, ImVec2(sx + fs * 0.4f, p0.y + height * 0.5f), cOnGreen, v3d);
        modeEnd = sx + fs * 0.4f + cw + fs * 1.0f;
    }

    // quit cross and minimize dash (not in window mode: the window has its own title bar)
    const float xs = fs * 1.1f;
    const ImVec2 xc(p1.x - fs * 1.4f, p0.y + height * 0.5f);
    const ImVec2 mc(xc.x - xs * 2.3f, xc.y);
    if(!m_windowMode)
    {
        ImGui::SetCursorScreenPos(ImVec2(xc.x - xs, xc.y - xs));
        if(ImGui::InvisibleButton("##quit", ImVec2(xs * 2, xs * 2)))
            PostMessage(m_options.outputWindow, WM_DESTROY, 0, 0); // same as Quit in the notification area menu
        const bool xHov = ImGui::IsItemHovered();
        if(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            dl->AddRect(ImVec2(xc.x - xs, xc.y - xs), ImVec2(xc.x + xs, xc.y + xs), cWhite, 0.0f, 0, 2.0f);
        if(xHov)
        {
            ImGui::SetTooltip("%s", Tr("Quit"));
            dl->AddRectFilled(ImVec2(xc.x - xs, xc.y - xs), ImVec2(xc.x + xs, xc.y + xs), ImGui::GetColorU32(ImVec4(0.85f, 0.15f, 0.15f, 0.9f)));
        }
        dl->AddLine(ImVec2(xc.x - xs * 0.45f, xc.y - xs * 0.45f), ImVec2(xc.x + xs * 0.45f, xc.y + xs * 0.45f), cWhite, 2.0f);
        dl->AddLine(ImVec2(xc.x - xs * 0.45f, xc.y + xs * 0.45f), ImVec2(xc.x + xs * 0.45f, xc.y - xs * 0.45f), cWhite, 2.0f);

        // minimize dash, left of the cross: hides the HUD (same as the hotkey)
        ImGui::SetCursorScreenPos(ImVec2(mc.x - xs, mc.y - xs));
        if(ImGui::InvisibleButton("##minimize", ImVec2(xs * 2, xs * 2)))
            PostMessage(m_options.outputWindow, WM_HOTKEY, HOTKEY_TOGGLEUI, 1);
        if(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            dl->AddRect(ImVec2(mc.x - xs, mc.y - xs), ImVec2(mc.x + xs, mc.y + xs), cWhite, 0.0f, 0, 2.0f);
        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s: %s", Tr("Hide HUD"), Options::HotkeyText(m_options.hotkeys[HOTKEY_TOGGLEUI]).c_str());
            dl->AddRectFilled(ImVec2(mc.x - xs, mc.y - xs), ImVec2(mc.x + xs, mc.y + xs), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f)));
        }
        dl->AddLine(ImVec2(mc.x - xs * 0.5f, mc.y + xs * 0.25f), ImVec2(mc.x + xs * 0.5f, mc.y + xs * 0.25f), cWhite, 2.0f);
    }

    // cells, from the right: profile, HDR (the ones that do not fit are left out)
    std::string profile = m_options.IsDesktopSettings() ? std::string(Tr("Desktop")) : m_options.profileFile;
    if(profile.size() > 4 && _stricmp(profile.c_str() + profile.size() - 4, ".ini") == 0)
        profile.resize(profile.size() - 4);
    if(profile.size() > 18)
        profile = profile.substr(0, 17) + "...";
    const bool hdrOn = WindowsHdrOn(m_options.outputWindow);
    struct Cell { std::string label, value; };
    const Cell cells[2] = { { Tr("Profile"), profile }, { "HDR", hdrOn ? "ON" : "OFF" } };
    float      right    = m_windowMode ? p1.x - fs * 0.6f : mc.x - xs - fs * 0.6f;
    for(int i = 1; i >= 0; i--)
    {
        const float cw = (std::max)(textW(fs * 1.0f, cells[i].value.c_str()), textW(fs * 0.85f, cells[i].label.c_str())) + fs * 2.0f;
        if(right - cw < modeEnd)
            break;
        const float cx = right - cw * 0.5f;
        dl->AddText(font, fs * 0.85f, ImVec2(cx - textW(fs * 0.85f, cells[i].label.c_str()) * 0.5f, p0.y + height * 0.5f - fs * 1.15f), cDim, cells[i].label.c_str());
        dl->AddText(font, fs * 1.0f, ImVec2(cx - textW(fs * 1.0f, cells[i].value.c_str()) * 0.5f, p0.y + height * 0.5f + fs * 0.0f), cWhite, cells[i].value.c_str());
        dl->AddLine(ImVec2(right - cw, p0.y + height * 0.2f), ImVec2(right - cw, p1.y - height * 0.2f), cAccDim, 2.0f);
        right -= cw;
    }

    dl->AddRectFilled(ImVec2(p0.x, p1.y - 2.0f), p1, cAccDim);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void UI::Render()
{
    // a HUD frame longer than 8 ms (the BFI / 3D can flash): logged with the section shown, to find what is slow
    struct SlowFrame
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~SlowFrame()
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if(ms > 8.0)
            {
                const std::string section = s_hudSection ? s_hudSection : "-";
                std::thread([ms, section]() { LsLog("slow HUD frame: %.1f ms (section %s)", ms, section.c_str()); }).detach();
            }
        }
    } slowFrame;
    const auto                        w0 = std::chrono::steady_clock::now();
    std::lock_guard<std::timed_mutex> imguiLock(m_imguiMutex); // the main thread feeds ImGui with the mouse / keyboard
    KeepMax(m_waitRenderUs, (int)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - w0).count());
    ImGui_ImplDX11_NewFrame();
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad; // no gamepad (the backend does not read XInput then)
    // Game capture, HUD shown: the Windows mouse pointer is hidden over Nbeam and the HUD draws its own (a visible Windows pointer makes
    // Windows compose the picture: frames shown twice, the BFI / 3D flickers). Elsewhere the Windows pointer is used as usual.
    const bool ownPointer          = OwnPointerWanted(m_options, m_windowMode);
    ImGui::GetIO().MouseDrawCursor = ownPointer || (m_options.ui && !m_windowMode && m_options.captureWindow != nullptr);
    ImGui_ImplWin32_NewFrame();
    if(ownPointer)
    {
        // the overlay never takes the focus: the position is read from the pointer (the messages do not reach ImGui while the HUD is hidden)
        POINT p {};
        if(GetCursorPos(&p) && ScreenToClient(m_options.outputWindow, &p))
            ImGui::GetIO().AddMousePosEvent((float)p.x, (float)p.y);
    }
    {
        // HUD scale (set by dragging the corner of the HUD): sizes and font follow, the layout is the same at every scale.
        ImGuiIO& io = ImGui::GetIO();
        float    k  = std::clamp(EffScale(), 0.6f, 3.0f);
        if(io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
            k = (std::min)(k, (std::min)(io.DisplaySize.x / (62 * s_baseFont), io.DisplaySize.y / (40 * s_baseFont))); // must fit on the screen
        k = (std::max)(k, 0.6f);
        const float fsz = roundf(s_baseFont * k);
        k               = fsz / s_baseFont;
        if(fabsf(k - s_appliedScale) > 0.001f)
        {
            ImGuiStyle ns = s_baseStyle;
            memcpy(ns.Colors, ImGui::GetStyle().Colors, sizeof(ns.Colors));
            ns.ScaleAllSizes(k);
            ns.FontScaleMain = k;
            ns.FontSizeBase  = ImGui::GetStyle().FontSizeBase;
            ns.FontScaleDpi  = ImGui::GetStyle().FontScaleDpi;
            ImGui::GetStyle() = ns;
            m_fontSize        = fsz;
            s_appliedScale    = k;
        }
        s_iconNbeam = m_logo.get();
        s_iconSB    = m_logoSB.get();
    }
    ImGui::NewFrame();
    if(m_options.ui)
    {
        auto viewport = ImGui::GetMainViewport()->Size;

        const float fs = m_fontSize;
        ImVec2 mainWindowSize((std::min)(62 * fs, viewport.x - 4 * fs), (std::min)(40 * fs, viewport.y));
        // default place of the HUD (before the user moves it): kept in units of the font size so it follows the DPI, and always on screen
        ImVec2 mainWindowPos(65.6f * fs, 28.95f * fs);
        mainWindowPos.x = (std::max)(0.0f, (std::min)(mainWindowPos.x, viewport.x - mainWindowSize.x));
        mainWindowPos.y = (std::max)(0.0f, (std::min)(mainWindowPos.y, viewport.y - mainWindowSize.y));
        // the HUD is a normal window: its edges only stretch it; the scale slider (Appearance) changes the size of the whole interface
        static ImVec2 s_hudSize(0, 0);
        static float  s_lastScale = 0.0f;
        if(m_windowMode)
        {
            // window mode: the HUD fills the whole window (the window itself is moved and sized by Windows)
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(viewport, ImGuiCond_Always);
        }
        else
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(48 * fs, viewport.x), (std::min)(28 * fs, viewport.y)), viewport);
            if(s_lastScale > 0.0f && s_hudSize.x > 0.0f && fabsf(EffScale() - s_lastScale) > 1e-4f)
            {
                const float k = EffScale() / s_lastScale; // the window follows the new scale
                ImGui::SetNextWindowSize(ImVec2(s_hudSize.x * k, s_hudSize.y * k), ImGuiCond_Always);
            }
            else
                ImGui::SetNextWindowSize(mainWindowSize, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(mainWindowPos, ImGuiCond_FirstUseEver);
        }
        s_lastScale = EffScale();
        // OSD layout: status banner on top, category rail on the left, settings of the category in the middle, action bar at the bottom.
        // The window has no title bar of its own (the banner is drawn instead) and no scrolling: the settings pane scrolls.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        const ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                          (m_windowMode ? (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus) : 0);
        const bool hudShown = ImGui::Begin(m_windowMode ? "Nbeam 3D Vision###NbeamWin" : "Nbeam 3D Vision###NbeamHud2", nullptr, hudFlags);
        s_hudSize = ImGui::GetWindowSize();
        ImGui::PopStyleVar();
        const ImVec2 hudSize = ImGui::GetWindowSize();
        if(hudShown)
        {
            const float headerH = fs * 3.6f, footerH = fs * 3.0f, railW = fs * 11.0f;
            const ImVec4 accent((m_options.hud.r), (m_options.hud.g), (m_options.hud.b), 1.0f);
            const ImVec4 accentHi((std::min)(accent.x * 1.25f + 0.03f, 1.0f), (std::min)(accent.y * 1.25f + 0.03f, 1.0f), (std::min)(accent.z * 1.25f + 0.03f, 1.0f), 1.0f);
            const ImVec4 accentDim(accent.x * 0.5f, accent.y * 0.5f, accent.z * 0.5f, 1.0f);
            gSectionColor = accentHi;

            // ---- status banner (logo, output mode, profile / 3D / HDR / FPS) ----
            ImGui::SetCursorPos(ImVec2(0, 0));
            RenderHudHeader(headerH);

            // ---- scale grip: dragging a corner of the HUD changes the scale of the whole interface (attached: top left corner, since the
            // bottom right is fixed; detached: bottom right corner). Double-click: back to 100 %. ----
            ImGui::SetCursorPos(ImVec2(0, 0));

            const float bodyH = (std::max)(ImGui::GetWindowHeight() - headerH - footerH, fs * 4); // header on top, footer at the very bottom of the window
            static const char* catKeys[] = { "Input / Output", "Render / Shaders", "3D", "Profiles", "Hotkeys", "Appearance", "Application", "Test" };
            static const int   catIcon[] = { 1, 2, 3, 4, 7, 6, 5, 0 }; // icon of each category (see DrawCategoryIcon); Test: the speedometer
            static const int   catOrder[] = { 0, 1, 2, 7, 3, 4, 5, 6 }; // order in the rail: Test right above Profiles
            const int          catCount  = (int)(sizeof(catKeys) / sizeof(catKeys[0]));
            static int         s_cat     = 0;
            static int         s_lastCat = -1;
            s_cat                        = std::clamp(s_cat, 0, catCount - 1);
            s_hudSection                 = catKeys[s_cat];

            // ---- category rail ----
            ImGui::SetCursorPos(ImVec2(0, headerH));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.7f, fs * 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
            ImGui::BeginChild("##rail", ImVec2(railW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, 0);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            const bool testHidden = !m_options.captureProcess.empty() && !m_options.motionTest; // a program runs: no Test category
            if(testHidden && s_cat == 7)
                s_cat = 0;
            for(int oi = 0; oi < catCount; oi++)
            {
                const int i = catOrder[oi];
                if(i == 7 && testHidden)
                    continue;
                ImGui::PushID(i);
                const ImVec2 sz(ImGui::GetContentRegionAvail().x, fs * 2.7f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if(ImGui::InvisibleButton("##cat", sz))
                    s_cat = i;
                const bool  hov = ImGui::IsItemHovered(), sel = i == s_cat;
                ImDrawList* dl  = ImGui::GetWindowDrawList();
                const ImVec2 q(p.x + sz.x, p.y + sz.y);
                if(sel)
                {
                    dl->AddRectFilled(p, q, ImGui::GetColorU32(accent));
                    dl->AddRect(p, q, ImGui::GetColorU32(accentHi), 0.0f, 0, 2.0f);
                }
                else if(hov)
                    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.30f)));
                const float lum    = 0.299f * accent.x + 0.587f * accent.y + 0.114f * accent.z;
                const ImU32 txtCol = sel ? (lum > 0.5f ? IM_COL32(8, 8, 8, 255) : IM_COL32(255, 255, 255, 255)) : (hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 220, 215, 255));
                const float tsz    = fs * 1.05f;
                DrawCategoryIcon(dl, catIcon[i], ImVec2(p.x + fs * 1.45f, p.y + sz.y * 0.5f), fs * 1.45f, txtCol);
                dl->AddText(ImGui::GetFont(), tsz, ImVec2(p.x + fs * 2.6f, p.y + (sz.y - tsz) * 0.5f), txtCol, Tr(catKeys[i]));
                ImGui::PopID();
            }
            ImGui::EndChild();

            // ---- settings of the selected category ----
            ImGui::SameLine(0, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.2f, fs * 0.9f));
            ImGui::BeginChild("##pane", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, 0);
            ImGui::PopStyleVar();
            if(s_cat != s_lastCat)
            {
                ImGui::SetScrollY(0.0f);
                s_lastCat = s_cat;
            }
            else if(s_restoreFrames > 0 && s_scrollY > 0.0f)
                ImGui::SetScrollY(s_scrollY); // after a restart the ImGui context is new: put the scroll position back
            else
                s_scrollY = ImGui::GetScrollY();
            if(s_restoreFrames > 0)
                s_restoreFrames--;
            {
                const ImVec2 tp = ImGui::GetCursorScreenPos();
                DrawCategoryIcon(ImGui::GetWindowDrawList(), catIcon[s_cat], ImVec2(tp.x + fs * 0.9f, tp.y + fs * 0.95f), fs * 1.7f, ImGui::GetColorU32(accentHi));
                ImGui::SetCursorScreenPos(ImVec2(tp.x + fs * 2.2f, tp.y));
            }
            ImGui::PushFont(m_largeFont, fs * 1.6f);
            ImGui::TextColored(accentHi, "%s", Tr(catKeys[s_cat]));
            ImGui::PopFont();
            {
                const ImVec2 a = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, a.y + fs * 0.1f), ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y + fs * 0.1f + 2.0f), ImGui::GetColorU32(accentDim));
                ImGui::Dummy(ImVec2(0, fs * 0.5f));
            }
            {
                // a screen below 80 Hz cannot run Nbeam: the message goes away when another connected screen is compatible
                static ULONGLONG s_rateAt  = 0;
                static bool      s_lowRate = false;
                const ULONGLONG  nowT      = GetTickCount64();
                if(nowT - s_rateAt > 1000)
                {
                    s_rateAt  = nowT;
                    s_lowRate = false;
                    if(m_options.vsyncRate > 1.0f && m_options.vsyncRate < 79.5f)
                    {
                        s_lowRate               = true;
                        const std::string outId = m_options.ScreenIdOfDisplay(m_options.shaderDisplayNo, m_displays);
                        for(const auto& m : m_options.MonitorsSnapshot())
                        {
                            int best = m.hz;
                            for(int r : m.rates)
                                best = (std::max)(best, r);
                            if(m.connected && m.id != outId && best >= 80)
                                s_lowRate = false;
                        }
                    }
                }
                if(s_lowRate)
                {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), "%s", Tr("Refresh rate not compatible with Nbeam 3D Vision: 80 Hz minimum are needed to work. Change the refresh rate or connect a compatible screen."));
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ImVec2(0, fs * 0.4f));
                }
            }
            ImGui::PushItemWidth(-fs * 14.0f); // controls line up in one column, the label is on their right

            if(s_cat == 1)
            {
            SectionHeader(Tr("Render Parameters"));

            // ---- SubFrames (the value used without BFI auto; the "BFI auto" checkbox is in the BFI sync block, only while a program is captured) ----
            const int activeMode = m_options.EffectiveSubFramesMode();
            if(m_pending.subFramesMode != SF_MANUAL && activeMode == SF_MANUAL && !m_options.emitter.autoSource)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr("BFI auto needs a captured program: the value below is used until then."));
                ImGui::PopTextWrapPos();
            }

            if(activeMode == SF_MANUAL && m_options.StereoLinked())
            {
                // 3D active: SubFrames is set by the emitter rate
                ImGui::Text(Tr("SubFrames: %d   (set by the 3D emitter rate)"), m_options.subFrames);
                if(m_options.subFrames == 1)
                    ShowAlertMarker(Tr("The effects won't be visible with just one subframe!"));
                else
                    ShowHelpMarker(Tr("SubFrames follows the emitter rate while 3D is active: display Hz / emitter Hz (480 Hz and 120 Hz emitter = 4)."));
            }
            else if(activeMode == SF_AUTO)
            {
                ImGui::Text(Tr("SubFrames: %d   (game %.1f fps, display %.0f Hz)"), m_options.subFrames, m_gameFps, m_options.vsyncRate);
                if(m_options.subFrames == 1)
                    ShowAlertMarker(Tr("The effects won't be visible with just one subframe!"));
                else
                    ShowHelpMarker(Tr("How many screen refreshes make one game image (Content FPS = screen Hz / SubFrames). Example: a 240 Hz screen with 4 = 60 images per second.\nWith BFI auto (below), Nbeam picks it for you from the game's frame rate."));
            }
            else
            {
                int subFramesSetting = m_options.subFramesSetting > 0 ? m_options.subFramesSetting : m_options.subFrames;
                if(SliderIntStep(Tr("SubFrames"), &subFramesSetting, MIN_SUBFRAMES, MAX_SUBFRAMES, 1))
                {
                    // applied live by the renderer, no restart (so no hitch while you step through the values)
                    m_options.subFramesSetting = std::clamp(subFramesSetting, MIN_SUBFRAMES, MAX_SUBFRAMES);
                    m_options.subFrames        = m_options.ResolveSubFrames();
                }
                if(m_options.subFrames == 1)
                    ShowAlertMarker(Tr("The effects won't be visible with just one subframe!"));
                else
                    ShowHelpMarker(Tr("How many screen refreshes make one game image (Content FPS = screen Hz / SubFrames). Example: a 240 Hz screen with 4 = 60 images per second.\nWith BFI auto (below), Nbeam picks it for you from the game's frame rate."));
            }

            // "Clear Frames" (BFI shader) sits right below SubFrames
            for(const auto& p : m_shaderManager.GetParameterInfos())
                if(p.type == ParameterType::ParamInt && p.name == "Clear Frames" && p.dropdown.empty())
                {
                    // never more clear frames than SubFrames (the stored value is kept, only the display and the edit are limited)
                    const int maxClear = (std::max)(p.p.ip.min, (std::min)(p.p.ip.max, m_options.subFrames));
                    int       clearV   = (std::min)(*p.p.ip.value, maxClear);
                    if(SliderIntStep(Tr(p.name.c_str()), &clearV, p.p.ip.min, maxClear, 1)) // same width as SubFrames (default item width)
                        *p.p.ip.value = clearV;
                    ShowHelpMarker(Tr(p.hint.c_str()));
                }

            if(m_options.captureWindow != nullptr)
            {
                // BFI sync, only while a program is captured:
                //  - None (not recommended): a game frame is taken at the start of each cycle;
                //  - Phase lock (2D): the cycle is moved so the game frames arrive just before the visible subframe;
                //  - Frame buffer: game frames kept in advance to smooth an irregular frame rate (also in 3D);
                // "BFI auto" (2D): the SubFrames follow the measured game frame rate; it works with every choice above.
                const bool in3D = m_options.emitter.sourceFormat != StereoSource::Off;
                int        mode = std::clamp(m_options.BfiSyncMode(), 0, 2); // (3D: always the frame buffer, the menu is hidden)
                static const char* modeNames[] = { "None (not recommended)", "Phase lock (best latency)", "Frame buffer (best stability)" };
                if(!in3D) // in 3D the frame buffer is always used: no choice
                {
                ImGui::PushItemWidth(m_fontSize * 16);
                if(ImGui::BeginCombo(Tr("BFI sync"), Tr(modeNames[mode]), 0))
                {
                    for(int i = 0; i < 3; i++)
                    {
                        if(ImGui::Selectable(Tr(modeNames[i]), i == mode) && i != mode)
                        {
                            m_options.bfiSync = i;
                            if(i == 2 && m_options.frameBuffer < 1)
                                m_options.frameBuffer = 1; // (at least one frame)
                            mode = i;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                ShowHelpMarker(Tr("How the BFI follows the game images (only while a program is captured).\nNone (not recommended): simple, but the wait before each image can be long.\nPhase lock (2D): keeps the wait short and steady.\nFrame buffer: keeps a few game images in advance so an irregular game stays smooth, with a little more delay.\nBFI auto (below) works with every choice."));
                }
                // the exact cap for the game (the content rate of the display, with its real refresh rate)
                static ULONGLONG s_hzTick = 0;
                static double    s_hzExact = 0.0;
                if(mode == 1 && GetTickCount64() - s_hzTick > 2000)
                {
                    s_hzTick  = GetTickCount64();
                    s_hzExact = m_options.shaderDisplayNo < m_displays.size() ? ExactRefreshRate(m_displays[m_options.shaderDisplayNo].monitor) : 0.0;
                }
                const double hz  = s_hzExact > 1.0 ? s_hzExact : (double)m_options.vsyncRate;
                const double cap = hz / (std::max)(m_options.subFrames, 1);
                if(mode == 2)
                {
                    // anti-stutter buffer, in game (content) frames: latency = frames x SubFrames / display Hz
                    const int fbMin = 1; // at least one frame
                    int       fb    = std::clamp(m_options.frameBuffer, fbMin, 5);
                    ImGui::PushItemWidth(m_fontSize * 16);
                    if(SliderIntStep(Tr("Frame buffer"), &fb, fbMin, 5, 1))
                        m_options.frameBuffer = std::clamp(fb, fbMin, 5); // applied live by the renderer
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    ImGui::TextDisabled(Tr("in use: %d"), m_options.EffectiveFrameBuffer());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%.0f ms)", 1000.0 * m_options.EffectiveFrameBuffer() * (std::max)(1, m_options.frameBufferStep) * (std::max)(m_options.subFrames, 1) * ((m_options.StereoAllowed() && m_options.emitter.sourceFormat != StereoSource::Off) ? 2.0 : 1.0) / (std::max)(m_options.vsyncRate, 1.0f));
                    ShowHelpMarker(Tr("Keeps a few game images in advance so an irregular game stays smooth, at the cost of a little delay. The value is the most it may use: Nbeam only uses what the game needs, and shows it next to the slider.\nOnly works while a program (.exe) is captured."));
                }
                if(mode == 1 && !in3D)
                {
                    ImGui::PushItemWidth(m_fontSize * 16);
                    SliderFloatStep(Tr("Measure interval"), &m_options.phaseInterval, 0.1f, 2.0f, "%.1f s", 0.1f);
                    ImGui::PopItemWidth();
                    ShowHelpMarker(Tr("Phase lock gives a shorter and steadier delay: Nbeam watches when the game images arrive and shifts its cycle to match. This sets how often it checks, 0.1 to 2 seconds (default 1).\nTip: cap the game close to the rate shown (RTSS, Reflex...) for the best result."));
                    const int    state     = m_options.phaseState;
                    const char*  st        = state == 2 ? Tr("locked") : state == 3 ? Tr("unsteady game frame rate") : state == 1 ? Tr("measuring...") : Tr("waiting for the game");
                    const ImVec4 col       = state == 2 ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : state == 3 ? ImVec4(1.0f, 0.65f, 0.25f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
                    ImGui::TextColored(col, "%s %s", Tr("Phase:"), st);
                    if(state >= 1)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled(Tr("(wait %.1f ms, %d corrections)"), m_options.phaseWaitMs, m_options.phaseCorrections);
                    }
                    ImGui::TextDisabled(Tr("Cap the game at %.3f fps"), cap);
                }
                // BFI auto: the SubFrames follow the game frame rate (2D)
                if(!in3D && !m_options.emitter.autoSource) // hidden in 3D and with Auto source (the SubFrames follow the emitter there)
                {
                    bool autoOn = m_pending.subFramesMode == SF_AUTO;
                    if(ToggleSwitch(Tr("BFI auto"), &autoOn))
                    {
                        const int sf = autoOn ? SF_AUTO : SF_MANUAL;
                        if(sf == SF_MANUAL)
                        {
                            if(m_options.subFramesSetting <= 0)
                                m_options.subFramesSetting = (std::max)(m_options.subFrames, MIN_SUBFRAMES);
                            m_options.subFrames = m_options.ResolveSubFrames();
                        }
                        m_pending.subFramesMode = sf;
                        if(m_options.emitter.sourceFormat == StereoSource::Off)
                            m_options.subFramesMode = sf; // applied live by the renderer (nothing to rebuild without 3D)
                        else
                            SetApplyRequired();
                    }
                    ShowHelpMarker(Tr("Nbeam measures the game's frame rate and adjusts SubFrames for you (e.g. 120 fps on a 480 Hz screen = 4). 2D only, while a program is captured."));
                    if(autoOn)
                    {
                        ImGui::PushItemWidth(m_fontSize * 16);
                        SliderFloatStep(Tr("Detection interval"), &m_options.autoSub.intervalSec, 0.1f, 2.0f, "%.1f s", 0.1f);
                        ImGui::PopItemWidth();
                        ShowHelpMarker(Tr("How often the game frame rate is measured to choose SubFrames, from 0.1 to 2 seconds (default 1). Short = follows frame rate changes quickly, long = steadier."));
                    }
                }
            }


            if(!m_options.SubFramesFollow3D()) // (with a 3D source the SubFrames follow the emitter: no threshold)
            {
                ToggleSwitch(Tr("Uniform brightness"), &m_options.uniformBrightness);
                ShowHelpMarker(Tr("When the SubFrames change, the BFI brightness changes too (more SubFrames = darker). Checked: the picture keeps the brightness it has at the safety threshold. The higher the threshold, the darker the picture. Simple BFI only."));
                ImGui::PushItemWidth(m_fontSize * 16);
                SliderIntStep(Tr("Safety threshold"), &m_options.lowFps.threshold, 2, MAX_SUBFRAMES, 1);
                ImGui::PopItemWidth();
                ShowHelpMarker(Tr("Above this number of SubFrames (BFI auto or Manual, also on the desktop) the BFI would visibly flicker, so the safety applies: see the switch below. Example: on a 480 Hz screen, 10 = 48 Hz."));
                bool disableAbove = m_options.lowFps.mode == 1;
                if(ToggleSwitch(Tr("Disable BFI above the safety threshold"), &disableAbove))
                    m_options.lowFps.mode = disableAbove ? 1 : 0;
                ShowHelpMarker(Tr("Off (default): above the safety threshold the BFI flashes twice as fast, then stays at the threshold rate. On: the BFI is switched off above the safety threshold."));

                if(m_lowFps)
                {
                    if(m_lowBypass)
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "%s", Tr("SubFrames above the safety threshold: BFI disabled"));
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), Tr("SubFrames above the safety threshold: BFI at %.0f Hz"), m_flickerHz);
                }
            }

            SectionHeader(Tr("Shader"));

            {
                // two choices, one above the other, with a vertical switch: up = BFI, down = sweep BFI via MPRT
                bool sweep = m_options.shaderProfileNo == 1;
                if(VerticalSelector("##shaderkind", Tr("BFI"), Tr("MPRT sweep"), &sweep))
                {
                    m_options.shaderProfileNo = sweep ? 1 : 0;
                    m_liveReloadRequired      = true; // the shader is switched live (no restart)
                }
            }
            ShowHelpMarker(Tr("Effect to use. BFI (switch up): black frames inserted between the images. MPRT sweep (switch down): the picture is lit band by band like a CRT beam, with one pulse per pixel as long as its brightest colour, so a moving coloured edge has no colour fringe."));

            // with a single subframe the effects are not visible: the parameters are greyed out
            const bool noEffects = m_options.subFrames <= 1;
            // the parameters are invisible and inert but keep their room, so nothing below them moves
            if(noEffects)
            {
                const ImVec2 notePos = ImGui::GetCursorPos();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr("Effects need 2 or more SubFrames: the shader is off and its parameters are hidden."));
                ImGui::PopTextWrapPos();
                ImGui::SetCursorPos(notePos); // the note sits on top of the hidden parameters, it takes no room
                ImGui::BeginDisabled();
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
            }
            ImGui::PushItemWidth(m_fontSize * 16);
            // the LCD inversion compensation only matters with the LCD anti-retention: hidden while it is off
            bool antiRetention = false;
            for(const auto& p : m_shaderManager.GetParameterInfos())
                if(p.type == ParameterType::ParamInt && p.name == "LCD Anti-retention")
                    antiRetention = *p.p.ip.value != 0;
            for(const auto& p : m_shaderManager.GetParameterInfos())
            {
                if(p.type == ParameterType::ParamInt && p.name == "Clear Frames" && p.dropdown.empty())
                    continue; // shown under SubFrames
                if(p.name == "LCD Inversion Compensation" && !antiRetention)
                    continue;
                switch(p.type)
                {
                case ParameterType::ParamFloat:
                    if(p.p.fp.max - p.p.fp.min >= 1.0f)
                        SliderFloatStep(Tr(p.name.c_str()), p.p.fp.value, p.p.fp.min, p.p.fp.max, "%.1f", 0.1f, 0.1f); // [-] / [+]: steps of 0.1
                    else // a small range (black level 0 - 0.2, compensation 0 - 0.02): finer steps, or the buttons would jump across it
                        SliderFloatStep(Tr(p.name.c_str()), p.p.fp.value, p.p.fp.min, p.p.fp.max, p.p.fp.max - p.p.fp.min >= 0.1f ? "%.2f" : "%.3f", AutoStep(p.p.fp.min, p.p.fp.max));
                    break;
                case ParameterType::ParamInt: {
                    if(p.name == "LCD Anti-retention")
                    {
                        bool on = *p.p.ip.value != 0; // Force Off / Auto: a check box
                        if(ToggleSwitch(Tr(p.name.c_str()), &on))
                            *p.p.ip.value = on ? 1 : 0;
                    }
                    else if(p.dropdown.size())
                    {
                        if(ImGui::BeginCombo(Tr(p.name.c_str()), Tr(p.dropdown.at(*p.p.ip.value).c_str()), 0))
                        {
                            for(const auto& option : p.dropdown)
                            {
                                auto selected = option.first == *p.p.ip.value;
                                if(ImGui::Selectable(Tr(option.second.c_str()), selected))
                                {
                                    *p.p.ip.value = option.first;
                                }
                                if(selected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    else
                    {
                        SliderIntStep(Tr(p.name.c_str()), p.p.ip.value, p.p.ip.min, p.p.ip.max, 1);
                    }
                }
                break;
                default:
                    abort();
                }
                ShowHelpMarker(Tr(p.hint.c_str()));
            }
            ImGui::PopItemWidth();

            if(m_shaderManager.GetParameterInfos().size())
            {
                if(ImGui::Button(Tr("Reset parameters")))
                {
                    m_shaderManager.ResetDefaults();
                }
            }
            if(noEffects)
            {
                ImGui::PopStyleVar();
                ImGui::EndDisabled();
            }

            }
            if(s_cat == 0)
            {
            {
                // the display captured when no process is set (or while it is not running)
                std::string label;
                for(const auto& window : m_inputs)
                    if(!window.hwnd && window.captureDisplayNo == m_pending.captureDisplayNo && window.captureMethod == m_pending.captureMethod)
                        label = window.name;
                if(ImGui::BeginCombo(Tr("Capture Input"), label.c_str(), 0))
                {
                    if(ImGui::IsWindowAppearing())
                        ScanWindows(); // once when the list opens, not on every frame
                    for(const auto& window : m_inputs)
                    {
                        if(window.hwnd)
                            continue; // programs are captured by their .exe (below)
                        auto selected = window.captureDisplayNo == m_pending.captureDisplayNo && window.captureMethod == m_pending.captureMethod;
                        if(ImGui::Selectable(window.name.c_str(), selected))
                        {
                            m_pending.captureDisplayNo = window.captureDisplayNo;
                            m_pending.captureMethod    = window.captureMethod;
                            SetApplyRequired();
                        }
                        if(selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ShowHelpMarker(Tr("Display captured when no linked program is running."));
            }

            {
                // the output screen: connected screens only (the remembered ones that are not connected are hidden)
                std::string cur = m_pending.outputAuto ? Tr("Automatic") : m_pending.outputOnProcess ? Tr("Process (over the game)") : m_displays[m_pending.shaderDisplayNo].name;
                if(ImGui::BeginCombo(Tr("Output Display"), cur.c_str(), 0))
                {
                    if(ImGui::Selectable(Tr("Automatic"), m_pending.outputAuto))
                    {
                        // starts on the Windows main display; the game window then decides (main display or over the game)
                        m_pending.outputAuto      = true;
                        m_pending.outputOnProcess = false;
                        const HMONITOR primary    = MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                        for(const auto& display : m_displays)
                            if(display.monitor == primary)
                                m_pending.shaderDisplayNo = display.no;
                        m_options.shaderScreenId.clear();
                        SetApplyRequired();
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("A game in full screen or borderless maximized: the output goes on the Windows main display. A game in a window smaller than the screen: the output goes over the game (Process)."));
                    for(const auto& display : m_displays)
                    {
                        auto selected = !m_pending.outputAuto && !m_pending.outputOnProcess && display.no == m_pending.shaderDisplayNo;
                        if(ImGui::Selectable(display.name.c_str(), selected))
                        {
                            m_pending.shaderDisplayNo = display.no;
                            m_pending.outputOnProcess = false;
                            m_pending.outputAuto      = false;
                            m_options.shaderScreenId.clear();
                            SetApplyRequired();
                        }
                        if(selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    if(ImGui::Selectable(Tr("Process (over the game)"), !m_pending.outputAuto && m_pending.outputOnProcess))
                    {
                        m_pending.outputOnProcess = true;
                        m_pending.outputAuto      = false;
                        SetApplyRequired();
                    }
                    ImGui::EndCombo();
                }
            }
            ShowHelpMarker(Tr("The screen Nbeam displays on (your high refresh rate one).\nProcess: the image is placed over the game window and follows it, so a windowed game gets an output of its own size. It needs the program to be added in the profile configuration.\nAutomatic: a game in full screen or borderless maximized puts the output on the Windows main display, a game in a smaller window puts it over the game.\nIt is best to have just one monitor connected."));

            if(ImGui::BeginCombo(Tr("Capture GPU"), m_adapters[m_pending.captureAdapterNo].name.c_str(), 0))
            {
                for(const auto& adapter : m_adapters)
                {
                    auto selected = adapter.no == m_pending.captureAdapterNo;
                    if(ImGui::Selectable(adapter.name.c_str(), selected))
                    {
                        m_pending.captureAdapterNo = adapter.no;
                        SetApplyRequired();
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ShowHelpMarker(
                Tr("GPU used for capture. This should be your primary/gaming GPU."));

            if(ImGui::BeginCombo(Tr("Shader GPU"), m_adapters[m_pending.shaderAdapterNo].name.c_str(), 0))
            {
                for(const auto& adapter : m_adapters)
                {
                    auto selected = adapter.no == m_pending.shaderAdapterNo;
                    if(ImGui::Selectable(adapter.name.c_str(), selected))
                    {
                        m_pending.shaderAdapterNo = adapter.no;
                        SetApplyRequired();
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ShowHelpMarker(Tr("GPU used for rendering. If you have another dGPU or iGPU, try using it here for best experience.\nIf you have a single GPU but are seeing it multiple times, try using the second version as Shader GPU here."));



            // below 3 queued frames (1, 2, 0 = tearing) 3D is not available: red message below
            int queueShown = m_pending.queueAuto ? -1 : m_pending.maxQueuedFrames; // (-1 = Auto)
            if(ImGui::BeginCombo(Tr("Queued Frames"), m_pending.queueAuto ? Tr("Auto") : Tr(m_queuedFrames[queueShown].c_str()), 0))
            {
                // Auto: 3 queued frames, one more (up to 6) each time a frame is late, back down after a long calm
                if(ImGui::Selectable(Tr("Auto"), m_pending.queueAuto))
                {
                    m_pending.queueAuto = true;
                    if(!m_options.queueAuto)
                    {
                        // the swap chain must have the buffers of the longest queue: live if it has them, else a new swap chain
                        if(!m_options.Tearing() && (int)m_options.swapChainBuffers >= Options::QUEUE_AUTO_MAX + 1)
                        {
                            m_options.queueAuto    = true;
                            m_options.queueAutoCur = Options::QUEUE_AUTO_MIN;
                            m_options.liveLatency  = true;
                        }
                        else
                            SetApplyRequired();
                    }
                }
                if(m_pending.queueAuto)
                    ImGui::SetItemDefaultFocus();
                // order shown: 0 (tearing allowed), 1, 2, 3 (default = driver default), 4 ... 10
                std::vector<int> order = { Options::QUEUE_TEARING, 1, 2, 0 };
                for(int v = 4; v <= Options::QUEUE_MAX; v++)
                    order.push_back(v);
                for(const int i : order)
                {
                    auto selected = !m_pending.queueAuto && i == queueShown;
                    if(ImGui::Selectable(Tr(m_queuedFrames[i].c_str()), selected))
                    {
                        m_pending.maxQueuedFrames = i;
                        m_pending.queueAuto       = false;
                        // the swap chain keeps its buffers: a lower or equal latency is applied live, a higher one needs a new swap chain
                        if(i != Options::QUEUE_TEARING && !m_options.Tearing() && (i ? i + 1 : 3) <= (int)m_options.swapChainBuffers)
                        {
                            m_options.maxQueuedFrames = i;
                            m_options.queueAuto       = false;
                            m_options.liveLatency     = true;
                        }
                        else
                            SetApplyRequired();
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ShowHelpMarker(Tr("Frames Windows keeps in advance before showing them: a longer queue absorbs the moments when the game takes the graphics card (a missed refresh: the glasses lose step for a moment), at the price of about one refresh of delay per frame.\012Auto: 3 frames, then one more (up to 6) each time a frame is late, and back down one at a time after a long calm. It is a different setting from the frame buffer, which smooths the game's own frame times."));
            if(queueShown == Options::QUEUE_TEARING || (queueShown >= 1 && queueShown < 3))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_fontSize * 24);
                ImGui::TextUnformatted(Tr("3D is not available below 3 queued frames. Choose 3 or more to use 3D."));
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            if(queueShown == Options::QUEUE_TEARING)
            {
                {
                    // 0 = tearing allowed: explained, strongly discouraged with BFI
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_fontSize * 24);
                    ImGui::TextUnformatted(Tr("Warning: 0 (tearing allowed) is strongly discouraged with BFI. With no queue the image can change in the middle of a screen refresh: the tear line cuts the picture, and the image frames and black frames of the BFI no longer line up with the refreshes. You get bands, uneven brightness and flicker, and the clearer motion the BFI is there for is lost. Choose another value."));
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
                ToggleSwitch(Tr("Beam racing"), &m_options.beamRacing);
                ShowHelpMarker(Tr("Hides the tearing where the effect makes it invisible, by following the real position of the screen scan. With BFI and plain picture, the image is sent just before the screen refresh: no visible tear, a little more delay. The MPRT sweep can reach an even lower delay. Not used in 3D. Uses one processor core while waiting; the Nbeam window must be shown directly (nothing over it)."));
                if(m_options.beamRacing)
                {
                    ImGui::SliderInt(Tr("Tearing lead"), &m_options.beamLead, 0, 20, "%d %%");
                    ShowHelpMarker(Tr("How early the flip is sent, in % of a refresh, to make up for the time it takes to reach the screen. If a tear shows at the top of the picture, increase it; if the latency matters more, lower it."));
                    if(m_options.beamRacingState == 2)
                        ImGui::TextDisabled("%s", Tr("Not used in 3D: classic timing."));
                    if(m_options.beamRacingState < 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "%s", Tr("The scanout position cannot be read on this display: normal tearing is used."));
                }
                else
                {
                    ImGui::SliderInt(Tr("Tearing control"), &m_options.tearingLine, 0, 100, "%d %%");
                    ShowHelpMarker(Tr("Where the tear line lands on the screen. 0 = the flip is right after the vertical blank, so the tear is at the very top and hidden. Higher values move it down the picture. Adjust while watching a horizontal pan."));
                }
            }
            ShowHelpMarker(Tr("Images queued before the screen. Increase it if you see irregular flashing: lower = less delay, higher = more stable. The Windows default is usually 3.\n0 = no queue: lowest delay, but tearing is allowed. In 3D the minimum is 3 (turn 3D off to go lower)."));

            // "Direct presentation" (experimental): the overlay is no longer a layered window, so Windows can show it directly (renderer restart)
            if(ToggleSwitch(Tr("Direct presentation (experimental)"), &m_options.directPresent))
                PostMessage(m_options.outputWindow, WM_USER_RESTART, NB_RESTART_TAG, 0);
            ShowHelpMarker(Tr("Experimental. Windows shows Nbeam directly on the screen instead of blending it with the desktop: about one refresh less delay and a steadier BFI. There is no click-through any more: while the HUD is hidden, the overlay ignores clicks and hides the Windows pointer. Games that read the mouse directly keep working; games and menus that need the Windows pointer will not get the clicks. While the game is not the window in front (Alt+Tab, another program), Direct presentation is switched off so you can use the other program with the mouse; it comes back when the game is in front again. Nothing must be shown over Nbeam (notifications, Discord or Steam overlays...). See the line below."));
            if(m_options.directPresent)
            {
                ToggleSwitch(Tr("Draw a pointer (direct presentation)"), &m_options.drawPointer);
                ShowHelpMarker(Tr("Direct presentation hides the Windows pointer over Nbeam. This draws one in its place, with the picture (it follows the BFI and the 3D), while the mouse moves (it disappears after 1 second without movement). It is not drawn while a game has captured the mouse to turn the camera (the mouse moves but the pointer stays put or is put back in the middle). Turn it off if the game draws its own pointer."));
            }
            {
                const int   pm = m_options.presentMode;
                const char* t  = pm == 0 ? Tr("composed by Windows") : pm == 1 ? Tr("direct (hardware plane)") : pm == 3 ? Tr("composition failure") : Tr("unknown");
                ImGui::TextDisabled(Tr("Presentation: %s, late frames: %.0f /min"), t, m_options.presentGlitches);
                ShowHelpMarker(Tr("How Windows shows Nbeam right now, and how many images stayed on screen longer or shorter than expected (a late image = a blink), per minute, measured over 10 seconds. Compare with and without Direct presentation in the same game. Not counted with tearing (Queued Frames 0)."));
            }

            if(ToggleSwitch(Tr("Hardware sRGB"), &m_pending.hardwareSrgb))
            {
                SetApplyRequired();
            }
            ShowHelpMarker(Tr("Reduces shader cost up to ~30% but with some quality loss and inability to adjust gamma (fixed at ~2.2). For slow iGPUs only."));


            // no Apply button: pending changes are applied as soon as you finish editing (see the end of this section)
            ToggleSwitch(Tr("Remember"), &m_options.rememberSettings);

            }
            if(s_cat == 2)
            {
            m_emitterPanel.displayHz = m_options.vsyncRate;
            m_emitterPanel.subFrames = m_options.subFrames;
    m_emitterPanel.stereoBlocked = !m_options.StereoManual();
            m_emitterPanel.queueBlocked  = !m_options.QueueAllows3D();
            m_emitterPanel.gameCapture   = m_options.captureWindow != nullptr;
            m_emitterPanel.outputOnProcess = m_options.outputOnProcess || m_options.processOutput;
            m_emitterPanel.fontSize  = m_fontSize;
            m_emitterPanel.status    = m_emitterStatus;
            m_emitterPanel.aiStatus  = m_aiStatus;
            m_emitterPanel.aiInferMs = m_aiInferMs;
            m_emitterPanel.aiFps     = m_aiFps;
            m_emitterPanel.aiLatencyMs = m_aiLatencyMs;
            if(RenderEmitterPanel(m_options.emitter, m_emitterPanel))
                m_emitterRestartRequired = true;
            if(m_emitterPanel.fullRestart)
            {
                m_emitterPanel.fullRestart = false;
                m_applyRequired            = true; // Full SBS toggled: the whole renderer and the capture restart (as the Restart button does)
            }

            }
            if(s_cat == 3)
            {
            // ---- "Manage profiles", above the profile configuration: rename, choose, delete and create the profiles ----
            SectionHeader(Tr("Manage profiles"));
            {
                if(s_profilesDirty)
                {
                    s_profiles      = Options::ListProfiles();
                    SortNoCase(s_profiles);
                    s_profilesDirty = false;
                }
                // The profiles only apply while their program runs (detections below); the desktop has its own settings. "Profile to adjust":
                // the motion test opens with that profile loaded, it is set over the cars; the end of the test goes back to the desktop.
                // a detected program runs: its profile is in use, the settings of the profiles are hidden (its profile is changed in the
                // detection below, applied at once)
                const bool programRuns = !m_options.captureProcess.empty() && !m_options.motionTest;
                if(programRuns)
                {
                    const ImVec4 warn(1.0f, 0.78f, 0.25f, 1.0f);
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(warn, "%s %s", Tr("Detected program running:"), m_options.captureProcess.c_str());
                    ImGui::Text("%s %s", Tr("Profile in use:"), m_options.IsDesktopSettings() ? Tr("Desktop") : m_options.profileFile.c_str());
                    ImGui::TextDisabled("%s", Tr("The profile settings come back when the program closes. To use another profile for this program, change it in the program list below: it is applied at once."));
                    ImGui::PopTextWrapPos();
                }
                else
                {
                    const bool adjusting   = m_options.motionTest && !m_options.IsDesktopSettings();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", Tr("Active settings:"));
                    ImGui::SameLine();
                    if(m_options.IsDesktopSettings())
                        ImGui::TextUnformatted(Tr("Desktop"));
                    else
                        ImGui::Text("%s (%s)", m_options.profileFile.c_str(), adjusting ? Tr("adjusting") : Tr("detected program"));
                    ImGui::PushItemWidth(m_fontSize * 14);
                    ImGui::InputText(Tr("Create a profile"), s_newProfile, sizeof(s_newProfile));
                    ImGui::SameLine();
                    if(ImGui::Button(Tr("Save as")) && s_newProfile[0])
                    {
                        std::string name = s_newProfile;
                        for(char& c : name)
                            if(strchr("\\/:*?\"<>|", c))
                                c = '_';
                        if(name.size() < 4 || _stricmp(name.c_str() + name.size() - 4, ".ini") != 0)
                            name += ".ini";
                        if(_stricmp(name.c_str(), "imgui.ini") != 0)
                        {
                            // written under the new name; the active settings stay the same
                            const std::string keep = m_options.profileFile;
                            m_options.profileFile  = name;
                            m_options.Save(m_shaderManager);
                            m_options.profileFile = keep;
                            s_profilesDirty       = true;
                            s_newProfile[0]       = 0;
                        }
                    }
                    ImGui::SameLine();
                    {
                        // small Explorer (folder) button: opens the Profils folder
                        const float  bh = ImGui::GetFrameHeight();
                        const ImVec2 p  = ImGui::GetCursorScreenPos();
                        if(ImGui::Button("##openProfiles", ImVec2(bh * 1.3f, bh)))
                        {
                            Options::EnsureProfilesDir();
                            ShellExecuteW(nullptr, L"open", Options::ProfilesDir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        }
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", Tr("Open the Profils folder in the Explorer"));
                        ImDrawList*  dl = ImGui::GetWindowDrawList();
                        const ImVec2 c(p.x + bh * 0.65f, p.y + bh * 0.5f);
                        const float  u  = bh * 0.30f;
                        const ImU32  fc = IM_COL32(245, 200, 80, 255), fb = IM_COL32(225, 170, 50, 255);
                        dl->AddRectFilled(ImVec2(c.x - u * 1.25f, c.y - u * 0.95f), ImVec2(c.x - u * 0.1f, c.y - u * 0.4f), fb, u * 0.12f);
                        dl->AddRectFilled(ImVec2(c.x - u * 1.25f, c.y - u * 0.55f), ImVec2(c.x + u * 1.25f, c.y + u * 0.95f), fc, u * 0.12f);
                    }
                    ShowHelpMarker(Tr("Saves the current settings as a profile under a new name (or overwrites an existing profile). Then link it to a program (below) and adjust it with \"Profile to adjust\" (Motion test)."));
                    ImGui::PopItemWidth();
                }
            }
            // ---- profile configuration: everything about the profiles in one section (the profiles, then the programs they go with) ----
            SectionHeader(Tr("Profile configuration"));
            // three tabs, in the style of a graphics driver panel: the profiles, the settings of each program (list on the left, its
            // settings on the right), and the settings for every program at once
            const bool profTabs = ImGui::BeginTabBar("##proftabs");
            {
                // the detections are also read by the main thread (ProfileLinkPoll): no change of the list while it reads it
                std::lock_guard<std::recursive_mutex> linksLock(m_options.linksMutex);
                if(s_profilesDirty)
                {
                    s_profiles      = Options::ListProfiles();
                    SortNoCase(s_profiles);
                    s_profilesDirty = false;
                }
                int  addAfter = -1;
                bool save     = false;

                // (the screens known to Nbeam are read again when Nbeam starts or restarts, never here: see WM_USER_RESTART)
                const auto  mons      = m_options.MonitorsSnapshot();
                const std::string cur = m_options.ScreenIdOfDisplay(m_options.shaderDisplayNo, m_displays); // the screen of the output now
                auto exeKey = [](const std::string& e) { return Options::NormalizeExeName(e); };
                // does another detection of the same program already use this screen?
                auto usedByOther = [&](size_t self, const std::string& id) {
                    for(size_t k = 0; k < m_options.profileLinks.size(); k++)
                        if(k != self && !id.empty() && !exeKey(m_options.profileLinks[self].exe).empty() && m_options.profileLinks[k].screenId == id && exeKey(m_options.profileLinks[k].exe) == exeKey(m_options.profileLinks[self].exe))
                            return true;
                    return false;
                };
                // first screen free for a detection of that program: the current output screen first, then connected ones, then remembered ones
                auto freeScreen = [&](size_t self) -> std::string {
                    if(!cur.empty() && !usedByOther(self, cur))
                        return cur;
                    for(int pass = 0; pass < 2; pass++)
                        for(const auto& m : mons)
                            if((pass == 1 || m.connected) && !usedByOther(self, m.id))
                                return m.id;
                    return "";
                };
                auto monLabel = [&](const Options::KnownMonitor& m) {
                    std::string s = m.name;
                    if(m.primary)
                        s += std::string(" [") + Tr("main") + "]";
                    if(!m.connected)
                        s += std::string(" [") + Tr("not connected") + "]";
                    if(m.hz > 0)
                        s += " - " + std::to_string(m.hz) + " Hz";
                    return s;
                };
                // old detections without a screen, or two detections of one program on the same screen: pick a free screen
                for(size_t i = 0; i < m_options.profileLinks.size(); i++)
                {
                    auto& link = m_options.profileLinks[i];
                    if((link.screenId.empty() || usedByOther(i, link.screenId)) && !mons.empty())
                    {
                        const std::string f = freeScreen(i);
                        if(f != link.screenId)
                        {
                            link.screenId = f;
                            save          = true;
                        }
                    }
                }
                std::vector<int> removeList;
                // the programs: one entry per .exe, in alphabetical order (a line whose name is still empty is the "new program")
                std::vector<std::string> procs;
                bool                     hasEmpty = false;
                for(const auto& l : m_options.profileLinks)
                {
                    const std::string k = exeKey(l.exe);
                    if(k.empty())
                        hasEmpty = true;
                    else if(std::find(procs.begin(), procs.end(), k) == procs.end())
                        procs.push_back(k);
                }
                SortNoCase(procs);
                {
                    const bool valid = s_procSel.empty() ? hasEmpty : std::find(procs.begin(), procs.end(), s_procSel) != procs.end();
                    if(!valid)
                        s_procSel = procs.empty() ? std::string() : procs[0];
                }
                const ImVec4 dimText = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                // ---- tab "Program settings": the list of the programs on the left, the settings of the selected one on the right ----
                if(profTabs && ImGui::BeginTabItem(Tr("Program settings")))
                {
                    const float listW = m_fontSize * 12.0f;
                    // the list and the settings use the whole height left in the window (no big empty area under them), at least 19 lines
                    const float paneH = (std::max)(m_fontSize * 19.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y);
                    ImGui::BeginGroup();
                    {
                        ImGui::BeginChild("##proglist", ImVec2(listW, paneH - ImGui::GetFrameHeightWithSpacing() * 3.0f), ImGuiChildFlags_Borders);
                        ImGui::TextDisabled("%d %s", (int)procs.size(), Tr("programs"));
                        ImGui::Separator();
                        for(const auto& k : procs)
                        {
                            bool on = false;
                            for(const auto& l : m_options.profileLinks)
                                on = on || (exeKey(l.exe) == k && l.enabled);
                            if(!on)
                                ImGui::PushStyleColor(ImGuiCol_Text, dimText); // unchecked program: greyed in the list
                            if(ImGui::Selectable(k.c_str(), k == s_procSel))
                                s_procSel = k;
                            if(!on)
                                ImGui::PopStyleColor();
                        }
                        if(hasEmpty && ImGui::Selectable(Tr("(new program)"), s_procSel.empty()))
                            s_procSel.clear();
                        if(procs.empty() && !hasEmpty)
                        {
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextDisabled("%s", Tr("No program yet: write a name above or add one from the taskbar."));
                            ImGui::PopTextWrapPos();
                        }
                        ImGui::EndChild();
                        if(ImGui::Button(Tr("Write a new process"), ImVec2(listW, 0)))
                        {
                            // an empty line to fill in: its name is typed in the field on the right
                            bool haveEmpty = false;
                            for(const auto& l : m_options.profileLinks)
                                haveEmpty = haveEmpty || exeKey(l.exe).empty();
                            if(!haveEmpty && m_options.profileLinks.size() < 64)
                            {
                                Options::ProfileLink nl;
                                nl.screenId = cur; // the first profile of a program: the current screen
                                nl.fpsCapMode = m_options.DefaultFpsCapMode();
                                nl.fpsLimit   = m_options.fpsLimitDefault;
                                nl.profile    = m_options.ProfileForNewProgram();
                                m_options.profileLinks.push_back(nl);
                                save = true;
                            }
                            s_procSel   = std::string();
                            s_procFocus = true;
                        }
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", Tr("Adds an empty program: type the name of its .exe (e.g. game.exe) in the field at the top right."));
                        // add a program chosen among the ones shown on the taskbar
                        static std::mutex               s_taskbarMutex;
                        static std::vector<std::string> s_taskbarExes; // scanned when the list is opened, by a thread of its own
                        static std::atomic<bool>        s_taskbarBusy { false };
                        if(ImGui::Button(Tr("Add a new process"), ImVec2(listW, 0)))
                        {
                            {
                                std::lock_guard<std::mutex> l(s_taskbarMutex);
                                s_taskbarExes.clear();
                            }
                            // asking Windows for the name of each program takes a few ms per window: never on the render thread
                            if(!s_taskbarBusy.exchange(true))
                                std::thread([]() {
                                    std::vector<std::string> found;
                                    EnumWindows(
                                    [](HWND w, LPARAM p) -> BOOL {
                                        auto&      list = *(std::vector<std::string>*)p;
                                        const auto ex   = GetWindowLongW(w, GWL_EXSTYLE);
                                        if(!IsWindowVisible(w) || GetWindowTextLengthW(w) == 0)
                                            return TRUE;
                                        if(GetWindow(w, GW_OWNER) != NULL && !(ex & WS_EX_APPWINDOW))
                                            return TRUE;
                                        if((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW))
                                            return TRUE;
                                        DWORD cloaked = 0;
                                        if(SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                                            return TRUE;
                                        DWORD pid = 0;
                                        GetWindowThreadProcessId(w, &pid);
                                        if(!pid || pid == GetCurrentProcessId())
                                            return TRUE;
                                        const std::string name = Options::ProcessNameOfWindow(w);
                                        static const char* ignored[] = { "explorer.exe", "searchhost.exe", "searchapp.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
                                                                         "textinputhost.exe", "applicationframehost.exe", "lockapp.exe", "dwm.exe" };
                                        if(name.empty())
                                            return TRUE;
                                        for(const char* ig : ignored)
                                            if(name == ig)
                                                return TRUE;
                                        if(std::find(list.begin(), list.end(), name) == list.end())
                                            list.push_back(name);
                                        return TRUE;
                                    },
                                        (LPARAM)&found);
                                    // Lossless Scaling is always offered: its scaling layer is what is detected and captured, its settings window does not
                                    // have to be open
                                    if(std::find(found.begin(), found.end(), std::string("losslessscaling.exe")) == found.end())
                                        found.push_back("losslessscaling.exe");
                                    std::sort(found.begin(), found.end());
                                    {
                                        std::lock_guard<std::mutex> l(s_taskbarMutex);
                                        s_taskbarExes = std::move(found);
                                    }
                                    s_taskbarBusy = false;
                                }).detach();
                            ImGui::OpenPopup("##taskbarExes");
                        }
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", Tr("Choose a program among the ones open on the taskbar."));
                        if(ImGui::BeginPopup("##taskbarExes"))
                        {
                            int                      shown = 0;
                            std::vector<std::string> exes;
                            {
                                std::lock_guard<std::mutex> l(s_taskbarMutex);
                                exes = s_taskbarExes;
                            }
                            for(const auto& exe : exes)
                            {
                                bool known = false; // programs already in the list are not offered
                                for(const auto& l : m_options.profileLinks)
                                    known = known || Options::NormalizeExeName(l.exe) == exe;
                                if(known)
                                    continue;
                                shown++;
                                if(ImGui::Selectable(exe.c_str()) && m_options.profileLinks.size() < 64)
                                {
                                    Options::ProfileLink nl;
                                    nl.exe      = exe;
                                    nl.screenId = cur;
                                    nl.fpsCapMode = m_options.DefaultFpsCapMode();
                                    nl.fpsLimit   = m_options.fpsLimitDefault;
                                    nl.profile    = m_options.ProfileForNewProgram();
                                    m_options.profileLinks.push_back(nl);
                                    s_procSel = exe; // the program just added is the one shown
                                    save      = true;
                                }
                            }
                            if(shown == 0)
                                ImGui::TextUnformatted(s_taskbarBusy ? Tr("Looking for the open programs...") : Tr("No program open on the taskbar"));
                            ImGui::EndPopup();
                        }
                        // removes the selected program (all its screens) from the detections
                        bool haveSel = false;
                        for(const auto& l : m_options.profileLinks)
                            haveSel = haveSel || exeKey(l.exe) == s_procSel;
                        if(!haveSel)
                            ImGui::BeginDisabled();
                        if(ImGui::Button(Tr("Delete the process"), ImVec2(listW, 0)))
                            for(size_t k = 0; k < m_options.profileLinks.size(); k++)
                                if(exeKey(m_options.profileLinks[k].exe) == s_procSel)
                                    removeList.push_back((int)k);
                        if(!haveSel)
                            ImGui::EndDisabled();
                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("%s", Tr("Removes the selected program from the list (its profile file is kept)."));
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    ImGui::BeginChild("##progpane", ImVec2(0, paneH), ImGuiChildFlags_Borders);
                    {
                        // the links of the selected program (one per screen)
                        std::vector<size_t> grp;
                        for(size_t k = 0; k < m_options.profileLinks.size(); k++)
                            if(exeKey(m_options.profileLinks[k].exe) == s_procSel)
                                grp.push_back(k);
                        if(grp.empty())
                            ImGui::TextDisabled("%s", Tr("Choose a program in the list."));
                        else
                        {
                            // its name: rewrite it here to change it
                            static std::string s_procBufFor = "\x01";
                            static char        s_procBuf[520];
                            if(s_procBufFor != s_procSel)
                            {
                                strncpy_s(s_procBuf, s_procSel.c_str(), sizeof(s_procBuf) - 1);
                                s_procBufFor = s_procSel;
                            }
                            if(s_procFocus)
                            {
                                ImGui::SetKeyboardFocusHere();
                                s_procFocus = false;
                            }
                            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.5f);
                            if(ImGui::InputTextWithHint("##procname", Tr("Path or name of the .exe"), s_procBuf, sizeof(s_procBuf)))
                            {
                                for(size_t k : grp)
                                    m_options.profileLinks[k].exe = s_procBuf;
                                s_procSel    = exeKey(s_procBuf);
                                s_procBufFor = s_procSel; // (the text stays as typed)
                            }
                            if(ImGui::IsItemDeactivatedAfterEdit())
                                save = true;
                            ShowHelpMarker(Tr("When a program is detected, its profile is loaded. When it closes, the desktop settings come back.\nWrite the name of the program (e.g. game.exe) or add it from the taskbar, then choose the profile to use on each screen. You can also press the \"Add the program to the profiles\" hotkey while it is in front.\nA program can have up to 3 profiles, one per screen. The output screen (where Nbeam displays) is used first."));
                            auto& head = m_options.profileLinks[grp[0]];
                            int   dropMember = -1;
                            if(ImGui::BeginTable("##progtable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                            {
                                ImGui::TableSetupColumn(Tr("Setting"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                                ImGui::TableSetupColumn(Tr("Current value"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
                                ImGui::TableHeadersRow();
                                auto rowLabel = [&](const char* label, bool indent = false) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::AlignTextToFramePadding();
                                    if(indent)
                                        ImGui::Indent(m_fontSize);
                                    ImGui::TextUnformatted(label);
                                    if(indent)
                                        ImGui::Unindent(m_fontSize);
                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::SetNextItemWidth(-FLT_MIN);
                                };
                                for(size_t n = 0; n < grp.size(); n++)
                                {
                                    auto& link = m_options.profileLinks[grp[n]];
                                    ImGui::PushID((int)grp[n]);
                                    char screenLabel[48];
                                    snprintf(screenLabel, sizeof(screenLabel), "%s %d", Tr("Screen"), (int)n + 1);
                                    rowLabel(screenLabel);
                                    {
                                        // screen of this profile: used when the program runs with that screen connected
                                        std::string label = Tr("Choose a screen");
                                        for(const auto& m : mons)
                                            if(m.id == link.screenId)
                                                label = monLabel(m);
                                        if(n > 0)
                                            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
                                        if(ImGui::BeginCombo("##screen", label.c_str(), 0))
                                        {
                                            for(const auto& m : mons)
                                            {
                                                if(m.id != link.screenId && usedByOther(grp[n], m.id))
                                                    continue; // one screen per profile of a program
                                                {
                                                    int best = m.hz;
                                                    for(int r : m.rates)
                                                        best = (std::max)(best, r);
                                                    if(m.id != link.screenId && best > 0 && best < 79)
                                                        continue; // under 80 Hz: not offered (Nbeam needs 80 Hz minimum)
                                                }
                                                if(ImGui::Selectable(monLabel(m).c_str(), m.id == link.screenId))
                                                {
                                                    link.screenId = m.id;
                                                    save          = true;
                                                }
                                            }
                                            ImGui::EndCombo();
                                        }
                                        if(ImGui::IsItemHovered())
                                            ImGui::SetTooltip("%s", Tr("Screen of this program (output and capture of the profile). The screens are recognised by their name and remembered, even when they are not connected."));
                                        if(n > 0)
                                        {
                                            ImGui::SameLine();
                                            if(ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), 0)))
                                                dropMember = (int)grp[n];
                                            if(ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", Tr("Remove this profile / screen"));
                                        }
                                    }
                                    rowLabel(Tr("Enabled"), true);
                                    {
                                        if(ToggleSwitch("##on", &link.enabled))
                                            save = true;
                                        if(ImGui::IsItemHovered())
                                            ImGui::SetTooltip("%s", Tr("Enable the detection of the program on this screen. Unchecked = ignored on this screen."));
                                    }
                                    if(n == 0)
                                    {
                                        rowLabel(Tr("Profile"), true);
                                        if(ImGui::BeginCombo("##profile", link.profile.empty() ? Tr("(no profile)") : link.profile.c_str(), 0))
                                        {
                                            for(const auto& name : s_profiles)
                                                if(ImGui::Selectable(name.c_str(), name == link.profile))
                                                {
                                                    for(size_t k : grp)
                                                        m_options.profileLinks[k].profile = name; // the same profile for every screen of the program
                                                    save = true;
                                                }
                                            ImGui::EndCombo();
                                        }
                                    }
                                    else
                                        link.profile = head.profile;
                                    rowLabel(Tr("Auto fps cap"), true);
                                    {
                                        int mode = link.fpsCapMode;
                                        if(FpsCapCombo("##capmode", mode, m_options.rtssAvailable, m_options.dcAvailable))
                                        {
                                            link.fpsCapMode = mode;
                                            save            = true;
                                        }
                                    }
                                    rowLabel(Tr("Frame limit"), true);
                                    {
                                        // frame limit of the program on this screen, written in the DisplayCommander.ini / relimiter.ini next to the game
                                        const bool addon = link.fpsCapMode == 2 || link.fpsCapMode == 3;
                                        ImGui::BeginDisabled(!addon);
                                        int lim = link.fpsLimit;
                                        if(ImGui::DragInt("##fpslimit", &lim, 0.5f, 0, 1000, lim == 0 ? Tr("No limit") : "%d fps"))
                                            link.fpsLimit = std::clamp(lim, 0, 1000);
                                        if(ImGui::IsItemDeactivatedAfterEdit())
                                            save = true;
                                        ImGui::EndDisabled();
                                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                            ImGui::SetTooltip("%s", addon ? Tr("Frame limit of this program on this screen (0 = no limit, up to 1000 fps; ReLimiter: at least 30). When this screen is the output screen, Nbeam writes it in the DisplayCommander.ini / relimiter.ini next to the game when Nbeam starts and each time the output screen changes, for every program at once, so it is ready before the game starts. A game without that file is ignored.")
                                                                          : Tr("Used when \"Auto fps cap\" is Display Commander or ReLimiter."));
                                    }
                                    rowLabel(Tr("Activation delay"), true);
                                    {
                                        int delay = link.delay;
                                        if(ImGui::DragInt("##delay", &delay, 0.2f, 0, 600, "%d s"))
                                            link.delay = std::clamp(delay, 0, 600);
                                        if(ImGui::IsItemDeactivatedAfterEdit())
                                            save = true;
                                        if(ImGui::IsItemHovered())
                                            ImGui::SetTooltip("%s", Tr("Activation delay: the program must have been running for this many seconds before the profile is applied."));
                                    }
                                    rowLabel(Tr("Refresh rate"), true);
                                    {
                                        // refresh rate of this screen while the program runs (every screen of the program changes when it is detected)
                                        char curHz[32];
                                        if(link.hz > 0)
                                            snprintf(curHz, sizeof(curHz), "%d Hz", link.hz);
                                        else
                                            snprintf(curHz, sizeof(curHz), "%s", Tr("Current Hz"));
                                        if(ImGui::BeginCombo("##hz", curHz, 0))
                                        {
                                            std::vector<int> rates;
                                            for(const auto& m : mons)
                                                if(m.id == link.screenId)
                                                    for(int r : m.rates)
                                                        if(r >= 79)
                                                            rates.push_back(r); // remembered rates, even when the screen is not connected (under 80 Hz: not offered)
                                            if(link.hz > 0 && std::find(rates.begin(), rates.end(), link.hz) == rates.end())
                                                rates.push_back(link.hz);
                                            std::sort(rates.begin(), rates.end());
                                            if(ImGui::Selectable(Tr("Current Hz"), link.hz == 0))
                                            {
                                                link.hz = 0;
                                                save    = true;
                                            }
                                            for(int hz : rates)
                                            {
                                                char t[32];
                                                snprintf(t, sizeof(t), "%d Hz", hz);
                                                if(ImGui::Selectable(t, hz == link.hz))
                                                {
                                                    link.hz = hz;
                                                    save    = true;
                                                }
                                            }
                                            ImGui::EndCombo();
                                        }
                                        if(ImGui::IsItemHovered())
                                            ImGui::SetTooltip("%s", Tr("Windows refresh rate of this screen while the program runs. \"Current Hz\" = no change. When the program is detected every screen of the program changes its rate, and the original rates come back when it closes."));
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndTable();
                            }
                            if(dropMember >= 0)
                                removeList.push_back(dropMember);
                            {
                                const bool canAdd = !s_procSel.empty() && (int)grp.size() < Options::MAX_LINKS_PER_EXE && !freeScreen(grp.back()).empty();
                                if(!canAdd)
                                    ImGui::BeginDisabled();
                                if(ImGui::Button(Tr("Add a screen")))
                                    addAfter = (int)grp.back();
                                if(!canAdd)
                                    ImGui::EndDisabled();
                                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("%s", Tr("Add another screen for this program (up to 3): it only changes its refresh rate while the program runs, the profile is the same for every screen. A screen has to have been connected once to be listed."));
                            }
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                // ---- tab "Global settings": a value for every program at once (each program can still be changed alone afterwards). A control
                // shows the shared value, or "(mixed)" when the programs differ ----
                if(profTabs && ImGui::BeginTabItem(Tr("Global settings")))
                {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", Tr("Changes every program at once."));
                    ImGui::PopTextWrapPos();
                    ShowHelpMarker(Tr("Changes every program at once: the check box, the delay, the profile (the same for every screen) and the refresh rate of each screen. You can still change each program on its own afterwards. \"(mixed)\" = the programs have different values."));
                    auto& links = m_options.profileLinks;
                    // "Profile when a program is added": None, or one of the profiles; given to every program added afterwards (HUD, shortcut, emitter button)
                    auto profileOnAddCombo = [&]() {
                        const std::string cur = m_options.ProfileForNewProgram();
                        if(ImGui::BeginCombo("##gprofileadd", cur.empty() ? Tr("None") : cur.c_str(), 0))
                        {
                            if(ImGui::Selectable(Tr("None"), cur.empty()))
                                m_options.profileOnAdd.clear(), save = true;
                            for(const auto& name : s_profiles)
                                if(ImGui::Selectable(name.c_str(), name == cur))
                                    m_options.profileOnAdd = name, save = true;
                            ImGui::EndCombo();
                        }
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", Tr("Profile given automatically to a program when it is added (by the HUD, the \"Add the program to the profiles\" shortcut or the button of the emitter). None = the program is added without a profile."));
                    };
                    if(links.empty())
                    {
                        ImGui::TextDisabled("%s", Tr("No program yet: write a name above or add one from the taskbar."));
                        if(ImGui::BeginTable("##globaladd", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn(Tr("Setting"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                            ImGui::TableSetupColumn(Tr("Current value"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(Tr("Profile when a program is added"));
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            profileOnAddCombo();
                            ImGui::EndTable();
                        }
                    }
                    else if(ImGui::BeginTable("##globaltable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::PushID("global");
                        ImGui::TableSetupColumn(Tr("Setting"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn(Tr("Current value"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
                        ImGui::TableHeadersRow();
                        auto rowLabel = [&](const char* label, bool indent = false) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            if(indent)
                                ImGui::Indent(m_fontSize);
                            ImGui::TextUnformatted(label);
                            if(indent)
                                ImGui::Unindent(m_fontSize);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                        };
                        // the programs (one head link per .exe)
                        std::vector<size_t> heads;
                        for(size_t k = 0; k < links.size(); k++)
                        {
                            bool first = true;
                            for(size_t h : heads)
                                if(exeKey(links[h].exe) == exeKey(links[k].exe))
                                    first = false;
                            if(first && !exeKey(links[k].exe).empty())
                                heads.push_back(k);
                        }
                        auto linkOn = [&](size_t head, const std::string& sid) -> int {
                            for(size_t k = 0; k < links.size(); k++)
                                if(links[k].screenId == sid && exeKey(links[k].exe) == exeKey(links[head].exe))
                                    return (int)k;
                            return -1;
                        };
                        auto countOf = [&](size_t head) {
                            int n = 0;
                            for(const auto& l : links)
                                if(exeKey(l.exe) == exeKey(links[head].exe))
                                    n++;
                            return n;
                        };
                        // one profile for every program, whatever the screen
                        rowLabel(Tr("Profile"));
                        {
                            std::string shared;
                            bool        same = true, firstSeen = true;
                            for(size_t h : heads)
                            {
                                if(firstSeen)
                                    shared = links[h].profile, firstSeen = false;
                                else if(links[h].profile != shared)
                                    same = false;
                            }
                            if(ImGui::BeginCombo("##gprofile", !same ? Tr("(mixed)") : (shared.empty() ? Tr("(no profile)") : shared.c_str()), 0))
                            {
                                auto apply = [&](const std::string& name) {
                                    for(auto& l : links)
                                        l.profile = name;
                                    save = true;
                                };
                                if(ImGui::Selectable(Tr("(no profile)"), same && shared.empty()))
                                    apply(std::string());
                                for(const auto& name : s_profiles)
                                    if(ImGui::Selectable(name.c_str(), same && name == shared))
                                        apply(name);
                                ImGui::EndCombo();
                            }
                            if(ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", Tr("Profile loaded for every program when it runs, whatever the screen. The other screens only change their refresh rate."));
                        }
                        rowLabel(Tr("Profile when a program is added"));
                        profileOnAddCombo();
                        // the refresh rate of each screen (3 at most): the places are those of the list of the screens (remembered, connected or not);
                        // each place offers every screen, and choosing one exchanges the two places
                        std::vector<const Options::KnownMonitor*> order;
                        for(const auto& m : mons)
                            order.push_back(&m);
                        if(order.size() > (size_t)Options::MAX_LINKS_PER_EXE)
                            order.resize((size_t)Options::MAX_LINKS_PER_EXE);
                        int         swapPlace = -1, swapWith = -1; // a place asked to take another screen
                        std::string removeScreen;                  // a screen not connected asked to be forgotten
                        for(size_t d = 0; d < order.size(); d++)
                        {
                            const Options::KnownMonitor* mon = order[d];
                            const std::string            sid = mon->id;
                            ImGui::PushID((int)d);
                            char screenLabel[48];
                            snprintf(screenLabel, sizeof(screenLabel), "%s %d", Tr("Screen"), (int)d + 1);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(screenLabel);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            if(ImGui::BeginCombo("##gsscreen", monLabel(*mon).c_str(), 0))
                            {
                                for(size_t k = 0; k < mons.size(); k++)
                                {
                                    ImGui::PushID((int)k);
                                    if(ImGui::Selectable(monLabel(mons[k]).c_str(), k == d))
                                    {
                                        swapPlace = (int)d;
                                        swapWith  = (int)k;
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndCombo();
                            }
                            if(ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", Tr("Every screen Nbeam knows, connected or not. Choosing one here puts it in this place and the screen that was here takes its old place."));
                            rowLabel(Tr("Screen to remove"), true);
                            {
                                ImGui::BeginDisabled(mon->connected);
                                if(ImGui::Button(Tr("Remove")))
                                    removeScreen = sid;
                                ImGui::EndDisabled();
                                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("%s", Tr("Removes this screen from Nbeam, with its detections (a program keeps its last one on another screen). Only a screen that is not connected can be removed; it comes back by itself if it is connected again."));
                            }
                            {
                                // the detections of every program on this screen
                                std::vector<int> ks;
                                for(size_t h : heads)
                                    if(const int k = linkOn(h, sid); k >= 0)
                                        ks.push_back(k);
                                ImGui::BeginDisabled(ks.empty());
                                rowLabel(Tr("Enabled"), true);
                                {
                                    bool allOn = !ks.empty(), anyOn = false;
                                    for(int k : ks)
                                    {
                                        allOn = allOn && links[k].enabled;
                                        anyOn = anyOn || links[k].enabled;
                                    }
                                    const bool mixed = anyOn && !allOn; // (some programs on, some off: the switch stays half way, a click turns them all on)
                                    if(ToggleSwitch("##gson", &allOn, mixed))
                                    {
                                        for(int k : ks)
                                            links[k].enabled = allOn;
                                        save = true;
                                    }
                                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                        ImGui::SetTooltip("%s", Tr("Checks / unchecks the detection of every program on this screen. Half way = some are on and some are off: a click turns them all on."));
                                }
                                rowLabel(Tr("Auto fps cap"), true);
                                {
                                    bool same = true;
                                    for(int k : ks)
                                        same = same && links[k].fpsCapMode == links[ks[0]].fpsCapMode;
                                    int gcap = !ks.empty() && same ? links[ks[0]].fpsCapMode : m_options.DefaultFpsCapMode();
                                    if(FpsCapCombo("##gscap", gcap, m_options.rtssAvailable, m_options.dcAvailable, !ks.empty() && !same))
                                    {
                                        for(int k : ks)
                                            links[k].fpsCapMode = gcap;
                                        m_options.fpsCapDefault = gcap; // (also the choice given to the programs added afterwards)
                                        save = true;
                                    }
                                }
                                rowLabel(Tr("Frame limit"), true);
                                {
                                    bool same = true;
                                    for(int k : ks)
                                        same = same && links[k].fpsLimit == links[ks[0]].fpsLimit;
                                    int glimit = !ks.empty() && same ? links[ks[0]].fpsLimit : m_options.fpsLimitDefault;
                                    if(ImGui::DragInt("##gslimit", &glimit, 0.5f, 0, 1000, !ks.empty() && !same ? Tr("(mixed)") : glimit == 0 ? Tr("No limit") : "%d fps"))
                                    {
                                        glimit = std::clamp(glimit, 0, 1000);
                                        for(int k : ks)
                                            links[k].fpsLimit = glimit;
                                        m_options.fpsLimitDefault = glimit; // (also the limit given to the programs added afterwards)
                                    }
                                    if(ImGui::IsItemDeactivatedAfterEdit())
                                        save = true;
                                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                        ImGui::SetTooltip("%s", Tr("Frame limit of every program on this screen whose \"Auto fps cap\" is Display Commander or ReLimiter (0 = no limit, up to 1000 fps; ReLimiter: at least 30). The limits of the output screen are written in the DisplayCommander.ini / relimiter.ini next to each game when Nbeam starts and each time the output screen changes, for every program at once. A game without that file is ignored."));
                                }
                                rowLabel(Tr("Activation delay"), true);
                                {
                                    bool same = true;
                                    for(int k : ks)
                                        same = same && links[k].delay == links[ks[0]].delay;
                                    int gdelay = ks.empty() ? 0 : links[ks[0]].delay;
                                    if(ImGui::DragInt("##gsdelay", &gdelay, 0.2f, 0, 600, same ? "%d s" : Tr("(mixed)")))
                                        for(int k : ks)
                                            links[k].delay = std::clamp(gdelay, 0, 600);
                                    if(ImGui::IsItemDeactivatedAfterEdit())
                                        save = true;
                                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                        ImGui::SetTooltip("%s", Tr("Activation delay of every program on this screen."));
                                }
                                ImGui::EndDisabled();
                            }
                            rowLabel(Tr("Refresh rate"), true);
                            {
                                int  hzShared = 0;
                                bool hzSame = true, hzFirst = true;
                                for(size_t h : heads)
                                {
                                    const int k  = linkOn(h, sid);
                                    const int hz = k >= 0 ? links[k].hz : 0;
                                    if(hzFirst)
                                        hzShared = hz, hzFirst = false;
                                    else if(hz != hzShared)
                                        hzSame = false;
                                }
                                char curHz[32];
                                if(!hzSame)
                                    snprintf(curHz, sizeof(curHz), "%s", Tr("(mixed)"));
                                else if(hzShared > 0)
                                    snprintf(curHz, sizeof(curHz), "%d Hz", hzShared);
                                else
                                    snprintf(curHz, sizeof(curHz), "%s", Tr("Current Hz"));
                                if(ImGui::BeginCombo("##gshz", curHz, 0))
                                {
                                    auto applyHz = [&](int hz) {
                                        std::vector<Options::ProfileLink> added;
                                        for(size_t h : heads)
                                        {
                                            const int k = linkOn(h, sid);
                                            if(k >= 0)
                                                links[k].hz = hz;
                                            else if(hz > 0 && countOf(h) < Options::MAX_LINKS_PER_EXE)
                                            {
                                                Options::ProfileLink nl = links[h]; // same program, profile, delay and check box: this screen only changes its rate
                                                nl.screenId             = sid;
                                                nl.hz                   = hz;
                                                added.push_back(nl);
                                            }
                                        }
                                        for(const auto& nl : added)
                                            if(links.size() < 64)
                                                links.push_back(nl);
                                        save = true;
                                    };
                                    if(ImGui::Selectable(Tr("Current Hz"), hzSame && hzShared == 0))
                                        applyHz(0);
                                    for(int hz : mon->rates)
                                    {
                                        if(hz < 79)
                                            continue; // under 80 Hz: not offered
                                        char t[32];
                                        snprintf(t, sizeof(t), "%d Hz", hz);
                                        if(ImGui::Selectable(t, hzSame && hz == hzShared))
                                            applyHz(hz);
                                    }
                                    ImGui::EndCombo();
                                }
                                if(ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s", Tr("Windows refresh rate of this screen while any of the programs runs."));
                            }
                            ImGui::PopID();
                        }
                        if(swapPlace >= 0 && swapWith >= 0 && swapPlace != swapWith)
                        {
                            m_options.SwapKnownMonitors(mons[(size_t)swapPlace].id, mons[(size_t)swapWith].id);
                            save = true;
                        }
                        if(!removeScreen.empty() && m_options.RemoveKnownMonitor(removeScreen))
                        {
                            // its detections go too, except the last one of a program: that one gets another screen (see the start of this tab)
                            for(size_t k = 0; k < links.size(); k++)
                            {
                                if(links[k].screenId != removeScreen)
                                    continue;
                                int others = 0;
                                for(size_t j = 0; j < links.size(); j++)
                                    if(j != k && exeKey(links[j].exe) == exeKey(links[k].exe) && links[j].screenId != removeScreen)
                                        others++;
                                if(others > 0)
                                    removeList.push_back((int)k);
                                else
                                    links[k].screenId.clear();
                            }
                            save = true;
                        }
                        ImGui::PopID();
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if(addAfter >= 0 && m_options.profileLinks.size() < 64)
                {
                    Options::ProfileLink nl = m_options.profileLinks[addAfter];
                    nl.profile.clear();
                    nl.screenId.clear();
                    nl.hz = 0;
                    m_options.profileLinks.insert(m_options.profileLinks.begin() + addAfter + 1, nl);
                    m_options.profileLinks[addAfter + 1].screenId = freeScreen((size_t)addAfter + 1);
                    save = true;
                }
                if(!removeList.empty())
                {
                    std::sort(removeList.begin(), removeList.end(), std::greater<int>());
                    removeList.erase(std::unique(removeList.begin(), removeList.end()), removeList.end());
                    for(int r : removeList)
                        if(r >= 0 && r < (int)m_options.profileLinks.size())
                            m_options.profileLinks.erase(m_options.profileLinks.begin() + r);
                    save = true;
                }
                if(save)
                    m_options.SaveApp();
            }
            if(profTabs)
                ImGui::EndTabBar();

            }
            if(s_cat == 7)
            {
            SectionHeader(Tr("Motion test"));
            {
                // "Profile to adjust", first line of the motion test: the field with the name of the profile (rewrite it to rename the profile)
                // and the arrow that picks the profile to adjust (the test opens with it loaded)
                if(s_profilesDirty)
                {
                    s_profiles      = Options::ListProfiles();
                    SortNoCase(s_profiles);
                    s_profilesDirty = false;
                }
                const bool programRuns = !m_options.captureProcess.empty() && !m_options.motionTest;
                const bool adjusting   = m_options.motionTest && !m_options.IsDesktopSettings();
                if(!programRuns)
                {
                    ImGui::PushItemWidth(m_fontSize * 14);
                    // "Profile to adjust": a field with the name of the profile (rewrite it to rename the profile), the arrow picks the
                    // profile to adjust (the motion test opens with it)
                    static std::string s_profSel;            // the profile shown in the field
                    static char        s_profBuf[128] = "";
                    static std::string s_profBufFor  = "\x01";
                    static std::string s_profNote;           // a refused rename (name taken, invalid...)
                    if(adjusting)
                        s_profSel = m_options.profileFile;
                    if(!s_profSel.empty() && std::find(s_profiles.begin(), s_profiles.end(), s_profSel) == s_profiles.end() && !adjusting)
                        s_profSel.clear(); // (deleted or renamed outside Nbeam)
                    if(s_profBufFor != s_profSel)
                    {
                        std::string shown = s_profSel;
                        if(shown.size() > 4 && _stricmp(shown.c_str() + shown.size() - 4, ".ini") == 0)
                            shown.resize(shown.size() - 4); // the name without .ini
                        strncpy_s(s_profBuf, shown.c_str(), sizeof(s_profBuf) - 1);
                        s_profBufFor = s_profSel;
                    }
                    ImGui::SetNextItemWidth(m_fontSize * 14 - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
                    if(s_profSel.empty())
                        ImGui::BeginDisabled();
                    ImGui::InputTextWithHint("##profname", Tr("(choose)"), s_profBuf, sizeof(s_profBuf));
                    if(ImGui::IsItemDeactivatedAfterEdit() && !s_profSel.empty())
                    {
                        // rename: Profils\old.ini -> Profils\new.ini, the programs and the active settings follow
                        std::string name = s_profBuf;
                        for(char& c : name)
                            if(strchr("\\/:*?\"<>|", c))
                                c = '_';
                        while(!name.empty() && (name.back() == ' ' || name.back() == '.'))
                            name.pop_back();
                        if(!name.empty() && (name.size() < 4 || _stricmp(name.c_str() + name.size() - 4, ".ini") != 0))
                            name += ".ini";
                        s_profNote.clear();
                        if(name.empty() || _stricmp(name.c_str(), "imgui.ini") == 0)
                            s_profNote = Tr("Invalid name.");
                        else if(_stricmp(name.c_str(), s_profSel.c_str()) != 0 || name != s_profSel)
                        {
                            const auto from = Options::ProfilesDir() / s_profSel;
                            const auto to   = Options::ProfilesDir() / name;
                            if(_stricmp(name.c_str(), s_profSel.c_str()) != 0 && std::filesystem::exists(to))
                                s_profNote = Tr("A profile with this name already exists.");
                            else if(!MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), 0))
                                s_profNote = Tr("The profile could not be renamed.");
                            else
                            {
                                {
                                    std::lock_guard<std::recursive_mutex> renameLock(m_options.linksMutex);
                                    for(auto& l : m_options.profileLinks)
                                        if(_stricmp(l.profile.c_str(), s_profSel.c_str()) == 0)
                                            l.profile = name;
                                }
                                if(_stricmp(m_options.profileFile.c_str(), s_profSel.c_str()) == 0)
                                    m_options.profileFile = name;
                                m_options.SaveApp();
                                s_profSel       = name;
                                s_profilesDirty = true;
                            }
                        }
                        s_profBufFor = "\x01"; // the field shows the real name again
                    }
                    if(s_profSel.empty())
                        ImGui::EndDisabled();
                    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                    if(ImGui::ArrowButton("##profarrow", ImGuiDir_Down))
                    {
                        s_profiles = Options::ListProfiles(); // the list is read again each time it opens
                        SortNoCase(s_profiles);
                        ImGui::OpenPopup("##proflist");
                    }
                    if(ImGui::BeginPopup("##proflist"))
                    {
                        if(s_profiles.empty())
                            ImGui::TextDisabled("%s", Tr("(none)"));
                        for(const auto& name : s_profiles)
                        {
                            const bool selected = adjusting && name == m_options.profileFile;
                            if(ImGui::Selectable(name.c_str(), selected) && !selected)
                            {
                                s_profSel             = name;
                                s_profNote.clear();
                                m_options.testProfile = name;
                                PostMessage(m_options.outputWindow, WM_USER_GLASSESTEST, 1, 0); // the motion test opens with it
                            }
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::TextUnformatted(Tr("Profile to adjust"));
                    ShowHelpMarker(Tr("Profiles only apply while their program runs (see the program list below); otherwise the desktop settings are used.\nThe arrow picks a profile to adjust: the motion test opens with it loaded. Change the settings and watch the result. Ending the test (Windows key or Escape) saves the profile and goes back to the desktop settings.\nRewrite the name in the field to rename the profile."));
                    if(!s_profNote.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", s_profNote.c_str());
                    ImGui::PopItemWidth();
                }
            }
            {
                // the test itself is started / stopped with the "Motion test" button of the bottom bar
                ImGui::PushItemWidth(m_fontSize * 16);
                int speed = m_options.ufoSpeed;
                if(SliderIntStep(Tr("Car speed"), &speed, 0, 3840, 120))
                    m_options.ufoSpeed = std::clamp(speed, 0, 7680);
                ShowHelpMarker(Tr("Speed of the cars in pixels per second, 0 to 3840 (0 = still, 960 = the usual motion test speed). Faster shows the motion blur more clearly."));
                int size = m_options.carSize;
                if(SliderIntStep(Tr("Car size"), &size, 25, 200, 5, nullptr))
                    m_options.carSize = std::clamp(size, 25, 200);
                ShowHelpMarker(Tr("Size of the cars, 25 to 200 %. Careful: smaller = more cars on the screen."));
                ImGui::ColorEdit3(Tr("Background colour"), m_options.testBg, ImGuiColorEditFlags_NoInputs);
                ShowHelpMarker(Tr("Colour of the cells behind the cars (the grid between them stays black)."));
                ImGui::PopItemWidth();
                // saved (Nbeam.app.cfg) once the controls are released
                static int   s_saved[2] = { -1, -1 };
                static float s_savedBg[3] = { -1, -1, -1 };
                const bool   changed = s_saved[0] != m_options.ufoSpeed || s_saved[1] != m_options.carSize || memcmp(s_savedBg, m_options.testBg, sizeof(s_savedBg)) != 0;
                if(changed && !ImGui::IsAnyItemActive())
                {
                    const bool first = s_saved[0] < 0;
                    s_saved[0] = m_options.ufoSpeed;
                    s_saved[1] = m_options.carSize;
                    memcpy(s_savedBg, m_options.testBg, sizeof(s_savedBg));
                    if(!first)
                        m_options.SaveApp();
                }
            }
            SectionHeader(Tr("Comparison"));
            if(ImGui::BeginCombo(Tr("Split-screen"), Tr(m_splitScreens[m_pending.splitScreen].c_str()), 0))
            {
                for(auto i = 0; i < m_splitScreens.size(); i++)
                {
                    auto selected = i == m_pending.splitScreen;
                    if(ImGui::Selectable(Tr(m_splitScreens[i].c_str()), selected))
                    {
                        m_pending.splitScreen = i;
                        SetApplyRequired();
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ShowHelpMarker(Tr("Split-screen mode for before/after comparison."));
            ToggleSwitch(Tr("Invert"), &m_options.splitInvert);
            ShowHelpMarker(Tr("Puts the effect on the other half of the screen (right or bottom instead of left or top)."));
            }
            if(s_cat == 6)
            {
            {
                static const char* langNames[] = { "Fran\xc3\xa7" "ais", "English" };
                int                lang        = GetLanguage();
                ImGui::PushItemWidth(m_fontSize * 9);
                if(ImGui::BeginCombo("Language / Langue", langNames[lang], 0))
                {
                    for(int i = 0; i < 2; i++)
                    {
                        if(ImGui::Selectable(langNames[i], i == lang) && i != lang)
                        {
                            SetLanguage(i);
                            Options::SaveLanguage(i);
                            RefreshHotkeyTexts();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
            }

            SectionHeader(Tr("Application"));
            {
                if(ToggleSwitch(Tr("Start with Windows"), &m_options.startWithWindows))
                {
                    m_options.ApplyAutoStart();
                    m_options.SaveApp();
                }
                ShowHelpMarker(Tr("Nbeam starts when you sign in to Windows."));
                if(ToggleSwitch(Tr("Start minimized"), &m_options.startMinimized))
                    m_options.SaveApp();
                ShowHelpMarker(Tr("Nbeam starts without showing the HUD (reduced to the notification area if that option is on)."));
                if(ToggleSwitch(Tr("Keep Dolby Atmos active"), &m_options.keepAudio))
                {
                    AudioKeepAliveSet(m_options.keepAudio);
                    m_options.SaveApp();
                }
                ShowHelpMarker(Tr("Nbeam plays a sound nobody can hear (about -90 dB, on the default Windows audio device) for as long as it runs, so Windows keeps the spatial audio (Dolby Atmos) active instead of falling back to the stereo mix."));
                if(ToggleSwitch(Tr("Run as administrator"), &m_options.runAsAdmin))
                    m_options.SaveApp();
                ShowHelpMarker(Tr("At the next start Nbeam asks Windows for administrator rights (a confirmation window appears). Useful for games or programs that run as administrator, which a normal Nbeam cannot capture or stay above."));
                if(ToggleSwitch(Tr("Minimize instead of closing"), &m_options.closeMinimizes))
                    m_options.SaveApp();
                ShowHelpMarker(Tr("The cross of the window minimizes it (to the notification area if that option is on: the profile detections keep working) instead of quitting Nbeam."));
                if(ToggleSwitch(Tr("Save the event log"), &m_options.saveLog))
                {
                    g_logEnabled = m_options.saveLog;
                    if(!m_options.saveLog)
                        DeleteEventLogs();
                    m_options.SaveApp();
                }
                ShowHelpMarker(Tr("Nbeam writes its events (Nbeam_ls.log), the capture of the O key (Nbeam_desync.log), the crash report and the log of the emitter reader in the \"log\" folder next to the program. Untick to delete all of them and record nothing any more: no event and no history of Nbeam is kept."));
            }

            }
            if(s_cat == 5)
            {
            SectionHeader(Tr("Appearance"));
            {
                ImGui::PushItemWidth(m_fontSize * 14);
                bool  changed = false;
                {
                    // HUD size: applied when the slider is released (the layout follows the size, so it must not move under the cursor)
                    static float s_sizePct = 100.0f;
                    static bool  s_sizeEdit = false;
                    if(!s_sizeEdit)
                        s_sizePct = HudScale() * 100.0f;
                    if(ToggleSwitch(Tr("Auto HUD size"), &m_options.hud.autoScale))
                        changed = true;
                    ShowHelpMarker(Tr("The size of the interface follows the Windows display scale of the screen it is on: 100 % in Windows gives the normal size, 150 % makes it 1.5 times bigger. Untick to choose the size yourself."));
                    if(!m_options.hud.autoScale) // (hidden while the size follows the Windows scale)
                    {
                        if(ImGui::SliderFloat(Tr("HUD Size"), &s_sizePct, 60.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
                            s_sizeEdit = true;
                        if(ImGui::IsItemDeactivatedAfterEdit())
                        {
                            HudScale() = std::clamp(s_sizePct / 100.0f, 0.6f, 2.0f); // the size of the mode in use (window or HUD over the screen)
                            s_sizeEdit = false;
                            changed    = true;
                        }
                        ShowHelpMarker(Tr("Size of the whole HUD, 60 to 200 %. Dragging the edges of the HUD stretches it."));
                    }
                }
                float col[3] = { m_options.hud.r, m_options.hud.g, m_options.hud.b };
                if(ImGui::ColorEdit3(Tr("Accent Color"), col, ImGuiColorEditFlags_NoInputs))
                {
                    m_options.hud.r = col[0];
                    m_options.hud.g = col[1];
                    m_options.hud.b = col[2];
                    changed         = true;
                }
                ShowHelpMarker(Tr("Main color of the HUD (default: NVIDIA green)."));
                changed |= SliderFloatStep(Tr("Contrast"), &m_options.hud.contrast, 0.5f, 1.8f, "%.2f", 0.01f);
                ShowHelpMarker(Tr("Higher = darker darks and brighter lights."));
                float pct = m_options.hud.transparency * 100.0f;
                if(SliderFloatStep(Tr("Transparency"), &pct, 0.0f, 80.0f, "%.0f %%", 1.0f))
                {
                    m_options.hud.transparency = pct / 100.0f;
                    changed                    = true;
                }
                ShowHelpMarker(Tr("How see-through the HUD background is. The window can also be resized and moved with the mouse."));
                if(WindowsHdrOn(m_options.outputWindow))
                {
                    changed |= SliderFloatStep(Tr("Black level (HDR)"), &m_options.hud.hdrBlackNits, 0.0f, 20.0f, "%.1f nits", 0.1f);
                    ShowHelpMarker(Tr("Captured image, Windows HDR only: values darker than this level become pure black (0 = unchanged)."));
                    changed |= SliderFloatStep(Tr("Grey level (HDR)"), &m_options.hud.hdrGrey, 0.5f, 2.0f, "%.2f", 0.01f);
                    ShowHelpMarker(Tr("Captured image, Windows HDR only: mid-tone curve below white. Above 1 = darker greys, below 1 = lighter greys (1 = unchanged). Black and highlights stay put."));
                    changed |= SliderFloatStep(Tr("Nits (HDR)"), &m_options.hud.hdrNits, -1000.0f, 1000.0f, "%+.0f nits", 1.0f);
                    ShowHelpMarker(Tr("Captured image only, Windows HDR only: brightness of the picture, -1000 to +1000 nits (0 = unchanged, -1000 = black screen). The HUD is not affected."));
                    UpdateCaptureTone();
                }
                else
                {
                    changed |= SliderFloatStep(Tr("HUD brightness (SDR)"), &m_options.hud.sdrBrightness, 20.0f, 100.0f, "%.0f %%", 1.0f);
                    ShowHelpMarker(Tr("Brightness of the HUD while Windows HDR is off, 20 to 100 % (100 = full brightness)."));
                }
                {
                    int sec = m_options.hud.bannerSeconds;
                    if(ImGui::SliderInt(Tr("3D banner duration"), &sec, 0, 11, sec > 10 ? Tr("Always") : "%d s", ImGuiSliderFlags_AlwaysClamp))
                    {
                        m_options.hud.bannerSeconds = sec;
                        m_appearanceDirty = true;
                        ShowBanner();
                    }
                    ShowHelpMarker(Tr("How long the 3D banner stays on screen, 0 to 10 seconds (0 = never). Past 10: always shown while 3D is active."));
                    const char* corners[] = { Tr("Bottom left"), Tr("Bottom right"), Tr("Top right"), Tr("Top left") };
                    int         corner    = std::clamp(m_options.hud.bannerCorner, 0, 3);
                    if(ImGui::Combo(Tr("3D banner position"), &corner, corners, 4))
                    {
                        m_options.hud.bannerCorner = corner;
                        m_appearanceDirty = true;
                        ShowBanner();
                    }
                    if(ToggleSwitch(Tr("Show stats"), &m_options.hud.showStats))
                        m_appearanceDirty = true;
                    ShowHelpMarker(Tr("Adds to the Nbeam banner (the banner then stays on while an effect or 3D is active): the RAM Nbeam uses, the render latency (one game picture, with the frame buffer added in brackets when it is used) and the late frames per minute (the picture was shown for more or fewer refreshes than planned)."));
                }
                if(ImGui::Button(Tr("Reset appearance")))
                {
                    const float keepScale = m_options.hud.scale, keepWin = m_options.hud.scaleWin;
                    m_options.hud          = Options::Hud();
                    m_options.hud.scale    = keepScale;
                    m_options.hud.scaleWin = keepWin;
                    changed       = true;
                }
                ImGui::PopItemWidth();
                if(changed)
                    SetStyle(ImGui::GetStyle());
                if(changed || ImGui::IsItemDeactivatedAfterEdit())
                    m_appearanceDirty = true;
                if(m_appearanceDirty && !ImGui::IsAnyItemActive())
                {
                    m_options.SaveApp();
                    m_appearanceDirty = false;
                }
            }

            }
            if(s_cat == 4)
            {
            SectionHeader(Tr("Hotkeys"));
            RenderHotkeys();
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();

            // ---- action bar ----
            ImGui::SetCursorPos(ImVec2(0, headerH + bodyH));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.2f, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
            ImGui::BeginChild("##footer", ImVec2(0, footerH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            {
                const ImGuiStyle& st        = ImGui::GetStyle();
                // window mode: the third button starts the effects (in the window) or stops them (in the HUD over the screen)
                const char*       labels[5] = { Tr("About"), Tr("Benchmark"), m_options.motionTest ? Tr("Stop test") : Tr("Motion test"),
                                                m_options.windowMode ? (m_windowMode ? Tr("Activate effects") : Tr("Deactivate effects")) : (m_options.effectsSuspended ? Tr("Show") : Tr("Hide")),
                                                Tr("Restart") };
                const float       btnH      = ImGui::GetFontSize() + st.FramePadding.y * 2;
                const float       y         = (footerH - btnH) * 0.5f;
                {
                    const ImVec2 a = ImGui::GetWindowPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + ImGui::GetWindowWidth(), a.y + 2.0f), ImGui::GetColorU32(accentDim));
                }
                // right: actions
                // the motion test button only exists on the desktop settings (hidden while a detected program runs)
                const bool showTest = m_options.motionTest || m_options.captureProcess.empty();
                float      total    = st.ItemSpacing.x * (showTest ? 4 : 3);
                for(int k = 0; k < 5; k++)
                    if(k != 2 || showTest)
                        total += ImGui::CalcTextSize(labels[k]).x + st.FramePadding.x * 2;
                const float x = ImGui::GetWindowWidth() - fs * 1.2f - total;
                // left: the render FPS (rendered / content / captured), red when something is not keeping up
                {
                    struct FpsCell { const char* label; double value; bool bad; const char* help; };
                    const FpsCell fc[3] = {
                        { Tr("Captured FPS"), m_inputFPS, false,
                          Tr("Provided by capture API.\nCan vary, ideally should be same as Content FPS (if it's higher, frame-limit your game to Content FPS using RTSS).") },
                        { Tr("Rendered FPS"), m_outputFPS, m_outputFPS < m_options.vsyncRate * 0.95f,
                          Tr("Frames sent for presentation. Should be same as Display Hz - if it isn't, your Shader GPU isn't keeping up.") },
                        { Tr("Content FPS"), m_options.vsyncRate / (std::max)(m_options.subFrames, 1), false,
                          Tr("Frame-limit your game to this (or slightly lower) value, including decimals, using RTSS.") }
                    };
                    ImDrawList*  fdl   = ImGui::GetWindowDrawList();
                    ImFont*      ffont = ImGui::GetFont();
                    const ImVec2 wp    = ImGui::GetWindowPos();
                    float        fx    = fs * 1.2f;
                    const float  limit = (std::max)(x, fs * 14.0f) - fs * 0.8f;
                    for(int k = 0; k < 3; k++)
                    {
                        char val[32];
                        snprintf(val, sizeof(val), "%.2f", fc[k].value);
                        const float lw = ffont->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.0f, fc[k].label).x;
                        const float vw = ffont->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0.0f, val).x;
                        const float cw = (std::max)(lw, vw);
                        if(fx + cw > limit)
                            break;
                        fdl->AddText(ffont, fs * 0.75f, ImVec2(wp.x + fx, wp.y + footerH * 0.5f - fs * 1.05f), IM_COL32(170, 176, 170, 255), fc[k].label);
                        fdl->AddText(ffont, fs * 1.15f, ImVec2(wp.x + fx, wp.y + footerH * 0.5f - fs * 0.15f), fc[k].bad ? IM_COL32(255, 90, 70, 255) : IM_COL32(245, 247, 245, 255), val);
                        ImGui::SetCursorPos(ImVec2(fx, footerH * 0.5f - fs * 1.05f));
                        ImGui::PushID(k);
                        ImGui::InvisibleButton("##fps", ImVec2(cw, fs * 2.1f));
                        ImGui::PopID();
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", fc[k].help);
                        fx += cw + fs * 1.6f;
                    }
                    if(!m_options.emitterPresent)
                    {
                        // between Content FPS and About: the 3D emitter is not connected (checked every 2 s)
                        const char* note = Tr("3D emitter not connected");
                        float       sz   = fs * 1.0f;
                        float       nw   = ffont->CalcTextSizeA(sz, FLT_MAX, 0.0f, note).x;
                        if(fx + nw > limit)
                        {
                            sz = fs * 0.8f;
                            nw = ffont->CalcTextSizeA(sz, FLT_MAX, 0.0f, note).x;
                        }
                        if(fx + nw <= limit)
                            fdl->AddText(ffont, sz, ImVec2(wp.x + fx, wp.y + (footerH - sz) * 0.5f), IM_COL32(255, 70, 60, 255), note);
                    }
                }
                ImGui::SetCursorPos(ImVec2((std::max)(x, fs * 14.0f), y));
                if(ImGui::Button(labels[0]))
                    ImGui::OpenPopup("About");
                ImGui::SameLine();
                if(m_windowMode)
                    ImGui::BeginDisabled(); // the benchmark needs the effects running
                if(ImGui::Button(labels[1]))
                    PostMessage(m_options.outputWindow, WM_USER_BENCHMARK, 0, 0);
                if(m_windowMode)
                    ImGui::EndDisabled();
                ImGui::SameLine();
                if(showTest)
                {
                    // motion test: start / stop (desktop settings only)
                    if(ImGui::Button(labels[2]))
                    {
                        const bool on = !m_options.motionTest;
                        m_options.testProfile.clear(); // (the active settings)
                        PostMessage(m_options.outputWindow, WM_USER_GLASSESTEST, on ? 1 : 0, 0);
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Starts the motion test: detailed cars scroll over the whole screen through all your effects, to compare the motion blur. The Windows key or Escape also stops it."));
                    ImGui::SameLine();
                }
                if(m_options.windowMode)
                {
                    if(ImGui::Button(labels[3]))
                        PostMessage(m_options.outputWindow, WM_USER_EFFECTS, m_windowMode ? 1 : 0, 0);
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", m_windowMode ? Tr("Starts the effects and the 3D on the screen (this window is hidden while they run). A detected program starts them by itself.")
                                                             : Tr("Stops the effects and the 3D and brings the window back."));
                }
                else
                {
                    if(ImGui::Button(labels[3]))
                        m_options.effectsSuspended = !m_options.effectsSuspended; // all effects and the emitter off / on (see Renderer::EffectsOff, EffectsHidden)
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Hide: turns off all the effects and the 3D emitter (the picture is shown as it is). They stay off, even when the HUD is closed, until Show is pressed."));
                }
                ImGui::SameLine();
                if(ImGui::Button(labels[4]))
                    PostMessage(m_options.outputWindow, WM_USER_RESTART, NB_RESTART_TAG, 0);

                ImVec2 popupSize = ImVec2((std::min)(m_fontSize * 34, viewport.x * 0.95f), (std::min)(m_fontSize * 34, viewport.y * 0.95f));
                ImGui::SetNextWindowSize(popupSize, ImGuiCond_Always);
                ImGui::SetNextWindowPos(ImVec2((viewport.x - popupSize.x) / 2, (viewport.y - popupSize.y) / 2), ImGuiCond_Always);
                if(ImGui::BeginPopup("About"))
                {
                    const bool fr = GetLanguage() == LANG_FR;
                    // red close cross, top right
                    {
                        const ImVec2 back = ImGui::GetCursorPos();
                        const float  sz   = ImGui::GetFrameHeight();
                        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - sz, ImGui::GetStyle().WindowPadding.y));
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.15f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.25f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.10f, 1.0f));
                        if(ImGui::Button("X##aboutclose", ImVec2(sz, sz)))
                            ImGui::CloseCurrentPopup();
                        ImGui::PopStyleColor(3);
                        ImGui::SetCursorPos(back);
                    }
                    // credits, each line centred
                    const std::string about = fr ? NBEAM_ABOUT_FR : NBEAM_ABOUT;
                    size_t start = 0;
                    while(start <= about.size())
                    {
                        size_t end = about.find('\n', start);
                        if(end == std::string::npos)
                            end = about.size();
                        const std::string line = about.substr(start, end - start);
                        const float w = ImGui::CalcTextSize(line.c_str()).x;
                        ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x, (ImGui::GetWindowWidth() - w) * 0.5f));
                        ImGui::TextUnformatted(line.empty() ? " " : line.c_str());
                        start = end + 1;
                    }
                    // the release notes, to read what is new after an update
                    SectionHeader(Tr("Release notes"));
                    const float closeH = ImGui::GetFrameHeightWithSpacing();
                    if(ImGui::BeginChild("##relnotes", ImVec2(0, -closeH), ImGuiChildFlags_Borders))
                    {
                        const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                        ImGui::PushTextWrapPos(0.0f);
                        for(const char* const* n = fr ? RELEASE_NOTES_FR : RELEASE_NOTES_EN; *n; ++n)
                        {
                            const char  kind = (*n)[0];
                            const char* text = *n + 1;
                            switch(kind)
                            {
                            case 'T':
                                ImGui::PushFont(m_largeFont, m_fontSize * 1.3f);
                                ImGui::TextColored(accent, "%s", text);
                                ImGui::PopFont();
                                break;
                            case 'H':
                                ImGui::Spacing();
                                ImGui::TextColored(accent, "%s", text);
                                ImGui::Separator();
                                break;
                            case 'S':
                                ImGui::Spacing();
                                ImGui::TextUnformatted(text);
                                break;
                            case 'B':
                                ImGui::Bullet();
                                ImGui::TextWrapped("%s", text);
                                break;
                            case 'N':
                                ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", text);
                                break;
                            default:
                                ImGui::TextWrapped("%s", text);
                                break;
                            }
                        }
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndChild();
                    if(ImGui::Button(Tr("New version")))
                        ShellExecuteW(nullptr, L"open", L"https://github.com/anto1444/Nbeam3DVision/releases", nullptr, nullptr, SW_SHOWNORMAL);
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();


            // Apply automatically: as soon as a setting that needs a restart has changed and you are no longer
            // dragging / typing in a field (so a slider drag or a typed number applies once, when you let go).
            if((m_applyRequired || m_emitterRestartRequired || m_liveReloadRequired) && !ImGui::IsAnyItemActive())
            {
                if(m_applyRequired)
                {
                    // display, GPU, capture, HDR, sRGB or swap chain buffers: the renderer has to restart
                    ApplyPendingChanges();
                    m_applyRequired = false;
                    PostMessage(m_options.outputWindow, WM_USER_RESTART, NB_RESTART_TAG, 0);
                }
                else
                {
                    // shader / 3D source / emitters: rebuilt by the render thread without a restart
                    if(m_liveReloadRequired || (m_emitterRestartRequired && m_options.emitter.sourceFormat != m_lastSource))
                        m_options.liveReload = true;
                    else if(m_emitterRestartRequired)
                        m_options.liveEmitters = true; // same 3D source: only the emitters restart
                }
                m_lastSource             = m_options.emitter.sourceFormat;
                m_emitterRestartRequired = false;
                m_liveReloadRequired     = false;
            }

            ////////////////////////////////////////////////////

            if(m_hasBenchmark)
            {
                m_hasBenchmark = false;
                ImGui::OpenPopup(Tr("Benchmark Result"));
            }
            ImGui::SetNextWindowSize(ImVec2(16 * m_fontSize, 12 * m_fontSize), ImGuiCond_Always);
            if(ImGui::BeginPopupModal(Tr("Benchmark Result")))
            {
                if(m_benchmarkResult.copyFPS != 0)
                    ImGui::Text(Tr("      Copy FPS: %8.0f\n"), m_benchmarkResult.copyFPS);
                ImGui::Text(Tr("    Shader FPS: %8.0f\n"), m_benchmarkResult.renderFPS);
                ImGui::Text(Tr("   Present FPS: %8.0f\n"), m_benchmarkResult.presentFPS);
                ImGui::Text(" -------------------------\n");
                ImGui::Text(Tr("   Overall FPS: %8.0f\n\n"), m_benchmarkResult.totalFPS);

                if(ImGui::Button(Tr("Close")))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if(m_hasError)
            {
                m_hasError = false;
                ImGui::OpenPopup(Tr("Error"));
            }
            if(ImGui::BeginPopupModal(Tr("Error")))
            {
                ImGui::Text(Tr("FATAL ERROR"));
                ImGui::Text(m_errorMessage.c_str());
                if(ImGui::Button(Tr("Close")))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    }
    if(BannerActive())
    {
        RenderBanner();
        RenderMenu();
    }
    // a shortcut capture started in the Hotkeys section, then another tab was opened: cancelled (the hotkeys work again)
    if(s_captureId >= 0 && s_hotkeysDrawnFrame != ImGui::GetFrameCount())
        CancelCaptureNow(m_options);
    {
        // a control was released after a change (slider, checkbox, list...): the active profile is saved (by the main thread)
        // (any control: the +/- step buttons of the sliders change a value without marking it as edited, so every release counts)
        static ImGuiID s_prevActive = 0;
        ImGuiContext&  g            = *ImGui::GetCurrentContext();
        if(s_prevActive != 0 && g.ActiveId != s_prevActive)
            RequestSave();
        s_prevActive = g.ActiveId;
    }
    ImGui::Render();
    // HDR output: the HUD stays an SDR image (sRGB colors converted to scRGB at the HUD brightness chosen in Appearance)
    // Windows HDR on: scRGB, 1.0 = 80 nits. Windows HDR off: the HUD is dimmed relative to full brightness (sdrBrightness / 100).
    const float sdrMul = (std::max)(m_options.hud.sdrBrightness / 100.0f, 0.001f);
    if(m_windowMode)
        ImGui_ImplDX11_SetHdrScale(0.0f, 0.0f); // the window is a plain SDR picture: colours as they are
    else if(WindowsHdrOn(m_options.outputWindow))
        ImGui_ImplDX11_SetHdrScale(m_options.useHdr ? (std::max)(s_sdrWhiteScale, 0.001f) : 0.0f, 0.0f);
    else
        ImGui_ImplDX11_SetHdrScale(m_options.useHdr ? sdrMul : 0.0f, m_options.useHdr ? 0.0f : sdrMul);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// After a lost graphics card the ImGui textures can already be invalid: releasing them must not take Nbeam down (it restarts anyway)
static void ImGuiBackendShutdownGuarded()
{
    __try
    {
        ImGui_ImplDX11_Shutdown();
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void UI::Stop()
{
    m_ready = false;

    m_logo   = nullptr;
    m_logoSB = nullptr;
    s_iconNbeam = nullptr;
    s_iconSB    = nullptr;
    // Quit can arrive while the renderer is being restarted: ImGui may already be shut down, then there is nothing to release
    if(ImGui::GetCurrentContext())
    {
        if(ImGui::GetIO().BackendRendererUserData)
            ImGuiBackendShutdownGuarded();
        if(ImGui::GetIO().BackendPlatformUserData)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    m_smallFont   = nullptr;
    m_largeFont   = nullptr;
    m_captureGPUs = nullptr;
    m_renderGPUs  = nullptr;
}

bool UI::Toggle()
{
    m_options.ui = !m_options.ui;
    if(m_options.ui)
        CloseMenu(); // the HUD and the mini menu are never shown together
    else
        CancelHotkeyCapture(); // a shortcut capture left open would keep every hotkey off (even the one that shows the HUD)
    return m_options.ui;
}

void UI::CancelHotkeyCapture()
{
    std::unique_lock<std::timed_mutex> lock(m_imguiMutex, std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(200)))
        return;
    CancelCaptureNow(m_options);
}

void UI::SetStyle(ImGuiStyle& style)
{
    style.WindowBorderHoverPadding = 8.0f; // grab zone of the HUD edges (stretching), a bit larger than the default 4
    // accent color (NVIDIA green #76B900 by default) on near-black
    const auto&  h      = m_options.hud;
    auto         mul    = [](float v, float k) { return (std::min)(v * k, 1.0f); };
    const ImVec4 g      = ImVec4(h.r, h.g, h.b, 1.00f);
    const ImVec4 gHi    = ImVec4((std::min)(mul(h.r, 1.25f) + 0.03f, 1.0f), (std::min)(mul(h.g, 1.25f) + 0.03f, 1.0f), (std::min)(mul(h.b, 1.25f) + 0.03f, 1.0f), 1.00f); // brighter (hover / active)
    const ImVec4 gDim   = ImVec4(h.r * 0.5f, h.g * 0.5f, h.b * 0.5f, 1.00f);
    const ImVec4 gDark  = ImVec4(h.r * 0.19f, h.g * 0.19f, h.b * 0.19f, 1.00f);
    auto         alpha  = [](ImVec4 c, float a) { c.w = a; return c; };
    const float  winA   = 1.0f - h.transparency;

    style.Colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.96f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.68f, 0.71f, 0.66f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.035f + h.r * 0.03f, 0.035f + h.g * 0.03f, 0.035f + h.b * 0.03f, winA);
    style.Colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_PopupBg]               = ImVec4(0.04f + h.r * 0.06f, 0.04f + h.g * 0.06f, 0.04f + h.b * 0.06f, 1.0f - h.transparency * 0.5f);
    style.Colors[ImGuiCol_Border]                = g;
    style.Colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_FrameBg]               = alpha(gDim, 0.55f);
    style.Colors[ImGuiCol_FrameBgHovered]        = alpha(g, 0.45f);
    style.Colors[ImGuiCol_FrameBgActive]         = alpha(gHi, 0.60f);
    style.Colors[ImGuiCol_TitleBg]               = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]         = gDim;
    style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_MenuBarBg]             = gDark;
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrab]         = gDim;
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = g;
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = gHi;
    style.Colors[ImGuiCol_CheckMark]             = gHi;
    style.Colors[ImGuiCol_SliderGrab]            = g;
    style.Colors[ImGuiCol_SliderGrabActive]      = gHi;
    style.Colors[ImGuiCol_Button]                = gDim;
    style.Colors[ImGuiCol_ButtonHovered]         = alpha(g, 0.85f);
    style.Colors[ImGuiCol_ButtonActive]          = gHi;
    style.Colors[ImGuiCol_Header]                = alpha(g, 0.30f);
    style.Colors[ImGuiCol_HeaderHovered]         = alpha(g, 0.70f);
    style.Colors[ImGuiCol_HeaderActive]          = g;
    // tabs follow the accent color of the HUD
    style.Colors[ImGuiCol_Tab]                   = alpha(gDim, 0.80f);
    style.Colors[ImGuiCol_TabHovered]            = alpha(gHi, 0.90f);
    style.Colors[ImGuiCol_TabSelected]           = g;
    style.Colors[ImGuiCol_TabSelectedOverline]   = gHi;
    style.Colors[ImGuiCol_TabDimmed]             = alpha(gDim, 0.55f);
    style.Colors[ImGuiCol_TabDimmedSelected]     = alpha(g, 0.75f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = gHi;
    style.Colors[ImGuiCol_SeparatorHovered]      = alpha(g, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive]       = gHi;
    style.Colors[ImGuiCol_ResizeGrip]            = alpha(g, 0.20f);
    style.Colors[ImGuiCol_ResizeGripHovered]     = alpha(g, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive]      = alpha(gHi, 0.95f);
    style.Colors[ImGuiCol_TextSelectedBg]        = alpha(g, 0.35f);
    style.Colors[ImGuiCol_UnsavedMarker]         = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_NavCursor]             = g;
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    style.Colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.20f, 0.30f, 0.10f, 0.20f);
    style.Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.10f, 0.20f, 0.00f, 0.45f);
    style.Colors[ImGuiCol_Separator]             = alpha(g, 0.55f);
    style.Colors[ImGuiCol_InputTextCursor]       = style.Colors[ImGuiCol_Text];
    style.Colors[ImGuiCol_TextLink]              = gHi;

    // contrast: stretch every color around mid-grey (alpha untouched)
    if(h.contrast != 1.0f)
        for(int i = 0; i < ImGuiCol_COUNT; i++)
        {
            ImVec4& c = style.Colors[i];
            c.x = std::clamp((c.x - 0.5f) * h.contrast + 0.5f, 0.0f, 1.0f);
            c.y = std::clamp((c.y - 0.5f) * h.contrast + 0.5f, 0.0f, 1.0f);
            c.z = std::clamp((c.z - 0.5f) * h.contrast + 0.5f, 0.0f, 1.0f);
        }
}

// tracer of the HUD clicks: the longest waits for the HUD lock since the last call (render thread, window thread), then reset
static void KeepMax(std::atomic<int>& v, int x)
{
    int cur = v.load();
    while(x > cur && !v.compare_exchange_weak(cur, x))
    { }
}

void UI::TakeLockWaits(double* render, double* window)
{
    const int r = m_waitRenderUs.exchange(0), w = m_waitInputUs.exchange(0);
    if(render)
        *render = r / 1000.0;
    if(window)
        *window = w / 1000.0;
}

// union of the HUD windows (and popups, tooltips) of the last frame: the renderer lays the HUD texture over this area only
bool UI::HudBounds(RECT& r) const
{
    // bounds of everything ImGui drew in the last frame: the HUD windows, and also the banner / notes, which are drawn without a
    // window (foreground draw list) and were left out before (the banner was never laid over the picture)
    if(!m_ready || !ImGui::GetCurrentContext())
        return false;
    const ImDrawData* dd = ImGui::GetDrawData();
    if(!dd || !dd->Valid)
        return false;
    bool  any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for(int n = 0; n < dd->CmdListsCount; n++)
        for(const ImDrawVert& v : dd->CmdLists[n]->VtxBuffer)
        {
            const float x = v.pos.x - dd->DisplayPos.x, y = v.pos.y - dd->DisplayPos.y;
            if(!any)
                x0 = x1 = x, y0 = y1 = y, any = true;
            else
                x0 = (std::min)(x0, x), y0 = (std::min)(y0, y), x1 = (std::max)(x1, x), y1 = (std::max)(y1, y);
        }
    if(!any)
        return false;
    r = { (LONG)floorf(x0) - 4, (LONG)floorf(y0) - 4, (LONG)ceilf(x1) + 4, (LONG)ceilf(y1) + 4 }; // a small margin
    return true;
}

// Stats of the banner (Appearance > Show stats): RAM used by Nbeam, estimated render latency (a game picture, the frame buffer added in
// brackets when it is used) and late frames per minute
static void CollectStats(const Options& o, std::string& ram, std::string& lat, std::string& late)
{
    static ULONGLONG s_ramTick = 0;
    static double    s_ramMb   = 0.0;
    if(GetTickCount64() - s_ramTick > 1000)
    {
        s_ramTick = GetTickCount64();
        struct MemCounters
        {
            DWORD  cb, PageFaultCount;
            SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
        } mc {};
        mc.cb = sizeof(mc);
        using Fn = BOOL(WINAPI*)(HANDLE, MemCounters*, DWORD);
        if(HMODULE k = GetModuleHandleW(L"kernel32.dll"))
            if(auto fn = (Fn)GetProcAddress(k, "K32GetProcessMemoryInfo"))
                if(fn(GetCurrentProcess(), &mc, sizeof(mc)))
                    s_ramMb = (double)mc.WorkingSetSize / (1024.0 * 1024.0);
    }
    char b[48];
    snprintf(b, sizeof(b), "%.0f MB", s_ramMb);
    ram = b;
    const double contentMs = 1000.0 * (std::max)(o.subFrames, 1) / (std::max)(o.vsyncRate, 1.0f);
    const int    fbFrames  = o.EffectiveFrameBuffer();
    const double bufMs     = fbFrames > 0 ? fbFrames * (std::max)(1, o.frameBufferStep) * contentMs : 0.0;
    if(bufMs > 0.0)
        snprintf(b, sizeof(b), "%.1f ms (+%.1f)", contentMs, bufMs);
    else
        snprintf(b, sizeof(b), "%.1f ms", contentMs);
    lat = b;
    snprintf(b, sizeof(b), "%.0f /min", o.presentGlitches);
    late = b;
}

// the banner is about 3D (emitter on) or about an effect (BFI / CRT) shown with the content refresh rate
bool UI::Banner3d() const
{
    return m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && m_options.StereoAllowed();
}

// 3D switched off because the game runs too fast for stable 3D (the banner follows the banner display time, like the others)
bool UI::Capped3d() const
{
    return !m_windowMode && m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && (m_options.emitter.fpsCapped || m_options.emitter.fullSbsBad);
}

bool UI::BannerFx() const
{
    return !m_windowMode && !m_options.effectsSuspended && m_options.subFrames > 1;
}

// stats asked: the banner stays on while 3D or an effect is active
bool UI::StatsBannerOn() const
{
    return m_ready && m_options.hud.showStats && !m_windowMode && (Banner3d() || BannerFx());
}

void UI::ShowBanner()
{
    if(m_windowMode)
    {
        m_bannerUntil = 0; // window mode: no 3D banner in the window (the effects are off there)
        return;
    }
    const int sec = m_options.hud.bannerSeconds;
    const bool on = (Banner3d() || BannerFx() || Capped3d()) && sec > 0; // (3D wins over the effect: the banner is the Nbeam 3D Vision one)
    m_bannerUntil = !on ? 0 : (sec > 10 ? ~0ULL : GetTickCount64() + (ULONGLONG)sec * 1000ULL);
}

void UI::ShowAdjustBanner(int kind)
{
    m_adjustKind  = kind;
    const int sec = m_options.hud.bannerSeconds;
    m_adjustUntil = sec <= 0 ? 0 : GetTickCount64() + (sec > 10 ? 5000ULL : (ULONGLONG)sec * 1000ULL);
}

void UI::ShowNote(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(m_noteMutex);
        m_noteText = text;
    }
    m_noteUntil = GetTickCount64() + 3000ULL;
}

bool UI::BannerActive() const
{
    const auto now = GetTickCount64();
    return m_ready && (StatsBannerOn() || (m_bannerUntil && now < m_bannerUntil) || (m_adjustUntil && now < m_adjustUntil) || (m_noteUntil && now < m_noteUntil) || (m_menuUntil && now < m_menuUntil));
}

// shown for 3 seconds when a 3D mode is active: tells that the emitter is on (or could not start)
void UI::RenderBanner()
{
    static const char* formats[] = { "Off", "SBS", "TB", "Checkerboard", "2D to 3D (AI)", "Interlaced H", "Interlaced V" };
    const ImU32 accent = IM_COL32((int)(m_options.hud.r * 255), (int)(m_options.hud.g * 255), (int)(m_options.hud.b * 255), 255);

    struct Line
    {
        std::string text;
        ImU32       color;
    };
    std::vector<Line> lines;
    if(m_noteUntil && GetTickCount64() < m_noteUntil)
    {
        std::lock_guard<std::mutex> lock(m_noteMutex);
        lines.push_back({ m_noteText, accent }); // one-line message (e.g. a program added by hotkey)
    }
    else if(m_adjustUntil && GetTickCount64() < m_adjustUntil)
    {
        // a 3D timing hotkey was used: the three timings, the one just changed in the accent color
        const int kind = m_adjustKind;
        char      buf[96];
        auto      add = [&](int k, const char* fmt, auto... v) {
            snprintf(buf, sizeof(buf), Tr(fmt), v...);
            lines.push_back({ buf, k == kind ? accent : IM_COL32(215, 215, 215, 255) });
        };
        add(2, "Sync Offset: %d us", m_options.emitter.syncOffsetUs);
        add(3, "Open Duration: %.0f us", m_options.emitter.nvidia.openDurationUs);
    }
    else if(Capped3d())
    {
        const auto& em = m_options.emitter;
        char        buf[160];
        if(em.fullSbsBad)
        {
            lines.push_back({ Tr("Full SBS impossible with this window"), IM_COL32(255, 90, 40, 255) });
            lines.push_back({ Tr("The captured window must be twice as wide as the screen (e.g. 3840x1080 on 1080p)"), IM_COL32(215, 215, 215, 255) });
            lines.push_back({ Tr("Resize the game window, or pick another 3D format"), IM_COL32(170, 170, 170, 255) });
        }
        else
        {
        lines.push_back({ Tr("3D disabled: frame rate too high"), IM_COL32(255, 90, 40, 255) });
        snprintf(buf, sizeof(buf), Tr("Game: %.0f fps  |  limit: %.0f fps"), em.cappedFps, (std::max)(em.fpsLimit - 5.0f, 1.0f)); // (shown: half the 3D rate; the 5 fps above it are Nbeam's margin)
        lines.push_back({ buf, IM_COL32(215, 215, 215, 255) });
        lines.push_back({ Tr("Cap the game frame rate to the limit: 3D comes back once under it (stability of the glasses)"), IM_COL32(170, 170, 170, 255) });
        }
    }
    else
    {
        bool failed = false;
        for(const auto& line : m_emitterStatus)
            if(line.find("FAILED") != std::string::npos)
                failed = true;
        const int fmt = (int)m_options.emitter.sourceFormat;
        if(!Banner3d() && !failed)
        {
            // BFI / CRT without 3D: the effect and the content refresh rate (screen Hz / SubFrames)
            const int  no = std::clamp(m_options.shaderProfileNo, 0, (int)m_shaders.size() - 1);
            char       buf[64];
            lines.push_back({ std::string(Tr(m_shaders[no].name.c_str())) + "  -  " + Tr("On"), accent });
            snprintf(buf, sizeof(buf), Tr("Refresh rate: %.1f Hz"), m_options.vsyncRate / (float)(std::max)(m_options.subFrames, 1));
            lines.push_back({ buf, IM_COL32(215, 215, 215, 255) });
        }
        else
        {
        std::string fmtName = Tr(formats[(fmt >= 0 && fmt < 7) ? fmt : 0]);
        if(m_options.emitter.autoSource)
            fmtName = std::string(Tr("Auto")) + ": " + fmtName;
        lines.push_back({ failed ? std::string(Tr("3D emitter: NOT FOUND (see Status)")) : std::string(Tr("3D emitter ON  -  ")) + fmtName,
                          failed ? IM_COL32(255, 90, 40, 255) : accent });
        }
        if(!failed && Banner3d())
        {
            // 3D refresh rate = half of the emitter (eye switch) rate
            const auto&  s      = m_options.emitter;
            const double syncHz = s.EffectiveSyncHz(m_options.vsyncRate, m_options.subFrames);
            char         buf[64];
            snprintf(buf, sizeof(buf), Tr("3D refresh rate: %.1f Hz"), syncHz / 2.0);
            lines.push_back({ buf, IM_COL32(215, 215, 215, 255) });
        }
        if(m_options.hud.showStats && !failed)
        {
            std::string ram, lat, late;
            CollectStats(m_options, ram, lat, late);
            lines.push_back({ std::string(Tr("RAM")) + ": " + ram, IM_COL32(215, 215, 215, 255) });
            lines.push_back({ std::string(Tr("Latency")) + ": " + lat, IM_COL32(215, 215, 215, 255) });
            lines.push_back({ std::string(Tr("Late frames")) + ": " + late, IM_COL32(215, 215, 215, 255) });
        }
    }
    const char* title = (m_options.emitter.sourceFormat != StereoSource::Off && !(m_noteUntil == 0 && false) && Banner3d()) || Capped3d() || m_adjustUntil > GetTickCount64() ? "Nbeam 3D Vision" : "Nbeam";

    auto  drawList = ImGui::GetForegroundDrawList();
    auto  size     = ImGui::GetMainViewport()->Size;
    float textW    = m_largeFont->CalcTextSizeA(m_fontSize * 3, size.x, 0, title).x;
    for(const auto& l : lines)
        textW = (std::max)(textW, m_smallFont->CalcTextSizeA(m_fontSize, size.x, 0, l.text.c_str()).x);
    const float boxH     = m_fontSize * (5.0f + 1.4f * (float)lines.size());
    const float boxW     = textW + m_fontSize * 4;
    const int   corner   = m_options.hud.bannerCorner;
    const float x0       = (corner == 1 || corner == 2) ? size.x - m_fontSize - boxW : m_fontSize;
    const float y0       = (corner == 2 || corner == 3) ? m_fontSize : size.y - m_fontSize - boxH;
    ImVec2      boxStart = ImVec2(x0, y0);
    ImVec2      boxEnd   = ImVec2(x0 + boxW, y0 + boxH);

    drawList->AddRectFilled(boxStart, boxEnd, IM_COL32(0, 0, 0, 255));
    boxStart.x += m_fontSize * 2;
    boxStart.y += m_fontSize;
    ImGui::PushFont(m_largeFont);
    drawList->AddText(boxStart, accent, title);
    ImGui::PopFont();
    boxStart.y += m_fontSize * 3;
    for(const auto& l : lines)
    {
        drawList->AddText(boxStart, l.color, l.text.c_str());
        boxStart.y += m_fontSize * 1.4f;
    }
}

void UI::CloseMenu()
{
    m_menuUntil       = 0;
    m_menuOpen        = false;
    m_menuEdit        = -1;
    m_menuPendingItem = -1;
    m_menuClicks      = 0;
    m_menuWheel       = 0;
    m_menuHeld        = 0;
}

// ---- 3D mini menu: driven only by the NVIDIA emitter ----
// wheel = move between the lines; click = select a line; wheel = change its value; click = confirm (the list choices are applied then)
void UI::MenuInput(int clicks, int wheel, int held)
{
    if(!m_ready)
        return;
    if(m_windowMode || !m_options.captureWindow)
    {
        // desktop mode (effects on or off, HUD shown or not): the button only adds the program to the profiles, exactly as the "add the program
        // to the profiles" shortcut does; there is no mini menu. The mini menu is for the game capture.
        CloseMenu();
        if(clicks > 0 && !held)
            PostMessage(m_options.outputWindow, WM_HOTKEY, HOTKEY_ADDEXE, 1);
        return;
    }
    if(m_options.emitter.hold)
        return;
    if(m_options.ui)
    {
        CloseMenu(); // the HUD is shown: no mini menu
        return;
    }
    const auto now = GetTickCount64();
    if(!(m_menuUntil && now < m_menuUntil))
        m_menuOpen = false; // the timer ran out: the next input opens the menu again
    m_menuClicks += clicks;
    m_menuWheel += wheel;
    m_menuHeld += held;
    if(held)
        m_menuLastHeld = now;
    const int sec = m_options.hud.bannerSeconds;
    m_menuUntil   = sec > 10 ? ~0ULL : now + (ULONGLONG)(sec > 0 ? sec : 5) * 1000ULL; // the same duration as the 3D banner
}

void UI::RenderMenu()
{
    if(m_options.ui)
    {
        CloseMenu();
        return;
    }
    const auto now = GetTickCount64();
    if(m_menuOpen && m_menuEdit >= 0 && m_menuUntil != ~0ULL)
    {
        const unsigned long long cur = m_menuUntil;
        if(cur < now + 5000)
            m_menuUntil = now + 5000; // the slider stays until the setting is confirmed
    }
    if(!(m_menuUntil && now < m_menuUntil))
    {
        m_menuOpen        = false;
        m_menuPendingItem = -1;
        m_menuEdit        = -1;
        return;
    }
    auto& s = m_options.emitter;

    // a line is either a live setting (adjust) or a heavy one (3D mode, AI resolution) chosen from a short list and applied on release
    struct Item
    {
        std::string                           label;
        std::string                           value;
        std::function<void(int)>              adjust;                    // live: n steps (positive = wheel up)
        int                                   count = 0;                 // heavy: number of choices
        int                                   cur   = 0;
        std::function<std::string(int)>       name;
        std::function<bool(int)>              enabled;
        std::function<void(int)>              apply;
        float                                 frac  = -1.0f;             // live line with a range: position 0..1 for the slider shown when it changes (-1 = none)
    };
    std::vector<Item> items;
    char              buf[64];
    auto addFloat = [&](const char* label, float* p, float lo, float hi, float step, const char* fmt) {
        snprintf(buf, sizeof(buf), fmt, *p);
        Item it;
        it.label  = Tr(label);
        it.value  = buf;
        it.frac   = hi > lo ? std::clamp((*p - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        it.adjust = [=](int n) { *p = std::clamp(*p + (float)n * step, lo, hi); };
        items.push_back(std::move(it));
    };
    auto addBool = [&](const char* label, bool* p) {
        Item it;
        it.label  = Tr(label);
        it.value  = *p ? Tr("On") : Tr("Off");
        it.adjust = [=](int n) { *p = n > 0; };
        items.push_back(std::move(it));
    };

    const double syncHz = s.EffectiveSyncHz(m_options.vsyncRate, m_options.subFrames);
    const int    range  = syncHz > 0 ? (int)(1e6 / syncHz) : 10000;
    const bool   on3d   = s.sourceFormat != StereoSource::Off;

    {
        // Auto source: above the 3D Source Format, which is hidden while it is on (the format then follows the detected 3D source)
        Item it;
        it.label  = Tr("Auto source");
        it.value  = s.autoSource ? Tr("On") : Tr("Off");
        it.adjust = [&s](int n) { s.autoSource = n > 0; };
        items.push_back(std::move(it));
    }
    // 3D mode (restarts the 3D renderer: applied when the button is released)
    if(!s.autoSource)
    {
        static const char* kNames[] = { "Off", "Side by Side (SBS)", "Top and Bottom (TB)", "Checkerboard", "2D to 3D (AI)", "Interlaced horizontal", "Interlaced vertical" };
        static std::vector<std::string> s_missing;
        static ULONGLONG                s_missingAt = 0;
        if(now - s_missingAt > 2000)
        {
            s_missing   = MissingAiFilesNextToExe();
            s_missingAt = now;
        }
        const bool aiOk = s_missing.empty();
        Item       it;
        it.label   = Tr("3D Source Format");
        static const int kOrder[7] = { 0, 1, 2, 3, 5, 6, 4 };
        it.count   = 7;
        it.cur     = 0;
        for(int k = 0; k < 7; k++)
            if(kOrder[k] == (int)s.sourceFormat)
                it.cur = k;
        it.name    = [](int p) { return std::string(Tr(kNames[kOrder[p]])); };
        it.enabled = [aiOk](int p) { return kOrder[p] != 4 || aiOk; }; // (the item is hidden while Auto source is on)
        it.apply   = [this](int p) {
            const int i = kOrder[p];
            auto& e = m_options.emitter;
            if((int)e.sourceFormat == i)
                return;
            e.sourceFormat   = (StereoSource)i;
            m_options.liveReload = true; // the 3D source changed: rebuilt without a restart
            m_lastSource         = e.sourceFormat;
        };
        items.push_back(std::move(it));
    }
    if(on3d)
    {
        // emitter rate: the display rate divided by n, between 80 and 165 Hz (what the glasses detect); chosen then confirmed like the 3D mode
        std::vector<double> rates;
        for(int n = 1; n <= 20 && m_options.vsyncRate > 1.0f; n++)
        {
            const double hz = (double)m_options.vsyncRate / n;
            if(hz > 165.5)
                continue;
            if(hz < 79.5)
                break;
            rates.push_back(hz);
        }
        if(!rates.empty())
        {
            int cur = 0;
            for(int i = 1; i < (int)rates.size(); i++)
                if(std::fabs(rates[i] - syncHz) < std::fabs(rates[cur] - syncHz))
                    cur = i;
            Item it;
            it.label = Tr("Emitter frequency");
            it.count = (int)rates.size();
            it.cur   = cur;
            it.name  = [rates](int i) {
                char t[32];
                snprintf(t, sizeof(t), "%.3f Hz", rates[std::clamp(i, 0, (int)rates.size() - 1)]);
                return std::string(t);
            };
            const bool lockRate = s.syncAuto || s.autoSource; // (the rate is chosen by Sync Auto / Auto source: not changed from the mini menu)
            it.enabled = [lockRate, cur](int p) { return !lockRate || p == cur; };
            it.apply = [this, rates](int i) {
                auto& e      = m_options.emitter;
                e.syncAuto   = false;
                e.syncRateHz = rates[std::clamp(i, 0, (int)rates.size() - 1)];
                m_emitterRestartRequired = true;
            };
            items.push_back(std::move(it));
        }
    }
    if(on3d)
    {
        if(s.sourceFormat == StereoSource::AiDepth)
        {
            static const int kWidths[] = { 224, 308, 420, 518, 672, 840, 1008 };
            int              idx       = 1;
            for(int i = 0; i < 7; i++)
                if(std::abs(kWidths[i] - s.ai.modelWidth) < std::abs(kWidths[idx] - s.ai.modelWidth))
                    idx = i;
            Item it;
            it.label = Tr("AI resolution (px)");
            it.count = 7;
            it.cur   = idx;
            it.name  = [](int i) { return std::to_string(kWidths[i]); };
            it.apply = [&s](int i) { s.ai.modelWidth = kWidths[std::clamp(i, 0, 6)]; };
            items.push_back(std::move(it));
            addFloat("Depth Strength (%)", &s.ai.strength, 0.0f, 10.0f, 0.1f, "%.1f");
            addFloat("Depth smoothing (%)", &s.ai.smoothing, 0.0f, 100.0f, 1.0f, "%.0f");
            addFloat("Anti-halo (%)", &s.ai.antiHalo, -300.0f, 300.0f, 2.0f, "%.0f");
        }
        {
            snprintf(buf, sizeof(buf), "%d", s.syncOffsetUs);
            Item it;
            it.label  = Tr("Sync Offset (us)");
            it.value  = buf;
            it.frac   = range > 0 ? std::clamp((float)(s.syncOffsetUs + range) / (float)range, 0.0f, 1.0f) : 0.0f;
            it.adjust = [&s, range](int n) { s.syncOffsetUs = std::clamp(s.syncOffsetUs + n * 50, -range, 0); }; // (50 us per tick of the wheel)
            items.push_back(std::move(it));
        }
        addFloat("Open Duration (us)", &s.nvidia.openDurationUs, 0.0f, (float)range, 50.0f, "%.0f"); // (50 us per tick of the wheel)
        // convergence, shown -100..+100 like the slider
        {
            const float conv = (50.0f - s.ai.convergence) * 2.0f;
            snprintf(buf, sizeof(buf), "%+.0f", conv);
            Item it;
            it.label  = Tr("Convergence");
            it.value  = buf;
            it.frac   = std::clamp((conv + 100.0f) / 200.0f, 0.0f, 1.0f);
            it.adjust = [&s](int n) {
                const float c    = std::clamp((50.0f - s.ai.convergence) * 2.0f + (float)n, -100.0f, 100.0f);
                s.ai.convergence = std::clamp(50.0f - c * 0.5f, 0.0f, 100.0f);
            };
            items.push_back(std::move(it));
        }
        addBool("Swap Eyes", &s.swapEyes);
    }

    // ---- the program: add it to the profiles, and the profile it uses (each applied like the 3D mode: choose, then press again) ----
    {
        static std::vector<std::string> s_profList;
        static ULONGLONG                s_profAt = 0;
        if(now - s_profAt > 2000)
        {
            s_profList = Options::ListProfiles();
            SortNoCase(s_profList);
            s_profAt = now;
        }
        // the captured program, or the one in the foreground (not Nbeam, not the Windows shell)
        std::string prog = m_options.captureProcess;
        if(prog.empty())
        {
            HWND  fg  = GetForegroundWindow();
            DWORD pid = 0;
            if(fg)
                GetWindowThreadProcessId(fg, &pid);
            if(pid && pid != GetCurrentProcessId())
                prog = Options::ProcessNameOfWindow(fg);
            static const char* ignored[] = { "explorer.exe", "searchhost.exe", "searchapp.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe", "applicationframehost.exe", "lockapp.exe", "dwm.exe" };
            for(const char* ig : ignored)
                if(prog == ig)
                    prog.clear();
        }
        if(!prog.empty())
        {
            const std::string screen = m_options.ScreenIdOfDisplay(m_options.shaderDisplayNo, m_displays);
            bool              known  = false;
            int               curProf = -1;
            {
                std::lock_guard<std::recursive_mutex> lk(m_options.linksMutex);
                const Options::ProfileLink* match = nullptr;
                for(const auto& l : m_options.profileLinks)
                    if(Options::NormalizeExeName(l.exe) == prog)
                    {
                        known = true;
                        if(!match)
                            match = &l; // the first line of the program decides its profile
                    }
                if(match)
                    for(int i = 0; i < (int)s_profList.size(); i++)
                        if(s_profList[i] == match->profile)
                            curProf = i;
            }
            if(!known)
            {
                Item it;
                it.label   = Tr("Add the program to the profiles");
                it.count   = 2;
                it.cur     = known ? 1 : 0;
                it.name    = [](int i) { return std::string(i ? Tr("Added") : Tr("Not added")); };
                it.apply   = [this, prog, screen](int i) {
                    if(i != 1)
                        return;
                    std::lock_guard<std::recursive_mutex> lk(m_options.linksMutex);
                    for(const auto& l : m_options.profileLinks)
                        if(Options::NormalizeExeName(l.exe) == prog)
                            return;
                    if(m_options.profileLinks.size() >= 64)
                        return;
                    Options::ProfileLink nl;
                    nl.exe      = prog;
                    nl.screenId = screen;
                    nl.fpsCapMode = m_options.DefaultFpsCapMode();
                    nl.fpsLimit   = m_options.fpsLimitDefault;
                    nl.profile    = m_options.ProfileForNewProgram();
                    m_options.profileLinks.push_back(nl);
                    m_options.SaveApp();
                    ShowNote(prog + " " + Tr("added to the profile detections"));
                };
                items.push_back(std::move(it));
            }
            if(known && !s_profList.empty())
            {
                Item it;
                it.label = Tr("Profile of the program");
                it.count = (int)s_profList.size();
                it.cur   = curProf;
                it.name  = [](int i) { return i >= 0 && i < (int)s_profList.size() ? s_profList[i] : std::string(Tr("(no profile)")); };
                it.apply = [this, prog](int i) {
                    if(i < 0 || i >= (int)s_profList.size())
                        return;
                    std::lock_guard<std::recursive_mutex> lk(m_options.linksMutex);
                    bool changed = false;
                    for(auto& l : m_options.profileLinks)
                        if(Options::NormalizeExeName(l.exe) == prog)
                        {
                            l.profile = s_profList[i]; // one profile per program: the same for all its screens (the detection loads it right away)
                            changed   = true;
                        }
                    if(changed)
                        m_options.SaveApp();
                };
                items.push_back(std::move(it));
            }
        }
    }

    {
        // the last line: all the effects and the emitter off / on (as the Hide / Show button and its hotkey)
        Item it;
        it.label  = Tr("Disable effects");
        it.value  = m_options.effectsSuspended ? Tr("On") : Tr("Off");
        it.adjust = [this](int n) { m_options.effectsSuspended = n > 0; };
        items.push_back(std::move(it));
    }

    // ---- pending input ----
    const int clicks = m_menuClicks.exchange(0);
    const int wheel  = m_menuWheel.exchange(0);
    const int held   = m_menuHeld.exchange(0);
    const int count  = (int)items.size();
    bool      changed = false;
    if(!m_menuOpen)
    {
        // any input opens the menu on the first line
        if(clicks || wheel || held)
        {
            m_menuOpen        = true;
            m_menuIndex       = 0;
            m_menuPendingItem = -1;
            m_menuEdit        = -1;
            m_menuNavRest     = 0;
        }
        else
            return;
    }
    else
    {
        m_menuIndex = std::clamp(m_menuIndex, 0, count - 1);
        if(m_menuEdit >= count)
        {
            m_menuEdit        = -1;
            m_menuPendingItem = -1;
        }
        // the wheel (button held or not) moves between the lines, or, once a line is selected by a click, changes its value
        const int turn = wheel + held;
        if(turn != 0)
        {
            if(m_menuEdit < 0)
            {
                // (less sensitive than the settings: 3 ticks of the wheel per line; up = previous line)
                m_menuNavRest += turn;
                const int nav = m_menuNavRest / 3;
                m_menuNavRest -= nav * 3;
                if(nav != 0)
                    m_menuIndex = std::clamp(m_menuIndex - nav, 0, count - 1);
            }
            else
            {
                Item& it = items[m_menuEdit];
                if(it.count > 0)
                {
                    // a choice from a short list (3D mode, rate of the emitter, AI resolution, profile): applied when confirmed
                    // (less sensitive than the other settings: 3 ticks of the wheel per choice)
                    m_menuChoiceRest += turn;
                    const int choiceSteps = m_menuChoiceRest / 3;
                    m_menuChoiceRest -= choiceSteps * 3;
                    int       v   = m_menuPendingVal;
                    const int dir = choiceSteps > 0 ? 1 : -1;
                    for(int k = 0; k < std::abs(choiceSteps); k++)
                    {
                        int nv = v + dir;
                        while(nv >= 0 && nv < it.count && it.enabled && !it.enabled(nv))
                            nv += dir;
                        if(nv >= 0 && nv < it.count)
                            v = nv;
                    }
                    m_menuPendingVal = v;
                }
                else if(it.adjust)
                {
                    // live setting (glasses timing, convergence, ...): applied at once while the wheel turns
                    it.adjust(turn);
                    RequestSave();
                    changed     = true;
                }
            }
        }
        // a click selects the highlighted line, or confirms the selected one (a click right after a turn with the button held is its release)
        for(int i = 0; i < clicks; i++)
        {
            if(held != 0 || now - m_menuLastHeld < 1500)
                break;
            if(m_menuEdit >= 0)
            {
                Item& it = items[m_menuEdit];
                if(it.count > 0 && it.apply && m_menuPendingItem == m_menuEdit && m_menuPendingVal != it.cur)
                {
                    it.apply(m_menuPendingVal);
                    RequestSave();
                    changed     = true;
                }
                m_menuPendingItem = -1;
                m_menuEdit        = -1;
                m_menuNavRest     = 0;
            }
            else
            {
                Item& it = items[m_menuIndex];
                m_menuEdit    = m_menuIndex;
                m_menuNavRest = 0;
                if(it.count > 0)
                {
                    m_menuPendingItem = m_menuIndex;
                    m_menuPendingVal  = it.cur;
                    m_menuChoiceRest  = 0;
                }
            }
        }
    }
    if(changed)
        return RenderMenu(); // draw the new values (the list is rebuilt from the settings)

    // ---- drawing: stacked on the 3D banner (above it when the banner is at the bottom, below it when it is at the top) ----
    const ImU32 accent   = IM_COL32((int)(m_options.hud.r * 255), (int)(m_options.hud.g * 255), (int)(m_options.hud.b * 255), 255);
    auto        drawList = ImGui::GetForegroundDrawList();
    const auto  size     = ImGui::GetMainViewport()->Size;
    const float fs       = m_fontSize;
    const float rowH     = fs * 1.7f;
    const int   maxRows  = 9;
    const int   rows     = (std::min)(count, maxRows);
    const int   first    = std::clamp(m_menuIndex - rows / 2, 0, count - rows);
    const char* help[]   = { Tr("Wheel: move up / down"), Tr("Click: select the setting"), Tr("Wheel: change it, click again: confirm"), Tr("Last line: disable the effects") };
    std::vector<std::string> shown(count);
    for(int i = 0; i < count; i++)
    {
        const Item& it = items[i];
        if(it.count > 0)
        {
            const bool pend = m_menuPendingItem == i;
            shown[i]        = it.name(pend ? m_menuPendingVal : it.cur);
        }
        else
            shown[i] = it.value;
        if(m_menuEdit == i)
            shown[i] = "< " + shown[i] + " >"; // selected: the wheel changes it
    }
    // while a setting is being adjusted only the slider is shown (the menu comes back when it is confirmed)
    if(m_menuEdit < 0)
    {
    float boxW = fs * 26.0f;
    for(int i = 0; i < count; i++)
        boxW = (std::max)(boxW, m_smallFont->CalcTextSizeA(fs, size.x, 0, items[i].label.c_str()).x + m_smallFont->CalcTextSizeA(fs, size.x, 0, shown[i].c_str()).x + fs * 5.0f);
    for(const char* h : help)
        boxW = (std::max)(boxW, m_smallFont->CalcTextSizeA(fs, size.x, 0, h).x + fs * 2.5f);
    const float helpH   = fs * 1.35f * 4.0f + fs * 0.8f;
    const float boxH    = rowH * (float)rows + fs * 1.2f + helpH;
    const float bannerH = fs * (5.0f + 1.4f * 2.0f); // height of the banner (two lines)
    const int   corner  = std::clamp(m_options.hud.bannerCorner, 0, 3);
    const float x0      = (corner == 1 || corner == 2) ? size.x - fs - boxW : fs;
    const bool  top     = corner == 2 || corner == 3;
    const float y0      = top ? fs + bannerH + fs * 0.5f : size.y - fs - bannerH - fs * 0.5f - boxH;
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + boxW, y0 + boxH), IM_COL32(0, 0, 0, 255));
    float y = y0 + fs * 0.6f;
    for(int i = first; i < first + rows; i++)
    {
        const bool sel = i == m_menuIndex;
        if(sel)
            drawList->AddRectFilled(ImVec2(x0 + fs * 0.4f, y), ImVec2(x0 + boxW - fs * 0.4f, y + rowH), accent);
        const ImU32 col = sel ? IM_COL32(0, 0, 0, 255) : IM_COL32(215, 215, 215, 255);
        const float ty  = y + (rowH - fs) * 0.5f;
        drawList->AddText(ImVec2(x0 + fs, ty), col, items[i].label.c_str());
        const float vw = m_smallFont->CalcTextSizeA(fs, size.x, 0, shown[i].c_str()).x;
        drawList->AddText(ImVec2(x0 + boxW - fs - vw, ty), sel ? col : accent, shown[i].c_str());
        y += rowH;
    }
    // how to use it (for beginners)
    y += fs * 0.3f;
    drawList->AddLine(ImVec2(x0 + fs, y), ImVec2(x0 + boxW - fs, y), IM_COL32(90, 90, 90, 255));
    y += fs * 0.5f;
    for(const char* h : help)
    {
        drawList->AddText(ImVec2(x0 + fs, y), IM_COL32(170, 170, 170, 255), h);
        y += fs * 1.35f;
    }
    }
    // the slider shown when a setting has just changed: Nbeam logo, name, bar (when the setting has a range), value; rounded, in the accent colour
    if(m_menuEdit >= 0 && m_menuEdit < count)
    {
        const Item& it  = items[m_menuEdit];
        const float rr  = fs * 1.5f; // half the height: fully rounded ends
        const float oh  = rr * 2.0f;
        const float ow  = (std::min)(size.x - fs * 2.0f, fs * 30.0f);
        const float ox  = (size.x - ow) * 0.5f;
        const float oy  = size.y - fs * 4.0f - oh;
        const ImVec4 ac = ImVec4(m_options.hud.r, m_options.hud.g, m_options.hud.b, 1.0f);
        const float lum = 0.299f * ac.x + 0.587f * ac.y + 0.114f * ac.z;
        const ImU32 txt  = lum > 0.6f ? IM_COL32(20, 20, 20, 255) : IM_COL32(255, 255, 255, 255);
        const ImU32 dark = ImGui::GetColorU32(ImVec4(ac.x * 0.35f, ac.y * 0.35f, ac.z * 0.35f, 1.0f));
        const ImU32 fill = ImGui::GetColorU32(ImVec4(ac.x + (1.0f - ac.x) * 0.85f, ac.y + (1.0f - ac.y) * 0.85f, ac.z + (1.0f - ac.z) * 0.85f, 1.0f));
        drawList->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + ow, oy + oh), accent, rr);
        // the logo, where the left arrow was
        const float ls = oh - fs * 0.5f;
        const float lx = ox + fs * 0.25f, ly = oy + fs * 0.25f;
        drawList->AddCircleFilled(ImVec2(lx + ls * 0.5f, ly + ls * 0.5f), ls * 0.5f, dark, 48); // (behind the logo, for a logo with transparent parts)
        if(m_logo)
            drawList->AddImageRounded(ImTextureRef((ImTextureID)(uintptr_t)m_logo.get()), ImVec2(lx, ly), ImVec2(lx + ls, ly + ls), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ls * 0.5f); // the logo fills the whole circle
        float       x      = lx + ls + fs * 0.7f;
        const float labelW = m_smallFont->CalcTextSizeA(fs, size.x, 0, it.label.c_str()).x;
        drawList->AddText(ImVec2(x, oy + (oh - fs) * 0.5f), txt, it.label.c_str());
        x += labelW + fs * 0.8f;
        const std::string val = it.count > 0 ? it.name(m_menuPendingItem == m_menuEdit ? m_menuPendingVal : it.cur) : it.value;
        const float       vw  = m_smallFont->CalcTextSizeA(fs * 1.3f, size.x, 0, val.c_str()).x;
        const float       vx  = ox + ow - rr * 0.7f - vw;
        if(it.frac >= 0.0f)
        {
            const float th = fs * 0.9f, tx0 = x, tx1 = vx - fs * 0.8f, ty = oy + (oh - th) * 0.5f;
            if(tx1 - tx0 > th * 2.0f)
            {
                drawList->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + th), dark, th * 0.5f);
                const float fw = (std::max)(th, (tx1 - tx0) * it.frac); // (never narrower than a round end)
                drawList->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx0 + fw, ty + th), fill, th * 0.5f);
            }
        }
        drawList->AddText(m_smallFont, fs * 1.3f, ImVec2(vx, oy + (oh - fs * 1.3f) * 0.5f), txt, val.c_str());
    }
}

void UI::UpdateCaptureTone(bool refreshHdr)
{
    if(refreshHdr)
        RefreshHdrState(m_options.outputWindow); // main thread, once a second
    const bool hdr = m_options.useHdr && WindowsHdrOn(m_options.outputWindow);
    m_options.captureBlack = hdr ? std::clamp(m_options.hud.hdrBlackNits, 0.0f, 20.0f) / 80.0f : 0.0f;
    m_options.captureGrey  = hdr ? std::clamp(m_options.hud.hdrGrey, 0.5f, 2.0f) : 1.0f;
    // Nits: above 0 the picture gets brighter by that many nits; below 0 it fades down to a black picture at -1000 (the HUD, drawn after,
    // is never affected)
    const float nits       = std::clamp(m_options.hud.hdrNits, -1000.0f, 1000.0f);
    m_options.captureGain  = !hdr ? 1.0f : nits >= 0.0f ? 1.0f + nits / ((std::max)(s_sdrWhiteScale, 0.1f) * 80.0f) : 1.0f + nits / 1000.0f;
}

void UI::RefreshHotkeyTexts()
{
    s_hotkeyMessage = std::string(Tr("Press ")) + Options::HotkeyText(m_options.hotkeys[HOTKEY_TOGGLEUI]);
    s_hotkeyMessage += Tr(" for UI");
    s_hotkeyHelp    = Tr("Global hotkeys (change them in the Hotkeys section):\n\n");
    for(int id : { HOTKEY_TOGGLEUI, HOTKEY_RESTART, HOTKEY_QUIT, HOTKEY_SWAPEYES, HOTKEY_EFFECTS, HOTKEY_ADDEXE })
        s_hotkeyHelp += std::string("  ") + Tr(Options::HotkeyActionName(id)) + ": " + Options::HotkeyText(m_options.hotkeys[id]) + "\n";
}

// Hotkeys section: click a button, then press the new key or combination (any key, modifiers optional). Esc alone cancels, Backspace alone clears.
void UI::RenderHotkeys()
{
    s_hotkeysDrawnFrame = ImGui::GetFrameCount();
    static const int ids[] = { HOTKEY_TOGGLEUI, HOTKEY_RESTART, HOTKEY_QUIT, HOTKEY_SWAPEYES, HOTKEY_EFFECTS, HOTKEY_ADDEXE, HOTKEY_SUBFRAMES_DEC, HOTKEY_SUBFRAMES_INC, HOTKEY_CLEAR_DEC,
                              HOTKEY_CLEAR_INC, HOTKEY_SYNCOFFSET_DEC, HOTKEY_SYNCOFFSET_INC, HOTKEY_DURATION_DEC, HOTKEY_DURATION_INC };
    auto      restoreAll = [&]() {
        m_options.hotkeysSuspended = false;
        for(int id = 0; id < HOTKEY_COUNT; id++)
            m_options.UnregisterHotkey(id);
        for(int id = 0; id < HOTKEY_COUNT; id++)
            m_options.RegisterHotkey(id);
        m_options.SaveApp(); // hotkeys are global: saved in Nbeam.app.cfg, whatever the active profile
    };

    // a click anywhere but on the box being set cancels the change (a click on another box then starts that one)
    static bool s_captureHovered = false;
    if(s_captureId >= 0 && ImGui::IsMouseClicked(0) && !s_captureHovered)
    {
        s_hotkeyError.clear();
        CancelCaptureNow(m_options);
    }

    if(s_captureId >= 0)
    {
        // keyboard: the first key pressed is the new shortcut
        // (the global hotkeys are unregistered while capturing so they cannot fire)
        unsigned mods = 0;
        if(GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if(GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
        if(GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
        if((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
        for(unsigned vk = 8; vk < 255; vk++)
        {
            if(vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || (vk >= VK_LSHIFT && vk <= VK_RMENU))
                continue;
            if(!(GetAsyncKeyState((int)vk) & 0x8000))
                continue;
            if(vk == VK_ESCAPE && !mods)
            {
                s_hotkeyError.clear();
            }
            else if(vk == VK_BACK && !mods)
            {
                if(s_captureId == HOTKEY_TOGGLEUI || s_captureId == HOTKEY_EFFECTS)
                    s_hotkeyError = "This shortcut cannot be empty."; // the HUD / the effects must always be reachable
                else
                {
                    m_options.hotkeys[s_captureId].mods = 0, m_options.hotkeys[s_captureId].vk = 0; // Backspace clears the shortcut
                    s_hotkeyError.clear();
                }
            }
            else
            {
                // a shortcut already used by another action: the two actions swap their shortcuts
                int other = -1;
                for(int id : ids)
                    if(id != s_captureId && m_options.hotkeys[id].vk == vk && m_options.hotkeys[id].mods == mods)
                        other = id;
                auto& hkA = m_options.hotkeys[s_captureId];
                if(other >= 0 && IsProtectedHotkey(other) && !hkA.vk)
                    s_hotkeyError = "This shortcut cannot be empty."; // the other action would be left without a key
                else
                {
                    const auto oldA = hkA;
                    hkA.mods = mods, hkA.vk = vk;
                    if(other >= 0)
                    {
                        auto& hkB = m_options.hotkeys[other];
                        hkB.mods = oldA.mods, hkB.vk = oldA.vk;
                    }
                    restoreAll(); // registers everything again
                    s_hotkeyError.clear();
                }
            }
            s_captureId = -1;
            restoreAll();
            RefreshHotkeyTexts();
            break;
        }
    }

    // what an action is bound to: its keyboard shortcut
    auto bindingText = [](const Options::Hotkey& h) {
        std::string t;
        if(h.vk)
            t = Options::HotkeyText(h);
        return t.empty() ? std::string(Tr("None")) : t;
    };
    for(int id : ids)
    {
        ImGui::PushID(id);
        const bool  capturing = s_captureId == id;
        std::string label     = capturing ? std::string(Tr("Press a key... (Esc cancels, Backspace clears)")) : bindingText(m_options.hotkeys[id]);
        ImGui::TextUnformatted(Tr(Options::HotkeyActionName(id)));
        // one box per action, lined up on the right edge of the HUD so the action names stay readable
        const float boxWidth = m_fontSize * 21.5f;
        ImGui::SameLine(ImGui::GetCursorPosX() + (std::max)(ImGui::GetContentRegionAvail().x - boxWidth, m_fontSize * 9));
        if(ImGui::Button((label + "###hk").c_str(), ImVec2(boxWidth, 0)))
        {
            if(capturing)
                CancelCaptureNow(m_options); // a click on the box being set cancels it
            else if(s_captureId < 0)
            {
                s_captureId = id;
                s_hotkeyError.clear();
                m_options.hotkeysSuspended = true;
                for(int i = 0; i < HOTKEY_COUNT; i++)
                    m_options.UnregisterHotkey(i);
            }
        }
        if(capturing)
            s_captureHovered = ImGui::IsItemHovered();
        ImGui::PopID();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Click a box, then press a key or a key combination - the NVIDIA emitter only drives the 3D menu. A click elsewhere or Esc cancels, Backspace clears."));
    ImGui::PopTextWrapPos();
    if(ImGui::Button(Tr("Reset hotkeys")) && s_captureId < 0)
    {
        Options defaults;
        for(int id = 0; id < HOTKEY_COUNT; id++)
            m_options.hotkeys[id] = defaults.hotkeys[id];
        restoreAll();
        RefreshHotkeyTexts();
    }
    if(!s_hotkeyError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "%s", Tr(s_hotkeyError.c_str()));
}

void UI::SetApplyRequired()
{
    m_applyRequired = true;
}

void UI::ClearPendingChanges()
{
    m_applyRequired            = false;
    m_pending.captureAdapterNo = m_options.captureAdapterNo;
    m_pending.shaderAdapterNo  = m_options.shaderAdapterNo;
    m_pending.captureDisplayNo = m_options.captureDisplayNo;
    m_pending.captureWindow    = m_options.captureWindow;
    m_pending.shaderDisplayNo  = m_options.shaderDisplayNo;
    m_pending.outputOnProcess  = m_options.outputOnProcess;
    m_pending.outputAuto       = m_options.outputAuto;
    m_pending.subFramesMode    = m_options.subFramesMode;
    m_pending.hardwareSrgb     = m_options.hardwareSrgb;
    m_pending.captureMethod    = m_options.captureMethod;
    m_pending.splitScreen      = m_options.splitScreen;
    m_pending.maxQueuedFrames  = m_options.maxQueuedFrames;
    m_pending.queueAuto        = m_options.queueAuto;
    m_pending.useHdr           = m_options.useHdr;
}

void UI::ApplyPendingChanges()
{
    m_options.captureAdapterNo = m_pending.captureAdapterNo;
    m_options.shaderAdapterNo  = m_pending.shaderAdapterNo;
    m_options.captureDisplayNo = m_pending.captureDisplayNo;
    // the captured program comes from the active profile link (its window is looked up now, display capture while it does not run)
    m_options.captureWindow = m_options.captureProcess.empty() ? NULL : m_options.FindCaptureWindow();
    m_pending.captureWindow = m_options.captureWindow;
    m_options.outputOnProcess  = m_pending.outputOnProcess;
    m_options.outputAuto       = m_pending.outputAuto;
    m_options.shaderDisplayNo  = m_pending.shaderDisplayNo;
    m_options.subFramesMode    = m_pending.subFramesMode;
    m_options.hardwareSrgb     = m_pending.hardwareSrgb;
    m_options.captureMethod    = m_pending.captureMethod;
    m_options.splitScreen      = m_pending.splitScreen;
    m_options.maxQueuedFrames  = m_pending.maxQueuedFrames;
    m_options.queueAuto        = m_pending.queueAuto;
    m_options.useHdr           = m_pending.useHdr;
}

void UI::SetBenchmark(const BenchmarkResult& result)
{
    m_benchmarkResult = result;
    m_hasBenchmark    = true;
}

void UI::SetError(const char* message)
{
    m_hasError     = true;
    m_errorMessage = message;
}

void UI::ScanWindows()
{
    // the displays (and the capture method of each): programs are captured by their .exe, so their windows are not listed here
    m_inputs.clear();
    for(const auto& capture : m_captures)
    {
        for(const auto& display : m_displays)
        {
            m_inputs.emplace_back((HWND)0, display.name + " (" + capture.name + ")", true, capture.no, display.no);
        }
    }
}

} // namespace Nbeam