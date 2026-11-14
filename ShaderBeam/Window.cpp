/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include "resource.h"
#include "FrameLimiter.h"

#include "ShaderBeam.h"
#include "Lang.h"
#include "DepthAI.h"
#include "StereoConvert.h"
#include "Gamepad.h"
#include "Helpers.h"
#include "CarTest.h"
#include "HudFont.h"
#include <shellapi.h>
#include <commctrl.h>
// common controls 6 (task dialog with a clickable link for the emitter warning)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <map>
#include <set>
#include <shobjidl.h>
#include <dbghelp.h>
#include <csignal>
#include <deque>
#include <fstream>
#include <thread>
#include <atomic>
#include <ctime>
#include <exception>
#pragma comment(lib, "dbghelp.lib")

// ---- crash log -----------------------------------------------------------------------------------------------------------------
// When Nbeam crashes, Nbeam_crash.txt (next to the exe) receives the error and the call stack with function names and source lines
// (read from Nbeam3DVision.pdb when it is next to the exe or where it was built). It is the only way to find where a crash happens.
static bool          s_quitting     = false; // WM_DESTROY handled: nothing restarts any more
static DWORD         s_mainThreadId = 0;
static volatile LONG s_crashLogged  = 0;
static void         StartKeyboardHook(HWND hWnd); // keyboard hook for the hotkeys (see LowLevelKeyboard)
static void         StopKeyboardHook();
static void         CreateSettingsWindow(HINSTANCE hInstance); // window mode (see RunOrStandby)
static void         RunOrStandby(HWND hWnd);
static void         ShowSettings(bool activate);
static void         SetEffectsOn(HWND hWnd, bool on, bool showWindow);
static void         StopSettingsUI();
// window mode state (see RunOrStandby)
static HWND                              s_setWnd       = NULL;  // the window of the window mode
static bool                              s_setUi        = false; // the HUD is drawn in that window right now
static bool                              s_standby      = false; // window mode, effects off: the overlay is stopped
static bool                              s_effectsOn    = false; // window mode: the effects are wanted (activated)
static bool                              s_effectsByLink = false; // ... activated by a profile detection (stopped when its program closes)
static bool                              s_setShow      = true;  // the window is shown when the effects stop (false: kept in the notification area)
static bool                              s_setActivate  = true;  // ... and gets the focus
static bool                              s_setReduce    = false; // the effects stopped by the button over a game: the window comes back reduced (notification area or minimized), not in front of the game
static bool                              s_hudAfterStart = false; // window mode turned off: the HUD over the screen is shown after the start
// motion test (3D panel, Test): saucers over the whole display instead of the desktop; ended by unchecking it, the Windows key or Escape
static bool                              s_testOn          = false;
static bool                              s_testFromStandby = false; // started from the window: the window comes back at the end
static HANDLE                            s_testProcess     = NULL;  // the motion test process (Nbeam3DVision.exe /cartest, see CarTest.h)
static HWND                              s_testWnd         = NULL;  // ... its window, captured like a game
static ::ShaderBeam::CarTestParams       s_testSent;                // the settings last sent to the test window
static bool                              s_testParamsSent  = false;
static HANDLE                            s_testJob         = NULL;  // the test process is in it: it is killed when Nbeam ends, whatever the way
static const wchar_t*                    EMITTER_DRIVER_URL = L"https://www.nvidia.com/en-us/drivers/nv3dvisionusb/390_41/nv3dvisionusb-dch-driver/";
static bool                              EmitterRecognized();
static void                              EndMotionTest(HWND hWnd);
static void                              StartMotionTest(HWND hWnd);

static void WriteCrashStack(FILE* f, CONTEXT ctx)
{
    HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    STACKFRAME64 sf {};
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;
    for(int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++)
    {
        const DWORD64 a = sf.AddrPC.Offset;
        if(!a)
            break;
        char    mod[MAX_PATH] = "?";
        HMODULE hm            = nullptr;
        if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* modName = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;
        DWORD64         disp = 0;
        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof(line);
        DWORD ld          = 0;
        if(SymFromAddr(proc, a, &disp, sym))
        {
            if(SymGetLineFromAddr64(proc, a, &ld, &line))
                fprintf(f, "  %s!%s + 0x%llx  (%s:%lu)\n", modName, sym->Name, (unsigned long long)disp, line.FileName, line.LineNumber);
            else
                fprintf(f, "  %s!%s + 0x%llx\n", modName, sym->Name, (unsigned long long)disp);
        }
        else
            fprintf(f, "  %s + 0x%llx\n", modName, (unsigned long long)(hm ? a - (DWORD64)hm : a));
    }
    SymCleanup(proc);
}

