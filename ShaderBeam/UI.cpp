/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"

#include "UI.h"
#include "Helpers.h"
#include "CaptureBase.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends\imgui_impl_win32.h"
#include "backends\imgui_impl_dx11.h"

#include "HudFont.h"
#include <atomic>
#include <functional>
#include "UIWidgets.h"
#include "Gamepad.h"
#include "LogoShaderBeam.h"
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

const char* SHADERBEAM_ABOUT = "             " SHADERBEAM_TITLE "\n"
                               "              Version 2.0.0\n"
                               "         Made by anto1444, 2026\n"
                               "\n"
                               "   Based on ShaderBeam (c) 2025-2026 mausimus\n"
                               "         Distributed under MIT License\n"
                               "\n"
                               "    Includes Blur Busters CRT Beam Simulator\n"
                               "        by Mark Rejhon @BlurBusters and\n"
                               "        Timothy Lottes @NOTimothyLottes\n"
                               "          https://blurbusters.com/crt\n\n";
const char* SHADERBEAM_ABOUT_FR = "             " SHADERBEAM_TITLE "\n"
                                  "              Version 2.0.0\n"
                                  "         Fait par anto1444, 2026\n"
                                  "\n"
                                  "   Basé sur ShaderBeam (c) 2025-2026 mausimus\n"
                                  "         Distribué sous licence MIT\n"
                                  "\n"
                                  "    Inclut le Blur Busters CRT Beam Simulator\n"
                                  "        de Mark Rejhon @BlurBusters et\n"
                                  "        Timothy Lottes @NOTimothyLottes\n"
                                  "          https://blurbusters.com/crt\n\n";

static std::string s_hotkeyMessage;
static std::string s_hotkeyHelp;
static int         s_captureId = -1;
static int         s_padCaptureId = -1; // action whose gamepad combination is being captured
static bool        s_padArmed     = false; // all buttons released once since the capture started
static unsigned    s_padMax       = 0;     // buttons held during the capture

static int s_hotkeysDrawnFrame = -1; // last HUD frame in which the Hotkeys section was drawn

static void CancelCaptureNow(ShaderBeam::Options& o)
{
    if(s_captureId < 0 && s_padCaptureId < 0 && !o.hotkeysSuspended)
        return;
    s_captureId        = -1;
    s_padCaptureId     = -1;
    s_padMax           = 0;
    s_padArmed         = false;
    o.hotkeysSuspended = false;
    for(int id = 0; id < HOTKEY_COUNT; id++)
        o.UnregisterHotkey(id);
    for(int id = 0; id < HOTKEY_COUNT; id++)
        o.RegisterHotkey(id);
}

// gamepad focus between the category rail and the settings pane (D-pad left / right)
static ImGuiWindow* s_railWin       = nullptr;
static ImGuiWindow* s_paneWin       = nullptr;
static int          s_padFocus      = 0;     // 1 = move the focus to the rail, 2 = to the pane (done while that part is drawn)
static std::string s_hotkeyError;

