/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include "resource.h"

#include "ShaderBeam.h"
#include "Lang.h"
#include "DepthAI.h"
#include "Gamepad.h"
#include <shellapi.h>
#include <map>
#include <shobjidl.h>

ShaderBeam::ShaderBeam s_shaderBeam;

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst; // current instance
WCHAR     szTitle[MAX_LOADSTRING]; // The title bar text
WCHAR     szWindowClass[MAX_LOADSTRING]; // the main window class name
static UINT s_showMsg = 0; // sent by a second launch: show the HUD of the running instance
static bool s_startHidden = false; // started with /tray (Windows startup)

// Forward declarations of functions included in this code module:
ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    s_startHidden = lpCmdLine && wcsstr(lpCmdLine, L"/tray") != nullptr;

    // a single instance: a second launch asks the first one to show its HUD, then leaves
    s_showMsg = RegisterWindowMessageW(L"Nbeam3DVision_ShowHud");
    CreateMutexW(nullptr, FALSE, L"Local\\Nbeam3DVision_SingleInstance");
    if(GetLastError() == ERROR_ALREADY_EXISTS)
    {
        LoadStringW(hInstance, IDC_SHADERBEAM, szWindowClass, MAX_LOADSTRING);
        if(HWND other = FindWindowW(szWindowClass, nullptr))
            PostMessage(other, s_showMsg, 0, 0);
        return 0;
    }

    // tells Windows the program is ready for messages: the "busy" cursor of the start goes away at once
    {
        MSG m;
        PeekMessage(&m, nullptr, 0, 0, PM_NOREMOVE);
    }

    // per-monitor DPI awareness: every screen reports its real pixel size (otherwise a 4K screen at 160 % looked like 2400x1350)
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_SHADERBEAM, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if(!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SHADERBEAM));

    // first start (the profile folder has just been created): warning about the screen
    if(::ShaderBeam::Options::FirstRun())
    {
        const bool fr = ::ShaderBeam::GetLanguage() == LANG_FR;
        MessageBoxW(nullptr,
                    fr ? L"Attention : ce logiciel peut faire scintiller l'\u00e9cran, le marquer temporairement ou d\u00e9finitivement s'il est trop lent. "
                         L"Veuillez suivre les conseils du README. Le r\u00e9sultat peut varier d'un \u00e9cran \u00e0 l'autre."
                       : L"Warning: this software can make the screen flicker, and mark it temporarily or permanently if it is too slow. "
                         L"Please follow the advice in the README. The result may vary from one screen to another.",
                    L"Nbeam 3D Vision",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }

    MSG msg;

    // Main message loop:
    while(GetMessage(&msg, nullptr, 0, 0))
    {
        if(!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}

//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.cbClsExtra    = 0;
    wcex.cbWndExtra    = 0;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SHADERBEAM));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = CreateSolidBrush(0x00000000);
    wcex.lpszMenuName  = 0;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm       = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

static void BringToFront(HWND hWnd, bool activate)
{
    UINT flags = SWP_NOMOVE | SWP_NOSIZE;
    if(!activate)
    {
        flags |= SWP_NOACTIVATE;
    }
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
}

// Gives the keyboard / pad focus to a window of another program, as a click on it would (plain SetForegroundWindow is refused
// when the calling program is not in front: the input queues of the two threads are attached for the call).
static void ForceForeground(HWND h)
{
    if(!h || !IsWindow(h))
        return;
    HWND fg = GetForegroundWindow();
    if(fg == h)
        return;
    const DWORD me  = GetCurrentThreadId();
    const DWORD ft  = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    const bool  att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if(att)
        AttachThreadInput(me, ft, FALSE);
}

static bool      s_appReady = false;
static bool      s_focusFallback = false; // the captured program went to the background: display capture until it is in front again

// Click-through: mouse input goes to the window below (the game). The window stays layered so only WS_EX_TRANSPARENT toggles.
static void SetClickThrough(HWND hWnd, bool through)
{
    LONG ex_style = GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED;
    if(through)
        ex_style |= WS_EX_TRANSPARENT;
    else
        ex_style &= ~WS_EX_TRANSPARENT;
    SetWindowLong(hWnd, GWL_EXSTYLE, ex_style);
}

