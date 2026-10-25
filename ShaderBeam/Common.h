/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "EmitterSettings.h"
#include "SubFrameModes.h"

namespace ShaderBeam
{

#define SHADERBEAM_TITLE "Nbeam 3D Vision v1.0.0"

#define MIN_SUBFRAMES 1
#define MAX_SUBFRAMES 20
#define MAX_INPUTS 5

#define WM_USER_RESTART (WM_USER)
#define WM_USER_BENCHMARK (WM_USER + 1)
#define WM_USER_NOWINDOW (WM_USER + 2)

#define HOTKEY_TOGGLEUI 0
#define HOTKEY_RESTART 1
#define HOTKEY_QUIT 2
#define HOTKEY_SWAPEYES 3
#define HOTKEY_DESKTOPOFF 4 // toggles "Effects always disabled (desktop capture)"

// adjustment hotkeys (decrease / increase pairs, one step per press)
#define HOTKEY_DETECTION 5 // checks / unchecks the profile detections (all of them)
#define HOTKEY_ADJUST_FIRST 6
#define HOTKEY_SUBFRAMES_DEC 6
#define HOTKEY_SUBFRAMES_INC 7
#define HOTKEY_CLEAR_DEC 8
#define HOTKEY_CLEAR_INC 9
#define HOTKEY_SYNCOFFSET_DEC 10
#define HOTKEY_SYNCOFFSET_INC 11
#define HOTKEY_DURATION_DEC 12
#define HOTKEY_DURATION_INC 13

#define HOTKEY_COUNT 14

//#define RGB_TEST

struct AdapterInfo
{
    unsigned                     no;
    std::string                  name;
    winrt::com_ptr<IDXGIAdapter> adapter;
    LUID                         luid;
};

struct DisplayInfo
{
    unsigned    no;
    unsigned    width;
    unsigned    height;
    std::string name;
    HMONITOR    monitor;
};

struct ShaderInfo
{
    unsigned    no;
    std::string name;
};

enum ParameterType
{
    ParamFloat,
    ParamInt
};

struct ParameterInfo
{
    unsigned                          no;
    std::string                       name;
    std::string                       hint;
    ParameterType                     type;
    const std::map<int, std::string>& dropdown;
    union
    {
        struct
        {
            float* value;
            float  min;
            float  max;
            float  def;
        } fp;
        struct
        {
            int* value;
            int  min;
            int  max;
            int  def;
        } ip;
    } p;
};

struct InputInfo
{
    HWND        hwnd;
    std::string name;
    bool        fullscreen;
    unsigned    captureMethod;
    unsigned    captureDisplayNo;
};

class CaptureBase;

struct CaptureInfo
{
    unsigned                     no;
    std::string                  name;
    std::shared_ptr<CaptureBase> api;
};

struct BenchmarkResult
{
    float totalFPS { 0 };
    float copyFPS { 0 };
    float renderFPS { 0 };
    float presentFPS { 0 };
};

constexpr int MONITOR_LCD  = 0;

class ShaderManager;

struct Options
{
    // UI options
    bool ui { true };
    // HUD appearance (accent color = NVIDIA green by default)
    struct Hud
    {
        float sdrBrightness { 100.0f }; // Windows in SDR: HUD brightness, 20 .. 100 % (100 = full brightness)
        float hdrBlackNits { 0.0f };   // Windows HDR only: black level of the captured image, 0 .. 20 nits (values below it become pure black)
        float hdrNits { 0.0f };        // Windows HDR only: brightness of the captured image only (not the HUD), -1000 .. +1000 nits around SDR white (0 = unchanged)
        float hdrGrey { 1.0f };        // Windows HDR only: grey level (mid-tone gamma) of the captured image, 0.5 .. 2.0 (1 = unchanged)
        float r { 0.463f }, g { 0.725f }, b { 0.0f };
        float contrast { 1.0f };     // 0.5 .. 1.8
        float transparency { 0.0f }; // 0 = opaque .. 0.8
        int   bannerSeconds { 5 };   // 3D banner display time: 0 .. 10 s, 11 = always
        int   bannerCorner { 0 };    // 0 bottom left, 1 bottom right, 2 top right, 3 top left
        float scale { 1.0f };        // HUD size (dragging the corner of the HUD changes it), 0.6 .. 2.0
    } hud;
    bool charts { false };
    int  shaderProfileNo { 0 };
    int  captureAdapterNo { 0 };
    int  shaderAdapterNo { 0 };
    int  captureDisplayNo { 0 };
    int  shaderDisplayNo { 0 };
    bool outputOnProcess { false }; // output "Process": the final image covers the window of the captured process, whatever its size
    int  subFrames { 0 };        // effective value (derived; in Auto mode it follows the measured game frame rate)
    int  subFramesSetting { 0 }; // Manual mode value, 1..MAX_SUBFRAMES (0 in an old profile = pick display Hz / 60 once)
    int  subFramesMode { SF_MANUAL };
    int  StereoSubFrames() const;  // 3D active: SubFrames from the emitter rate (0 = none: 3D off, or Auto found no rate in 80-165 Hz)
    bool StereoLinked() const;     // SubFrames is driven by the emitter rate right now
    int  ManualSubFrames() const;  // what the Manual mode runs: the 3D value when 3D is active, else the typed value
    int  ResolveSubFrames() const; // Manual value (display Hz / 60 when none was ever set)
    // Auto needs a captured process (window); without one the mode falls back to Manual.
    int  EffectiveSubFramesMode() const { return (subFramesMode == SF_AUTO && captureWindow) ? SF_AUTO : SF_MANUAL; }
    AutoSettings   autoSub; // Auto: detection interval
    LowFpsSettings lowFps;  // Auto: behaviour of the BFI when the game runs below the threshold
    // 3D (stereo emitter) only exists in Manual mode
    bool StereoAllowed() const { return EffectiveSubFramesMode() == SF_MANUAL; }
    int  captureMethod { 0 };
    int  splitScreen { 0 };
    bool hardwareSrgb { false };
    int  monitorType { MONITOR_LCD };
    int  frameBuffer { 0 };    // anti-stutter buffer: game frames kept in advance (-1 = Auto, 0 = off, 1..5); profile setting
    mutable int frameBufferAuto { 1 }; // Auto: the size the renderer currently uses (1..5)
    int  EffectiveFrameBuffer() const { return frameBuffer < 0 ? frameBufferAuto : frameBuffer; }
    mutable float captureBlack { 0.0f };  // black level of the captured image in scRGB units (nits / 80)
    mutable float captureGain { 1.0f };   // brightness gain of the captured image (1 = unchanged)
    mutable float captureGrey { 1.0f };   // grey level (gamma) of the captured image
    bool useHdr { true }; // HDR on by default; no UI option, set "useHdr" in a profile file to change
    int  maxQueuedFrames { 0 }; // queued presentation frames: 0 = driver default, 1..14, QUEUE_TEARING = 0 frames, tearing allowed
    static constexpr int QUEUE_TEARING = 15;
    int  tearingLine { 0 };     // tearing control: where the flip happens in the refresh (0 = right after vblank, hidden at the top; 100 = bottom), profile setting
    bool Tearing() const { return maxQueuedFrames == QUEUE_TEARING; }
    bool rememberSettings { true };