static FILE* OpenCrashLog(const char* what, unsigned code)
{
    if(!::ShaderBeam::g_logEnabled || InterlockedExchange(&s_crashLogged, 1))
        return nullptr;
    FILE* f = nullptr;
    try
    {
        _wfopen_s(&f, (::ShaderBeam::Options::ExeDir() / L"Nbeam_crash.txt").wstring().c_str(), L"a");
    }
    catch(...)
    { }
    if(!f)
        return nullptr;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "=== %04d-%02d-%02d %02d:%02d:%02d  Nbeam 3.0.0  %s 0x%08X  thread %lu (%s)\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what, code,
            GetCurrentThreadId(), GetCurrentThreadId() == s_mainThreadId ? "main" : "other (render / emitter / AI)");
    return f;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    FILE* f = OpenCrashLog("exception", ep->ExceptionRecord->ExceptionCode);
    if(f)
    {
        if(ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(f, "  %s at address %p\n", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : (ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : "read"),
                    (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        WriteCrashStack(f, *ep->ContextRecord);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void CrashHere(const char* what)
{
    FILE* f = OpenCrashLog(what, 0);
    if(f)
    {
        CONTEXT ctx {};
        RtlCaptureContext(&ctx);
        WriteCrashStack(f, ctx);
        fclose(f);
    }
}

static void InstallCrashLog()
{
    s_mainThreadId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(CrashFilter);
    signal(SIGABRT, [](int) { CrashHere("abort"); });
    std::set_terminate([]() {
        CrashHere("terminate (uncaught C++ exception)");
        std::abort();
    });
    _set_purecall_handler([]() {
        CrashHere("pure virtual call");
        std::abort();
    });
}

ShaderBeam::ShaderBeam s_shaderBeam;

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst; // current instance
WCHAR     szTitle[MAX_LOADSTRING]; // The title bar text
WCHAR     szWindowClass[MAX_LOADSTRING]; // the main window class name
static UINT s_showMsg = 0; // sent by a second launch: show the HUD of the running instance
static bool s_startHidden = false; // started with /tray (Windows startup)
static ULONGLONG s_linkReadyAt = 0; // the profile detections start only after this moment (Nbeam fully started first)

// Forward declarations of functions included in this code module:
ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// Task dialog of Windows (common controls 6, see the manifest below), looked up at run time: plain message box if it is not there
static HRESULT CALLBACK EmitterWarningCallback(HWND, UINT msg, WPARAM, LPARAM lParam, LONG_PTR)
{
    if(msg == TDN_HYPERLINK_CLICKED && lParam)
        ShellExecuteW(nullptr, L"open", (LPCWSTR)lParam, nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

// No 3D emitter recognized, shown before Nbeam starts: OK starts Nbeam, the red cross (or Escape) closes it. Returns true for OK.
static bool ShowEmitterWarning()
{
    const bool   fr    = ::ShaderBeam::GetLanguage() == LANG_FR;
    const wchar_t* title = fr ? L"Attention : \u00e9metteur non reconnu" : L"Warning: emitter not recognized";
    std::wstring text  = fr ? L"Pour utiliser les fonctions 3D st\u00e9r\u00e9oscopiques de Nbeam 3D Vision, veuillez brancher l'\u00e9metteur ou installer son pilote :\n\n"
                            : L"To use the stereoscopic 3D features of Nbeam 3D Vision, please plug in the emitter or install its driver:\n\n";
    using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    auto    td = cc ? (TaskDialogIndirectFn)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
    if(td)
    {
        const std::wstring content = text + L"<a href=\"" + EMITTER_DRIVER_URL + L"\">" + (fr ? L"Pilote NVIDIA 3D Vision USB (390.41)" : L"NVIDIA 3D Vision USB driver (390.41)") + L"</a>";
        TASKDIALOGCONFIG cfg {};
        cfg.cbSize             = sizeof(cfg);
        cfg.dwFlags            = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION; // the red cross / Escape: IDCANCEL (Nbeam closes)
        cfg.dwCommonButtons    = TDCBF_OK_BUTTON;
        cfg.pszWindowTitle     = L"Nbeam 3D Vision";
        cfg.pszMainIcon        = TD_WARNING_ICON;
        cfg.pszMainInstruction = title;
        cfg.pszContent         = content.c_str();
        cfg.pfCallback         = EmitterWarningCallback;
        int button = 0;
        if(SUCCEEDED(td(&cfg, &button, nullptr, nullptr)))
        {
            FreeLibrary(cc);
            return button == IDOK;
        }
    }
    if(cc)
        FreeLibrary(cc);
    text = std::wstring(title) + L"\n\n" + text + EMITTER_DRIVER_URL;
    return MessageBoxW(nullptr, text.c_str(), L"Nbeam 3D Vision", MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST) == IDOK;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    InstallCrashLog(); // Nbeam_crash.txt next to the exe if Nbeam crashes
    std::thread([] { ::ShaderBeam::MissingAiFilesNextToExe(); ::ShaderBeam::StereoConverter::WarmCache(); }).detach(); // AI files and 3D shaders looked at / compiled once, off the render thread
    // the motion test: a process of its own, started by Nbeam (no single instance check, nothing else of Nbeam)
    if(lpCmdLine && wcsstr(lpCmdLine, L"/cartest"))
        return ::ShaderBeam::RunCarTest(hInstance, lpCmdLine);
    s_startHidden = lpCmdLine && wcsstr(lpCmdLine, L"/tray") != nullptr;
    // started by the watchdog (a complete restart): waits until the previous Nbeam has left (it holds the single instance), 20 s at most
    if(lpCmdLine && wcsstr(lpCmdLine, L"/noreadback"))
        ::ShaderBeam::g_autoNoReadback = true; // the previous Nbeam lost the graphics card with the 3D Auto picture analysis
    if(lpCmdLine)
        if(const wchar_t* after = wcsstr(lpCmdLine, L"/restartafter:"))
            if(const DWORD oldPid = wcstoul(after + 14, nullptr, 10))
                if(HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, oldPid))
                {
                    WaitForSingleObject(h, 20000);
                    CloseHandle(h);
                }

    // "Run as administrator": starts again through Windows (UAC) with administrator rights, before the single instance check (this process leaves).
    // If the confirmation is refused Nbeam simply goes on without the rights.
    if(::ShaderBeam::Options::AppRunAsAdmin() && !::ShaderBeam::Options::IsElevated())
    {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        SHELLEXECUTEINFOW sei {};
        sei.cbSize       = sizeof(sei);
        sei.lpVerb       = L"runas";
        sei.lpFile       = exe;
        sei.lpParameters = lpCmdLine;
        sei.nShow        = SW_SHOWNORMAL;
        if(ShellExecuteExW(&sei))
            return 0;
    }

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

    // no 3D emitter recognized: warning with the link of the driver (highlighted, clickable) and an OK button

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

    ::ShaderBeam::NvidiaKeysService(false); // stops the reader of the emitter wheel and button
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
    if(::ShaderBeam::Options::IsScalerExe(::ShaderBeam::Options::ProcessNameOfWindow(h)))
        return; // Lossless Scaling's layer never takes the focus from the game
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

// Lossless Scaling's layer is captured: it ends its scaling (and closes its layer) as soon as the game loses the focus, so while it is
// captured Nbeam never takes the focus, not even for the HUD (the HUD is used with the mouse; the game keeps the keyboard)
static bool ScalerCaptured()
{
    const auto& o = s_shaderBeam.m_options;
    return o.captureWindow && ::ShaderBeam::Options::IsScalerExe(o.captureProcess);
}

static bool      s_appReady = false;
static bool      s_focusFallback = false; // the captured program went to the background: display capture until it is in front again

// Click-through: mouse input goes to the window below (the game). The window stays layered so only WS_EX_TRANSPARENT toggles.
// "Direct presentation" (experimental): the window is never layered, so Windows can show it directly (independent flip / hardware
// plane, one composition less); there is no click-through then, but while the HUD is hidden the overlay never takes the focus (the game
// keeps the keyboard and its raw mouse input) and the clicks over it are ignored (see WndProc).
static void SetClickThrough(HWND hWnd, bool through)
{
    LONG ex_style = GetWindowLong(hWnd, GWL_EXSTYLE);
    if(s_shaderBeam.m_options.directPresent)
    {
        ex_style &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
        if(through || ScalerCaptured())
            ex_style |= WS_EX_NOACTIVATE;
        else
            ex_style &= ~WS_EX_NOACTIVATE;
    }
    else
    {
        ex_style |= WS_EX_LAYERED;
        ex_style &= ~WS_EX_NOACTIVATE;
        if(!through && ScalerCaptured())
            ex_style |= WS_EX_NOACTIVATE; // HUD over Lossless Scaling: clickable, but the game keeps the focus
        if(through)
            ex_style |= WS_EX_TRANSPARENT;
        else
            ex_style &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLong(hWnd, GWL_EXSTYLE, ex_style);
}

static bool s_menuOpen = false; // the notification area menu is shown: the overlay must not be raised above it

// Stays in front of a full-screen "always on top" window of another program (Lossless Scaling's output window, for example): both
// windows are topmost, so the one raised last wins. Checked every ~100 ms; only when such a window is really above the overlay
// is Nbeam raised again (no needless z-order changes). If the other program fights back (more than 20 raises in 2 s) it pauses 5 s.
static void KeepAboveScalerWindow(HWND hWnd)
{
    static ULONGLONG s_lastCheck = 0, s_pauseUntil = 0, s_windowStart = 0;
    static int       s_raises = 0;
    auto&            o        = s_shaderBeam.m_options;
    if(!s_appReady || s_standby || !s_shaderBeam.m_active || o.processUnfocused || s_menuOpen)
        return;
    const ULONGLONG now = GetTickCount64();
    if(now - s_lastCheck < 100 || now < s_pauseUntil)
        return;
    s_lastCheck = now;

    RECT mine;
    if(!IsWindowVisible(hWnd) || IsIconic(hWnd) || !GetWindowRect(hWnd, &mine))
        return;
    const long myArea = (long)(mine.right - mine.left) * (long)(mine.bottom - mine.top);
    if(myArea <= 0)
        return;
    const DWORD ownPid = GetCurrentProcessId();

    bool coveredByScaler = false;
    int  guard           = 0;
    for(HWND w = GetWindow(hWnd, GW_HWNDPREV); w && guard < 200; w = GetWindow(w, GW_HWNDPREV), guard++)
    {
        if(!IsWindowVisible(w) || IsIconic(w))
            continue;
        if(!(GetWindowLong(w, GWL_EXSTYLE) & WS_EX_TOPMOST))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if(pid == ownPid)
            continue;
        RECT r, inter;
        if(!GetWindowRect(w, &r) || !IntersectRect(&inter, &r, &mine))
            continue;
        const long area = (long)(inter.right - inter.left) * (long)(inter.bottom - inter.top);
        if(area * 10 >= myArea * 9) // covers at least 90% of the overlay: a full-screen window, not a tooltip or the taskbar
        {
            coveredByScaler = true;
            break;
        }
    }
    if(!coveredByScaler)
        return;

    if(now - s_windowStart > 2000)
    {
        s_windowStart = now;
        s_raises      = 0;
    }
    if(++s_raises > 20)
    {
        s_pauseUntil = now + 5000;
        s_raises     = 0;
        ::ShaderBeam::LsLog("overlay raised more than 20 times in 2 s: pause 5 s (something keeps going above it)");
        return;
    }
    ::ShaderBeam::LsLog("overlay raised above a full-screen always-on-top window (%d)", s_raises);
    BringToFront(hWnd, false);
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
    if(::ShaderBeam::Options::IsScalerExe(o.captureProcess))
    {
        // Lossless Scaling: the captured window is its scaling layer, which never has the focus (the game does): it counts as in
        // front as long as it is there and shown
        lostSince = 0;
        if(o.processUnfocused && !IsIconic(o.captureWindow))
        {
            o.processUnfocused = false;
            BringToFront(hWnd, false);
        }
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
#define TRAY_CMD_EFFECTS 5
#define TRAY_CMD_RESTART 6
#define TRAY_CMD_ADMIN 7

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
    if(s_standby)
    {
        ShowSettings(true); // window mode, effects off: the window
        return;
    }
    LeaveTray(hWnd);
    if(s_shaderBeam.m_options.ui)
        return;
    s_shaderBeam.m_options.ui               = true;
    SetClickThrough(hWnd, false);
    if(ScalerCaptured())
    {
        BringToFront(hWnd, false); // Lossless Scaling would stop if the game lost the focus
        return;
    }
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
    if(!s_testOn && !s_focusFallback && !o.captureProcess.empty() && !(o.captureWindow && IsWindow(o.captureWindow)))
        o.captureWindow = o.FindCaptureWindow();
    RECT r {};
    if(o.ProcessRect(r))
        SetWindowPos(hWnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    else if(o.shaderDisplayNo < s_shaderBeam.m_ui.m_displays.size())
        MoveToDisplay(hWnd, s_shaderBeam.m_ui.m_displays[o.shaderDisplayNo].monitor); // (no display at all: the start waits for one)
}

// Output "Process": the output window follows the game window (position at once, size after it stopped changing: the renderer restarts)
static void ProcessFollowPoll(HWND hWnd)
{
    auto&             o         = s_shaderBeam.m_options;
    static bool       hidden    = false;
    static ULONGLONG  diffSince = 0;
    static LONG       lastW = 0, lastH = 0;
    if(!s_appReady || !o.processOutput || s_standby)
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
            ::ShaderBeam::LsLog("output 'Process': the game area changed from %ux%u to %ldx%ld, restart", o.outputWidth, o.outputHeight, (long)w, (long)h);
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
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

// Output "Automatic": the game window decides. Full screen or borderless maximized (it covers its screen) -> the output on the Windows main
// display; a window smaller than its screen -> the output "Process" over the game; no game -> the Windows main display. The new state must
// last 1.5 s (no switch while the game changes its mode), and a switch restarts the renderer (as choosing it in the HUD does).
static void OutputAutoPoll(HWND hWnd)
{
    auto&            o         = s_shaderBeam.m_options;
    static int       want      = -1; // 0 main display, 1 over the game
    static ULONGLONG wantSince = 0, lastSwitch = 0;
    if(!o.outputAuto || !s_appReady || s_quitting || s_standby || s_testOn || o.processResizing)
    {
        want = -1;
        return;
    }
    int  state = 0;
    HWND w     = o.captureWindow;
    if(w && IsWindow(w) && !IsIconic(w) && IsWindowVisible(w) && !::ShaderBeam::Options::IsScalerExe(o.captureProcess))
    {
        RECT        r {};
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if(GetWindowRect(w, &r) && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT& m     = mi.rcMonitor;
            const bool  cover = r.left <= m.left + 2 && r.top <= m.top + 2 && r.right >= m.right - 2 && r.bottom >= m.bottom - 2;
            state             = cover ? 0 : 1;
        }
    }
    int mainNo = -1;
    {
        const HMONITOR primary = MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        for(const auto& d : s_shaderBeam.m_ui.m_displays)
            if(d.monitor == primary)
                mainNo = (int)d.no;
    }
    const bool now = state == 1 ? o.outputOnProcess : (!o.outputOnProcess && (mainNo < 0 || o.shaderDisplayNo == mainNo));
    const ULONGLONG t = GetTickCount64();
    if(now)
    {
        want = -1;
        return;
    }
    if(state != want)
    {
        want      = state;
        wantSince = t;
        return;
    }
    if(t - wantSince < 1500 || t - lastSwitch < 3000)
        return;
    lastSwitch = t;
    want       = -1;
    o.outputOnProcess = state == 1;
    if(state == 0 && mainNo >= 0)
        o.shaderDisplayNo = mainNo;
    o.shaderScreenId.clear();
    ::ShaderBeam::LsLog("output automatic: %s", state == 1 ? "over the game (windowed)" : "Windows main display (full screen / no game)");
    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
}

// Nbeam starts again completely (a new process, then this one leaves): used by the watchdog when the graphics card keeps being lost, the
// renderer restarts in a loop or nothing is shown any more. At most 3 times in 10 minutes (the times are kept in Nbeam_selfrestart.txt next to
// the exe), so a problem that comes back at once cannot make Nbeam restart for ever. The new Nbeam starts hidden (/tray) once this one is gone
// (/restartafter:<pid>) and takes the running program again by itself.
static ULONGLONG s_selfRestartAt = 0;
static bool      SelfRestart(HWND hWnd, const char* why)
{
    if(s_quitting || s_selfRestartAt)
        return false;
    const auto        path = ::ShaderBeam::Options::ExeDir() / L"Nbeam_selfrestart.txt";
    const long long   now  = (long long)time(nullptr);
    std::vector<long long> times;
    {
        std::ifstream in(path);
        long long     t = 0;
        while(in >> t)
            if(now >= t && now - t < 600)
                times.push_back(t);
    }
    if(times.size() >= 3)
    {
        ::ShaderBeam::LsLog("full restart refused (already 3 in the last 10 minutes): %s", why);
        return false;
    }
    times.push_back(now);
    {
        std::ofstream out(path, std::ios::trunc);
        for(long long t : times)
            out << t << "\n";
    }
    ::ShaderBeam::LsLog("full restart of Nbeam: %s", why);
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring       cmd = L"\"" + std::wstring(exe) + L"\" /tray /restartafter:" + std::to_wstring(GetCurrentProcessId()) + (::ShaderBeam::g_autoNoReadback ? L" /noreadback" : L"");
    STARTUPINFOW       si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    if(!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        ::ShaderBeam::LsLog("full restart impossible: Windows error %lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    s_selfRestartAt = GetTickCount64();
    PostMessage(hWnd, WM_DESTROY, 0, 0); // (the same way out as the Quit of the tray menu)
    std::thread([]() {
        Sleep(8000); // if Nbeam does not leave by itself, it is ended so that the new one can start
        ::ShaderBeam::LsLogFlush();
        ExitProcess(0);
    }).detach();
    return true;
}

// Watchdog, once a second: the graphics card lost 3 times in 2 minutes, or (while effects are showing) no picture presented for 15 s, restarts
// Nbeam completely. The 3D / BFI renderer restarts alone for a first loss; a loop of them or a black screen needs a clean start.
static void WatchdogPoll(HWND hWnd)
{
    auto&                         o = s_shaderBeam.m_options;
    static std::deque<ULONGLONG>  lost;
    static int                    seen     = 0;
    static ULONGLONG              lastLoss = 0;
    static ULONGLONG              lastPoll = 0;
    const ULONGLONG               now      = GetTickCount64();
    const bool                    slept    = lastPoll && now - lastPoll > 5000; // (the PC was asleep or frozen: nothing to judge for now)
    lastPoll                               = now;
    if(!s_appReady || s_quitting || slept)
        return;
    const int total = ::ShaderBeam::g_deviceLostTotal.load();
    for(; seen < total; seen++)
    {
        lost.push_back(now);
        lastLoss = now;
    }
    while(!lost.empty() && now - lost.front() > 120000)
        lost.pop_front();
    if(lost.size() >= 3)
    {
        lost.clear();
        SelfRestart(hWnd, "the graphics card was lost 3 times in 2 minutes");
        return;
    }
    const ULONGLONG ok = ::ShaderBeam::g_lastPresentOk.load();
    if(ok && s_shaderBeam.m_active && !s_standby && !s_testOn && !o.effectsSuspended && !o.processUnfocused && !o.motionTest && !o.emitter.hold && o.captureWindow && IsWindow(o.captureWindow) &&
       now - lastLoss > 40000 && now - ok > 15000)
    {
        ::ShaderBeam::g_lastPresentOk = now; // (not judged again at once)
        SelfRestart(hWnd, "no picture shown for 15 s (black screen)");
    }
}

// Automatic game frame cap of the detected program (its detection, "Auto fps cap" = RTSS), through the frame limit of the program in RTSS:
// 3D on: half the 3D rate (the limit of the banner); BFI / CRT: the content rate (screen Hz / SubFrames); no effect: no cap. Followed once a
// second (a change of profile or mode changes the cap). RTSS is started minimized when the program is detected and it is not running. The
// previous RTSS limit comes back when the program closes, when the option is unchecked or when Nbeam quits. A failure is tried again every 5 s.
static int s_activeLink = -1; // index of the detection that is active (ProfileLinkPoll), -1 = none
static ULONGLONG s_noWindowRestartAt = 0; // the window of the program closed: one restart at the end of the program (the detection ends), or at this time if it still runs

// Screen id (KnownMonitor::id) of the Windows main display ("" = not known yet)
static std::string PrimaryScreenId()
{
    const HMONITOR    mon = MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW    mi {};
    mi.cbSize = sizeof(mi);
    if(!mon || !GetMonitorInfoW(mon, &mi))
        return "";
    char gdi[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, gdi, (int)sizeof(gdi) - 1, nullptr, nullptr);
    const auto mons = s_shaderBeam.m_options.MonitorsSnapshot();
    for(const auto& m : mons)
        if(m.connected && _stricmp(m.gdi.c_str(), gdi) == 0)
            return m.id;
    for(const auto& m : mons)
        if(m.connected && m.primary)
            return m.id;
    return "";
}

static void FpsCapPoll()
{
    auto&            o       = s_shaderBeam.m_options;
    static HWND      lastWnd = NULL;
    static int       lastCap = 0;
    static bool      applied = false;
    static bool      started = false; // RTSS was started by Nbeam for this detection (once)
    static ULONGLONG lastTry = 0;
    int              cap     = 0;
    HWND             wnd     = NULL;
    if(s_appReady && !s_quitting && !s_testOn && o.rtssAvailable && !o.captureProcess.empty() && o.captureWindow && IsWindow(o.captureWindow))
    {
        std::lock_guard<std::recursive_mutex> lock(o.linksMutex);
        std::vector<const ::ShaderBeam::Options::ProfileLink*> order; // the detection that is active (its screen) first, then the other ones of the program
        if(s_activeLink >= 0 && s_activeLink < (int)o.profileLinks.size())
            order.push_back(&o.profileLinks[s_activeLink]);
        for(const auto& l : o.profileLinks)
            order.push_back(&l);
        for(const auto* lp : order)
            if(lp->enabled && ::ShaderBeam::Options::NormalizeExeName(lp->exe) == o.captureProcess)
            {
                const auto& l = *lp;
                if(l.fpsCapMode == 1)
                {
                    wnd = o.captureWindow;
                    if(o.emitter.sourceFormat != ::ShaderBeam::StereoSource::Off && o.Stereo3DWanted())
                    {
                        const double limit = o.StereoFpsLimit(); // (half the 3D rate + 5 fps of margin)
                        cap                = limit > 5.0 ? (int)std::lround(limit - 5.0) : 0;
                    }
                    else if(o.subFrames > 1 && o.vsyncRate > 1.0f && !o.effectsSuspended)
                        cap = (int)std::lround((double)o.vsyncRate / o.subFrames);
                    cap = cap > 0 ? std::clamp(cap, 20, 500) : 0;
                }
                break; // (the active detection of the program decides: the one of its screen)
            }
    }
    if(!wnd)
        started = false;
    const ULONGLONG now = GetTickCount64();
    if(wnd == lastWnd && cap == lastCap && (cap == 0 || applied || now - lastTry < 5000))
        return;
    const bool changed = wnd != lastWnd || cap != lastCap;
    lastWnd = wnd;
    lastCap = cap;
    lastTry = now;
    auto& limiter = ::ShaderBeam::GetFrameLimiter();
    if(cap > 0 && !::ShaderBeam::RtssRunning() && !started)
    {
        started = true;
        ::ShaderBeam::LsLog("frame cap: RTSS is not running, started minimized: %d", (int)::ShaderBeam::StartRtssMinimized());
        lastTry = now; // (it needs a few seconds before its profiles can be written: tried again in 5 s)
        applied = false;
        return;
    }
    const std::string before = limiter.Status();
    if(cap > 0)
        limiter.Update(wnd, (double)cap);
    else
        limiter.Release();
    const std::string status = limiter.Status();
    applied                  = status.rfind("RTSS:", 0) == 0;
    if(changed || status != before)
        ::ShaderBeam::LsLog("frame cap: %s", status.c_str());
}

// Automatic game frame cap through a ReShade addon that reads its frame limit from a settings file next to the game only when the game starts:
// Display Commander ("Auto fps cap" = Display Commander, DisplayCommander.ini) or ReLimiter ("Auto fps cap" = ReLimiter, relimiter.ini).
// Each program has a frame limit per screen. Nbeam writes the limits of EVERY program at once, outside the start of the games: when Nbeam starts,
// when the Windows main display changes (the limit of the main display), when a limit or a choice changes, and after a game has closed (the addon saves
// its own settings at exit). Only an existing settings file is rewritten: a game without it is ignored. Nothing is injected or hooked.
static void DcCapPoll()
{
    auto&                o       = s_shaderBeam.m_options;
    static unsigned long pid     = 0; // program with Display Commander / ReLimiter that is running (its closing writes everything again)
    static std::string   lastSig;     // what was written the last time (screen + program + addon + limit): another value writes everything again
    static ULONGLONG     again   = 0; // a second pass after a game closed (in case the addon was still saving its file)
    if(!s_appReady || s_quitting || s_testOn)
        return;
    const ULONGLONG now = GetTickCount64();
    // the program of the detection that is running now: its full path is kept for the writing (when "exe" is only a file name)
    if(!o.captureProcess.empty() && o.captureWindow && IsWindow(o.captureWindow))
    {
        bool addon = false;
        {
            std::lock_guard<std::recursive_mutex> lock(o.linksMutex);
            for(const auto& l : o.profileLinks)
                addon = addon || (l.enabled && (l.fpsCapMode == 2 || l.fpsCapMode == 3) && ::ShaderBeam::Options::NormalizeExeName(l.exe) == o.captureProcess);
        }
        unsigned long id = 0;
        if(addon && !pid)
        {
            const auto p = ::ShaderBeam::ProgramPathOfWindow(o.captureWindow, &id);
            if(id && !p.empty())
            {
                pid = id;
                char utf[MAX_PATH * 3] = {};
                WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), utf, (int)sizeof(utf) - 1, nullptr, nullptr);
                bool saved = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(o.linksMutex);
                    for(auto& l : o.profileLinks)
                        if(::ShaderBeam::Options::NormalizeExeName(l.exe) == o.captureProcess && l.path != utf)
                        {
                            l.path = utf;
                            saved  = true;
                        }
                }
                if(saved)
                    o.SaveApp();
            }
        }
    }
    if(pid && !::ShaderBeam::ProgramRunning(pid))
    {
        pid = 0;
        lastSig.clear(); // the game closed: everything is written again now, and once more in 3 s
        again = now + 3000;
    }
    if(again && now >= again)
    {
        again = 0;
        lastSig.clear();
    }
    // the limit of each program: its detection on the Windows main display (or its detection without a screen). When the main display changes,
    // every settings file is written again with the limits of the new main display
    const std::string screen = PrimaryScreenId();
    struct Item
    {
        std::string key, path;
        int         mode, limit;
        bool        onScreen;
    };
    std::vector<Item> items;
    {
        std::lock_guard<std::recursive_mutex> lock(o.linksMutex);
        for(const auto& l : o.profileLinks)
        {
            if(!l.enabled || (l.fpsCapMode != 2 && l.fpsCapMode != 3) || (!l.screenId.empty() && l.screenId != screen))
                continue;
            const bool        full = l.exe.find('\\') != std::string::npos || l.exe.find('/') != std::string::npos;
            const std::string p    = full ? l.exe : l.path;
            if(p.empty())
                continue;
            const std::string key = ::ShaderBeam::Options::NormalizeExeName(l.exe);
            const bool        on  = !l.screenId.empty();
            auto              f   = std::find_if(items.begin(), items.end(), [&](const Item& it) { return it.key == key; });
            if(f == items.end())
                items.push_back({ key, p, l.fpsCapMode, l.fpsLimit, on });
            else if(on && !f->onScreen)
                *f = { key, p, l.fpsCapMode, l.fpsLimit, on }; // (the link of this screen wins over the one without a screen)
        }
    }
    std::string sig = screen;
    for(const auto& it : items)
        sig += "|" + it.path + "#" + std::to_string(it.mode) + "#" + std::to_string(it.limit);
    if(sig == lastSig)
        return;
    lastSig = sig;
    for(const auto& it : items)
    {
        bool               ok   = false;
        const std::wstring wp   = WideOf(it.path.c_str());
        const auto         text = it.mode == 3 ? ::ShaderBeam::RlWriteLimit(wp, it.limit, ok) : ::ShaderBeam::DcWriteLimit(wp, it.limit, ok);
        ::ShaderBeam::LsLog("frame cap (written for every program): %s", text.c_str());
    }
}

// Capture Process (.exe): as soon as the program runs its window is captured (twice in a row, once a second)
static void ProcessAppearPoll(HWND hWnd)
{
    auto&      o    = s_shaderBeam.m_options;
    static int seen = 0;
    if(o.captureProcess.empty())
        s_focusFallback = false;
    if(s_standby || s_testOn)
        return; // window mode, effects off (the program is looked for when the effects start), or glasses test (whole display)
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
    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
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
    s_shaderBeam.m_options.emitterPresent = EmitterRecognized(); // the 3D menu is hidden until an emitter is recognized
    // no 3D emitter recognized: the warning (with the clickable driver link) comes first; Nbeam starts after OK, the red cross closes it
    if(!s_shaderBeam.m_options.emitterPresent && !ShowEmitterWarning())
    {
        ExitProcess(0); // (nothing started yet: Nbeam just leaves, the settings are not touched)
    }
    PlaceWindow(hWnd);
    CreateSettingsWindow(hInstance);

    // The window is only shown once the renderer is running: no black screen while it starts.
    // Hidden start: "Start minimized", the /tray argument, or a linked program already running (the HUD is not shown).
    // With the notification area option Nbeam then starts asleep, unless a linked program is running.
    auto& opt           = s_shaderBeam.m_options;
    bool  linkedRunning = false;
    for(const auto& link : opt.profileLinks)
        linkedRunning = linkedRunning || (link.enabled && !link.profile.empty() && ::ShaderBeam::Options::IsExeOnTaskbar(link.exe));
    const bool wantMinimized = s_startHidden || opt.startMinimized;
    if(opt.windowMode)
    {
        // window mode: the window opens (the effects start later: button, hotkey or a detected program)
        s_effectsOn   = false;
        s_setShow     = !wantMinimized || !opt.minimizeToTray;
        s_setActivate = !wantMinimized;
        RunOrStandby(hWnd);
        SetUnhandledExceptionFilter(CrashFilter);
        if(wantMinimized && s_setWnd)
        {
            if(opt.minimizeToTray)
                UpdateTrayIcon(hWnd);
            else
                ShowWindow(s_setWnd, SW_SHOWMINNOACTIVE);
        }
    }
    else
    {
    s_shaderBeam.Start();
    SetUnhandledExceptionFilter(CrashFilter); // again: a driver loaded by the start may have replaced it
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
    }
    s_shaderBeam.m_options.RefreshMonitors(false); // connected screens (names, refresh rates) for the profile detections
    // A program of a detection already running: Nbeam first starts completely on the default (or last) profile (capture, 3D, AI model loaded),
    // then the detection switches 2.5 s later. Switching while the first start was still going on could hang or crash Nbeam.
    s_linkReadyAt = GetTickCount64() + (linkedRunning ? 2500 : 500);
    SetTimer(hWnd, 1, 1000, NULL);
    SetTimer(hWnd, 2, 15, NULL); // hotkey fallback (see HotkeyPoll)
    StartKeyboardHook(hWnd);     // hotkeys over games that read the keyboard in exclusive mode
    s_shaderBeam.m_options.rtssAvailable = ::ShaderBeam::RtssInstalled(); // once at start: the automatic frame cap by RTSS is greyed out without RTSS
    s_shaderBeam.m_options.dcAvailable   = ::ShaderBeam::DcInstalled();   // (same for Display Commander)
    s_appReady = true;
    ::ShaderBeam::AudioKeepAliveSet(s_shaderBeam.m_options.keepAudio); // inaudible sound: keeps Dolby Atmos active
    ::ShaderBeam::NvidiaKeysService(true); // wheel and button of the NVIDIA emitter, also without 3D
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
//
// Keyboard hook (WH_KEYBOARD_LL) on its own time-critical thread: the keys are seen by Windows itself, before the game reads them,
// so the hotkeys also work over games that read the keyboard in exclusive mode (DirectInput) or raw input, where the polling sees
// nothing. The keys are never swallowed. A hotkey seen by both ways only fires once (FireOnce).
#define WM_USER_LLKEY (WM_USER + 3)
static HWND              s_hookWnd      = NULL;
static DWORD             s_hookThreadId = 0;
static HANDLE            s_hookThread   = NULL;
static ULONGLONG         s_lastFire[HOTKEY_COUNT] {};

static bool FireOnce(int id)
{
    const ULONGLONG now = GetTickCount64();
    if(now - s_lastFire[id] < 200)
        return false; // already fired by the other way (hook / polling) for this press
    s_lastFire[id] = now;
    return true;
}

static LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wParam, LPARAM lParam)
{
    if(code == HC_ACTION)
    {
        static bool      down[256] {};
        static ULONGLONG last = 0;
        const auto*      k    = (const KBDLLHOOKSTRUCT*)lParam;
        const ULONGLONG  now  = GetTickCount64();
        if(now - last > 2000)
            memset(down, 0, sizeof(down)); // a lost key release (locked session...) cannot leave a modifier stuck
        last = now;
        const DWORD vk = k->vkCode & 0xFF;
        const bool  isDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool  repeat = isDown && down[vk];
        down[vk]           = isDown;
        const bool modifier = vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU ||
                              vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN;
        if(isDown && !repeat && !modifier && s_hookWnd)
        {
            unsigned mods = 0;
            if(down[VK_LCONTROL] || down[VK_RCONTROL] || down[VK_CONTROL]) mods |= MOD_CONTROL;
            if(down[VK_LMENU] || down[VK_RMENU] || down[VK_MENU]) mods |= MOD_ALT;
            if(down[VK_LSHIFT] || down[VK_RSHIFT] || down[VK_SHIFT]) mods |= MOD_SHIFT;
            if(down[VK_LWIN] || down[VK_RWIN]) mods |= MOD_WIN;
            PostMessage(s_hookWnd, WM_USER_LLKEY, vk, mods); // matched on the main thread; the hook returns at once
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static DWORD WINAPI KeyboardHookThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); // keys handled at once, even while a game loads
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboard, GetModuleHandleW(NULL), 0);
    MSG   msg;
    while(GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    if(hook)
        UnhookWindowsHookEx(hook);
    return 0;
}

static void StartKeyboardHook(HWND hWnd)
{
    if(s_hookThread)
        return;
    s_hookWnd    = hWnd;
    s_hookThread = CreateThread(NULL, 0, KeyboardHookThread, NULL, 0, &s_hookThreadId);
}

static void StopKeyboardHook()
{
    if(!s_hookThread)
        return;
    PostThreadMessageW(s_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(s_hookThread, 1000);
    CloseHandle(s_hookThread);
    s_hookThread = NULL;
    s_hookWnd    = NULL;
}

static void AdjustApply(int kind, int dir, int step);

// a key press seen by the hook: the hotkey with exactly this key and these modifiers fires
static void HookKey(HWND hWnd, unsigned vk, unsigned mods)
{
    auto& o = s_shaderBeam.m_options;
    if(!s_appReady || o.hotkeysSuspended)
        return;
    // O (no modifier, the key is not taken away from the game): the last 3 seconds of the glasses timing are written to Nbeam_desync.log
    // by the glasses tracer, to see exactly when a small desynchronisation happens
    if(vk == 'O' && mods == 0 && s_shaderBeam.m_active)
    {
        static ULONGLONG s_lastO = 0;
        const ULONGLONG  tick    = GetTickCount64();
        if(tick - s_lastO > 500)
        {
            s_lastO = tick;
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            ::ShaderBeam::g_desyncRequestQpc = q.QuadPart;
        }
    }
    for(int id = 0; id < HOTKEY_COUNT; id++)
    {
        const auto& hk = o.hotkeys[id];
        if(!hk.vk || hk.vk != vk || hk.mods != mods || !FireOnce(id))
            continue;
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
        if(pressed && !down[id] && FireOnce(id))
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
            o.emitter.syncOffsetUs = std::clamp(o.emitter.syncOffsetUs + dir * step, -range, 0); // negative only
        else
            o.emitter.nvidia.openDurationUs = std::clamp(o.emitter.nvidia.openDurationUs + (float)(dir * step), 0.0f, (float)range);
        break;
    }
    }
    s_shaderBeam.m_ui.RequestSave(); // the adjusted value is saved into the active profile
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
        if(!FireOnce(id))
            continue; // already done by the keyboard hook
        const int kind = (id - HOTKEY_ADJUST_FIRST) / 2;
        const int dir  = ((id - HOTKEY_ADJUST_FIRST) % 2) ? 1 : -1;
        AdjustApply(kind, dir, kind >= 2 ? 100 : 1); // 3D timings: 100 us per press; SubFrames / Clear frames: 1
    }
}

// The wheel and the button of the NVIDIA emitter only drive the 3D mini menu (UI::MenuInput)
static void EmitterInputPoll()
{
    auto&     o      = s_shaderBeam.m_options;
    auto&     em     = ::ShaderBeam::Pad::Emitter();
    const int wheel  = em.menuWheel.exchange(0);
    const int held   = em.menuHeld.exchange(0);
    const int clicks = em.menuClicks.exchange(0);
    if(s_appReady && !o.hotkeysSuspended && (wheel || held || clicks))
        s_shaderBeam.m_ui.MenuInput(clicks, wheel, held);
}

// ---- Window mode ----------------------------------------------------------------------------------------------------
// Nbeam opens as a normal window showing the HUD layout: the settings are made there while the effects
// are off (the overlay renderer is stopped, nothing is captured). The effects start (overlay over the screen, as before) when
// "Activate effects" is pressed, with the Show / hide effects hotkey, or when a detected program runs; they stop and the window
// comes back when that program closes or when they are deactivated. While they run, the HUD over the screen works as before.
// ImGui is used by one side at a time: the window (main thread) while the effects are off, the renderer while they run.
static winrt::com_ptr<ID3D11Device>        s_setDev;
static winrt::com_ptr<ID3D11DeviceContext> s_setCtx;
static winrt::com_ptr<IDXGISwapChain1>     s_setChain;
static winrt::com_ptr<ID3D11RenderTargetView> s_setRtv;
static UINT                              s_setW = 0, s_setH = 0;
static const wchar_t*                    SETTINGS_CLASS = L"Nbeam3DVisionWindow";

static void StopSettingsUI()
{
    if(!s_setUi)
        return;
    KillTimer(s_setWnd, 1);
    s_shaderBeam.m_ui.Stop();
    s_shaderBeam.m_ui.SetWindowMode(false);
    s_setUi = false;
    s_setRtv   = nullptr;
    s_setChain = nullptr;
    if(s_setCtx)
        s_setCtx->ClearState();
    s_setCtx = nullptr;
    s_setDev = nullptr;
    s_setW = s_setH = 0;
}

static bool SetRenderTarget()
{
    s_setRtv = nullptr;
    winrt::com_ptr<ID3D11Texture2D> back;
    if(FAILED(s_setChain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())))
        return false;
    return SUCCEEDED(s_setDev->CreateRenderTargetView(back.get(), nullptr, s_setRtv.put()));
}

// the HUD in the window: its own small Direct3D device and swap chain (the overlay ones do not exist while the effects are off)
static bool StartSettingsUI()
{
    StopSettingsUI();
    auto& ui = s_shaderBeam.m_ui;
    auto& o  = s_shaderBeam.m_options;
    if(ui.m_displays.empty())
        s_shaderBeam.RefreshDisplayList();
    s_shaderBeam.RefreshAdapters(); // after a graphics driver update the old graphics card objects no longer work (the window stayed white)
    if(ui.m_displays.empty() || ui.m_adapters.empty())
        return false;
    if(o.shaderDisplayNo >= ui.m_displays.size())
        o.shaderDisplayNo = 0;
    if(o.captureDisplayNo >= ui.m_displays.size())
        o.captureDisplayNo = 0;
    if(o.shaderAdapterNo >= ui.m_adapters.size())
        o.shaderAdapterNo = 0;
    if(o.captureAdapterNo >= ui.m_adapters.size())
        o.captureAdapterNo = 0;
    o.outputWidth  = ui.m_displays[o.shaderDisplayNo].width; // shown in the banner of the HUD
    o.outputHeight = ui.m_displays[o.shaderDisplayNo].height;
    s_shaderBeam.UpdateVsyncRate();
    try
    {
        s_setDev = ::ShaderBeam::Helpers::CreateD3DDevice(ui.m_adapters[o.shaderAdapterNo].adapter.get());
        s_setDev->GetImmediateContext(s_setCtx.put());
        winrt::com_ptr<IDXGIDevice>   dxgiDevice  = s_setDev.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter>  dxgiAdapter;
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(dxgiDevice->GetAdapter(dxgiAdapter.put()));
        winrt::check_hresult(dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 d {};
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = 2;
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(factory->CreateSwapChainForHwnd(s_setDev.get(), s_setWnd, &d, nullptr, nullptr, s_setChain.put()));
        factory->MakeWindowAssociation(s_setWnd, DXGI_MWA_NO_ALT_ENTER);
        if(!SetRenderTarget())
            throw std::runtime_error("render target");
        ui.SetWindowMode(true);
        o.ui = true;
        const float dpi = (float)GetDpiForWindow(s_setWnd);
        ui.Start(s_setWnd, dpi / USER_DEFAULT_SCREEN_DPI, s_setDev, s_setCtx);
    }
    catch(...)
    {
        s_shaderBeam.m_ui.SetWindowMode(false);
        s_setRtv   = nullptr;
        s_setChain = nullptr;
        s_setCtx   = nullptr;
        s_setDev   = nullptr;
        return false;
    }
    RECT rc {};
    GetClientRect(s_setWnd, &rc);
    s_setW = (UINT)(rc.right - rc.left);
    s_setH = (UINT)(rc.bottom - rc.top);
    s_setUi = true;
    SetTimer(s_setWnd, 1, 16, NULL); // drawn about 60 times a second while it is visible
    return true;
}

static void RenderSettings()
{
    if(!s_setUi || !IsWindowVisible(s_setWnd) || IsIconic(s_setWnd))
        return;
    RECT rc {};
    GetClientRect(s_setWnd, &rc);
    const UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if(w == 0 || h == 0)
        return;
    // graphics card lost (driver update, reset): the window starts again on a new device, asked once
    static ULONGLONG s_lostAsked = 0;
    auto             deviceLost  = [&]() {
        const HRESULT reason = s_setDev ? s_setDev->GetDeviceRemovedReason() : E_FAIL;
        if(reason == S_OK)
            return false;
        if(GetTickCount64() - s_lostAsked > 3000)
        {
            s_lostAsked = GetTickCount64();
            PostMessage(s_shaderBeam.m_options.outputWindow, WM_USER_RESTART, NB_RESTART_TAG, 0);
        }
        return true;
    };
    if(deviceLost())
        return;
    if(w != s_setW || h != s_setH)
    {
        s_setRtv = nullptr;
        s_setCtx->OMSetRenderTargets(0, nullptr, nullptr);
        if(FAILED(s_setChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) || !SetRenderTarget())
        {
            deviceLost();
            return;
        }
        s_setW = w;
        s_setH = h;
    }
    ID3D11RenderTargetView* rtv = s_setRtv.get();
    s_setCtx->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4] = { 0.05f, 0.055f, 0.05f, 1.0f };
    s_setCtx->ClearRenderTargetView(rtv, clear);
    try
    {
        s_shaderBeam.m_ui.Render();
    }
    catch(...)
    { }
    const HRESULT hr = s_setChain->Present(1, 0);
    if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        deviceLost(); // graphics card lost: the window starts again
}

// shows the window (taskbar, notification area, second launch, HUD hotkey)
static bool s_setMaxPending = false; // window mode: shown maximized the first time (as it was when Nbeam was last closed)

static void ShowSettings(bool activate)
{
    if(!s_setWnd)
        return;
    s_setShow = true;
    s_inTray  = false;
    if(s_setMaxPending)
    {
        // first time shown: maximized as it was when Nbeam was closed
        s_setMaxPending = false;
        ShowWindow(s_setWnd, SW_SHOWMAXIMIZED);
        if(activate)
            ForceForeground(s_setWnd);
        return;
    }
    if(IsIconic(s_setWnd))
        ShowWindow(s_setWnd, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);
    else
        ShowWindow(s_setWnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if(activate)
        ForceForeground(s_setWnd);
}

static void HideSettings()
{
    if(s_setWnd)
        ShowWindow(s_setWnd, SW_HIDE);
}

// the state the renderer must be in: started (effects on) or the window (window mode, effects off). Called instead of Start().
static void RunOrStandby(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    if(o.windowMode && !s_effectsOn)
    {
        // window mode, effects off: the overlay stays stopped and hidden, the HUD is drawn in the window
        ShowWindow(hWnd, SW_HIDE);
        TaskbarButton(hWnd, false);
        s_standby = true;
        if(!StartSettingsUI())
        {
            SetTimer(hWnd, TIMER_RETRY_START, 2000, nullptr); // no display / graphics card yet: try again
            return;
        }
        if(s_setReduce)
        {
            // the effects were stopped by the button over a game: Nbeam reduces itself instead of coming back in front of the game
            s_setReduce = false;
            s_setShow   = false;
            if(o.minimizeToTray)
            {
                HideSettings();
                UpdateTrayIcon(hWnd);
                s_inTray = true;
            }
            else if(s_setWnd)
                ShowWindow(s_setWnd, SW_SHOWMINNOACTIVE);
            s_setActivate = false;
            return;
        }
        if(s_setShow)
            ShowSettings(s_setActivate);
        s_setActivate = false;
        return;
    }
    const bool fromStandby = s_standby;
    if(s_setUi)
        StopSettingsUI();
    HideSettings();
    s_standby = false;
    if(fromStandby)
    {
        o.ui = false; // effects activated: HUD hidden over the screen (its hotkey shows it)
        SetClickThrough(hWnd, true);
    }
    s_shaderBeam.Start();
    if(fromStandby)
    {
        ShowWindow(hWnd, SW_SHOWNA); // shown once the renderer runs (no black screen while it starts)
        BringToFront(hWnd, false);
    }
    if(s_hudAfterStart)
    {
        s_hudAfterStart = false;
        ShowWindow(hWnd, SW_SHOWNA);
        o.ui = false;
        PostMessage(hWnd, WM_HOTKEY, HOTKEY_TOGGLEUI, 1); // window mode turned off: the HUD over the screen is shown
    }
    else if(fromStandby)
    {
        const HWND game = (HWND)o.captureWindow;
        if(game && IsWindow(game) && !IsIconic(game))
            ForceForeground(game); // the game keeps the focus
    }
}

static bool EmitterRecognized()
{
    return ::ShaderBeam::NvidiaEmitterReady() || s_shaderBeam.m_options.emitter.comPort.comPortNo >= 0;
}

// the settings of the test window, from the options: the layout follows the 3D source format (2D for 2D to 3D by AI, which makes its own
// depth from the 2D picture)
static ::ShaderBeam::CarTestParams TestParams()
{
    const auto&                   o = s_shaderBeam.m_options;
    ::ShaderBeam::CarTestParams   p;
    // (Auto source with no 3D yet: the cars are drawn side by side, which the Auto source then detects)
    const auto                    f = o.emitter.autoSource && o.emitter.sourceFormat == ::ShaderBeam::StereoSource::Off ? ::ShaderBeam::StereoSource::SideBySide : o.emitter.sourceFormat;
    if(o.StereoAllowed())
        p.layout = f == ::ShaderBeam::StereoSource::SideBySide ? 1 : f == ::ShaderBeam::StereoSource::TopBottom ? 2 : f == ::ShaderBeam::StereoSource::Checkerboard ? 3 : f == ::ShaderBeam::StereoSource::InterlacedH ? 4 : f == ::ShaderBeam::StereoSource::InterlacedV ? 5 : 0;
    p.speed     = std::clamp(o.ufoSpeed, 0, 7680);
    p.carSize   = std::clamp(o.carSize, 25, 200);
    for(int i = 0; i < 3; i++)
        p.bg[i] = o.testBg[i];
    p.accent[0] = o.hud.r;
    p.accent[1] = o.hud.g;
    p.accent[2] = o.hud.b;
    p.french    = ::ShaderBeam::GetLanguage() == LANG_FR ? 1 : 0;
    return p;
}

static void SendTestParams()
{
    if(!s_testWnd || !IsWindow(s_testWnd))
        return;
    const auto p = TestParams();
    if(s_testParamsSent && memcmp(&p, &s_testSent, sizeof(p)) == 0)
        return;
    COPYDATASTRUCT cd {};
    cd.dwData = ::ShaderBeam::CARTEST_COPYDATA;
    cd.cbData = sizeof(p);
    cd.lpData = (void*)&p;
    DWORD_PTR result = 0;
    if(SendMessageTimeoutW(s_testWnd, WM_COPYDATA, (WPARAM)s_shaderBeam.m_options.outputWindow, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &result))
    {
        s_testSent       = p;
        s_testParamsSent = true;
    }
}

static BOOL CALLBACK FindTestWindow(HWND w, LPARAM lParam)
{
    auto* found = (std::pair<DWORD, HWND>*)lParam;
    DWORD pid   = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[64] = {};
    if(pid == found->first && GetClassNameW(w, cls, 63) && wcscmp(cls, ::ShaderBeam::CARTEST_CLASS) == 0)
    {
        found->second = w;
        return FALSE;
    }
    return TRUE;
}

static void CloseTestProcess()
{
    if(s_testWnd && IsWindow(s_testWnd))
        PostMessage(s_testWnd, WM_CLOSE, 0, 0);
    if(s_testProcess)
    {
        if(WaitForSingleObject(s_testProcess, 1500) != WAIT_OBJECT_0)
            TerminateProcess(s_testProcess, 0);
        CloseHandle(s_testProcess);
    }
    s_testProcess = NULL;
    s_testWnd     = NULL;
}

// Motion test: the cars window (a process of its own) is opened over the output screen and captured like a game. With a profile, the
// profile is loaded for the test (it is adjusted over the cars); the end of the test goes back to the desktop settings.
static void StartMotionTest(HWND hWnd)
{
    auto&             o       = s_shaderBeam.m_options;
    const std::string profile = o.testProfile;
    o.testProfile.clear();
    const bool tune = !profile.empty() && std::filesystem::exists(::ShaderBeam::Options::ProfilesDir() / profile);
    if(s_testOn)
    {
        // already running: another profile to adjust (the current one is saved first)
        if(tune && _stricmp(profile.c_str(), o.profileFile.c_str()) != 0)
        {
            o.pendingProfile = profile;
            s_quietUntil     = GetTickCount64() + 3000;
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
        }
        return;
    }
    if(s_linkActive || !o.captureProcess.empty())
        return; // a detected program runs: no test
    // the output screen
    RECT mr { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if(o.shaderDisplayNo < s_shaderBeam.m_ui.m_displays.size())
    {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if(GetMonitorInfoW(s_shaderBeam.m_ui.m_displays[o.shaderDisplayNo].monitor, &mi))
            mr = mi.rcMonitor;
    }
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /cartest " + std::to_wstring(mr.left) + L" " + std::to_wstring(mr.top) + L" " + std::to_wstring(mr.right - mr.left) + L" " +
                       std::to_wstring(mr.bottom - mr.top) + L" " + std::to_wstring(GetCurrentProcessId());
    // the two processes are tied: the test process is in a job that kills it when Nbeam ends (quit, crash...), and it watches Nbeam itself
    if(!s_testJob)
    {
        s_testJob = CreateJobObjectW(nullptr, nullptr);
        if(s_testJob)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li {};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(s_testJob, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }
    }
    STARTUPINFOW        si {};
    PROCESS_INFORMATION pi {};
    si.cb = sizeof(si);
    if(!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return;
    if(s_testJob)
        AssignProcessToJobObject(s_testJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    s_testProcess = pi.hProcess;
    std::pair<DWORD, HWND> found(pi.dwProcessId, (HWND)NULL);
    for(int i = 0; i < 60 && !found.second; i++)
    {
        EnumWindows(FindTestWindow, (LPARAM)&found);
        if(!found.second)
        {
            if(WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
                break; // (it could not start: graphics card...)
        }
    }
    if(!found.second)
    {
        CloseTestProcess();
        return;
    }
    s_testWnd         = found.second;
    s_testOn          = true;
    s_testFromStandby = s_standby;
    o.motionTest      = true;
    s_testParamsSent  = false; // (sent by TestPoll once the effects run with the right settings: the window stays black until then)
    if(tune && _stricmp(profile.c_str(), o.profileFile.c_str()) != 0)
        o.pendingProfile = profile; // the desktop settings are saved first, then the profile is loaded
    // the test window is the captured "game"
    o.captureProcess = ::ShaderBeam::Options::ProcessNameOfWindow(s_testWnd);
    o.captureTitle   = ::ShaderBeam::CARTEST_TITLE_A;
    o.captureWindow  = s_testWnd;
    s_focusFallback  = false;
    ::ShaderBeam::Options::BringToForeground(s_testWnd);
    if(s_standby)
    {
        s_effectsOn     = true;
        s_effectsByLink = false;
    }
    s_hudAfterStart = tune; // a profile to adjust: the HUD is shown over the cars
    s_quietUntil    = GetTickCount64() + 3000;
    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
}

static void EndMotionTest(HWND hWnd)
{
    if(!s_testOn)
        return;
    s_testOn     = false;
    auto& o      = s_shaderBeam.m_options;
    o.motionTest = false;
    CloseTestProcess();
    o.captureProcess.clear();
    o.captureTitle.clear();
    o.captureWindow    = NULL;
    o.processUnfocused = false;
    s_focusFallback    = false;
    s_hudAfterStart    = false;
    if(!o.IsDesktopSettings())
        o.pendingProfile = ::ShaderBeam::Options::DESKTOP_SETTINGS; // the adjusted profile is saved, the desktop settings come back
    s_quietUntil = GetTickCount64() + 3000;
    if(s_testFromStandby)
        SetEffectsOn(hWnd, false, true); // back to the window
    else
        PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
}

// motion test: the Windows key or Escape ends it, and so does the end of the test window (Escape in it); its settings follow the HUD,
// sent only once the effects run with the settings of the test (a profile to adjust is loaded first): no picture with other settings
static void TestPoll(HWND hWnd)
{
    if(!s_testOn)
        return;
    if((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
       (s_testProcess && WaitForSingleObject(s_testProcess, 0) == WAIT_OBJECT_0) || !IsWindow(s_testWnd))
    {
        EndMotionTest(hWnd);
        return;
    }
    if(s_shaderBeam.m_active && s_shaderBeam.m_options.pendingProfile.empty())
        SendTestParams();
}

// Notice shown on the screen while the effects are off (window mode: the overlay is stopped, so its banner cannot be drawn): a small window
// on top of everything, drawn exactly as the banner of the overlay (same Inter fonts and sizes, black box, "Nbeam" in the accent colour, the
// text below in light grey, same margins and corner), for 3 s. It takes no click and no focus.
static HWND         s_toastWnd = NULL;
static HFONT        s_toastTitleFont = NULL, s_toastTextFont = NULL;
static std::wstring s_toastText;
static int          s_toastUnit = 16;

static LRESULT CALLBACK ToastWndProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(w, &ps);
        RECT        rc {};
        GetClientRect(w, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        const auto& h = s_shaderBeam.m_options.hud;
        const int   f = s_toastUnit;
        HGDIOBJ     old = SelectObject(dc, s_toastTitleFont);
        SetTextColor(dc, RGB((int)(h.r * 255), (int)(h.g * 255), (int)(h.b * 255)));
        TextOutW(dc, 2 * f, f, L"Nbeam", 5);
        SelectObject(dc, s_toastTextFont);
        SetTextColor(dc, RGB(215, 215, 215));
        TextOutW(dc, 2 * f, 4 * f, s_toastText.c_str(), (int)s_toastText.size());
        SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(w, 1);
        ShowWindow(w, SW_HIDE);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProc(w, message, wParam, lParam);
}

static void ShowToast(const std::string& text)
{
    const HINSTANCE inst = GetModuleHandleW(NULL);
    if(!s_toastWnd)
    {
        WNDCLASSEXW wc {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ToastWndProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Nbeam3DVisionNotice";
        RegisterClassExW(&wc);
        s_toastWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
        if(!s_toastWnd)
            return;
        SetLayeredWindowAttributes(s_toastWnd, 0, 255, LWA_ALPHA);
    }
    // on the main screen of Windows, at the corner chosen for the banner; sizes follow the scale of that screen (16 px at 100 %)
    HMONITOR    mon = MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if(HMODULE sh = LoadLibraryW(L"shcore.dll"))
    {
        if(auto fn = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor"))
            fn(mon, 0, &dx, &dy);
        FreeLibrary(sh);
    }
    // the size of the banner text: 16 px at 100 % (HUD size, or the Windows scale of the screen with "Auto HUD size")
    const auto& hud = s_shaderBeam.m_options.hud;
    const float sc  = hud.autoScale ? (float)dx / 96.0f : std::clamp(hud.scale, 0.6f, 2.0f);
    const int   f   = (std::max)(8, (int)roundf(16.0f * sc));
    s_toastUnit     = f;
    // the fonts of the HUD (Inter Medium for the text, Inter Bold for the title), taken from the exe, for this process only
    static bool fontsAdded = false;
    if(!fontsAdded)
    {
        DWORD n = 0;
        AddFontMemResourceEx((void*)HudFontMedium_data, HudFontMedium_size, nullptr, &n);
        AddFontMemResourceEx((void*)HudFontBold_data, HudFontBold_size, nullptr, &n);
        fontsAdded = true;
    }
    if(s_toastTitleFont)
        DeleteObject(s_toastTitleFont);
    if(s_toastTextFont)
        DeleteObject(s_toastTextFont);
    // (a positive height = the height of the line, as the font sizes of the HUD)
    s_toastTitleFont = CreateFontW(3 * f, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Inter");
    s_toastTextFont  = CreateFontW(f, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Inter Medium");
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    s_toastText.assign((size_t)(std::max)(n, 0), L'\0');
    if(n > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), s_toastText.data(), n);
    HDC     dc  = GetDC(s_toastWnd);
    SIZE    st {}, sx {};
    HGDIOBJ old = SelectObject(dc, s_toastTitleFont);
    GetTextExtentPoint32W(dc, L"Nbeam", 5, &st);
    SelectObject(dc, s_toastTextFont);
    GetTextExtentPoint32W(dc, s_toastText.c_str(), (int)s_toastText.size(), &sx);
    SelectObject(dc, old);
    ReleaseDC(s_toastWnd, dc);
    const int   bw     = (int)(std::max)(st.cx, sx.cx) + 4 * f;
    const int   bh     = (int)(f * 6.4f);
    const RECT& r      = mi.rcMonitor;
    const int   corner = s_shaderBeam.m_options.hud.bannerCorner; // 0 bottom left, 1 bottom right, 2 top right, 3 top left
    const int   x      = (corner == 1 || corner == 2) ? r.right - f - bw : r.left + f;
    const int   y      = (corner == 2 || corner == 3) ? r.top + f : r.bottom - f - bh;
    SetWindowPos(s_toastWnd, HWND_TOPMOST, x, y, bw, bh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s_toastWnd, nullptr, TRUE);
    UpdateWindow(s_toastWnd);
    SetTimer(s_toastWnd, 1, 3000, nullptr); // (the duration of the note of the banner)
}

// a short notice: in the banner of the overlay, or, while the effects are off (the overlay is stopped), in the notice window
static void Notify(const std::string& text)
{
    s_shaderBeam.m_ui.ShowNote(text);
    if(s_standby)
        ShowToast(text);
}

// window mode: effects on / off (button, hotkey, notification area menu)
static void SetEffectsOn(HWND hWnd, bool on, bool showWindow)
{
    s_effectsOn     = on;
    s_effectsByLink = false;
    if(!on)
    {
        s_setShow     = s_setShow || showWindow;
        s_setActivate = showWindow;
    }
    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
}

static LRESULT CALLBACK SettingsWndProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
{
    if(s_setUi && s_shaderBeam.m_ui.Input(w, message, wParam, lParam))
        return true;
    switch(message)
    {
    case WM_TIMER:
        if(wParam == 1)
            RenderSettings();
        return 0;
    case WM_SIZE:
        if(wParam != SIZE_MINIMIZED)
            RenderSettings(); // drawn at once while the edges are dragged
        return 0;
    case WM_GETMINMAXINFO: {
        const float k = (float)GetDpiForWindow(w) / USER_DEFAULT_SCREEN_DPI;
        auto*       mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = (LONG)(640 * k);
        mm->ptMinTrackSize.y = (LONG)(420 * k);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if(s_setUi)
            StartSettingsUI(); // fonts and sizes for the new screen
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        // the cross: "Minimize instead of closing" minimizes the window (to the notification area with that option, the detections keep
        // working), otherwise Nbeam quits
        if(s_shaderBeam.m_options.closeMinimizes)
        {
            if(s_shaderBeam.m_options.minimizeToTray)
            {
                s_setShow = false;
                HideSettings();
                UpdateTrayIcon(s_shaderBeam.m_options.outputWindow);
            }
            else
                ShowWindow(w, SW_MINIMIZE);
        }
        else
            PostMessage(s_shaderBeam.m_options.outputWindow, WM_DESTROY, 0, 0);
        return 0;
    }
    return DefWindowProc(w, message, wParam, lParam);
}

static void CreateSettingsWindow(HINSTANCE hInstance)
{
    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SHADERBEAM));
    wc.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = SETTINGS_CLASS;
    RegisterClassExW(&wc);
    // centred on the main screen, 62 x 40 font sizes (the size of the HUD at 100 %)
    const UINT  dpi = GetDpiForSystem();
    const float k   = (float)dpi / USER_DEFAULT_SCREEN_DPI;
    RECT        r { 0, 0, (LONG)(62 * 16 * k), (LONG)(40 * 16 * k) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT      work {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + (std::max)(0, (int)(work.right - work.left - ww) / 2);
    const int y = work.top + (std::max)(0, (int)(work.bottom - work.top - wh) / 2);
    s_setWnd = CreateWindowExW(0, SETTINGS_CLASS, L"Nbeam 3D Vision", WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr, hInstance, nullptr);
    // the place and size it had when Nbeam was last closed (kept in Nbeam.app.cfg), if that place is still on a connected screen
    const auto& o = s_shaderBeam.m_options;
    if(s_setWnd && o.setWinRect[2] - o.setWinRect[0] > 100)
    {
        RECT saved { o.setWinRect[0], o.setWinRect[1], o.setWinRect[2], o.setWinRect[3] };
        if(MonitorFromRect(&saved, MONITOR_DEFAULTTONULL))
        {
            WINDOWPLACEMENT wp {};
            wp.length           = sizeof(wp);
            GetWindowPlacement(s_setWnd, &wp);
            wp.showCmd          = SW_HIDE; // (shown later, by ShowSettings)
            wp.rcNormalPosition = saved;
            SetWindowPlacement(s_setWnd, &wp);
            s_setMaxPending = o.setWinMax;
        }
    }
}

// window mode: the place, size and maximized state of the window are kept when Nbeam closes
static void SaveSettingsPlacement()
{
    if(!s_setWnd || !IsWindow(s_setWnd))
        return;
    WINDOWPLACEMENT wp {};
    wp.length = sizeof(wp);
    if(!GetWindowPlacement(s_setWnd, &wp))
        return;
    const RECT& r = wp.rcNormalPosition;
    if(r.right - r.left <= 100 || r.bottom - r.top <= 100)
        return;
    auto& o         = s_shaderBeam.m_options;
    o.setWinRect[0] = r.left;
    o.setWinRect[1] = r.top;
    o.setWinRect[2] = r.right;
    o.setWinRect[3] = r.bottom;
    o.setWinMax     = wp.showCmd == SW_SHOWMAXIMIZED || (IsZoomed(s_setWnd) != FALSE) || (s_setMaxPending && !IsWindowVisible(s_setWnd));
    o.SaveApp();
}

// the HUD goes back to the notification area (taskbar button removed) when a linked program starts or stops
static void ReduceHud(HWND hWnd)
{
    auto& o = s_shaderBeam.m_options;
    if(!s_appReady)
        return;
    if(s_standby)
        return; // window mode, effects off: nothing over the screen
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
        SendMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0); // renderer without 3D right before the black screen
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
    ::ShaderBeam::LsLog("refresh rate set to %d Hz (%s): the change took %d ms, the renderer comes back in %d ms (average change %d ms)", hz, ok ? "ok" : "failed", (int)d, (int)(s_restartAt - now), (int)o.hzSwitchMs);
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
        PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
    }
    const bool active = !s_hzTargets.empty() && o.captureWindow && IsWindow(o.captureWindow);
    if(s_restartAt && GetTickCount64() >= s_restartAt)
    {
        s_restartAt    = 0;
        o.emitter.hold = false; // 3D comes back
        PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0); // vsync rate / sub frames follow the new refresh rates
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
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
        }
    }
    else if(!s_hzSaved.empty())
        RestoreRefreshRate(hWnd);
    if(!active)
        failedHz.clear();
}

// A display or resolution change that Nbeam did not make itself (screen connected / disconnected, resolution changed in Windows
// or by a game): the renderer restarts after a short moment, with the list of the displays read again.
// Nbeam needs 80 Hz minimum: when the output screen drops below 79 Hz while it runs (3 seconds in a row, not during a change made by
// Nbeam itself) it shows the error and closes, unless another connected screen is compatible
static void LowRatePoll(HWND hWnd)
{
    static int  s_lowCount = 0;
    static bool s_shown    = false;
    auto&       o          = s_shaderBeam.m_options;
    if(s_shown || !s_appReady || s_standby || s_quitting || s_testOn || o.emitter.hold || GetTickCount64() < s_ownChangeUntil)
    {
        s_lowCount = 0;
        return;
    }
    int hz = 0;
    if(HMONITOR mon = o.OutputMonitor(s_shaderBeam.m_ui.m_displays))
    {
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm {};
        dm.dmSize = sizeof(dm);
        if(GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hz = (int)dm.dmDisplayFrequency;
    }
    if(hz <= 1 || hz >= 79)
    {
        s_lowCount = 0;
        return;
    }
    if(++s_lowCount < 3)
        return;
    bool              other = false;
    const std::string outId = o.ScreenIdOfDisplay(o.shaderDisplayNo, s_shaderBeam.m_ui.m_displays);
    for(const auto& m : o.MonitorsSnapshot())
    {
        int best = m.hz;
        for(int r : m.rates)
            best = (std::max)(best, r);
        if(m.connected && m.id != outId && best >= 80)
            other = true;
    }
    if(other)
        return;
    s_shown = true;
    const char* msg = ::ShaderBeam::Tr("Refresh rate not compatible with Nbeam 3D Vision: 80 Hz minimum are needed to work. Change the refresh rate or connect a compatible screen.");
    wchar_t     wide[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wide, 511);
    MessageBoxW(nullptr, wide, L"Nbeam 3D Vision", MB_ICONERROR | MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    PostMessage(hWnd, WM_DESTROY, 0, 0); // same as Quit in the notification area menu
}

static void DisplayChangePoll(HWND hWnd)
{
    if(!s_appReady || !s_displayChangeAt || GetTickCount64() - s_displayChangeAt < 1000)
        return;
    s_displayChangeAt = 0;
    s_refreshDisplays = true;
    // a screen was connected / disconnected or changed mode: Nbeam restarts minimized (the HUD does not open on the new layout)
    ReduceHud(hWnd);
    s_quietUntil = GetTickCount64() + 3000;
    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
}

// A program that is detected is only taken once its picture moves: two checks one second apart that both show movement (the capture must not
// start on a banner or a loading screen of the game). The window is read as DWM shows it, as a small grid; nothing is sent to the program.
// While the window cannot be read, or nothing moves (a still menu, a graphics configuration window), nothing is applied and the check goes on.
static bool GrabGrid(HWND w, std::vector<unsigned char>& out)
{
    RECT r {};
    if(!w || !IsWindow(w) || IsIconic(w) || !GetWindowRect(w, &r))
        return false;
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    if(ww < 64 || wh < 64)
        return false;
    constexpr int GW = 64, GH = 36;
    HDC           scr = GetDC(NULL);
    HDC           big = CreateCompatibleDC(scr);
    HDC           sm  = CreateCompatibleDC(scr);
    HBITMAP       bb  = CreateCompatibleBitmap(scr, ww, wh);
    BITMAPINFO    bi {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = GW;
    bi.bmiHeader.biHeight      = -GH;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits = nullptr;
    HBITMAP sb   = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool    ok   = false;
    if(bb && sb && bits)
    {
        HGDIOBJ ob = SelectObject(big, bb), os = SelectObject(sm, sb);
        ok         = PrintWindow(w, big, 2 /*PW_RENDERFULLCONTENT*/) != 0;
        if(!ok)
            ok = BitBlt(big, 0, 0, ww, wh, scr, r.left, r.top, SRCCOPY) != 0; // (what is on the screen at that place)
        if(ok)
        {
            SetStretchBltMode(sm, HALFTONE);
            SetBrushOrgEx(sm, 0, 0, nullptr);
            ok = StretchBlt(sm, 0, 0, GW, GH, big, 0, 0, ww, wh, SRCCOPY) != 0;
        }
        if(ok)
        {
            out.assign((unsigned char*)bits, (unsigned char*)bits + GW * GH * 4);
            GdiFlush();
        }
        SelectObject(big, ob);
        SelectObject(sm, os);
    }
    if(bb)
        DeleteObject(bb);
    if(sb)
        DeleteObject(sb);
    DeleteDC(big);
    DeleteDC(sm);
    ReleaseDC(NULL, scr);
    return ok;
}

static bool ProgramAlive(const std::string& exe)
{
    auto&                       o = s_shaderBeam.m_options;
    static std::string          key;
    static std::vector<unsigned char> prev;
    static int                  good  = 0;
    static ULONGLONG            last  = 0;
    const std::string           k     = ::ShaderBeam::Options::NormalizeExeName(exe);
    const ULONGLONG             now   = GetTickCount64();
    if(k != key)
    {
        key = k;
        prev.clear();
        good  = 0;
        last  = 0;
    }
    if(last && now - last < 800)
        return false; // (one check per second)
    last = now;
    HWND                       w = o.TaskbarWindowOfExe(exe);
    std::vector<unsigned char> cur;
    if(!w || !GrabGrid(w, cur))
    {
        // hidden, minimized or not readable (yet): nothing is applied, the program is checked again
        prev.clear();
        good  = 0;
        return false;
    }
    bool moved = false;
    if(prev.size() == cur.size())
    {
        int changed = 0;
        for(size_t i = 0; i + 3 < cur.size(); i += 4)
        {
            const int d = abs((int)cur[i] - (int)prev[i]) + abs((int)cur[i + 1] - (int)prev[i + 1]) + abs((int)cur[i + 2] - (int)prev[i + 2]);
            if(d > 18)
                changed++;
        }
        moved = changed * 100 >= (int)(cur.size() / 4); // at least 1 % of the grid changed
    }
    prev = std::move(cur);
    if(moved)
        good++;
    else
        good = 0;
    if(good >= 2)
    {
        ::ShaderBeam::LsLog("detection %s: the picture moves (2 checks), the program is taken", k.c_str());
        key.clear();
        return true;
    }
    return false;
}

// Profile links: when a linked program is running its profile is loaded (the same restart as choosing it in the HUD); when it
// closes the profile that was active before comes back. Checked once a second; a program must be seen twice in a row.
static void ProfileLinkPoll(HWND hWnd)
{
    auto&              o         = s_shaderBeam.m_options;
    static int         active    = -1;
    static int         candidate = -1;
    static ULONGLONG   candidateSince = 0;
    static int         prevShaderNo = 0, prevCaptureNo = 0;
    static std::string activeProfile; // the profile of the active detection (a change of it in the HUD is applied at once)
    if(!s_appReady || GetTickCount64() < s_linkReadyAt || s_testOn)
        return; // (no profile switch during the glasses test)
    // a copy of the detections, taken under the lock: the HUD (render thread) may add or remove some at the same time
    std::vector<::ShaderBeam::Options::ProfileLink> links;
    {
        std::lock_guard<std::recursive_mutex> linksLock(o.linksMutex);
        links = o.profileLinks;
    }
    // Detections of the same program (up to 3, each for a different screen): the profile of the Windows main display wins, then the
    // profile of another connected screen; if none of its screens is connected the first one is used.
    int match = -1;
    {
        const auto            mons = o.MonitorsSnapshot();
        std::string           group;
        int                   inGroup = 0, bestScore = -1;
        std::map<std::string, bool> running;
        std::map<std::string, std::string> winScreen; // screen where each running program has its window
        for(size_t i = 0; i < links.size(); i++)
        {
            const auto& l = links[i];
            if(!l.enabled || l.profile.empty())
                continue;
            if(!l.screenId.empty() && !std::any_of(mons.begin(), mons.end(), [&](const ::ShaderBeam::Options::KnownMonitor& m) { return m.id == l.screenId && m.connected; }))
                continue; // the screen of this detection is not connected: its settings are ignored until it is back
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
                for(const auto& other : links)
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
                score = 1;
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
    // the program keeps its active detection while it runs (and while it is still checked): no switch from one screen to another
    if(match >= 0 && active >= 0 && match != active && active < (int)links.size() && links[active].enabled && !links[active].profile.empty() &&
       (links[active].screenId.empty() || HzMonitor(links[active].screenId)) &&
       ::ShaderBeam::Options::NormalizeExeName(links[active].exe) == ::ShaderBeam::Options::NormalizeExeName(links[match].exe))
        match = active;
    if(match >= 0)
    {
        if(match == active)
        {
            // the program keeps running but its profile was changed in the detection: the new profile is loaded now (the current
            // settings are saved into the previous one first)
            const std::string& now = links[match].profile;
            if(_stricmp(now.c_str(), activeProfile.c_str()) != 0 && std::filesystem::exists(::ShaderBeam::Options::ProfilesDir() / now))
            {
                activeProfile = now;
                if(_stricmp(now.c_str(), o.profileFile.c_str()) != 0)
                {
                    o.pendingProfile = now;
                    s_quietUntil     = GetTickCount64() + 3000;
                    PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
                }
            }
            return;
        }
        if(match != candidate)
        {
            candidate      = match;
            candidateSince = GetTickCount64();
            return;
        }
        if(GetTickCount64() - candidateSince < (ULONGLONG)links[match].delay * 1000ULL)
            return; // activation delay of the link
        if(!::ShaderBeam::Options::IsScalerExe(links[match].exe) && !ProgramAlive(links[match].exe))
            return; // the program is taken once its picture moves
        const std::string target = links[match].profile;
        if(!std::filesystem::exists(::ShaderBeam::Options::ProfilesDir() / target))
            return;
        if(active < 0)
        {
            prevShaderNo = o.shaderDisplayNo;
            prevCaptureNo = o.captureDisplayNo;
        }
        {
            // the screen of this detection (when it is connected); the output and the capture of the profile follow it
            const auto& sid = links[match].screenId;
            const int   dn  = sid.empty() ? -1 : o.DisplayNoForScreen(sid, s_shaderBeam.m_ui.m_displays);
            o.linkShaderDisplayNo  = dn;
            o.linkCaptureDisplayNo = dn;
        }
        active        = match;
        s_activeLink  = match;
        activeProfile = target;
        candidate     = -1;
        if(target != o.profileFile)
            o.pendingProfile = target;
        if(o.windowMode && !s_effectsOn)
        {
            s_effectsOn     = true; // window mode: the detected program starts the effects
            s_effectsByLink = true;
        }
        s_quietUntil = GetTickCount64() + 3000;
        s_linkActive = true;
        // the linked program is the captured process (its window, and the output over it with the "Process" output)
        o.captureProcess = ::ShaderBeam::Options::NormalizeExeName(links[match].exe);
        o.captureTitle.clear();
        s_focusFallback = false;
        o.captureWindow = o.captureProcess.empty() ? NULL : o.FindCaptureWindow();
        if(::ShaderBeam::Options::IsScalerExe(o.captureProcess))
            ::ShaderBeam::LsLog("detection ON: capture of the layer hwnd=%p", (void*)o.captureWindow);
        ReduceHud(hWnd);
        if(o.captureWindow)
            ::ShaderBeam::Options::BringToForeground(o.captureWindow); // the detected program comes to the front
        {
            // refresh rate of the active detection only (the one of the screen that is used): the other screens of the program are left alone;
            // "Current Hz" = no change. 3D only starts after the change
            s_hzTargets.clear();
            {
                const auto& l = links[match];
                if(l.enabled && l.hz > 0 && !l.screenId.empty() && HzMonitor(l.screenId))
                    s_hzTargets.push_back({ l.screenId, l.hz });
            }
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
        PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
    }
    else
    {
        candidate = -1;
        if(active >= 0)
        {
            active       = -1;
            s_activeLink = -1;
            s_noWindowRestartAt = 0;
            activeProfile.clear();
            s_linkActive = false;
            if(s_effectsByLink)
            {
                s_effectsOn     = false; // window mode: the effects started by this program stop with it, the window comes back
                s_effectsByLink = false;
                s_setActivate   = false;
            }
            // the desktop settings come back (the profile of the program is saved first)
            const std::string back = ::ShaderBeam::Options::DESKTOP_SETTINGS;
            if(!o.IsDesktopSettings())
                o.pendingProfile = back;
            o.linkShaderDisplayNo  = prevShaderNo; // back to the screen used before the detection
            o.linkCaptureDisplayNo = prevCaptureNo;
            s_quietUntil = GetTickCount64() + 3000;
            if(::ShaderBeam::Options::IsScalerExe(o.captureProcess))
                ::ShaderBeam::LsLog("detection OFF: back to the desktop settings");
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
                SendMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
                RestoreRefreshRate(hWnd);
            }
            else
                PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
        }
    }
}

#define TIMER_MENU_TOP 4 // window timer: while the notification area menu is open, it is raised above the taskbar

static BOOL CALLBACK RaiseMenuProc(HWND w, LPARAM)
{
    wchar_t cls[16] = {};
    if(GetClassNameW(w, cls, 16) && wcscmp(cls, L"#32768") == 0 && IsWindowVisible(w))
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return TRUE;
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
            AppendMenuW(menu, MF_STRING | (o.runAsAdmin ? MF_CHECKED : 0), TRAY_CMD_ADMIN, WideOf(::ShaderBeam::Tr("Run as administrator")).c_str());
            AppendMenuW(menu, MF_STRING, TRAY_CMD_EFFECTS,
                        WideOf(::ShaderBeam::Tr(o.windowMode ? (s_effectsOn ? "Deactivate effects" : "Activate effects") : (o.effectsSuspended ? "Show effects" : "Hide effects"))).c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, TRAY_CMD_RESTART, WideOf(::ShaderBeam::Tr("Restart")).c_str());
            AppendMenuW(menu, MF_STRING, TRAY_CMD_QUIT, WideOf(::ShaderBeam::Tr("Quit")).c_str());
            POINT pt;
            GetCursorPos(&pt);
            s_quietUntil = GetTickCount64() + 600000; // activating the window for the menu must not open the HUD
            SetForegroundWindow(hWnd); // required for the menu to close when clicking elsewhere
            // the menu is kept above everything, the Windows taskbar included (both are "always on top" windows: the last one raised wins)
            s_menuOpen = true;
            SetTimer(hWnd, TIMER_MENU_TOP, 20, nullptr);
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hWnd, nullptr);
            KillTimer(hWnd, TIMER_MENU_TOP);
            s_menuOpen = false;
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
            else if(cmd == TRAY_CMD_ADMIN)
            {
                o.runAsAdmin = !o.runAsAdmin;
                o.SaveApp();
            }
            else if(cmd == TRAY_CMD_EFFECTS)
            {
                if(o.windowMode)
                    SetEffectsOn(hWnd, !s_effectsOn, false); // window mode: start / stop the effects
                else
                    o.effectsSuspended = !o.effectsSuspended; // same as the HUD button Show / Hide
            }
            else if(cmd == TRAY_CMD_RESTART)
                PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0); // same as the Restart button of the HUD
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
    if(message == WM_MOUSEACTIVATE && s_shaderBeam.m_options.ui && ScalerCaptured())
        return MA_NOACTIVATE; // HUD over Lossless Scaling: the click reaches the HUD, the game keeps the focus
    if(s_shaderBeam.m_options.directPresent && !s_shaderBeam.m_options.ui)
    {
        // "Direct presentation", HUD hidden: the overlay catches the mouse (no click-through) but stays out of the way: no focus taken,
        // clicks ignored (the game gets its raw mouse input anyway), no Windows pointer drawn over the game
        if(message == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if(message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(NULL);
            return TRUE;
        }
        // The mouse messages the game would have received without the overlay (the in-game overlay of ReShade, menus that do not read the raw
        // mouse) are sent to it with the position over its own window: the overlay is the window under the pointer, so they would be lost.
        const bool mouseButton = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
                                 message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                                 message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK;
        if(mouseButton || message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL)
        {
            HWND game = (HWND)s_shaderBeam.m_options.captureWindow;
            if(!game)
                game = GetForegroundWindow();
            if(game && game != hWnd && IsWindow(game) && !IsIconic(game))
            {
                if(message == WM_MOUSEWHEEL)
                {
                    PostMessage(game, message, wParam, lParam); // (the position of the wheel message is already in screen coordinates)
                }
                else
                {
                    POINT p { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
                    ClientToScreen(hWnd, &p);
                    ScreenToClient(game, &p);
                    SetLastError(0);
                    const BOOL  posted = PostMessage(game, message, wParam, MAKELPARAM((short)p.x, (short)p.y));
                    const DWORD err    = posted ? 0 : GetLastError();
                    // the first clicks and every refused one (a game running with more rights than Nbeam refuses the messages) are written to the log
                    static int s_fwdLogged = 0, s_fwdFailed = 0;
                    if(mouseButton && (s_fwdLogged < 8 || (!posted && s_fwdFailed < 20)))
                    {
                        char cls[64] = {};
                        GetClassNameA(game, cls, (int)sizeof(cls));
                        DWORD pid = 0;
                        GetWindowThreadProcessId(game, &pid);
                        ::ShaderBeam::LsLog("click sent to the game: message 0x%X, window %p (class '%s', process %lu) at %d,%d, %s (error %lu)", (unsigned)message, (void*)game, cls,
                              (unsigned long)pid, (int)p.x, (int)p.y, posted ? "accepted" : "refused by Windows", (unsigned long)err);
                        s_fwdLogged++;
                        if(!posted)
                            s_fwdFailed++;
                    }
                }
            }
            if(mouseButton || message == WM_MOUSEWHEEL)
                return 0;
        }
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
        if(LOWORD(wParam) != WA_INACTIVE && s_appReady && !s_inTray && !s_testOn && GetTickCount64() > s_quietUntil)
            ShowHud(hWnd);
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_DISPLAYCHANGE:
        if(s_appReady)
        {
            s_shaderBeam.m_options.RefreshMonitors(!s_hzSaved.empty()); // a screen was connected / disconnected or changed mode
            // only a real change of the screens (layout, resolution, rate) restarts: a game window resized by Full SBS can also send this
            // message, and restoring it sends it again (restart loop between a Full SBS profile and another one)
            static std::string s_lastLayout;
            std::string        layout;
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
                MONITORINFOEXW mi {};
                mi.cbSize = sizeof(mi);
                DEVMODEW dm {};
                dm.dmSize = sizeof(dm);
                if(GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
                    *(std::string*)p += std::to_string(mi.rcMonitor.left) + "," + std::to_string(mi.rcMonitor.top) + "," + std::to_string(dm.dmPelsWidth) + "x" +
                                        std::to_string(dm.dmPelsHeight) + "@" + std::to_string(dm.dmDisplayFrequency) + ";";
                return TRUE;
            }, (LPARAM)&layout);
            const bool same = layout == s_lastLayout;
            s_lastLayout    = layout;
            if(same)
                ::ShaderBeam::LsLog("display change ignored (same screens)");
            else if(GetTickCount64() > s_ownChangeUntil)
                s_displayChangeAt = GetTickCount64(); // not a refresh rate change of Nbeam: safety restart
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_TIMER: {
        if(wParam == TIMER_MENU_TOP)
        {
            EnumThreadWindows(GetCurrentThreadId(), RaiseMenuProc, 0);
            break;
        }
        if(wParam == TIMER_RETRY_START)
        {
            // the start failed (no display, graphics card or Windows error): try again
            KillTimer(hWnd, TIMER_RETRY_START);
            if(!s_quitting)
            {
                s_refreshDisplays = true; // the display list is read again
                PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
            }
            break;
        }
        if(wParam == 2)
        {
            if(s_appReady && !s_standby && s_tbShown != (!s_shaderBeam.m_options.minimizeToTray || s_shaderBeam.m_options.ui))
                TaskbarButton(hWnd, !s_tbShown); // with the tray option the taskbar button only exists while the HUD is visible
            HotkeyPoll(hWnd);
            AdjustPoll();
            EmitterInputPoll();
            TestPoll(hWnd);
            KeepAboveScalerWindow(hWnd);
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
        {
            // the emitter is looked for every 2 s (plugged in / unplugged): the red note of the bottom bar follows it
            static int s_emitterTick = 0;
            if((++s_emitterTick % 2) == 0)
                s_shaderBeam.m_options.emitterPresent = EmitterRecognized();
        }
        // settings changed in the HUD or by a hotkey: saved at once into the active profile (the profile of the detected program
        // while it runs), so nothing is lost if Nbeam is closed or stops unexpectedly
        if((s_shaderBeam.m_active || s_standby) && s_shaderBeam.m_ui.TakeSaveRequest())
            s_shaderBeam.m_options.Save(s_shaderBeam.Shaders());
        RefreshRatePoll(hWnd);
        DisplayChangePoll(hWnd);
        LowRatePoll(hWnd);
        s_shaderBeam.m_ui.UpdateCaptureTone(true);
        ProcessAppearPoll(hWnd);
        FpsCapPoll();
        DcCapPoll();
        if(s_noWindowRestartAt && GetTickCount64() >= s_noWindowRestartAt)
        {
            s_noWindowRestartAt = 0;
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0); // the program closed its window but still runs
        }
        OutputAutoPoll(hWnd);
        WatchdogPoll(hWnd);
        if(s_shaderBeam.m_options.ui)
            s_shaderBeam.UpdateVsyncRate();
        break;
    }
    case WM_USER_RESTART: {
        if(s_quitting)
            break; // a restart still queued while Nbeam closes: ignored
        ::ShaderBeam::LsLog("restart (process='%s' window=%p standby=%d) by %s", s_shaderBeam.m_options.captureProcess.c_str(), (void*)s_shaderBeam.m_options.captureWindow, (int)s_standby, wParam ? (const char*)wParam : "?");
        {
            // the screens (names, refresh rates) are read again at each restart, in a thread of their own (a slow reading that disturbs the
            // graphics driver: never while the effects run, never on the render thread). Nbeam start reads them too.
            static std::atomic<bool> s_monBusy { false };
            if(!s_monBusy.exchange(true))
            {
                bool keepHz = false; // a detection changes the refresh rate of a screen while its program runs
                std::lock_guard<std::recursive_mutex> linksLock(s_shaderBeam.m_options.linksMutex);
                for(const auto& l : s_shaderBeam.m_options.profileLinks)
                    keepHz = keepHz || (l.hz > 0 && l.enabled && s_shaderBeam.m_options.captureWindow && IsWindow(s_shaderBeam.m_options.captureWindow));
                std::thread([keepHz]() {
                    s_shaderBeam.m_options.RefreshMonitors(keepHz);
                    s_monBusy = false;
                }).detach();
            }
        }
        {
            // restarts in a loop (8 in 20 s): not a normal use, Nbeam starts again completely
            static std::deque<ULONGLONG> s_restartTimes;
            const ULONGLONG              t = GetTickCount64();
            s_restartTimes.push_back(t);
            while(!s_restartTimes.empty() && t - s_restartTimes.front() > 20000)
                s_restartTimes.pop_front();
            if(s_restartTimes.size() >= 8)
            {
                s_restartTimes.clear();
                if(SelfRestart(hWnd, "the renderer restarted 8 times in 20 s"))
                    break;
            }
        }
        KillTimer(hWnd, TIMER_RETRY_START);
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
        SetClickThrough(hWnd, !s_shaderBeam.m_options.ui); // (window style for "Direct presentation", set while nothing is presented)
        if(!s_standby || !s_shaderBeam.m_options.windowMode || s_effectsOn)
            PlaceWindow(hWnd);
        RunOrStandby(hWnd); // started, or the window of the window mode
        break;
    }
    break;
    case WM_USER_BENCHMARK: {
        if(s_shaderBeam.m_active)
            s_shaderBeam.RunBenchmark();
        break;
    }
    case WM_USER_AUTOSOURCE: {
        // Auto source found a 3D format (or the window changed: 0 = 2D): the 3D source format changes as if it was chosen by hand
        auto& o = s_shaderBeam.m_options;
        auto& e = o.emitter;
        if(e.autoSource && (wParam <= 3 || wParam == 5 || wParam == 6) && (int)e.sourceFormat != (int)wParam)
        {
            e.sourceFormat = (::ShaderBeam::StereoSource)wParam;
            o.liveReload   = true;
            // the HUD must know the format the 3D now runs with: otherwise choosing Off afterwards (Auto source unchecked) was seen as "no
            // change of format", only the emitters were stopped, and the picture stayed frozen (3D still built for the detected format)
            s_shaderBeam.m_ui.SourceRebuilt(e.sourceFormat);
        }
        break;
    }
    case WM_USER_GLASSESTEST: {
        // motion test: 1 = start (or adjust another profile, Options::testProfile), 0 = end
        if(wParam == 0)
            EndMotionTest(hWnd);
        else
            StartMotionTest(hWnd);
        break;
    }
    case WM_USER_EFFECTS: {
        auto& o = s_shaderBeam.m_options;
        if(wParam == 2)
        {
            // the window mode option changed (Application)
            if(o.windowMode)
            {
                s_effectsOn     = s_linkActive; // a detected program running keeps its effects
                s_effectsByLink = s_linkActive;
                s_setShow       = true;
                s_setActivate   = true;
            }
            else
            {
                s_effectsOn     = true;
                s_effectsByLink = false;
                s_hudAfterStart = s_standby; // from the window: the HUD over the screen takes over
            }
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
        }
        else if(o.windowMode)
        {
            if(wParam == 1)
                SetEffectsOn(hWnd, true, true);
            else
            {
                s_setReduce = true; // the button over the game: the window stays reduced
                SetEffectsOn(hWnd, false, false);
            }
        }
        break;
    }
    case WM_USER_LLKEY:
        HookKey(hWnd, (unsigned)wParam, (unsigned)lParam);
        break;
    case WM_USER_NOWINDOW: {
        if(::ShaderBeam::Options::IsScalerExe(s_shaderBeam.m_options.captureProcess))
            ::ShaderBeam::LsLog("captured layer window closed");
        s_shaderBeam.m_options.captureWindow = s_shaderBeam.m_options.captureProcess.empty() ? NULL : s_shaderBeam.m_options.FindCaptureWindow(); // another window of the same program, or the display
        if(s_linkActive && !s_shaderBeam.m_options.captureProcess.empty() && !s_shaderBeam.m_options.captureWindow)
        {
            // the game closes: a single restart, the one of the end of its detection (desktop settings back), instead of one for the display
            // capture and a second one 2 s later. The program leaves the taskbar: the desktop mode comes back (if it still has no window after 10 s the restart is done then).
            s_noWindowRestartAt = GetTickCount64() + 10000;
            ProfileLinkPoll(hWnd);
            break;
        }
        PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
        break;
    }
    case WM_HOTKEY: {
        switch(wParam)
        {
        case HOTKEY_QUIT:
            s_testOn = false;
            CloseTestProcess();
            PostMessage(hWnd, WM_DESTROY, 0, 0); // same as Quit in the notification area menu
            break;
        case HOTKEY_RESTART:
            PostMessage(hWnd, WM_USER_RESTART, NB_RESTART_TAG, 0);
            break;
        case HOTKEY_SWAPEYES:
            s_shaderBeam.m_options.emitter.swapEyes = !s_shaderBeam.m_options.emitter.swapEyes;
            s_shaderBeam.m_ui.RequestSave();
            break;
        case HOTKEY_ADDEXE: {
            // the program in the foreground (not Nbeam, not the Windows shell) is added to the profile detections
            if(s_testOn)
                break; // (the test window is Nbeam)
            auto& o   = s_shaderBeam.m_options;
            std::lock_guard<std::recursive_mutex> linksLock(o.linksMutex);
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
                Notify(::ShaderBeam::Tr("No program in the foreground to add"));
                break;
            }
            bool known = false;
            for(const auto& l : o.profileLinks)
                known = known || ::ShaderBeam::Options::NormalizeExeName(l.exe) == name;
            if(known)
                Notify(name + " " + ::ShaderBeam::Tr("is already in the list"));
            else if(o.profileLinks.size() < 64)
            {
                ::ShaderBeam::Options::ProfileLink nl;
                nl.exe      = name;
                nl.screenId = o.ScreenIdOfDisplay(o.shaderDisplayNo, s_shaderBeam.m_ui.m_displays);
                nl.fpsCapMode = o.DefaultFpsCapMode();
                nl.fpsLimit   = o.fpsLimitDefault;
                nl.profile    = o.ProfileForNewProgram();
                o.profileLinks.push_back(nl);
                o.SaveApp();
                Notify(name + " " + ::ShaderBeam::Tr("added to the profile detections"));
            }
            break;
        }
        case HOTKEY_EFFECTS:
            if(s_testOn)
            {
                EndMotionTest(hWnd);
                break;
            }
            if(s_shaderBeam.m_options.windowMode)
            {
                // window mode: the effects start / stop (the window comes back without taking the focus from the game)
                SetEffectsOn(hWnd, !s_effectsOn, false);
                break;
            }
            // same as the HUD button Show / Hide: all effects and the 3D emitter off / on
            s_shaderBeam.m_options.effectsSuspended = !s_shaderBeam.m_options.effectsSuspended;
            s_shaderBeam.m_ui.ShowNote(::ShaderBeam::Tr(s_shaderBeam.m_options.effectsSuspended ? "Effects hidden" : "Effects shown"));
            break;
        case HOTKEY_TOGGLEUI:
            if(s_standby)
            {
                // window mode, effects off: the hotkey shows the window, or sends it back to the notification area / taskbar
                if(s_setWnd && IsWindowVisible(s_setWnd) && !IsIconic(s_setWnd) && GetForegroundWindow() == s_setWnd)
                {
                    if(s_shaderBeam.m_options.minimizeToTray)
                    {
                        s_setShow = false;
                        ShowWindow(s_setWnd, SW_HIDE);
                    }
                    else
                        ShowWindow(s_setWnd, SW_MINIMIZE);
                }
                else
                    ShowSettings(true);
                break;
            }
            if(s_shaderBeam.m_ui.Toggle())
            {
                // UI visible: the effects keep their state (HUD button Show / Hide)
                LeaveTray(hWnd);
                SetClickThrough(hWnd, false);
                if(ScalerCaptured())
                    BringToFront(hWnd, false); // over Lossless Scaling the game keeps the focus (LS stops scaling otherwise)
                else
                {
                    BringToFront(hWnd, true);
                    ForceForeground(hWnd); // the HUD gets the focus, as if it had been clicked
                }
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
        // Quit can arrive twice (double click, hotkey + menu): the second one would stop everything again and crash on the way out
        if(s_quitting)
            break;
        s_quitting = true;
        ::ShaderBeam::AudioKeepAliveSet(false);
        ::ShaderBeam::GetFrameLimiter().Release(); // (the game gets its RTSS frame limit back)
        s_testOn   = false;
        CloseTestProcess(); // the cars window closes with Nbeam
        StopKeyboardHook();
        KillTimer(hWnd, TIMER_RETRY_START);
        RestoreRefreshRateNow();
        RemoveTrayIcon(hWnd);
        SaveSettingsPlacement(); // (before the window is closed)
        StopSettingsUI();
        s_standby = false;
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