// Captured process: while it is not the foreground window (another window in front, or minimised) the effects and the 3D
// decoding are off (the raw image is shown); they come back as soon as it is in front again. Nbeam's own window (HUD) is neutral.
static void ProcessFocusPoll(HWND hWnd)
{
    auto&             o = s_shaderBeam.m_options;
    static ULONGLONG  lostSince = 0, lastBottom = 0;
    if(!s_appReady || !s_shaderBeam.m_active)
        return;
    if(!o.captureWindow || !IsWindow(o.captureWindow))
    {
        if(o.processUnfocused)
        {
            o.processUnfocused = false;
            BringToFront(hWnd, false);
        }
        lostSince = 0;
        return;
    }
    HWND fg = GetForegroundWindow();
    if(!fg)
        return;
    DWORD fpid = 0, pid = 0;
    GetWindowThreadProcessId(fg, &fpid);
    if(fpid == GetCurrentProcessId())
        return;
    GetWindowThreadProcessId(o.captureWindow, &pid);
    if(!IsIconic(o.captureWindow) && fpid == pid)
    {
        lostSince = 0;
        if(o.processUnfocused)
        {
            o.processUnfocused = false;
            BringToFront(hWnd, false); // the overlay is back in front of the game
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if(!lostSince)
        lostSince = now;
    else if(now - lostSince > 150)
        o.processUnfocused = true;
    // out of focus: the overlay goes to the very back so the other windows can be used and switched between freely
    if(o.processUnfocused && !o.ui && now - lastBottom > 500)
    {
        SetWindowPos(hWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        lastBottom = now;
    }
}

// ---- notification area (tray) -------------------------------------------------------------------------------------
#define WM_TRAYICON (WM_USER + 10)
#define TRAY_ID 1
#define TRAY_CMD_QUIT 2
#define TRAY_CMD_AUTOSTART 3
#define TRAY_CMD_STARTMIN 4
#define TRAY_CMD_DESKTOPOFF 5
#define TRAY_CMD_PROFILE_BASE 100

static bool          s_trayAdded   = false;
static bool          s_inTray      = false; // sent to the tray: no taskbar button, HUD hidden
static ITaskbarList* s_taskbar     = nullptr;
static ULONGLONG     s_quietUntil  = 0;     // automatic wake / profile switch: a window activation at that moment must not open the HUD
static bool          s_linkActive  = false; // a linked program is running (its profile applied)
static bool          s_tbShown     = true;  // taskbar button state

static std::wstring WideOf(const char* utf8)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if(n > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}

static void TaskbarButton(HWND hWnd, bool show)
{
    s_tbShown = show;
    if(!s_taskbar)
    {
        if(FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList, (void**)&s_taskbar)) || !s_taskbar)
            return;
        s_taskbar->HrInit();
    }
    if(show)
        s_taskbar->AddTab(hWnd);
    else
        s_taskbar->DeleteTab(hWnd);
}

// the icon follows the "Minimize to the notification area" option
static void UpdateTrayIcon(HWND hWnd)
{
    const bool want = s_shaderBeam.m_options.minimizeToTray;
    if(want == s_trayAdded)
        return;
    NOTIFYICONDATAW nid {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hWnd;
    nid.uID              = TRAY_ID;
    if(want)
    {
        nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon            = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_SHADERBEAM));
        wcscpy_s(nid.szTip, L"Nbeam 3D Vision");
        s_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
    else
    {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        s_trayAdded = false;
        if(s_inTray)
        {
            s_inTray = false;
            TaskbarButton(hWnd, true);
        }
    }
}

static void RemoveTrayIcon(HWND hWnd)
{
    if(!s_trayAdded)
        return;
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    s_trayAdded = false;
}

static void LeaveTray(HWND hWnd)
{
    if(!s_inTray)
        return;
    s_inTray = false;
    TaskbarButton(hWnd, true);
}

static void MoveToDisplay(HWND hWnd, HMONITOR monitor);

// make the HUD visible again (taskbar click, restore)
static void ShowHud(HWND hWnd)
{
    LeaveTray(hWnd);
    if(s_shaderBeam.m_options.ui)
        return;
    s_shaderBeam.m_options.ui               = true;
    s_shaderBeam.m_options.effectsSuspended = true;
    SetClickThrough(hWnd, false);
    BringToFront(hWnd, true);
    SetForegroundWindow(hWnd);
}

static void MoveToDisplay(HWND hWnd, HMONITOR monitor)
{
    RECT        clientRect;
    MONITORINFO info;
    info.cbSize = sizeof(info);
    GetMonitorInfo(monitor, &info);
    clientRect.top    = info.rcMonitor.top;
    clientRect.left   = info.rcMonitor.left;
    clientRect.right  = info.rcMonitor.right;
    clientRect.bottom = info.rcMonitor.bottom;
    AdjustWindowRect(&clientRect, GetWindowLong(hWnd, GWL_STYLE), GetMenu(hWnd) != 0);
    SetWindowPos(hWnd, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top, SWP_FRAMECHANGED);
}

// Places the output window: over the captured process (output "Process") or on the chosen display
static void PlaceWindow(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    if(!s_focusFallback && !o.captureProcess.empty() && !(o.captureWindow && IsWindow(o.captureWindow)))
        o.captureWindow = o.FindCaptureWindow();
    RECT r {};
    if(o.ProcessRect(r))
        SetWindowPos(hWnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    else
        MoveToDisplay(hWnd, s_shaderBeam.m_ui.m_displays[o.shaderDisplayNo].monitor);
}

// Output "Process": the output window follows the game window (position at once, size after it stopped changing: the renderer restarts)
static void ProcessFollowPoll(HWND hWnd)
{
    auto&             o         = s_shaderBeam.m_options;
    static bool       hidden    = false;
    static ULONGLONG  diffSince = 0;
    static LONG       lastW = 0, lastH = 0;
    if(!s_appReady || !o.processOutput)
    {
        o.processResizing = false;
        return;
    }
    if(o.captureWindow && IsWindow(o.captureWindow) && IsIconic(o.captureWindow))
    {
        if(!hidden)
        {
            ShowWindow(hWnd, SW_HIDE);
            hidden = true;
        }
        return;
    }
    if(hidden)
    {
        ShowWindow(hWnd, SW_SHOWNA);
        hidden = false;
    }
    RECT r {};
    if(!o.ProcessRect(r))
        return;
    if((unsigned)(r.right - r.left) != o.outputWidth || (unsigned)(r.bottom - r.top) != o.outputHeight)
    {
        // being resized: effects off (raw image) while the size keeps changing, one single restart once it is stable
        const LONG w = r.right - r.left, h = r.bottom - r.top;
        const ULONGLONG now = GetTickCount64();
        if(!diffSince || w != lastW || h != lastH)
        {
            diffSince = now;
            lastW     = w;
            lastH     = h;
        }
        o.processResizing = true;
        if(now - diffSince > 400)
        {
            diffSince = 0;
            PostMessage(hWnd, WM_USER_RESTART, 0, 0);
        }
        return;
    }
    o.processResizing = false;
    diffSince = 0;
    RECT cur {};
    GetWindowRect(hWnd, &cur);
    if(cur.left != r.left || cur.top != r.top)
        SetWindowPos(hWnd, HWND_TOPMOST, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// Capture Process (.exe): as soon as the program runs its window is captured (twice in a row, once a second)
static void ProcessAppearPoll(HWND hWnd)
{
    auto&      o    = s_shaderBeam.m_options;
    static int seen = 0;
    if(o.captureProcess.empty())
        s_focusFallback = false;
    if(s_focusFallback)
    {
        // wait until the program is in front again
        HWND fg = GetForegroundWindow();
        if(!fg || ::ShaderBeam::Options::ProcessNameOfWindow(fg) != o.captureProcess)
            return;
        s_focusFallback = false;
    }
    if(!s_appReady || o.captureProcess.empty() || (o.captureWindow && IsWindow(o.captureWindow)))
    {
        seen = 0;
        return;
    }
    HWND w = o.FindCaptureWindow();
    if(!w)
    {
        seen = 0;
        return;
    }
    if(++seen < 2)
        return;
    seen            = 0;
    o.captureWindow = w;
    ::ShaderBeam::Options::BringToForeground(w); // the program that was just detected comes to the front
    PostMessage(hWnd, WM_USER_RESTART, 0, 0);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance; // Store instance handle in our global variable

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_POPUP | WS_OVERLAPPEDWINDOW | WS_EX_WINDOWEDGE, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if(!hWnd)
    {
        return FALSE;
    }

    LONG cur_style = GetWindowLong(hWnd, GWL_STYLE);
    cur_style &= ~WS_OVERLAPPEDWINDOW;
    SetWindowLong(hWnd, GWL_STYLE, cur_style);
    SetWindowDisplayAffinity(hWnd, WDA_EXCLUDEFROMCAPTURE);

    s_shaderBeam.Create(hWnd);
    PlaceWindow(hWnd);

    // The window is only shown once the renderer is running: no black screen while it starts.
    // Hidden start: "Start minimized", the /tray argument, or a linked program already running (the HUD is not shown).
    // With the notification area option Nbeam then starts asleep, unless a linked program is running.
    auto& opt           = s_shaderBeam.m_options;
    bool  linkedRunning = false;
    for(const auto& link : opt.profileLinks)
        linkedRunning = linkedRunning || (link.enabled && !link.profile.empty() && ::ShaderBeam::Options::IsExeOnTaskbar(link.exe));
    const bool wantMinimized = s_startHidden || opt.startMinimized;
    s_shaderBeam.Start();
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    if(wantMinimized || linkedRunning)
    {
        opt.ui = false;
        SetClickThrough(hWnd, true);
        BringToFront(hWnd, false);
        if(wantMinimized && opt.minimizeToTray)
        {
            UpdateTrayIcon(hWnd);
            s_inTray = true;
            TaskbarButton(hWnd, false);
        }
    }
    else
    {
        // the HUD is shown (and active) when Nbeam opens
        s_shaderBeam.m_options.ui = true;
        SetClickThrough(hWnd, false);
        BringToFront(hWnd, true);
        s_shaderBeam.m_options.BringToForeground(hWnd);
    }
    s_shaderBeam.m_options.RefreshMonitors(false); // connected screens (names, refresh rates) for the profile detections
    SetTimer(hWnd, 1, 1000, NULL);
    SetTimer(hWnd, 2, 15, NULL); // hotkey fallback (see HotkeyPoll)
    s_appReady = true;
    return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
// Hotkeys must work in every circumstance. RegisterHotKey is the primary way (global, works over games), but it fails
// when another program already owns the combination, and its registration can be lost. For those hotkeys, poll the keyboard
// (edge triggered, exact modifiers) and retry registering about once a second.
static void HotkeyPoll(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    if(o.hotkeysSuspended)
        return;
    static bool     down[HOTKEY_COUNT] {};
    static unsigned tick = 0;
    const bool      retry = (++tick % 66) == 0;
    auto            key   = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    unsigned        mods  = 0;
    if(key(VK_CONTROL)) mods |= MOD_CONTROL;
    if(key(VK_MENU)) mods |= MOD_ALT;
    if(key(VK_SHIFT)) mods |= MOD_SHIFT;
    if(key(VK_LWIN) || key(VK_RWIN)) mods |= MOD_WIN;
    for(int id = 0; id < HOTKEY_ADJUST_FIRST; id++) // (the adjustment hotkeys are handled by AdjustPoll)
    {
        if(!o.hotkeys[id].vk || o.hotkeyOk[id])
        {
            down[id] = false;
            continue;
        }
        if(retry && o.RegisterHotkey(id))
            continue;
        const bool pressed = mods == o.hotkeys[id].mods && key((int)o.hotkeys[id].vk);
        if(pressed && !down[id])
            PostMessage(hWnd, WM_HOTKEY, id, 0);
        down[id] = pressed;
    }
}

// Adjustment hotkeys: SubFrames, Clear frames, Sync offset, Open duration (decrease / increase).
// RegisterHotKey keeps the keys away from the game but gives no "held" information, so the keyboard is polled:
// one step per press (timings: 100 us), nothing while the key is held.
static void AdjustApply(int kind, int dir, int step)
{
    auto& o = s_shaderBeam.m_options;
    switch(kind)
    {
    case 0: // SubFrames (Manual mode only)
        if(o.EffectiveSubFramesMode() == ::ShaderBeam::SF_MANUAL && !o.StereoLinked())
        {
            const int cur = o.subFramesSetting > 0 ? o.subFramesSetting : o.subFrames;
            const int v   = std::clamp(cur + dir, (int)MIN_SUBFRAMES, (int)MAX_SUBFRAMES);
            if(v != o.subFramesSetting)
            {
                o.subFramesSetting = v;
                o.subFrames        = o.ResolveSubFrames(); // applied live by the renderer
            }
        }
        break;
    case 1: // Clear frames (BFI shader)
        for(const auto& p : s_shaderBeam.Shaders().GetParameterInfos())
            if(p.type == ::ShaderBeam::ParameterType::ParamInt && p.name == "Clear Frames")
                *p.p.ip.value = std::clamp((std::min)(*p.p.ip.value, (std::max)(p.p.ip.min, o.subFrames)) + dir, p.p.ip.min, (std::max)(p.p.ip.min, (std::min)(p.p.ip.max, o.subFrames)));
        break;
    default: {
        // timings are limited to the eye period (like the sliders)
        const double syncHz = o.emitter.EffectiveSyncHz(o.vsyncRate, o.subFrames);
        const int    range  = syncHz > 0 ? (int)(1e6 / syncHz) : 10000;
        if(kind == 2)
            o.emitter.syncOffsetUs = std::clamp(o.emitter.syncOffsetUs + dir * step, -range, range);
        else
            o.emitter.nvidia.openDurationUs = std::clamp(o.emitter.nvidia.openDurationUs + (float)(dir * step), 0.0f, (float)range);
        break;
    }
    }
    if(kind >= 2)
        s_shaderBeam.m_ui.ShowAdjustBanner(kind); // 3D timings only: values on screen for 3 more seconds
}

static void AdjustPoll()
{
    auto& o = s_shaderBeam.m_options;
    if(!s_appReady || o.hotkeysSuspended)
        return;
    static bool      down[HOTKEY_COUNT] {};
    auto             key  = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    unsigned         mods = 0;
    if(key(VK_CONTROL)) mods |= MOD_CONTROL;
    if(key(VK_MENU)) mods |= MOD_ALT;
    if(key(VK_SHIFT)) mods |= MOD_SHIFT;
    if(key(VK_LWIN) || key(VK_RWIN)) mods |= MOD_WIN;
    for(int id = HOTKEY_ADJUST_FIRST; id < HOTKEY_COUNT; id++)
    {
        const auto& hk = o.hotkeys[id];
        if(!hk.vk || mods != hk.mods || !key((int)hk.vk))
        {
            down[id] = false;
            continue;
        }
        if(down[id])
            continue; // holding the key does nothing: one step per press
        down[id] = true;
        const int kind = (id - HOTKEY_ADJUST_FIRST) / 2;
        const int dir  = ((id - HOTKEY_ADJUST_FIRST) % 2) ? 1 : -1;
        AdjustApply(kind, dir, kind >= 2 ? 100 : 1); // 3D timings: 100 us per press; SubFrames / Clear frames: 1
    }
}

// Gamepad shortcuts (XInput): every action can have a button combination. Edge triggered; when several combinations are held,
// the one that contains the others wins (Back+Start does not also fire an action bound to Back alone).
static void PadPoll(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    ::ShaderBeam::Pad::Poll();
    if(!s_appReady || o.hotkeysSuspended)
        return;
    static bool down[HOTKEY_COUNT] {};
    const unsigned b = ::ShaderBeam::Pad::Buttons();
    bool           held[HOTKEY_COUNT] {};
    for(int id = 0; id < HOTKEY_COUNT; id++)
    {
        const unsigned m = o.hotkeys[id].pad;
        held[id]         = m && (b & m) == m;
    }
    for(int id = 0; id < HOTKEY_COUNT; id++)
    {
        if(!held[id])
            continue;
        for(int other = 0; other < HOTKEY_COUNT; other++)
            if(other != id && held[other] && (o.hotkeys[other].pad & o.hotkeys[id].pad) == o.hotkeys[id].pad && o.hotkeys[other].pad != o.hotkeys[id].pad)
                held[id] = false; // a larger combination containing this one is held
    }
    for(int id = 0; id < HOTKEY_COUNT; id++)
    {
        if(!held[id])
        {
            down[id] = false;
            continue;
        }
        if(down[id])
            continue; // holding does nothing: one step per press
        down[id] = true;
        if(id >= HOTKEY_ADJUST_FIRST)
        {
            const int kind = (id - HOTKEY_ADJUST_FIRST) / 2;
            const int dir  = ((id - HOTKEY_ADJUST_FIRST) % 2) ? 1 : -1;
            AdjustApply(kind, dir, kind >= 2 ? 100 : 1);
        }
        else
            PostMessage(hWnd, WM_HOTKEY, id, 0);
    }
}

// the HUD goes back to the notification area (taskbar button removed) when a linked program starts or stops
static void ReduceHud(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    if(!s_appReady)
        return;
    if(o.ui)
        s_shaderBeam.m_ui.Toggle();
    SetClickThrough(hWnd, true);
    if(o.minimizeToTray)
    {
        UpdateTrayIcon(hWnd);
        s_inTray = true;
        TaskbarButton(hWnd, false);
    }
}

struct HzTarget { std::string screenId; int wanted; };
struct HzSaved { std::string screenId; int orig; };
static std::vector<HzTarget> s_hzTargets; // refresh rates wanted while the detected program runs (one per screen of the program)
static std::vector<HzSaved>  s_hzSaved;   // the original rates, restored when the program closes
static ULONGLONG s_restartAt     = 0; // restart of the renderer once the new refresh rate has settled (and 3D comes back)
static ULONGLONG s_ownChangeUntil = 0; // display changes made by Nbeam itself (refresh rate): no safety restart for them
static ULONGLONG s_displayChangeAt = 0; // a display / resolution change made by something else: Nbeam restarts
static bool      s_refreshDisplays = false;

// Changes the refresh rate of a display. The 3D is cut just before (the black screen of the mode change) and comes back
// avg + 0.25 s after the change started, the average being learned from the previous changes (kept in Nbeam.app.cfg).
static bool ChangeRefreshRate(HWND hWnd, HMONITOR mon, int hz)
{
    auto& o = s_shaderBeam.m_options;
    if(o.emitter.sourceFormat != ::ShaderBeam::StereoSource::Off && !o.emitter.hold)
    {
        o.emitter.hold = true;
        SendMessage(hWnd, WM_USER_RESTART, 0, 0); // renderer without 3D right before the black screen
    }
    const ULONGLONG t0 = GetTickCount64();
    s_ownChangeUntil   = t0 + 15000;
    const bool ok      = ::ShaderBeam::Options::SetDisplayRefreshRate(mon, hz);
    const ULONGLONG now = GetTickCount64();
    const double    d   = (double)(now - t0);
    if(ok)
    {
        const int n    = (std::min)(o.hzSwitchCount, 8);
        o.hzSwitchMs   = std::clamp((o.hzSwitchMs * n + d) / (n + 1), 100.0, 10000.0);
        o.hzSwitchCount++;
        o.SaveApp();
    }
    s_ownChangeUntil = now + 3000;
    // 3D (and the renderer for the new rate) come back: average duration of the change - time already spent + 0.25 s
    s_restartAt = now + (ULONGLONG)((std::max)(0.0, o.hzSwitchMs - d) + 250.0);
    return ok;
}

static HMONITOR HzMonitor(const std::string& screenId)
{
    const auto& displays = s_shaderBeam.m_ui.m_displays;
    const int   n        = s_shaderBeam.m_options.DisplayNoForScreen(screenId, displays);
    return n >= 0 && n < (int)displays.size() ? displays[n].monitor : (HMONITOR)0;
}

static void RestoreRefreshRate(HWND hWnd)
{
    auto saved = s_hzSaved;
    s_hzSaved.clear();
    for(const auto& sv : saved)
    {
        const HMONITOR mon = HzMonitor(sv.screenId);
        if(mon && ::ShaderBeam::Options::DisplayRefreshRate(mon) != sv.orig)
            ChangeRefreshRate(hWnd, mon, sv.orig);
    }
}

// quitting: the rates come back at once, nothing to restart
static void RestoreRefreshRateNow()
{
    for(const auto& sv : s_hzSaved)
        if(const HMONITOR mon = HzMonitor(sv.screenId))
            ::ShaderBeam::Options::SetDisplayRefreshRate(mon, sv.orig);
    s_hzSaved.clear();
    s_hzTargets.clear();
}

// Refresh rates of the detection (one per screen of the detected program): applied while the program is captured, every screen of the
// program changes at once; the original rates come back when it closes. Checked once a second; the renderer restarts once the new rates
// have settled.
static void RefreshRatePoll(HWND hWnd)
{
    auto&                      o         = s_shaderBeam.m_options;
    static std::map<std::string, int> failedHz;
    static ULONGLONG           holdSince = 0;
    if(!s_appReady)
        return;
    // safety: 3D is never held off for long when no change comes
    if(!o.emitter.hold)
        holdSince = 0;
    else if(holdSince == 0)
        holdSince = GetTickCount64();
    else if(!s_restartAt && GetTickCount64() - holdSince > 8000)
    {
        holdSince      = 0;
        o.emitter.hold = false;
        PostMessage(hWnd, WM_USER_RESTART, 0, 0);
    }
    const bool active = !s_hzTargets.empty() && o.captureWindow && IsWindow(o.captureWindow);
    if(s_restartAt && GetTickCount64() >= s_restartAt)
    {
        s_restartAt    = 0;
        o.emitter.hold = false; // 3D comes back
        PostMessage(hWnd, WM_USER_RESTART, 0, 0); // vsync rate / sub frames follow the new refresh rates
        return;
    }
    if(s_restartAt)
        return; // a change is settling
    if(active)
    {
        bool anyPending = false;
        for(const auto& t : s_hzTargets)
        {
            const HMONITOR mon = HzMonitor(t.screenId);
            if(!mon)
                continue; // that screen is not connected
            const int cur = ::ShaderBeam::Options::DisplayRefreshRate(mon);
            if(cur <= 0 || cur == t.wanted)
                continue;
            anyPending = true;
            if(failedHz[t.screenId] == t.wanted)
                continue; // refused by Windows: do not try again every second
            if(std::none_of(s_hzSaved.begin(), s_hzSaved.end(), [&](const HzSaved& sv) { return sv.screenId == t.screenId; }))
                s_hzSaved.push_back({ t.screenId, cur });
            ChangeRefreshRate(hWnd, mon, t.wanted);
            if(::ShaderBeam::Options::DisplayRefreshRate(mon) != t.wanted)
                failedHz[t.screenId] = t.wanted;
        }
        if(!anyPending && o.emitter.hold && !s_restartAt)
        {
            o.emitter.hold = false; // already at the wanted rates: nothing to wait for
            PostMessage(hWnd, WM_USER_RESTART, 0, 0);
        }
    }
    else if(!s_hzSaved.empty())
        RestoreRefreshRate(hWnd);
    if(!active)
        failedHz.clear();
}

// A display or resolution change that Nbeam did not make itself (screen connected / disconnected, resolution changed in Windows
// or by a game): the renderer restarts after a short moment, with the list of the displays read again.
static void DisplayChangePoll(HWND hWnd)
{
    if(!s_appReady || !s_displayChangeAt || GetTickCount64() - s_displayChangeAt < 1000)
        return;
    s_displayChangeAt = 0;
    s_refreshDisplays = true;
    PostMessage(hWnd, WM_USER_RESTART, 0, 0);
}

// Profile links: when a linked program is running its profile is loaded (the same restart as choosing it in the HUD); when it
// closes the profile that was active before comes back. Checked once a second; a program must be seen twice in a row.
static void ProfileLinkPoll(HWND hWnd)
{
    auto&              o         = s_shaderBeam.m_options;
    static int         active    = -1;
    static int         candidate = -1;
    static ULONGLONG   candidateSince = 0;
    static std::string previous;
    static int         prevShaderNo = 0, prevCaptureNo = 0;
    if(!s_appReady)
        return;
    // Detections of the same program (up to 3, each for a different screen): the profile of the Windows main display wins, then the
    // profile of another connected screen; if none of its screens is connected the first one is used.
    int match = -1;
    {
        const auto            mons = o.MonitorsSnapshot();
        std::string           group;
        int                   inGroup = 0, bestScore = -1;
        std::map<std::string, bool> running;
        std::map<std::string, std::string> winScreen; // screen where each running program has its window
        for(size_t i = 0; i < o.profileLinks.size(); i++)
        {
            const auto& l = o.profileLinks[i];
            if(!l.enabled || l.profile.empty())
                continue;
            const std::string key = ::ShaderBeam::Options::NormalizeExeName(l.exe);
            if(key.empty() || (!group.empty() && key != group))
                continue;
            auto it = running.find(key);
            if(it == running.end())
                it = running.emplace(key, ::ShaderBeam::Options::IsExeOnTaskbar(l.exe)).first;
            if(!it->second)
                continue;
            if(winScreen.find(key) == winScreen.end())
            {
                int same = 0; // the screen of the window only matters when the program has several detections
                for(const auto& other : o.profileLinks)
                    if(other.enabled && ::ShaderBeam::Options::NormalizeExeName(other.exe) == key)
                        same++;
                winScreen[key] = same > 1 ? o.ScreenIdOfExeWindow(l.exe) : std::string();
            }
            if(group.empty())
                group = key;
            if(++inGroup > ::ShaderBeam::Options::MAX_LINKS_PER_EXE)
                break;
            int score = 1; // no screen chosen (old detection): works on any screen
            if(!l.screenId.empty())
            {
                score = 0;
                for(const auto& m : mons)
                    if(m.id == l.screenId && m.connected)
                        score = m.primary ? 3 : 2;
                if(score > 0 && !winScreen[key].empty() && l.screenId == winScreen[key])
                    score = 4; // the program is on this screen right now: its detection wins (even on a screen that is not the main one)
            }
            if(score > bestScore)
            {
                bestScore = score;
                match     = (int)i;
            }
        }
    }
    if(match >= 0)
    {
        if(match == active)
            return;
        if(match != candidate)
        {
            candidate      = match;
            candidateSince = GetTickCount64();
            return;
        }
        if(GetTickCount64() - candidateSince < (ULONGLONG)o.profileLinks[match].delay * 1000ULL)
            return; // activation delay of the link
        const std::string target = o.profileLinks[match].profile;
        if(!std::filesystem::exists(::ShaderBeam::Options::ProfilesDir() / target))
            return;
        if(active < 0)
        {
            previous     = o.profileFile;
            prevShaderNo = o.shaderDisplayNo;
            prevCaptureNo = o.captureDisplayNo;
        }
        {
            // the screen of this detection (when it is connected); the output and the capture of the profile follow it
            const auto& sid = o.profileLinks[match].screenId;
            const int   dn  = sid.empty() ? -1 : o.DisplayNoForScreen(sid, s_shaderBeam.m_ui.m_displays);
            o.linkShaderDisplayNo  = dn;
            o.linkCaptureDisplayNo = dn;
        }
        active    = match;
        candidate = -1;
        if(target != o.profileFile)
            o.pendingProfile = target;
        s_quietUntil = GetTickCount64() + 3000;
        s_linkActive = true;
        // the linked program is the captured process (its window, and the output over it with the "Process" output)
        o.captureProcess = ::ShaderBeam::Options::NormalizeExeName(o.profileLinks[match].exe);
        o.captureTitle.clear();
        s_focusFallback = false;
        o.captureWindow = o.captureProcess.empty() ? NULL : o.FindCaptureWindow();
        ReduceHud(hWnd);
        if(o.captureWindow)
            ::ShaderBeam::Options::BringToForeground(o.captureWindow); // the detected program comes to the front
        {
            // refresh rates of the detections of this program (one per screen): every screen changes; 3D only starts after the change
            s_hzTargets.clear();
            const std::string key = ::ShaderBeam::Options::NormalizeExeName(o.profileLinks[match].exe);
            int               n   = 0;
            for(const auto& l : o.profileLinks)
                if(l.enabled && l.hz > 0 && !l.screenId.empty() && ::ShaderBeam::Options::NormalizeExeName(l.exe) == key && ++n <= ::ShaderBeam::Options::MAX_LINKS_PER_EXE)
                    s_hzTargets.push_back({ l.screenId, l.hz });
            bool stereo = false;
            ::ShaderBeam::Options::PeekProfile(target, stereo);
            if(stereo && o.captureWindow)
                for(const auto& t : s_hzTargets)
                    if(const HMONITOR mon = HzMonitor(t.screenId))
                    {
                        const int cur = ::ShaderBeam::Options::DisplayRefreshRate(mon);
                        if(cur > 0 && cur != t.wanted)
                            o.emitter.hold = true;
                    }
        }
        PostMessage(hWnd, WM_USER_RESTART, 0, 0);
    }
    else
    {
        candidate = -1;
        if(active >= 0)
        {
            active       = -1;
            s_linkActive = false;
            std::string back = o.profileFile; // the profile that comes back
            if(!previous.empty() && previous != o.profileFile && std::filesystem::exists(::ShaderBeam::Options::ProfilesDir() / previous))
            {
                o.pendingProfile = previous;
                back             = previous;
            }
            previous.clear();
            o.linkShaderDisplayNo  = prevShaderNo; // back to the screen used before the detection
            o.linkCaptureDisplayNo = prevCaptureNo;
            s_quietUntil = GetTickCount64() + 3000;
            o.captureProcess.clear();
            o.captureTitle.clear();
            o.captureWindow = NULL;
            s_focusFallback = false;
            ReduceHud(hWnd);
            s_hzTargets.clear();
            if(!s_hzSaved.empty())
            {
                // The rates come back: the previous profile is applied right now (3D cut at the same moment when it has no 3D, held until
                // the change is done when it has), then the rates are restored.
                bool stereo = false;
                ::ShaderBeam::Options::PeekProfile(back, stereo);
                o.emitter.hold = stereo;
                SendMessage(hWnd, WM_USER_RESTART, 0, 0);
                RestoreRefreshRate(hWnd);
            }
            else
                PostMessage(hWnd, WM_USER_RESTART, 0, 0);
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if(message == WM_TRAYICON)
    {
        if(LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK)
            ShowHud(hWnd);
        else if(LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            auto&       o    = s_shaderBeam.m_options;
            HMENU       menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | (o.startWithWindows ? MF_CHECKED : 0), TRAY_CMD_AUTOSTART, WideOf(::ShaderBeam::Tr("Start with Windows")).c_str());
            AppendMenuW(menu, MF_STRING | (o.startMinimized ? MF_CHECKED : 0), TRAY_CMD_STARTMIN, WideOf(::ShaderBeam::Tr("Start minimized")).c_str());
            AppendMenuW(menu, MF_STRING | (o.desktopEffectsOff ? MF_CHECKED : 0), TRAY_CMD_DESKTOPOFF, WideOf(::ShaderBeam::Tr("Effects always disabled (desktop capture)")).c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            // "Settings profile": the available profiles (the submenu opens when the mouse rests on it)
            const auto profiles = ::ShaderBeam::Options::ListProfiles();
            HMENU      sub      = CreatePopupMenu();
            for(size_t i = 0; i < profiles.size(); i++)
                AppendMenuW(sub, MF_STRING | (_stricmp(profiles[i].c_str(), o.profileFile.c_str()) == 0 ? MF_CHECKED : 0), (UINT_PTR)(TRAY_CMD_PROFILE_BASE + i), WideOf(profiles[i].c_str()).c_str());
            if(profiles.empty())
                AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"-");
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, WideOf(::ShaderBeam::Tr("Settings profile")).c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, TRAY_CMD_QUIT, WideOf(::ShaderBeam::Tr("Quit")).c_str());
            POINT pt;
            GetCursorPos(&pt);
            s_quietUntil = GetTickCount64() + 600000; // activating the window for the menu must not open the HUD
            SetForegroundWindow(hWnd); // required for the menu to close when clicking elsewhere
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            s_quietUntil = GetTickCount64() + 1000;
            PostMessage(hWnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
            if(cmd == TRAY_CMD_AUTOSTART)
            {
                o.startWithWindows = !o.startWithWindows;
                o.ApplyAutoStart();
                o.SaveApp();
            }
            else if(cmd == TRAY_CMD_STARTMIN)
            {
                o.startMinimized = !o.startMinimized;
                o.SaveApp();
            }
            else if(cmd == TRAY_CMD_DESKTOPOFF)
            {
                o.desktopEffectsOff = !o.desktopEffectsOff;
                o.SaveApp();
            }
            else if(cmd >= TRAY_CMD_PROFILE_BASE && (size_t)(cmd - TRAY_CMD_PROFILE_BASE) < profiles.size())
            {
                const std::string& name = profiles[cmd - TRAY_CMD_PROFILE_BASE];
                if(_stricmp(name.c_str(), o.profileFile.c_str()) != 0)
                {
                    o.pendingProfile = name;
                    s_quietUntil     = GetTickCount64() + 3000;
                    PostMessage(hWnd, WM_USER_RESTART, 0, 0);
                }
            }
            else if(cmd == TRAY_CMD_QUIT)
                PostMessage(hWnd, WM_DESTROY, 0, 0);
        }
        return 0;
    }
    if(s_showMsg && message == s_showMsg)
    {
        if(s_appReady)
            ShowHud(hWnd);
        return 0;
    }
    if(message == WM_LBUTTONDOWN && !s_shaderBeam.m_ui.MouseRequired())
    {
        // clicking away of ImGui - hide UI
        PostMessage(hWnd, WM_HOTKEY, HOTKEY_TOGGLEUI, 1);
        return true;
    }
    if(s_shaderBeam.m_ui.Input(hWnd, message, wParam, lParam))
    {
        return true;
    }

    switch(message)
    {
    case WM_SYSCOMMAND:
        // clicking the taskbar button: never minimise the overlay, bring the HUD back instead
        if((wParam & 0xFFF0) == SC_MINIMIZE)
        {
            if(s_appReady && !s_inTray && GetTickCount64() > s_quietUntil)
                ShowHud(hWnd);
            return 0;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_ACTIVATE:
        if(LOWORD(wParam) != WA_INACTIVE && s_appReady && !s_inTray && GetTickCount64() > s_quietUntil)
            ShowHud(hWnd);
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_DISPLAYCHANGE:
        if(s_appReady)
        {
            s_shaderBeam.m_options.RefreshMonitors(!s_hzSaved.empty()); // a screen was connected / disconnected or changed mode
            if(GetTickCount64() > s_ownChangeUntil)
                s_displayChangeAt = GetTickCount64(); // not a refresh rate change of Nbeam: safety restart
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_TIMER: {
        if(wParam == 2)
        {
            if(s_appReady && s_tbShown != (!s_shaderBeam.m_options.minimizeToTray || s_shaderBeam.m_options.ui))
                TaskbarButton(hWnd, !s_tbShown); // with the tray option the taskbar button only exists while the HUD is visible
            HotkeyPoll(hWnd);
            AdjustPoll();
            PadPoll(hWnd);
            ProcessFocusPoll(hWnd);
            ProcessFollowPoll(hWnd);
            {
                // detections are checked 4 times a second (was once a second): the profile changes faster
                static ULONGLONG s_linkTick = 0;
                const ULONGLONG  now        = GetTickCount64();
                // not while the HUD is being dragged with the mouse: the scan of the running programs would make it stutter
                const bool dragging = s_shaderBeam.m_options.ui && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
                if(now - s_linkTick >= 250 && !dragging)
                {
                    s_linkTick = now;
                    ProfileLinkPoll(hWnd);
                }
            }
            break;
        }
        if(s_shaderBeam.m_options.ui && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            break; // HUD being dragged: the slow checks of this second wait
        UpdateTrayIcon(hWnd);
        RefreshRatePoll(hWnd);
        DisplayChangePoll(hWnd);
        s_shaderBeam.m_ui.UpdateCaptureTone(true);
        ProcessAppearPoll(hWnd);
        if(s_shaderBeam.m_options.ui)
            s_shaderBeam.UpdateVsyncRate();
        break;
    }
    case WM_USER_RESTART: {
        s_shaderBeam.Stop();
        if(s_refreshDisplays)
        {
            // the displays changed: read the list again (the renderer is stopped)
            s_refreshDisplays = false;
            auto& so          = s_shaderBeam.m_options;
            s_shaderBeam.RefreshDisplayList();
            so.RefreshMonitors(false);
            const unsigned n = (unsigned)s_shaderBeam.m_ui.m_displays.size();
            if(so.shaderDisplayNo >= n)
                so.shaderDisplayNo = 0;
            if(so.captureDisplayNo >= n)
                so.captureDisplayNo = 0;
        }
        s_shaderBeam.SwitchProfile();
        PlaceWindow(hWnd);
        s_shaderBeam.Start();
        break;
    }
    break;
    case WM_USER_BENCHMARK: {
        s_shaderBeam.RunBenchmark();
        break;
    }
    case WM_USER_NOWINDOW: {
        s_shaderBeam.m_options.captureWindow = s_shaderBeam.m_options.captureProcess.empty() ? NULL : s_shaderBeam.m_options.FindCaptureWindow(); // another window of the same program, or the display
        PostMessage(hWnd, WM_USER_RESTART, 0, 0);
        break;
    }
    case WM_HOTKEY: {
        switch(wParam)
        {
        case HOTKEY_QUIT:
            PostMessage(hWnd, WM_DESTROY, 0, 0); // same as Quit in the notification area menu
            break;
        case HOTKEY_RESTART:
            PostMessage(hWnd, WM_USER_RESTART, 0, 0);
            break;
        case HOTKEY_SWAPEYES:
            s_shaderBeam.m_options.emitter.swapEyes = !s_shaderBeam.m_options.emitter.swapEyes;
            break;
        case HOTKEY_DETECTION: {
            // one hotkey for every detection: if one is checked they are all unchecked, otherwise all checked
            auto& o   = s_shaderBeam.m_options;
            bool  any = false;
            for(const auto& l : o.profileLinks)
                any = any || l.enabled;
            for(auto& l : o.profileLinks)
                l.enabled = !any;
            o.SaveApp();
            break;
        }
        case HOTKEY_ADDEXE: {
            // the program in the foreground (not Nbeam, not the Windows shell) is added to the profile detections
            auto& o   = s_shaderBeam.m_options;
            HWND  fg  = GetForegroundWindow();
            DWORD pid = 0;
            if(fg)
                GetWindowThreadProcessId(fg, &pid);
            std::string name;
            if(pid && pid != GetCurrentProcessId())
                if(HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
                {
                    wchar_t path[MAX_PATH * 2];
                    DWORD   size = (DWORD)(sizeof(path) / sizeof(path[0]));
                    if(QueryFullProcessImageNameW(h, 0, path, &size))
                    {
                        char utf[MAX_PATH * 4] = {};
                        WideCharToMultiByte(CP_UTF8, 0, path, (int)size, utf, (int)sizeof(utf) - 1, NULL, NULL);
                        name = ::ShaderBeam::Options::NormalizeExeName(utf);
                    }
                    CloseHandle(h);
                }
            static const char* ignored[] = { "explorer.exe", "searchhost.exe", "searchapp.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe", "applicationframehost.exe", "lockapp.exe", "dwm.exe" };
            for(const char* ig : ignored)
                if(name == ig)
                    name.clear();
            if(name.empty())
            {
                s_shaderBeam.m_ui.ShowNote(::ShaderBeam::Tr("No program in the foreground to add"));
                break;
            }
            bool known = false;
            for(const auto& l : o.profileLinks)
                known = known || ::ShaderBeam::Options::NormalizeExeName(l.exe) == name;
            if(known)
                s_shaderBeam.m_ui.ShowNote(name + " " + ::ShaderBeam::Tr("is already in the list"));
            else if(o.profileLinks.size() < 64)
            {
                ::ShaderBeam::Options::ProfileLink nl;
                nl.exe      = name;
                nl.screenId = o.ScreenIdOfDisplay(o.shaderDisplayNo, s_shaderBeam.m_ui.m_displays);
                o.profileLinks.push_back(nl);
                o.SaveApp();
                s_shaderBeam.m_ui.ShowNote(name + " " + ::ShaderBeam::Tr("added to the profile detections"));
            }
            break;
        }
        case HOTKEY_DESKTOPOFF:
            s_shaderBeam.m_options.desktopEffectsOff = !s_shaderBeam.m_options.desktopEffectsOff;
            s_shaderBeam.m_options.SaveApp();
            break;
        case HOTKEY_TOGGLEUI:
            if(s_shaderBeam.m_ui.Toggle())
            {
                // UI visible: the effects are suspended (the HUD button can turn them back on)
                s_shaderBeam.m_options.effectsSuspended = true;
                LeaveTray(hWnd);
                SetClickThrough(hWnd, false);
                BringToFront(hWnd, true);
                ForceForeground(hWnd); // the HUD gets the focus, as if it had been clicked
            }
            else
            {
                // UI hidden: the effects come back and the captured program gets the focus back, as if it had been clicked
                SetClickThrough(hWnd, true);
                const HWND game = (HWND)s_shaderBeam.m_options.captureWindow;
                if(game && IsWindow(game) && !IsIconic(game))
                    ForceForeground(game);
            }
            break;
        }
    }
    break;
    case WM_CLOSE:
        return 0; // Alt+F4 / taskbar close do nothing: Nbeam is quit with the Quit hotkey, the cross of the HUD or the notification area menu
    case WM_ERASEBKGND:
    case WM_SIZING:
        return 0;
    case WM_PAINT:
        ValidateRect(hWnd, NULL);
        return 0;
    case WM_DESTROY:
        RestoreRefreshRateNow();
        RemoveTrayIcon(hWnd);
        s_shaderBeam.Stop();
        s_shaderBeam.Destroy();
        KillTimer(hWnd, 1);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