    // global hotkeys (index = HOTKEY_* id). mods = MOD_* flags, vk = virtual key, 0 = none
    struct Hotkey
    {
        unsigned mods { 0 };
        unsigned vk { 0 };
        unsigned pad { 0 }; // gamepad combination (Pad:: button bits), 0 = none
    };
    Hotkey hotkeys[HOTKEY_COUNT] = {
        { MOD_CONTROL | MOD_SHIFT, 'B', 0x0030 }, // toggle UI (pad: Back + Start)
        { MOD_CONTROL | MOD_SHIFT, 'A' }, // restart
        { MOD_CONTROL | MOD_SHIFT, 'Q' }, // quit
        { MOD_CONTROL | MOD_SHIFT, 'E' }, // swap eyes
        { MOD_CONTROL | MOD_SHIFT, 'D' }, // effects always disabled (desktop capture)
        { MOD_CONTROL | MOD_SHIFT, 'L' }, // profile detection on / off
        { MOD_CONTROL | MOD_SHIFT, VK_F1 },  // SubFrames -
        { MOD_CONTROL | MOD_SHIFT, VK_F2 },  // SubFrames +
        { MOD_CONTROL | MOD_SHIFT, VK_F3 },  // Clear frames -
        { MOD_CONTROL | MOD_SHIFT, VK_F4 },  // Clear frames +
        { MOD_CONTROL | MOD_SHIFT, VK_F5 },  // Sync offset -
        { MOD_CONTROL | MOD_SHIFT, VK_F6 },  // Sync offset +
        { MOD_CONTROL | MOD_SHIFT, VK_F7 },  // Open duration -
        { MOD_CONTROL | MOD_SHIFT, VK_F8 },  // Open duration +
    };
    static const char* HotkeyActionName(int id);
    static std::string HotkeyText(const Hotkey& h); // "Ctrl+Shift+B"
    // hotkeys that Windows refused to register (another program owns the combination) are polled instead, so they still work
    mutable bool hotkeyOk[HOTKEY_COUNT] {};
    bool         hotkeysSuspended { false }; // while a new key is being captured in the UI
    bool               RegisterHotkey(int id) const;
    void               UnregisterHotkey(int id) const;