namespace ShaderBeam
{

// scroll position survives restarts (the ImGui context is recreated each time)
static std::vector<std::string> s_profiles;
static bool                     s_profilesDirty = true;
static char                     s_newProfile[64] = "";
static float s_scrollY       = 0.0f;
static int   s_restoreFrames = 0;

// logos shown by the category icons (set every frame from the UI members)
static ID3D11ShaderResourceView* s_iconNbeam = nullptr;
static ID3D11ShaderResourceView* s_iconSB    = nullptr;
// HUD scale: base style and base font size (scale 1), applied scale
static ImGuiStyle s_baseStyle;
static float      s_baseFont     = 16.0f;
static float      s_appliedScale = 1.0f;

// the ShaderBeam logo (embedded RGBA, straight alpha)
static winrt::com_ptr<ID3D11ShaderResourceView> CreateShaderBeamLogoTexture(ID3D11Device* device)
{
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC td {};
    td.Width            = kLogoShaderBeamSize;
    td.Height           = kLogoShaderBeamSize;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd {};
    sd.pSysMem     = kLogoShaderBeamRgba;
    sd.SysMemPitch = kLogoShaderBeamSize * 4;
    winrt::com_ptr<ID3D11Texture2D> tex;
    if(SUCCEEDED(device->CreateTexture2D(&td, &sd, tex.put())))
        device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    return srv;
}

// The Nbeam logo (the exe icon) as a texture for the title bar.
static winrt::com_ptr<ID3D11ShaderResourceView> CreateLogoTexture(ID3D11Device* device)
{
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SHADERBEAM), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
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
    m_logoSB = CreateShaderBeamLogoTexture(device.get());

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

bool UI::Input(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    try
    {
        // the render thread may be drawing the HUD right now: wait for it (a little, never forever)
        std::unique_lock<std::timed_mutex> lock(m_imguiMutex, std::defer_lock);
        if(!lock.try_lock_for(std::chrono::milliseconds(50)))
            return false;
        return m_ready && m_options.ui && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
    catch(...)
    { }
    return false;
}

bool UI::RenderRequired() const
{
    return m_ready && (m_options.ui || BannerActive());
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
    dl->AddText(font, verSz, ImVec2(titleX, p0.y + height * 0.5f + fs * 0.1f), cOnAcc, "v2.0.0");

    // output mode (like "3440 x 1440 | 240Hz")
    char mode[64];
    snprintf(mode, sizeof(mode), "%d x %d | %.0f Hz", m_options.outputWidth, m_options.outputHeight, m_options.vsyncRate);
    const float modeX = p0.x + shapeW + fs * 1.2f;
    dl->AddText(font, fs * 0.85f, ImVec2(modeX, p0.y + height * 0.5f - fs * 1.15f), cDim, Tr("Output"));
    dl->AddText(font, fs * 1.15f, ImVec2(modeX, p0.y + height * 0.5f + fs * 0.0f), cWhite, mode);
    float       modeEnd = modeX + (std::max)(textW(fs * 1.15f, mode), textW(fs * 0.85f, Tr("Output"))) + fs * 1.0f;

    // 3D, right next to the output mode: ON with the 3D refresh rate (half of the emitter rate), or OFF
    {
        const bool stereoOn = m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && m_options.StereoAllowed();
        char       v3d[48];
        if(stereoOn)
        {
            const auto&  es     = m_options.emitter;
            const double syncHz = es.EffectiveSyncHz(m_options.vsyncRate, m_options.subFrames);
            if(syncHz > 0.0)
                snprintf(v3d, sizeof(v3d), "%d x %d | %.0f Hz", m_options.outputWidth, m_options.outputHeight, syncHz / 2.0);
            else
                snprintf(v3d, sizeof(v3d), "%d x %d | ON", m_options.outputWidth, m_options.outputHeight);
        }
        else
            snprintf(v3d, sizeof(v3d), "OFF");
        const float cw = (std::max)(textW(fs * 1.15f, v3d), textW(fs * 0.85f, "3D"));
        const float sx = modeEnd + fs * 0.4f;
        dl->AddLine(ImVec2(sx - fs * 0.5f, p0.y + height * 0.2f), ImVec2(sx - fs * 0.5f, p1.y - height * 0.2f), cAccDim, 2.0f);
        dl->AddText(font, fs * 0.85f, ImVec2(sx + fs * 0.4f, p0.y + height * 0.5f - fs * 1.15f), cDim, "3D");
        dl->AddText(font, fs * 1.15f, ImVec2(sx + fs * 0.4f, p0.y + height * 0.5f), stereoOn ? cOnGreen : cWhite, v3d);
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
    std::string profile = m_options.profileFile;
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
    std::lock_guard<std::timed_mutex> imguiLock(m_imguiMutex); // the main thread feeds ImGui with the mouse / keyboard
    ImGui_ImplDX11_NewFrame();
    {
        // Gamepad navigation (D-pad / stick: move, A: activate, B: back, LB / RB: slow / fast on sliders), only while the HUD is open
        // and in front, so that a pad used for the game below never moves the HUD by accident.
        ImGuiIO& io = ImGui::GetIO();
        const bool padNav = m_options.ui && s_padCaptureId < 0 && GetForegroundWindow() == m_hwnd;
        if(padNav)
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }
    ImGui_ImplWin32_NewFrame();
    {
        // D-pad Left / Right switch between the category rail and the settings pane, handled here and not by ImGui's directional move
        // (which depends on where the focus happens to be): the press is turned into a focus request (s_padFocus) and hidden from ImGui.
        // Not while a control is being tweaked or a list is open: there Left / Right belong to that control.
        ImGuiIO&      io = ImGui::GetIO();
        ImGuiContext& g  = *ImGui::GetCurrentContext();
        static bool   prevL = false, prevR = false;
        const unsigned b = Pad::Buttons();
        const bool     L = (b & Pad::LEFT) != 0, R = (b & Pad::RIGHT) != 0;
        const bool     busy = g.ActiveId != 0 || g.OpenPopupStack.Size > 0;
        if(m_options.ui && (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) && !busy)
        {
            if(L && !prevL)
                s_padFocus = 1;
            if(R && !prevR)
                s_padFocus = 2;
            // the events are applied in order within the frame (no trickling): the key ends up released for ImGui
            io.ConfigInputTrickleEventQueue = false;
            if(L)
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
            if(R)
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
        }
        prevL = L;
        prevR = R;
    }
    {
        // HUD scale (set by dragging the corner of the HUD): sizes and font follow, the layout is the same at every scale.
        ImGuiIO& io = ImGui::GetIO();
        float    k  = std::clamp(HudScale(), 0.6f, 2.0f);
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
            if(s_lastScale > 0.0f && s_hudSize.x > 0.0f && fabsf(HudScale() - s_lastScale) > 1e-4f)
            {
                const float k = HudScale() / s_lastScale; // the window follows the new scale
                ImGui::SetNextWindowSize(ImVec2(s_hudSize.x * k, s_hudSize.y * k), ImGuiCond_Always);
            }
            else
                ImGui::SetNextWindowSize(mainWindowSize, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(mainWindowPos, ImGuiCond_FirstUseEver);
        }
        s_lastScale = HudScale();
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
            static const char* catKeys[] = { "Input / Output", "Render / Shaders", "3D", "Profiles", "Hotkeys", "Appearance", "Application" };
            static const int   catIcon[] = { 1, 2, 3, 4, 7, 6, 5 }; // icon of each category (see DrawCategoryIcon)
            const int          catCount  = (int)(sizeof(catKeys) / sizeof(catKeys[0]));
            static int         s_cat     = 0;
            static int         s_lastCat = -1;
            s_cat                        = std::clamp(s_cat, 0, catCount - 1);

            // ---- category rail ----
            ImGui::SetCursorPos(ImVec2(0, headerH));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.7f, fs * 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
            ImGui::BeginChild("##rail", ImVec2(railW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, 0);
            s_railWin = ImGui::GetCurrentWindow();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            for(int i = 0; i < catCount; i++)
            {
                ImGui::PushID(i);
                const ImVec2 sz(ImGui::GetContentRegionAvail().x, fs * 2.7f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if(ImGui::InvisibleButton("##cat", sz))
                    s_cat = i;
                const bool  hov = ImGui::IsItemHovered(), sel = i == s_cat;
                ImDrawList* dl  = ImGui::GetWindowDrawList();
                const ImVec2 q(p.x + sz.x, p.y + sz.y);
                if(s_padFocus == 1 && i == s_cat)
                {
                    // gamepad: the focus goes to the selected category
                    ImGuiWindow* w = ImGui::GetCurrentWindow();
                    ImGui::FocusWindow(w);
                    ImGui::SetFocusID(ImGui::GetItemID(), w);
                    ImGui::SetNavCursorVisible(true);
                    s_padFocus = 0;
                }
                if(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
                    dl->AddRect(ImVec2(p.x + 3.0f, p.y + 3.0f), ImVec2(q.x - 3.0f, q.y - 3.0f), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f); // gamepad focus
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
            s_paneWin = ImGui::GetCurrentWindow();
            if(s_padFocus == 2)
            {
                // gamepad: the focus goes to the settings (the control that had it last, else the first one)
                ImGui::FocusWindow(s_paneWin);
                ImGui::NavInitWindow(s_paneWin, false);
                ImGui::SetNavCursorVisible(true);
                s_padFocus = 0;
            }
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
            ImGui::PushItemWidth(-fs * 14.0f); // controls line up in one column, the label is on their right

            if(s_cat == 1)
            {
            SectionHeader(Tr("Render Parameters"));

            if(ImGui::BeginCombo(Tr("Monitor Type"), m_monitorTypes[m_pending.monitorType].c_str(), 0))
            {
                for(int m = 0; m < m_monitorTypes.size(); m++)
                {
                    auto selected = m == m_pending.monitorType;
                    if(ImGui::Selectable(m_monitorTypes[m].c_str(), selected))
                    {
                        m_pending.monitorType = m;
                        m_options.monitorType = m; // applied live
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // ---- SubFrames: one slider, "Auto" before 1 (only while a program is captured) ----
            const int activeMode = m_options.EffectiveSubFramesMode();
            if(m_pending.subFramesMode != SF_MANUAL && activeMode == SF_MANUAL)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr("Auto needs a captured process: choose one in Capture Input. Manual is used until then."));
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
            else
            {
                const bool haveProcess       = m_options.captureWindow != nullptr;
                int        subFramesSetting = activeMode == SF_AUTO ? 0 : (m_options.subFramesSetting > 0 ? m_options.subFramesSetting : m_options.subFrames);
                if(SliderIntStep(Tr("SubFrames"), &subFramesSetting, haveProcess ? 0 : MIN_SUBFRAMES, MAX_SUBFRAMES, 1, Tr("Auto")))
                {
                    const int i = subFramesSetting <= 0 ? SF_AUTO : SF_MANUAL;
                    if(i == SF_MANUAL)
                    {
                        m_options.subFramesSetting = std::clamp(subFramesSetting, MIN_SUBFRAMES, MAX_SUBFRAMES);
                        // applied live by the renderer, no restart (so no hitch while you step through the values)
                        m_options.subFrames = m_options.ResolveSubFrames();
                    }
                    m_pending.subFramesMode = i;
                    if(m_options.emitter.sourceFormat == StereoSource::Off)
                        m_options.subFramesMode = i; // applied live by the renderer (nothing to rebuild without 3D)
                    else
                        SetApplyRequired();
                }
                if(activeMode == SF_AUTO)
                {
                    ImGui::Text(Tr("SubFrames: %d   (game %.1f fps, display %.0f Hz)"), m_options.subFrames, m_gameFps, m_options.vsyncRate);
                    if(m_options.subFrames == 1)
                        ShowAlertMarker(Tr("The effects won't be visible with just one subframe!"));
                    else
                        ShowHelpMarker(Tr("Number of display frames per content frame, determines Content FPS (display Hz / SubFrames).\nAuto (before 1, only while a program is captured, not with 3D): the game frame rate is measured regularly (Detection interval) and SubFrames = display Hz / game fps, e.g. 120 fps on 480 Hz = 4, 240 fps = 2."));
                    ImGui::PushItemWidth(m_fontSize * 16);
                    SliderFloatStep(Tr("Detection interval"), &m_options.autoSub.intervalSec, 0.0f, 5.0f, "%.2f s", 0.25f);
                    ImGui::PopItemWidth();
                    ShowHelpMarker(Tr("How often the game frame rate is measured to choose SubFrames, from 0 to 5 seconds. 0 = as fast as practical (every 0.25 s). Short = follows frame rate changes quickly, long = steadier."));
                }
                else
                {
                    if(m_options.subFrames == 1)
                        ShowAlertMarker(Tr("The effects won't be visible with just one subframe!"));
                    else
                        ShowHelpMarker(Tr("Number of display frames per content frame, determines Content FPS (display Hz / SubFrames).\nAuto (before 1, only while a program is captured, not with 3D): the game frame rate is measured regularly (Detection interval) and SubFrames = display Hz / game fps, e.g. 120 fps on 480 Hz = 4, 240 fps = 2."));
                }
            }

            // "Clear Frames" (Simple BFI shader) sits right below SubFrames
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

            {
                // anti-stutter buffer, in game (content) frames: latency = frames x SubFrames / display Hz
                const bool haveProcess = m_options.captureWindow != nullptr; // only works with a captured program
                if(!haveProcess)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.50f, 0.08f, 0.08f, 0.70f));
                }
                int fb = m_options.frameBuffer;
                if(SliderIntStep(Tr("Frame buffer"), &fb, haveProcess ? -1 : 0, 5, 1, Tr("Auto"), -1))
                    m_options.frameBuffer = std::clamp(fb, haveProcess ? -1 : 0, 5); // applied live by the renderer
                if(!haveProcess)
                    ImGui::PopStyleColor(2);
                if(m_options.FrameBufferIsAuto() && haveProcess && m_options.EffectiveFrameBuffer() == 0)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(0)");
                }
                if(m_options.EffectiveFrameBuffer() > 0 && (m_options.frameBuffer > 0 || haveProcess))
                {
                    ImGui::SameLine();
                    if(m_options.FrameBufferIsAuto())
                        ImGui::TextDisabled("%d,", m_options.EffectiveFrameBuffer());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%.0f ms)", 1000.0 * m_options.EffectiveFrameBuffer() * (std::max)(1, m_options.frameBufferStep) * (std::max)(m_options.subFrames, 1) * ((m_options.StereoAllowed() && m_options.emitter.sourceFormat != StereoSource::Off) ? 2.0 : 1.0) / (std::max)(m_options.vsyncRate, 1.0f));
                }
                ShowHelpMarker(Tr("Anti-stutter buffer: Auto = the size (0 to 5) adjusts itself to the irregularity of the game frame rate (it grows at once after a stall, shrinks slowly, and drops from 1 to 0 only after 4 steady seconds). 0 = off. 1 to 5 = game frames kept in advance to smooth an irregular frame rate, at the cost of latency (frames x SubFrames / display Hz, halved rate with 3D). With SubFrames on a fixed value, a game running at a divisor of the content rate (for 120: 60, 40, 30, 15 fps...) has each of its frames shown for the same number of content frames, evenly. With 2D to 3D (AI) it works the same way; the synchronisation of the depth maps is a separate option (Map buffer, in the 3D panel) and adds up with it. Only works while a program (.exe) is captured, with the same GPU for capture and render."));
            }


            if(activeMode != SF_MANUAL)
            {
                ImGui::Checkbox(Tr("Uniform brightness"), &m_options.uniformBrightness);
                ShowHelpMarker(Tr("Auto changes the SubFrames with the game frame rate, so the brightness of the BFI changes too (more SubFrames = darker). Checked: the picture is kept at the brightness the BFI has at the low FPS threshold, whatever the frame rate (also when the BFI is disabled or doubled below it). The lower the threshold, the darker the picture. Simple BFI only."));
                ImGui::PushItemWidth(m_fontSize * 16);
                SliderFloatStep(Tr("Low FPS threshold"), &m_options.lowFps.threshold, 20.0f, 120.0f, "%.0f fps", 1.0f);
                ImGui::PopItemWidth();
                ShowHelpMarker(Tr("Below this game frame rate the BFI would flicker too slowly. Between the threshold and half of it, the BFI is doubled (47 fps with a 48 threshold = BFI at 94 Hz; each game frame is lit twice). Below half of it, the BFI stays at the threshold rate (24 fps and under = 48 Hz). With Disable, the BFI is switched off below the threshold and the image is shown as it is."));
                static const char* lowModes[] = { "Double the BFI rate", "Disable BFI" };
                const int          lowMode    = std::clamp(m_options.lowFps.mode, 0, 1);
                ImGui::PushItemWidth(m_fontSize * 16);
                if(ImGui::BeginCombo(Tr("Below threshold"), Tr(lowModes[lowMode]), 0))
                {
                    for(int i = 0; i < 2; i++)
                        if(ImGui::Selectable(Tr(lowModes[i]), i == lowMode))
                            m_options.lowFps.mode = i;
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                ShowHelpMarker(Tr("What to do when the game runs below the threshold: double the flash rate of the BFI, or disable the BFI."));

                if(m_lowFps)
                {
                    if(m_lowBypass)
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), Tr("Low FPS: BFI disabled (game %.1f fps)"), m_gameFps);
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), Tr("Low FPS: BFI at %.0f Hz (game %.1f fps)"), m_flickerHz, m_gameFps);
                }
            }

            SectionHeader(Tr("Shader"));

            if(ImGui::BeginCombo(Tr("Type"), Tr(m_shaders[m_options.shaderProfileNo].name.c_str()), 0))
            {
                for(const auto& shader : m_shaders)
                {
                    auto selected = shader.no == m_options.shaderProfileNo;
                    if(ImGui::Selectable(Tr(shader.name.c_str()), selected))
                    {
                        if(shader.no != m_options.shaderProfileNo)
                        {
                            m_options.shaderProfileNo = shader.no;
                            m_liveReloadRequired      = true; // the shader is switched live (no restart)
                        }
                    }
                    if(selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ShowHelpMarker(Tr("Shader effect to use."));

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
            for(const auto& p : m_shaderManager.GetParameterInfos())
            {
                if(p.type == ParameterType::ParamInt && p.name == "Clear Frames" && p.dropdown.empty())
                    continue; // shown under SubFrames
                switch(p.type)
                {
                case ParameterType::ParamFloat:
                    SliderFloatStep(Tr(p.name.c_str()), p.p.fp.value, p.p.fp.min, p.p.fp.max, "%.3f", AutoStep(p.p.fp.min, p.p.fp.max));
                    break;
                case ParameterType::ParamInt: {
                    if(p.name == "LCD Anti-retention")
                    {
                        bool on = *p.p.ip.value != 0; // Force Off / Auto: a check box
                        if(ImGui::Checkbox(Tr(p.name.c_str()), &on))
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
                        ScanWindows(); // enumerating the windows is slow: once when the list opens, not on every frame
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
                // the output screen: connected screens, then the remembered ones that are not connected (kept in the profile with their refresh rates)
                const auto  known = m_options.MonitorsSnapshot();
                auto        label = [&](const Options::KnownMonitor& m) {
                    std::string t = m.name;
                    if(m.hz > 0)
                        t += " (" + std::to_string(m.hz) + " Hz)";
                    if(!m.connected)
                        t += std::string(" [") + Tr("not connected") + "]";
                    return t;
                };
                std::string cur = m_pending.outputOnProcess ? Tr("Process (over the game)") : m_displays[m_pending.shaderDisplayNo].name;
                if(ImGui::BeginCombo(Tr("Output Display"), cur.c_str(), 0))
                {
                    for(const auto& display : m_displays)
                    {
                        auto selected = !m_pending.outputOnProcess && display.no == m_pending.shaderDisplayNo;
                        if(ImGui::Selectable(display.name.c_str(), selected))
                        {
                            m_pending.shaderDisplayNo = display.no;
                            m_pending.outputOnProcess = false;
                            m_options.shaderScreenId.clear();
                            SetApplyRequired();
                        }
                        if(selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    for(const auto& m : known)
                    {
                        if(m.connected)
                            continue;
                        // remembered but not connected: visible (with its refresh rates), not selectable
                        ImGui::Selectable(label(m).c_str(), false, ImGuiSelectableFlags_Disabled);
                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !m.rates.empty())
                        {
                            std::string r;
                            for(int hz : m.rates)
                                r += (r.empty() ? "" : ", ") + std::to_string(hz);
                            ImGui::SetTooltip("%s %s Hz", Tr("Refresh rates:"), r.c_str());
                        }
                    }
                    if(ImGui::Selectable(Tr("Process (over the game)"), m_pending.outputOnProcess))
                    {
                        m_pending.outputOnProcess = true;
                        SetApplyRequired();
                    }
                    ImGui::EndCombo();
                }
            }
            ShowHelpMarker(Tr("Display to output to (your high refresh one).\nProcess: the final image is applied over the window of the captured process and follows it, so a windowed game gets an output of its own size. It needs a profile detection: the linked program is captured and followed; while it is not running, the display chosen before is used.\nHaving just one monitor connected is recommended for best performance."));

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

            if(ImGui::BeginCombo(Tr("Queued Frames"), Tr(m_queuedFrames[m_pending.maxQueuedFrames].c_str()), 0))
            {
                // order shown: 0 (tearing allowed), 1, 2, 3 (default = driver default), 4 ... 14
                std::vector<int> order = { Options::QUEUE_TEARING, 1, 2, 0 };
                for(int v = 4; v < Options::QUEUE_TEARING; v++)
                    order.push_back(v);
                for(const int i : order)
                {
                    auto selected = i == m_pending.maxQueuedFrames;
                    if(ImGui::Selectable(Tr(m_queuedFrames[i].c_str()), selected))
                    {
                        m_pending.maxQueuedFrames = i;
                        // the swap chain keeps its buffers: a lower or equal latency is applied live, a higher one needs a new swap chain
                        if(i != Options::QUEUE_TEARING && !m_options.Tearing() && (i ? i + 1 : 3) <= (int)m_options.swapChainBuffers)
                        {
                            m_options.maxQueuedFrames = i;
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
            if(m_pending.maxQueuedFrames == Options::QUEUE_TEARING)
            {
                {
                    // always shown with 0: tearing breaks the left / right alternation, strongly discouraged with 3D
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_fontSize * 24);
                    ImGui::TextUnformatted(Tr("Warning: 0 (tearing allowed) is strongly discouraged in 3D. Tearing breaks the left / right alternation and gives ghosting and flicker in the glasses. Choose another value."));
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
                ImGui::Checkbox(Tr("Beam racing"), &m_options.beamRacing);
                ShowHelpMarker(Tr("Beam racing: the tear is placed where the effect hides it, by reading the real position of the screen scanout. BFI and plain picture: the flip is sent just before the vertical blank (no visible tear, a little more latency than plain tearing). Not used in 3D (the glasses keep the classic timing). CRT Beam Simulator (top-to-bottom or bottom-to-top scan): the flip may also land above the lit bands, for a lower latency. Uses one processor core while waiting. Needs a display whose scanout position can be read; Nbeam's window must be shown directly (nothing over it)."));
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
            ShowHelpMarker(Tr("Queued presentation frames, increase if you're getting irregular flashing.\nTrade-off between input latency and stability.\nThese are output (Display Hz) frames. Driver default is usually 3.\nOption 0: no queue, tearing allowed for the lowest latency (strongly discouraged in 3D)."));

            if(ImGui::Checkbox(Tr("Hardware sRGB"), &m_pending.hardwareSrgb))
            {
                SetApplyRequired();
            }
            ShowHelpMarker(Tr("Reduces shader cost up to ~30% but with some quality loss and inability to adjust gamma (fixed at ~2.2). For slow iGPUs only."));


            // no Apply button: pending changes are applied as soon as you finish editing (see the end of this section)
            ImGui::Checkbox(Tr("Remember"), &m_options.rememberSettings);

            }
            if(s_cat == 2)
            {
            m_emitterPanel.displayHz = m_options.vsyncRate;
            m_emitterPanel.subFrames = m_options.subFrames;
    m_emitterPanel.stereoBlocked = !m_options.StereoAllowed();
            m_emitterPanel.fontSize  = m_fontSize;
            m_emitterPanel.status    = m_emitterStatus;
            m_emitterPanel.aiStatus  = m_aiStatus;
            m_emitterPanel.aiInferMs = m_aiInferMs;
            m_emitterPanel.aiFps     = m_aiFps;
            m_emitterPanel.aiLatencyMs = m_aiLatencyMs;
            if(RenderEmitterPanel(m_options.emitter, m_emitterPanel))
                m_emitterRestartRequired = true;

            }
            if(s_cat == 3)
            {
            // ---- settings profiles (*.ini in the Profils folder) ----
            {
                if(s_profilesDirty)
                {
                    s_profiles      = Options::ListProfiles();
                    s_profilesDirty = false;
                }
                ImGui::PushItemWidth(m_fontSize * 14);
                if(ImGui::BeginCombo(Tr("Settings Profile"), m_options.profileFile.c_str(), 0))
                {
                    for(const auto& name : s_profiles)
                    {
                        const bool selected = name == m_options.profileFile;
                        if(ImGui::Selectable(name.c_str(), selected) && !selected)
                        {
                            m_options.pendingProfile = name;
                            PostMessage(m_options.outputWindow, WM_USER_RESTART, 0, 0);
                        }
                        if(selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ShowHelpMarker(Tr("Choose one of the .ini files of the Profils folder (next to the exe).\nThe current settings are saved into the active profile first, then the chosen profile is loaded (the renderer restarts)."));
                ImGui::SameLine();
                {
                    bool isDefault = !m_options.defaultProfile.empty() && _stricmp(m_options.defaultProfile.c_str(), m_options.profileFile.c_str()) == 0;
                    if(ImGui::Checkbox(Tr("Default profile"), &isDefault))
                    {
                        std::lock_guard<std::recursive_mutex> linksLock(m_options.linksMutex); // SaveApp (main thread too) reads it
                        m_options.defaultProfile = isDefault ? m_options.profileFile : std::string();
                        m_options.SaveApp();
                    }
                    ShowHelpMarker(Tr("The active profile becomes the default profile: it always comes back when a detected program is no longer running. Only one profile can be the default one.\nThe effects hidden with Show / Hide stay hidden in the default profile; any other profile shows them again when it is loaded."));
                }
                ImGui::SameLine();
                if(ImGui::Button(Tr("Refresh")))
                    s_profilesDirty = true;
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
                        m_options.profileFile = name;
                        m_options.Save(m_shaderManager);
                        m_options.RememberProfile();
                        s_profilesDirty = true;
                        s_newProfile[0] = 0;
                    }
                }
                ShowHelpMarker(Tr("Saves the current settings under a new name (or overwrites an existing profile) and makes it the active profile."));
                ImGui::PopItemWidth();
            }


            SectionHeader(Tr("Profile detection"));
            {
                // the detections are also read by the main thread (ProfileLinkPoll): no change of the list while it reads it
                std::lock_guard<std::recursive_mutex> linksLock(m_options.linksMutex);
                ShowHelpMarker(Tr("When a program is detected running, its profile is loaded. When the program closes, the default profile comes back (or the previous one if no profile is set as default).\nEnter the full path of the .exe or just its file name (e.g. game.exe), or press the \"Add the program to the profiles\" hotkey while the program is in front.\nEach program can have up to 3 profiles, each for a different screen: the profile of the Windows main screen is used first, then the one of another connected screen. The output goes to the screen of the profile."));
                if(s_profilesDirty)
                {
                    s_profiles      = Options::ListProfiles();
                    s_profilesDirty = false;
                }
                int  addAfter = -1;
                bool save     = false;

                // screens known to Nbeam (connected ones, and the ones remembered from the past), refreshed from time to time
                static ULONGLONG s_monTick = 0;
                if(GetTickCount64() - s_monTick > 4000)
                {
                    bool overridden = false; // a detection changes the refresh rate of a screen while its program runs
                    for(const auto& l : m_options.profileLinks)
                        overridden = overridden || (l.hz > 0 && l.enabled && m_options.captureWindow && IsWindow(m_options.captureWindow));
                    m_options.RefreshMonitors(overridden);
                    s_monTick = GetTickCount64();
                }
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
                // "Global settings": one line that sets a value for every detection at once (each detection can still be changed alone afterwards).
                // A control shows the shared value, or "(mixed)" when the detections differ.
                if(!m_options.profileLinks.empty())
                {
                    auto& links = m_options.profileLinks;
                    ImGui::PushID("global");
                    bool allOn = true;
                    for(const auto& l : links)
                        allOn = allOn && l.enabled;
                    if(ImGui::Checkbox("##gon", &allOn))
                    {
                        for(auto& l : links)
                            l.enabled = allOn;
                        save = true;
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Checks / unchecks every detection."));
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(Tr("Global settings"));
                    ShowHelpMarker(Tr("Sets a value for every program at once: checking the box checks every detection, the delay applies to every program, and for each connected screen (1, 2, 3) the profile and the refresh rate chosen here are given to every program on that screen (a program without a detection for that screen gets one). Each program can still be changed on its own line afterwards. \"(mixed)\" = the programs have different values."));
                    ImGui::SameLine();
                    {
                        bool sameDelay = true;
                        for(const auto& l : links)
                            sameDelay = sameDelay && l.delay == links[0].delay;
                        int gdelay = links[0].delay;
                        ImGui::SetNextItemWidth(m_fontSize * 4.0f);
                        if(ImGui::DragInt("##gdelay", &gdelay, 0.2f, 0, 600, sameDelay ? "%d s" : Tr("(mixed)")))
                            for(auto& l : links)
                                l.delay = std::clamp(gdelay, 0, 600);
                        if(ImGui::IsItemDeactivatedAfterEdit())
                            save = true;
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", Tr("Activation delay of every program."));
                    }
                    // one line per connected screen (Windows order, at most 3): profile and refresh rate for every program on that screen
                    {
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
                        const int nScreens = (int)(std::min)(m_displays.size(), (size_t)Options::MAX_LINKS_PER_EXE);
                        for(int d = 0; d < nScreens; d++)
                        {
                            const std::string sid = m_options.ScreenIdOfDisplay((unsigned)d, m_displays);
                            if(sid.empty())
                                continue;
                            const Options::KnownMonitor* mon = nullptr;
                            for(const auto& m : mons)
                                if(m.id == sid)
                                    mon = &m;
                            ImGui::PushID(d);
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s %d", Tr("Screen"), d + 1);
                            if(ImGui::IsItemHovered() && mon)
                                ImGui::SetTooltip("%s", monLabel(*mon).c_str());
                            ImGui::SameLine(m_fontSize * 5.0f);
                            // profile of every program on this screen
                            std::string shared;
                            bool        same = true, firstSeen = true;
                            for(size_t h : heads)
                            {
                                const int k = linkOn(h, sid);
                                const std::string p = k >= 0 ? links[k].profile : std::string();
                                if(firstSeen)
                                    shared = p, firstSeen = false;
                                else if(p != shared)
                                    same = false;
                            }
                            ImGui::SetNextItemWidth(m_fontSize * 7.5f);
                            if(ImGui::BeginCombo("##gsprofile", !same ? Tr("(mixed)") : (shared.empty() ? Tr("(no profile)") : shared.c_str()), 0))
                            {
                                auto apply = [&](const std::string& name) {
                                    std::vector<Options::ProfileLink> added;
                                    for(size_t h : heads)
                                    {
                                        const int k = linkOn(h, sid);
                                        if(k >= 0)
                                            links[k].profile = name;
                                        else if(!name.empty() && countOf(h) < Options::MAX_LINKS_PER_EXE)
                                        {
                                            Options::ProfileLink nl = links[h]; // same program, delay and check box
                                            nl.profile              = name;
                                            nl.screenId             = sid;
                                            nl.hz                   = 0;
                                            added.push_back(nl);
                                        }
                                    }
                                    for(const auto& nl : added)
                                        if(links.size() < 64)
                                            links.push_back(nl);
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
                                ImGui::SetTooltip("%s", Tr("Profile loaded for every program when it runs on this screen."));
                            ImGui::SameLine();
                            // refresh rate of this screen for every program
                            {
                                int  hzShared = 0;
                                bool hzSame = true, hzFirst = true;
                                for(size_t h : heads)
                                {
                                    const int k = linkOn(h, sid);
                                    if(k < 0)
                                        continue;
                                    if(hzFirst)
                                        hzShared = links[k].hz, hzFirst = false;
                                    else if(links[k].hz != hzShared)
                                        hzSame = false;
                                }
                                char curHz[32];
                                if(!hzSame)
                                    snprintf(curHz, sizeof(curHz), "%s", Tr("(mixed)"));
                                else if(hzShared > 0)
                                    snprintf(curHz, sizeof(curHz), "%d Hz", hzShared);
                                else
                                    snprintf(curHz, sizeof(curHz), "%s", Tr("Current Hz"));
                                ImGui::SetNextItemWidth(m_fontSize * 4.6f);
                                if(ImGui::BeginCombo("##gshz", curHz, 0))
                                {
                                    auto applyHz = [&](int hz) {
                                        for(auto& l : links)
                                            if(l.screenId == sid)
                                                l.hz = hz;
                                        save = true;
                                    };
                                    if(ImGui::Selectable(Tr("Current Hz"), hzSame && hzShared == 0))
                                        applyHz(0);
                                    if(mon)
                                        for(int hz : mon->rates)
                                        {
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
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
                // one line per program: its exe, then one (profile, screen) pair per screen on the same line (the pairs that do not fit
                // continue on the next line, under the exe)
                std::vector<bool> done(m_options.profileLinks.size(), false);
                std::vector<int>  removeList;
                for(size_t first = 0; first < m_options.profileLinks.size(); first++)
                {
                    if(done[first])
                        continue;
                    std::vector<size_t> grp { first };
                    done[first] = true;
                    const std::string key = exeKey(m_options.profileLinks[first].exe);
                    if(!key.empty())
                        for(size_t k = first + 1; k < m_options.profileLinks.size(); k++)
                            if(!done[k] && exeKey(m_options.profileLinks[k].exe) == key)
                            {
                                grp.push_back(k);
                                done[k] = true;
                            }
                    auto& head = m_options.profileLinks[first];
                    ImGui::PushID((int)first);
                    bool        en        = head.enabled;
                    const float rowStartX = ImGui::GetCursorPosX();
                    const float limitX    = rowStartX + ImGui::GetContentRegionAvail().x;
                    // right end (window coordinates) of the last item drawn
                    auto lastEnd = [&]() { return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX(); };
                    if(ImGui::Checkbox("##on", &en))
                    {
                        for(size_t k : grp)
                            m_options.profileLinks[k].enabled = en;
                        save = true;
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Enable this detection. Unchecked = ignored (nothing is loaded when the program runs)."));
                    ImGui::SameLine();
                    const float indentX = ImGui::GetCursorPosX();
                    char        buf[520];
                    strncpy_s(buf, head.exe.c_str(), sizeof(buf) - 1);
                    ImGui::SetNextItemWidth(m_fontSize * 8);
                    if(ImGui::InputTextWithHint("##exe", Tr("Path or name of the .exe"), buf, sizeof(buf)))
                        for(size_t k : grp)
                            m_options.profileLinks[k].exe = buf;
                    if(ImGui::IsItemDeactivatedAfterEdit())
                        save = true;
                    const float pairW = m_fontSize * (6.5f + 7.5f + 4.6f) + ImGui::GetStyle().ItemSpacing.x * 3 + m_fontSize * 1.6f;
                    int         dropMember = -1;
                    for(size_t n = 0; n < grp.size(); n++)
                    {
                        auto& link = m_options.profileLinks[grp[n]];
                        if(lastEnd() + ImGui::GetStyle().ItemSpacing.x + pairW <= limitX)
                            ImGui::SameLine(); // same line as the exe (or as the previous pair)
                        else
                            ImGui::SetCursorPosX(indentX); // does not fit: next line, under the exe
                        ImGui::PushID((int)grp[n]);
                        ImGui::SetNextItemWidth(m_fontSize * 6.5f);
                        if(ImGui::BeginCombo("##profile", link.profile.empty() ? Tr("(no profile)") : link.profile.c_str(), 0))
                        {
                            for(const auto& name : s_profiles)
                                if(ImGui::Selectable(name.c_str(), name == link.profile))
                                {
                                    link.profile = name;
                                    save         = true;
                                }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        {
                            // screen of this profile: used when the program runs with that screen connected
                            std::string label = Tr("Choose a screen");
                            for(const auto& m : mons)
                                if(m.id == link.screenId)
                                    label = m.name + (m.connected ? "" : " *");
                            ImGui::SetNextItemWidth(m_fontSize * 7.5f);
                            if(ImGui::BeginCombo("##screen", label.c_str(), 0))
                            {
                                for(const auto& m : mons)
                                {
                                    if(m.id != link.screenId && usedByOther(grp[n], m.id))
                                        continue; // one screen per profile of a program
                                    if(ImGui::Selectable(monLabel(m).c_str(), m.id == link.screenId))
                                    {
                                        link.screenId = m.id;
                                        save          = true;
                                    }
                                    if(ImGui::IsItemHovered() && !m.rates.empty())
                                    {
                                        std::string r;
                                        for(int hz : m.rates)
                                            r += (r.empty() ? "" : ", ") + std::to_string(hz);
                                        ImGui::SetTooltip("%s %s Hz", Tr("Refresh rates:"), r.c_str());
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if(ImGui::IsItemHovered())
                            {
                                std::string full;
                                for(const auto& m : mons)
                                    if(m.id == link.screenId)
                                        full = monLabel(m) + "\n";
                                ImGui::SetTooltip("%s%s", full.c_str(), Tr("Screen of this profile. The screens are recognised by their name and remembered, even when they are not connected.\nWhen several screens are connected, the profile of the Windows main screen is used first."));
                            }
                        }
                        ImGui::SameLine();
                        {
                            // refresh rate of this screen while the program runs (every screen of the program changes when it is detected)
                            char cur[32];
                            if(link.hz > 0)
                                snprintf(cur, sizeof(cur), "%d Hz", link.hz);
                            else
                                snprintf(cur, sizeof(cur), "%s", Tr("Current Hz"));
                            ImGui::SetNextItemWidth(m_fontSize * 4.6f);
                            if(ImGui::BeginCombo("##hz", cur, 0))
                            {
                                std::vector<int> rates;
                                for(const auto& m : mons)
                                    if(m.id == link.screenId)
                                        rates = m.rates; // remembered rates, even when the screen is not connected
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
                        if(n > 0)
                        {
                            ImGui::SameLine();
                            if(ImGui::Button("-", ImVec2(m_fontSize * 1.6f, 0)))
                                dropMember = (int)grp[n];
                            if(ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", Tr("Remove this profile / screen"));
                        }
                        ImGui::PopID();
                    }
                    if(dropMember >= 0)
                        removeList.push_back(dropMember);
                    // shared by the profiles of the program: activation delay, add a screen, remove the program
                    if(lastEnd() + ImGui::GetStyle().ItemSpacing.x + m_fontSize * 8.0f <= limitX)
                        ImGui::SameLine();
                    else
                        ImGui::SetCursorPosX(indentX);
                    ImGui::SetNextItemWidth(m_fontSize * 4.0f);
                    int delay = head.delay;
                    if(ImGui::DragInt("##delay", &delay, 0.2f, 0, 600, "%d s"))
                        for(size_t k : grp)
                            m_options.profileLinks[k].delay = std::clamp(delay, 0, 600);
                    if(ImGui::IsItemDeactivatedAfterEdit())
                        save = true;
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Activation delay: the program must have been running for this many seconds before the profile is applied."));
                    ImGui::SameLine();
                    {
                        const bool canAdd = !key.empty() && (int)grp.size() < Options::MAX_LINKS_PER_EXE && !freeScreen(grp.back()).empty();
                        if(!canAdd)
                            ImGui::BeginDisabled();
                        if(ImGui::Button("+"))
                            addAfter = (int)grp.back();
                        if(!canAdd)
                            ImGui::EndDisabled();
                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("%s", Tr("Add another profile for this program, on another screen (up to 3, one per screen). A screen has to have been connected once to be listed."));
                    }
                    ImGui::SameLine();
                    if(ImGui::Button("X"))
                        for(size_t k : grp)
                            removeList.push_back((int)k);
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, m_fontSize * 0.2f));
                }
                if(ImGui::Button(Tr("Add detection")) && m_options.profileLinks.size() < 64)
                {
                    Options::ProfileLink nl;
                    nl.screenId = cur; // the first profile of a program: the current screen
                    m_options.profileLinks.push_back(nl);
                    save = true;
                }
                // add a program chosen among the ones shown on the taskbar
                {
                    static std::vector<std::string> s_taskbarExes; // scanned when the list is opened
                    ImGui::SameLine();
                    if(ImGui::Button(Tr("Add from the taskbar")))
                    {
                        s_taskbarExes.clear();
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
                            (LPARAM)&s_taskbarExes);
                        std::sort(s_taskbarExes.begin(), s_taskbarExes.end());
                        ImGui::OpenPopup("##taskbarExes");
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Choose a program among the ones open on the taskbar."));
                    if(ImGui::BeginPopup("##taskbarExes"))
                    {
                        int shown = 0;
                        for(const auto& exe : s_taskbarExes)
                        {
                            bool known = false; // programs already in the detections are not listed
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
                                m_options.profileLinks.push_back(nl);
                                save = true;
                            }
                        }
                        if(shown == 0)
                            ImGui::TextUnformatted(Tr("No program open on the taskbar"));
                        ImGui::EndPopup();
                    }
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
                if(ImGui::Checkbox(Tr("Start with Windows"), &m_options.startWithWindows))
                {
                    m_options.ApplyAutoStart();
                    m_options.SaveApp();
                }
                ShowHelpMarker(Tr("Nbeam starts when you sign in to Windows."));
                if(ImGui::Checkbox(Tr("Start minimized"), &m_options.startMinimized))
                    m_options.SaveApp();
                ShowHelpMarker(Tr("Nbeam starts without showing the HUD (reduced to the notification area if that option is on)."));
                if(ImGui::Checkbox(Tr("Window mode"), &m_options.windowMode))
                {
                    m_options.SaveApp();
                    PostMessage(m_options.outputWindow, WM_USER_EFFECTS, 2, 0);
                }
                ShowHelpMarker(Tr("Nbeam opens as a normal window (like Lossless Scaling): the settings are made there, with the effects off. The effects start when a detected program runs or when Activate effects is pressed, and stop when that program closes, when Deactivate effects is pressed or with the Show / hide effects hotkey. While they run, the HUD works as before (its hotkey). Unchecked: the HUD over the screen only, as before."));
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
                    if(ImGui::SliderFloat(Tr("HUD Size"), &s_sizePct, 60.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
                        s_sizeEdit = true;
                    if(ImGui::IsItemDeactivatedAfterEdit())
                    {
                        HudScale() = std::clamp(s_sizePct / 100.0f, 0.6f, 2.0f); // the size of the mode in use (window or HUD over the screen)
                        s_sizeEdit          = false;
                        changed             = true;
                    }
                    ShowHelpMarker(Tr("Size of the whole HUD, 60 to 200 %. Dragging the edges of the HUD stretches it."));
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
                    ShowHelpMarker(Tr("Captured image only, Windows HDR only: brightness of the picture from -1000 to +1000 nits around the SDR white level (0 = unchanged). The HUD is not affected."));
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
                const char*       labels[4] = { Tr("About"), Tr("Benchmark"),
                                                m_options.windowMode ? (m_windowMode ? Tr("Activate effects") : Tr("Deactivate effects")) : (m_options.effectsSuspended ? Tr("Show") : Tr("Hide")),
                                                Tr("Restart") };
                const float       btnH      = ImGui::GetFontSize() + st.FramePadding.y * 2;
                const float       y         = (footerH - btnH) * 0.5f;
                {
                    const ImVec2 a = ImGui::GetWindowPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + ImGui::GetWindowWidth(), a.y + 2.0f), ImGui::GetColorU32(accentDim));
                }
                // right: actions
                float total = st.ItemSpacing.x * 3;
                for(const char* l : labels)
                    total += ImGui::CalcTextSize(l).x + st.FramePadding.x * 2;
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
                if(m_options.windowMode)
                {
                    if(ImGui::Button(labels[2]))
                        PostMessage(m_options.outputWindow, WM_USER_EFFECTS, m_windowMode ? 1 : 0, 0);
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", m_windowMode ? Tr("Starts the effects and the 3D on the screen (this window is hidden while they run). A detected program starts them by itself.")
                                                             : Tr("Stops the effects and the 3D and brings the window back."));
                }
                else
                {
                    if(ImGui::Button(labels[2]))
                        m_options.effectsSuspended = !m_options.effectsSuspended; // all effects and the emitter off / on (see Renderer::EffectsOff, EffectsHidden)
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", Tr("Hide: turns off all the effects and the 3D emitter (the picture is shown as it is). They stay off, even when the HUD is closed, until Show is pressed."));
                }
                ImGui::SameLine();
                if(ImGui::Button(labels[3]))
                    PostMessage(m_options.outputWindow, WM_USER_RESTART, 0, 0);

                ImVec2 popupSize = ImVec2(m_fontSize * 24, m_fontSize * 15);
                ImGui::SetNextWindowSize(popupSize, ImGuiCond_Always);
                ImGui::SetNextWindowPos(ImVec2((viewport.x - popupSize.x) / 2, (viewport.y - popupSize.y) / 2), ImGuiCond_Always);
                if(ImGui::BeginPopup("About"))
                {
                    ImGui::TextUnformatted(GetLanguage() == LANG_FR ? SHADERBEAM_ABOUT_FR : SHADERBEAM_ABOUT);
                    if(ImGui::Button(Tr("Close")))
                        ImGui::CloseCurrentPopup();
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
                    PostMessage(m_options.outputWindow, WM_USER_RESTART, 0, 0);
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
    }
    // a shortcut capture started in the Hotkeys section, then another tab was opened: cancelled (the hotkeys work again)
    if((s_captureId >= 0 || s_padCaptureId >= 0) && s_hotkeysDrawnFrame != ImGui::GetFrameCount())
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

void UI::Stop()
{
    m_ready = false;

    m_logo   = nullptr;
    m_logoSB = nullptr;
    s_iconNbeam = nullptr;
    s_iconSB    = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_smallFont   = nullptr;
    m_largeFont   = nullptr;
    m_captureGPUs = nullptr;
    m_renderGPUs  = nullptr;
}

bool UI::Toggle()
{
    m_options.ui = !m_options.ui;
    if(!m_options.ui)
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

// the HUD is being dragged with the mouse: the renderer redraws it at the full rate meanwhile (30 times a second otherwise)
bool UI::HudMoving() const
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    return m_ready && g && g->MovingWindow != nullptr;
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

void UI::ShowBanner()
{
    if(m_windowMode)
    {
        m_bannerUntil = 0; // window mode: no 3D banner in the window (the effects are off there)
        return;
    }
    const int sec = m_options.hud.bannerSeconds;
    const bool on = m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && m_options.StereoAllowed() && sec > 0;
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
    return m_ready && ((m_bannerUntil && now < m_bannerUntil) || (m_adjustUntil && now < m_adjustUntil) || (m_noteUntil && now < m_noteUntil));
}

// shown for 3 seconds when a 3D mode is active: tells that the emitter is on (or could not start)
void UI::RenderBanner()
{
    static const char* formats[] = { "Off", "SBS", "TB", "Checkerboard", "2D to 3D (AI)" };
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
    else
    {
        bool failed = false;
        for(const auto& line : m_emitterStatus)
            if(line.find("FAILED") != std::string::npos)
                failed = true;
        const int fmt = (int)m_options.emitter.sourceFormat;
        lines.push_back({ failed ? std::string(Tr("3D emitter: NOT FOUND (see Status)")) : std::string(Tr("3D emitter ON  -  ")) + Tr(formats[(fmt >= 0 && fmt < 5) ? fmt : 0]),
                          failed ? IM_COL32(255, 90, 40, 255) : accent });
        if(!failed)
        {
            // 3D refresh rate = half of the emitter (eye switch) rate
            const auto&  s      = m_options.emitter;
            const double syncHz = s.EffectiveSyncHz(m_options.vsyncRate, m_options.subFrames);
            char         buf[64];
            snprintf(buf, sizeof(buf), Tr("3D refresh rate: %.1f Hz"), syncHz / 2.0);
            lines.push_back({ buf, IM_COL32(215, 215, 215, 255) });
        }
    }

    auto  drawList = ImGui::GetForegroundDrawList();
    auto  size     = ImGui::GetMainViewport()->Size;
    float textW    = m_largeFont->CalcTextSizeA(m_fontSize * 3, size.x, 0, "Nbeam 3D Vision").x;
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
    drawList->AddText(boxStart, accent, "Nbeam 3D Vision");
    ImGui::PopFont();
    boxStart.y += m_fontSize * 3;
    for(const auto& l : lines)
    {
        drawList->AddText(boxStart, l.color, l.text.c_str());
        boxStart.y += m_fontSize * 1.4f;
    }
}

void UI::UpdateCaptureTone(bool refreshHdr)
{
    if(refreshHdr)
        RefreshHdrState(m_options.outputWindow); // main thread, once a second
    const bool hdr = m_options.useHdr && WindowsHdrOn(m_options.outputWindow);
    m_options.captureBlack = hdr ? std::clamp(m_options.hud.hdrBlackNits, 0.0f, 20.0f) / 80.0f : 0.0f;
    m_options.captureGrey  = hdr ? std::clamp(m_options.hud.hdrGrey, 0.5f, 2.0f) : 1.0f;
    m_options.captureGain  = hdr ? (std::max)(0.05f, 1.0f + std::clamp(m_options.hud.hdrNits, -1000.0f, 1000.0f) / ((std::max)(s_sdrWhiteScale, 0.1f) * 80.0f)) : 1.0f;
}

void UI::RefreshHotkeyTexts()
{
    s_hotkeyMessage = std::string(Tr("Press ")) + Options::HotkeyText(m_options.hotkeys[HOTKEY_TOGGLEUI]);
    if(m_options.hotkeys[HOTKEY_TOGGLEUI].pad)
        s_hotkeyMessage += " / " + Pad::Text(m_options.hotkeys[HOTKEY_TOGGLEUI].pad);
    s_hotkeyMessage += Tr(" for UI");
    s_hotkeyHelp    = Tr("Global hotkeys (change them in the Hotkeys section):\n\n");
    for(int id : { HOTKEY_TOGGLEUI, HOTKEY_RESTART, HOTKEY_QUIT, HOTKEY_SWAPEYES, HOTKEY_EFFECTS, HOTKEY_DETECTION, HOTKEY_ADDEXE })
        s_hotkeyHelp += std::string("  ") + Tr(Options::HotkeyActionName(id)) + ": " + Options::HotkeyText(m_options.hotkeys[id]) + (m_options.hotkeys[id].pad ? " / " + Pad::Text(m_options.hotkeys[id].pad) : std::string()) + "\n";
}

// Hotkeys section: click a button, then press the new key or combination (any key, modifiers optional). Esc alone cancels, Backspace alone clears.
void UI::RenderHotkeys()
{
    s_hotkeysDrawnFrame = ImGui::GetFrameCount();
    static const int ids[] = { HOTKEY_TOGGLEUI, HOTKEY_RESTART, HOTKEY_QUIT, HOTKEY_SWAPEYES, HOTKEY_EFFECTS, HOTKEY_DETECTION, HOTKEY_ADDEXE, HOTKEY_SUBFRAMES_DEC, HOTKEY_SUBFRAMES_INC, HOTKEY_CLEAR_DEC,
                              HOTKEY_CLEAR_INC, HOTKEY_SYNCOFFSET_DEC, HOTKEY_SYNCOFFSET_INC, HOTKEY_DURATION_DEC, HOTKEY_DURATION_INC };
    auto      restoreAll = [&]() {
        m_options.hotkeysSuspended = false;
        for(int id = 0; id < HOTKEY_COUNT; id++)
            m_options.UnregisterHotkey(id);
        for(int id = 0; id < HOTKEY_COUNT; id++)
            m_options.RegisterHotkey(id);
        m_options.SaveApp(); // hotkeys are global: saved in Nbeam.app.cfg, whatever the active profile
    };

    if(s_captureId >= 0)
    {
        // poll the keyboard (the global hotkeys are unregistered while capturing so they cannot fire)
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
                bool clash = false;
                for(int id : ids)
                    if(id != s_captureId && m_options.hotkeys[id].vk == vk && m_options.hotkeys[id].mods == mods)
                        clash = true;
                if(clash)
                    s_hotkeyError = "Already used by another action.";
                else
                {
                    const auto old = m_options.hotkeys[s_captureId];
                    m_options.hotkeys[s_captureId].mods = mods, m_options.hotkeys[s_captureId].vk = vk;
                    restoreAll(); // registers everything again; check this one
                    if(false)
                    {
                        m_options.hotkeys[s_captureId] = old;
                        s_hotkeyError                  = "That combination is taken by another program.";
                    }
                    else
                        s_hotkeyError.clear();
                }
            }
            s_captureId = -1;
            restoreAll();
            RefreshHotkeyTexts();
            break;
        }
    }

    if(s_padCaptureId >= 0)
    {
        // gamepad combination: wait until every button is released, then record what was held together; Esc cancels, Backspace clears
        const unsigned b = Pad::Buttons();
        bool           done = false;
        if(GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            s_hotkeyError.clear();
            done = true;
        }
        else if(GetAsyncKeyState(VK_BACK) & 0x8000)
        {
            m_options.hotkeys[s_padCaptureId].pad = 0;
            s_hotkeyError.clear();
            done = true;
        }
        else if(!s_padArmed)
        {
            if(b == 0)
                s_padArmed = true;
        }
        else if(b)
            s_padMax |= b;
        else if(s_padMax)
        {
            bool clash = false;
            for(int id : ids)
                if(id != s_padCaptureId && m_options.hotkeys[id].pad == s_padMax)
                    clash = true;
            if(clash)
                s_hotkeyError = "Already used by another action.";
            else
            {
                m_options.hotkeys[s_padCaptureId].pad = s_padMax;
                s_hotkeyError.clear();
            }
            done = true;
        }
        if(done)
        {
            s_padCaptureId = -1;
            s_padMax       = 0;
            restoreAll();
            RefreshHotkeyTexts();
        }
    }

    for(int id : ids)
    {
        ImGui::PushID(id);
        const bool capturing = s_captureId == id;
        std::string label    = capturing ? std::string(Tr("Press keys... (Esc cancels, Backspace clears)")) : Options::HotkeyText(m_options.hotkeys[id]);
        ImGui::TextUnformatted(Tr(Options::HotkeyActionName(id)));
        // the key buttons are lined up on the right edge of the HUD so the action names stay readable
        const float keyWidth = m_fontSize * 11.5f, padWidth = m_fontSize * 10.0f;
        ImGui::SameLine(ImGui::GetCursorPosX() + (std::max)(ImGui::GetContentRegionAvail().x - keyWidth - padWidth - ImGui::GetStyle().ItemSpacing.x, m_fontSize * 9));
        if(ImGui::Button((label + "###hk").c_str(), ImVec2(keyWidth, 0)) && s_captureId < 0 && s_padCaptureId < 0)
        {
            s_captureId = id;
            s_hotkeyError.clear();
            m_options.hotkeysSuspended = true;
            for(int i = 0; i < HOTKEY_COUNT; i++)
                m_options.UnregisterHotkey(i);
        }
        ImGui::SameLine();
        const bool        padCapturing = s_padCaptureId == id;
        const std::string padLabel     = padCapturing ? std::string(Tr("Press buttons...")) : (m_options.hotkeys[id].pad ? Pad::Text(m_options.hotkeys[id].pad) : std::string(Tr("Pad: none")));
        if(ImGui::Button((padLabel + "###pad").c_str(), ImVec2(padWidth, 0)) && s_captureId < 0 && s_padCaptureId < 0)
        {
            s_padCaptureId = id;
            s_padArmed     = false;
            s_padMax       = 0;
            s_hotkeyError.clear();
            m_options.hotkeysSuspended = true;
        }
        ImGui::PopID();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Gamepad (XInput): click a Pad button, then hold the buttons together and release. Esc cancels, Backspace clears. The pad moves the HUD only while the HUD is open and in front."));
    ImGui::PopTextWrapPos();
    if(ImGui::Button(Tr("Reset hotkeys")) && s_captureId < 0 && s_padCaptureId < 0)
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
    m_pending.subFramesMode    = m_options.subFramesMode;
    m_pending.hardwareSrgb     = m_options.hardwareSrgb;
    m_pending.captureMethod    = m_options.captureMethod;
    m_pending.splitScreen      = m_options.splitScreen;
    m_pending.monitorType      = m_options.monitorType;
    m_pending.maxQueuedFrames  = m_options.maxQueuedFrames;
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
    m_options.shaderDisplayNo  = m_pending.shaderDisplayNo;
    m_options.subFramesMode    = m_pending.subFramesMode;
    m_options.hardwareSrgb     = m_pending.hardwareSrgb;
    m_options.captureMethod    = m_pending.captureMethod;
    m_options.splitScreen      = m_pending.splitScreen;
    m_options.monitorType      = m_pending.monitorType;
    m_options.maxQueuedFrames  = m_pending.maxQueuedFrames;
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

// See http://blogs.msdn.com/b/oldnewthing/archive/2007/10/08/5351207.aspx
BOOL IsAltTabWindow(HWND hwnd)
{
    // Start at the root owner
    HWND hwndWalk = GetAncestor(hwnd, GA_ROOTOWNER);

    // See if we are the last active visible popup
    HWND hwndTry;
    while((hwndTry = GetLastActivePopup(hwndWalk)) != hwndTry)
    {
        if(IsWindowVisible(hwndTry))
            break;
        hwndWalk = hwndTry;
    }
    return hwndWalk == hwnd;
}

void UI::AddWindow(HWND hWnd)
{
    wchar_t name[128];
    wchar_t wclass[128];
    if(hWnd != m_options.outputWindow && hWnd != m_hwnd && IsWindowVisible(hWnd) && IsAltTabWindow(hWnd) && !IsIconic(hWnd) && GetWindowText(hWnd, name, 127))
    {
        auto style      = GetWindowLongPtr(hWnd, GWL_STYLE);
        bool fullscreen = !(style & WS_CAPTION) && !(style & WS_THICKFRAME);
        if(fullscreen)
        {
            auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            if(monitor == m_options.captureMonitor)
            {
                RECT windowRect;
                fullscreen =
                    GetWindowRect(hWnd, &windowRect) && windowRect.right - windowRect.left == m_options.outputWidth && windowRect.bottom - windowRect.top == m_options.outputHeight;
            }
        }

        if(GetClassNameW(hWnd, wclass, 127) && (wcscmp(wclass, L"Windows.UI.Core.CoreWindow") == 0 || wcscmp(wclass, L"ApplicationFrameWindow") == 0))
        {
            DWORD cloaked = FALSE;
            if(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked == DWM_CLOAKED_SHELL)
                return;
        }

        std::string displayName = "Window: " + Helpers::WCharToString(name) + " (" + std::to_string((uint64_t)hWnd) + ")";
        m_inputs.emplace_back(hWnd, displayName, fullscreen, 0, 0);
    }
}

static BOOL CALLBACK EnumWindowsProc(_In_ HWND hWnd, _In_ LPARAM lParam)
{
    ((UI*)lParam)->AddWindow(hWnd);
    return true;
}

void UI::ScanWindows()
{
    m_inputs.clear();

    if(m_captures[0].api->SupportsWindowCapture())
    {
        EnumWindows(EnumWindowsProc, (LPARAM)this);
        std::sort(m_inputs.begin(), m_inputs.end(), [](const InputInfo& a, const InputInfo& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    }

    std::vector<InputInfo> desktops;
    for(const auto& capture : m_captures)
    {
        for(const auto& display : m_displays)
        {
            desktops.emplace_back((HWND)0, display.name + " (" + capture.name + ")", true, capture.no, display.no);
        }
    }
    m_inputs.insert(m_inputs.begin(), desktops.begin(), desktops.end());

    /*
    std::sort(m_inputs.begin(), m_inputs.end(), [](const InputInfo& a, const InputInfo& b) {
        if(a.fullscreen == b.fullscreen)
            return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
        else
            return a.fullscreen > b.fullscreen;
    });
    m_inputs.insert(m_inputs.begin(), InputInfo { (HWND)INVALID_HANDLE_VALUE, "-- FULLSCREEN WINDOWS", true });
    for(auto iter = m_inputs.begin(); iter < m_inputs.end(); iter++)
    {
        if(!iter->fullscreen)
        {
            m_inputs.insert(iter, InputInfo { (HWND)INVALID_HANDLE_VALUE, "-- OTHER WINDOWS", false });
            break;
        }
    }*/
}

} // namespace ShaderBeam