    // settings profiles: *.ini files next to the exe; profileFile is the active one (file name only)
    std::string profileFile { "Nbeam3DVision.ini" };
    std::string pendingProfile; // set by the UI, applied on the next restart
    static std::filesystem::path ExeDir();
    static std::filesystem::path ProfilesDir();   // "Profils" folder next to the exe: where the profile .ini files live
    static bool                  FirstRun(); // true when the profile folder was created by this start (first launch)
    static void                  EnsureProfilesDir(); // creates it if missing and moves the old profiles (*.ini next to the exe) into it
    static std::vector<std::string> ListProfiles(); // *.ini file names (without imgui.ini)
    void InitProfile();                              // read the remembered active profile name (and the interface language)
    static void SaveLanguage(int lang);              // LANG_FR / LANG_EN, remembered in Nbeam3DVision.language.txt
    void RememberProfile() const;                    // write it

    // application settings (not part of a profile), saved in Nbeam.app.cfg next to the exe
    volatile bool effectsSuspended { true }; // the effects are off while the HUD is visible (the button can turn them on); hiding the HUD always turns them on
    bool          startWithWindows { false };
    bool          minimizeToTray { true }; // the notification area icon always exists
    bool          startMinimized { false };
    volatile bool desktopEffectsOff { false };   // tray menu: effects (and 3D decoding) are always off while the desktop is captured (no program / window selected)
    struct ProfileLink
    {
        std::string exe;     // full path of the program, or just its file name
        std::string profile; // profile (.ini) loaded while that program runs
        int         delay { 0 }; // seconds the program must have been running before the profile is applied
        bool        enabled { true }; // unchecked = the link is ignored
        std::string screenId;         // screen (KnownMonitor::id) this profile is for; up to 3 links of the same program, each on a different screen
        int         hz { 0 };         // Windows refresh rate of that screen while the program runs (0 = no change); every screen of the program changes at detection
    };
    std::vector<ProfileLink> profileLinks;
    static constexpr int     MAX_LINKS_PER_EXE = 3;

    // screens seen by Nbeam, remembered between runs (Nbeam.app.cfg) with their refresh rates
    struct KnownMonitor
    {
        std::string      id;          // identification (monitor device path, from the display configuration)
        std::string      name;        // friendly name (EDID)
        int              hz { 0 };    // refresh rate when last seen
        std::vector<int> rates;       // refresh rates supported at the resolution when last seen
        bool             connected { false }; // runtime only
        bool             primary { false };   // runtime only: Windows main display
        std::string      gdi;         // runtime only: \\.\DISPLAYn
    };
    std::vector<KnownMonitor> knownMonitors;
    std::vector<std::string>  displayIds;        // runtime: screen id of each display number (filled when the display list is built)
    std::string               shaderScreenId;    // profile: screen chosen for the output (resolved to a display number when the profile is applied)
    std::string               captureScreenId;   // profile: screen chosen for the capture
    int                       linkShaderDisplayNo { -1 };  // one-shot: display numbers applied after the next profile switch (screen of the active detection)
    int                       linkCaptureDisplayNo { -1 };
    void                      RefreshMonitors(bool keepHz);  // enumerates the connected screens and updates the remembered ones
    std::vector<KnownMonitor> MonitorsSnapshot() const;
    int                       DisplayNoForScreen(const std::string& id, const std::vector<DisplayInfo>& displays) const; // index in displays of a connected screen, -1 if none
    std::string               ScreenIdOfDisplay(unsigned displayNo, const std::vector<DisplayInfo>& displays) const;
    static std::vector<int>  DisplayRefreshRates(HMONITOR monitor); // rates (Hz) the display supports at its current resolution
    static int               DisplayRefreshRate(HMONITOR monitor);  // current rate, 0 if unknown
    static bool              SetDisplayRefreshRate(HMONITOR monitor, int hz);
    HMONITOR                 OutputMonitor(const std::vector<DisplayInfo>& displays) const; // display the output is shown on
    void                     LoadApp();
    void                     SaveApp() const;
    void                     ApplyAutoStart() const; // adds / removes the Windows startup entry (current user)
    static bool              IsExeOnTaskbar(const std::string& pathOrName); // the program has a window shown on the taskbar (otherwise it counts as closed)
    std::string               ScreenIdOfExeWindow(const std::string& pathOrName) const; // screen where the program's taskbar window is ("" if unknown)
    static void              PeekProfile(const std::string& file, bool& stereo); // 3D of a profile, without loading it
    double                   hzSwitchMs { 1500.0 }; // average duration of a refresh rate change (black screen), learned
    int                      hzSwitchCount { 0 };

    // stereo emitter (NVIDIA 3D Vision / ComPort), see EmitterSettings.h
    StereoEmitterSettings emitter;

    // internal options
    bool     exclusive { false };
    unsigned wgcBuffers { 16 };
    unsigned gpuThreadPriority { 29 };

    // derived
    // set by the UI, handled by the render thread between two frames (no restart of the renderer)
    bool        liveReload { false };  // shader / inputs / 3D emitters are rebuilt
    bool        liveEmitters { false }; // only the 3D emitters are restarted
    bool        liveLatency { false }; // frame latency changed
    HWND        outputWindow { 0 };
    HMONITOR    captureMonitor { 0 };
    HWND        captureWindow { 0 };
    unsigned    outputWidth { 0 };
    unsigned    outputHeight { 0 };
    float       vsyncDuration { 0 };
    float       vsyncRate { 0 };
    unsigned    swapChainBuffers { 0 };
    bool        crossAdapter { false };
    DXGI_FORMAT format { DXGI_FORMAT_B8G8R8A8_UNORM };

    // captured process, remembered in the profile: executable name + window title (a window handle does not survive a restart)
    std::string        captureProcess;
    std::string        captureTitle;
    static std::string ProcessNameOfWindow(HWND hWnd);
    static std::string NormalizeExeName(const std::string& text); // file name (lower case) of what was typed: a full path is accepted
    bool               ProcessRect(RECT& r) const;                // output "Process": frame rectangle of the captured window (false = use the display)
    volatile bool      processUnfocused { false };                 // the captured program is not in front: effects off
    volatile bool      processResizing { false };                  // the game window is being resized: effects are off until it is stable
    volatile bool      processOutput { false };                    // running: the output window currently covers the captured process
    HWND               FindCaptureWindow() const; // window of captureProcess (best title match), NULL when it is not running
    static void        BringToForeground(HWND hWnd);

    void Save(const ShaderManager& shaderManager) const;
    void Load(ShaderManager& shaderManager);
};
} // namespace ShaderBeam
