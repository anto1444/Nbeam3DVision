/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "EmitterSettings.h"
#include "SubFrameModes.h"
#include <mutex>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace Nbeam
{

#define NBEAM_TITLE "Nbeam 3D Vision"

#define MIN_SUBFRAMES 1
#define MAX_SUBFRAMES 20
#define MAX_INPUTS 5

#define WM_USER_RESTART (WM_USER)
#define NB_STR2(x) #x
#define NB_STR(x) NB_STR2(x)
#define NB_RESTART_TAG ((WPARAM)(__FILE__ ":" NB_STR(__LINE__))) // (wParam of a restart: who asked for it, written to Nbeam_ls.log)
#define WM_USER_BENCHMARK (WM_USER + 1)
#define WM_USER_NOWINDOW (WM_USER + 2)
#define WM_USER_EFFECTS (WM_USER + 4) // window mode: wParam 1 = activate the effects, 0 = deactivate, 2 = the window mode option changed
#define WM_USER_AUTOSOURCE (WM_USER + 6) // Auto source found a 3D format: wParam = StereoSource (0 = back to 2D)
#define WM_USER_EMITTERSTUCK (WM_USER + 7) // the glasses signals are stuck (inconsistent emitter timing): error message, then Nbeam closes
#define WM_USER_GLASSESTEST (WM_USER + 5) // 3D panel: wParam = glasses test to show (1, 2), 0 = end it
#define TIMER_RETRY_START 3 // window timer: the start failed (no display, Windows / graphics card error): try again

#define HOTKEY_TOGGLEUI 0
#define HOTKEY_RESTART 1
#define HOTKEY_QUIT 2
#define HOTKEY_SWAPEYES 3
#define HOTKEY_EFFECTS 4 // shows / hides the effects (same as the HUD button Show / Hide)

// adjustment hotkeys (decrease / increase pairs, one step per press)
#define HOTKEY_ADDEXE 5 // adds the program in the foreground to the profile detections
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
        bool  showStats { false };   // banner of the HUD: RAM used, render latency (with the frame buffer) and late frames
        float scale { 1.0f };        // HUD size (dragging the corner of the HUD changes it), 0.6 .. 2.0
        bool  autoScale { true };    // size follows the Windows display scale (100 % in Windows = normal size); off: the sizes below
        float scaleWin { 1.0f };     // size of the interface in the window of the window mode (separate from the HUD over the screen)
    } hud;
    bool charts { false };
    int  shaderProfileNo { 0 };
    int  captureAdapterNo { 0 };
    int  shaderAdapterNo { 0 };
    int  captureDisplayNo { 0 };
    int  shaderDisplayNo { 0 };
    bool outputOnProcess { false }; // output "Process": the final image covers the window of the captured process, whatever its size
    bool outputAuto { false };      // output "Automatic": a full screen / borderless maximized game -> the Windows main display; a smaller windowed game -> "Process"
    int  subFrames { 0 };        // effective value (derived; in Auto mode it follows the measured game frame rate)
    int  subFramesSetting { 0 }; // Manual mode value, 1..MAX_SUBFRAMES (0 in an old profile = pick display Hz / 60 once)
    int  subFramesMode { SF_MANUAL };
    int  PickStereoDivisor(double gameFps) const; // Sync Auto: the n (display Hz / n) whose eye pair rate best fits the game frame rate
    int  StereoSubFrames() const;  // 3D active: SubFrames from the emitter rate (0 = none: 3D off, or Auto found no rate in 80-165 Hz)
    bool StereoLinked() const;     // SubFrames is driven by the emitter rate right now
    int  ManualSubFrames() const;  // what the Manual mode runs: the 3D value when 3D is active, else the typed value
    int  ResolveSubFrames() const; // Manual value (display Hz / 60 when none was ever set)
    // Auto needs a captured process (window); without one the mode falls back to Manual.
    // Auto source: the SubFrames follow the emitter rate (as with any 3D source), so BFI auto is not used while it is on.
    int  EffectiveSubFramesMode() const { return (subFramesMode == SF_AUTO && captureWindow && !emitter.autoSource) ? SF_AUTO : SF_MANUAL; }
    bool SubFramesFollow3D() const { return emitter.sourceFormat != StereoSource::Off || emitter.autoSource; } // a 3D source, or Auto source looking for one
    AutoSettings   autoSub; // Auto: detection interval
    LowFpsSettings lowFps;  // SubFrames threshold: behaviour of the BFI when the SubFrames (Auto or Manual) go above it
    bool uniformBrightness { false }; // brightness kept at the level the BFI has at the SubFrames threshold (see Renderer::UpdateSubFramesMode)
    // phase lock (BFI sync 1 or 3, 2D, captured process, no buffer in use): the cycle is shifted by whole refreshes so the game frames
    // arrive just before the visible subframe (see Renderer::PhaseSample); it follows the SubFrames of BFI auto
    float          phaseInterval { 1.0f };  // seconds between two measures (0.1 to 2), profile setting
    volatile int   phaseState { 0 };        // 0 = off, 1 = measuring, 2 = locked, 3 = game frames at an unsteady moment of the cycle
    volatile float phaseWaitMs { 0.0f };    // median wait of the game frames before they are shown (ms)
    volatile int   phaseCorrections { 0 };  // cycle shifts made since the effects started
    // 3D (stereo emitter) only exists in Manual mode
    bool StereoManual() const { return EffectiveSubFramesMode() == SF_MANUAL; }
    // an emitter rate exists for this display: display Hz / n between 80 and 165 Hz (the range the glasses detect); without it 3D is not available
    bool StereoRateOk() const
    {
        if(vsyncRate <= 1.0f)
            return true; // not measured yet
        for(int n = MIN_SUBFRAMES; n <= MAX_SUBFRAMES; n++)
        {
            const double hz = (double)vsyncRate / n;
            if(hz >= 79.5 && hz <= 165.5)
                return true;
        }
        return false;
    }
    // experimental Full SBS (SBS only): needs a game capture and an output that does not cover the game (not "Process" output)
    bool FullSbsAvailable() const { return captureWindow != 0 && !outputOnProcess && !processOutput; }
    bool FullSbsActive() const { return emitter.sourceFormat == StereoSource::SideBySide && emitter.fullSbs && FullSbsAvailable(); }
    bool Stereo3DWanted() const { return StereoManual() && StereoRateOk() && QueueAllows3D(); }
    bool StereoAllowed() const { return Stereo3DWanted() && !emitter.fpsCapped && !emitter.fullSbsBad; } // (capped: the game runs too fast for stable 3D)
    // game frame rate above which 3D is switched off for stability: half the highest emitter rate + 5 fps (480 Hz: 160 Hz -> 85 fps;
    // 240 Hz: 120 Hz -> 65 fps); 0 when the display has no emitter rate
    double StereoFpsLimit() const
    {
        for(int n = MIN_SUBFRAMES; n <= MAX_SUBFRAMES && vsyncRate > 1.0f; n++)
        {
            const double hz = (double)vsyncRate / n;
            if(hz > 165.5)
                continue;
            return hz >= 79.5 ? hz * 0.5 + 5.0 : 0.0;
        }
        return 0.0;
    }
    int  captureMethod { 0 };
    int  splitScreen { 0 };
    bool splitInvert { false }; // split-screen: the effect on the other half (right / bottom)
    bool hardwareSrgb { false };
    // BFI sync (profile, the menu shown while a program is captured): 0 = none, 1 = phase lock, 2 = frame buffer (frameBuffer)
    int  bfiSync { 0 };
    int  frameBuffer { 0 };    // "Frame buffer": the most game frames kept in advance (1..5, at least 1; the renderer uses 1 up to it); profile setting
    mutable int frameBufferStep { 1 }; // buffer: content frames (eye pairs in 3D) each game frame is shown for (game fps = a divisor of the content rate, SubFrames Manual)
    mutable int frameBufferAuto { 0 }; // the buffer the renderer currently uses (1 up to the value chosen)
    // queued presentation frames; 3D needs at least 3 (0 = the default, which is 3): with 1, 2 or tearing allowed 3D is not available
    // Auto queue: starts at 3 and goes up to 6 when frames are late (Renderer::AdaptQueue); QueueSetting() is the length in use
    int  QueueSetting() const { return queueAuto ? queueAutoCur : maxQueuedFrames; }
    bool QueueAllows3D() const { const int q = QueueSetting(); return q == 0 || (q >= 3 && q < QUEUE_TEARING); }
    int  BfiSyncMode() const { return emitter.sourceFormat != StereoSource::Off ? 2 : bfiSync; } // in 3D the frame buffer is always used (the phase lock is 2D only)
    bool FrameBufferMode() const { return BfiSyncMode() == 2; } // the mode that keeps game frames in advance and adapts the size
    int  BufferSetting() const { return FrameBufferMode() ? (std::min)((std::max)(frameBuffer, 1), 5) : 0; } // the most the buffer may use (0 = none)
    bool FrameBufferIsAuto() const { return BufferSetting() > 0; } // the size is adapted by the renderer
    int  EffectiveFrameBuffer() const { return FrameBufferIsAuto() ? (std::max)(frameBufferAuto, 1) : 0; }
    bool PhaseLockWanted() const { return BfiSyncMode() == 1; }
    mutable float captureBlack { 0.0f };  // black level of the captured image in scRGB units (nits / 80)
    mutable float captureGain { 1.0f };   // brightness gain of the captured image (1 = unchanged)
    mutable float captureGrey { 1.0f };   // grey level (gamma) of the captured image
    bool useHdr { true }; // HDR on by default; no UI option, set "useHdr" in a profile file to change
    int  maxQueuedFrames { 0 }; // queued presentation frames: 0 = driver default, 1..QUEUE_MAX, QUEUE_TEARING = 0 frames, tearing allowed
    static constexpr int QUEUE_TEARING = 15;
    static constexpr int QUEUE_MAX     = 10; // longest queue that can be chosen (a profile saved with a longer one gets this one)
    bool queueAuto { false };           // "Auto" queue (profile setting): the length adapts between QUEUE_AUTO_MIN and QUEUE_AUTO_MAX
    mutable int queueAutoCur { 3 };     // the length in use while queueAuto
    static constexpr int QUEUE_AUTO_MIN = 3, QUEUE_AUTO_MAX = 6;
    int  tearingLine { 0 };     // tearing control: where the flip happens in the refresh (0 = right after vblank, hidden at the top; 100 = bottom), profile setting
    bool Tearing() const { return QueueSetting() == QUEUE_TEARING; }
    bool beamRacing { false }; // tearing: the flip is timed on the real scanout position so the tear lands where the effect hides it; profile setting
    bool directSuspended { false }; // runtime: the captured game lost the focus, so Direct presentation is off until it is in front again
    bool DirectActive() const { return directPresent && !directSuspended; }
    bool drawPointer { false };   // "Draw a pointer" (profile, with Direct presentation): Nbeam draws a pointer while the real one moves (see UI.cpp)
    bool directPresent { false }; // "Direct presentation" (experimental, profile): the overlay is never a layered window, so Windows can show it
                                  // directly (independent flip / hardware plane) instead of composing it; no click-through (see SetClickThrough)
    volatile int   presentMode { -1 };       // measured: -1 unknown, 0 composed by Windows, 1 direct (hardware plane), 3 composition failure
    volatile float presentGlitches { 0.0f }; // measured: frames shown for more or fewer refreshes than presented (late / dropped), per minute
    int  beamLead { 3 };       // beam racing: how early (% of the refresh) the flip is sent, to make up for its delay; profile setting
    mutable int beamRacingState { 0 }; // 1 = working, -1 = the scanout position cannot be read on this display (normal tearing is used)
    bool rememberSettings { true };

    // global hotkeys (index = HOTKEY_* id). mods = MOD_* flags, vk = virtual key, 0 = none
    struct Hotkey
    {
        unsigned mods { 0 };
        unsigned vk { 0 };
    };
    Hotkey hotkeys[HOTKEY_COUNT] = {
        { MOD_CONTROL | MOD_SHIFT, 'B' }, // toggle UI
        { MOD_CONTROL | MOD_SHIFT, 'A' }, // restart
        { MOD_CONTROL | MOD_SHIFT, 'Q' }, // quit
        { MOD_CONTROL | MOD_SHIFT, 'E' }, // swap eyes
        { MOD_CONTROL | MOD_SHIFT, 'D' }, // show / hide the effects
        { MOD_CONTROL | MOD_SHIFT, 'P' }, // add the foreground program to the detections
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
    bool         appHotkeys { false }; // the hotkeys were read from Nbeam.app.cfg (global): the profiles no longer change them
    template<class Section> void ReadHotkeys(Section& hk);
    bool         hotkeysSuspended { false }; // while a new key is being captured in the UI
    bool               RegisterHotkey(int id) const;
    void               UnregisterHotkey(int id) const;

    // Settings: the desktop has its own settings (DESKTOP_SETTINGS, used whenever no detected program runs); the profiles (*.ini of the
    // Profils folder) only apply while their program runs (profile detections), or while one is adjusted over the motion test.
    // profileFile is the active file (file name only).
    static constexpr const char* DESKTOP_SETTINGS = "Bureau.cfg";
    std::string profileFile { DESKTOP_SETTINGS };
    std::string pendingProfile; // set by the UI, applied on the next restart
    std::string testProfile;    // set by the UI before it starts the motion test: the profile adjusted over the test (empty = the desktop settings)
    bool        IsDesktopSettings() const { return _stricmp(profileFile.c_str(), DESKTOP_SETTINGS) == 0; }
    static std::filesystem::path ExeDir();
    static std::filesystem::path ProfilesDir();   // "Profils" folder next to the exe: where the profile .ini files live
    static std::filesystem::path LogFile(const wchar_t* name); // "log" folder next to the exe (created when needed): where the log files live
    static bool                  FirstRun(); // true when the profile folder was created by this start (first launch)
    static void                  EnsureProfilesDir(); // creates it if missing and moves the old profiles (*.ini next to the exe) into it
    static std::vector<std::string> ListProfiles(); // *.ini file names (without imgui.ini)
    void InitProfile();                              // read the remembered active profile name (and the interface language)
    static void SaveLanguage(int lang);              // LANG_FR / LANG_EN, remembered in Nbeam3DVision.language.txt
    void RememberProfile() const;                    // write it

    // application settings (not part of a profile), saved in Nbeam.app.cfg next to the exe
    volatile bool effectsSuspended { false }; // HUD button Show / Hide: all effects and the 3D emitter are off until Show is pressed (HUD shown or not)
    bool          startWithWindows { false };
    bool          minimizeToTray { true }; // the notification area icon always exists
    bool          keepAudio { true };      // inaudible sound on the default audio device while Nbeam runs (keeps Dolby Atmos active), Nbeam.app.cfg
    bool          startMinimized { false };
    bool          runAsAdmin { false }; // "Run as administrator": at start Nbeam asks Windows (UAC) to start again with administrator rights
    bool          windowMode { true }; // window mode, always on: Nbeam opens as a normal window, the effects run only when activated or when a detected program runs
    bool          saveLog { true }; // "Save the event log" (Application): off = no event or history of Nbeam is recorded, and the files are deleted
    bool          closeMinimizes { true }; // "Minimize instead of closing": the cross of the window minimizes it (to the notification area with that option)
    volatile bool motionTest { false };   // motion test running (the cars window, see CarTest.h, captured like a game); not saved
    int           ufoSpeed { 960 };       // motion test (Nbeam.app.cfg): speed of the cars, pixels per second (0 = still)
    int           carSize { 100 };        // ... size of the cars, % (100 = 3 rows; smaller = more cars)
    float         testBg[3] { 0.16f, 0.17f, 0.19f }; // ... background colour of the cells
    bool          rtssAvailable { false }; // RTSS is installed on this PC (checked once at start): otherwise its automatic frame cap is greyed out
    bool          dcAvailable { false };   // Display Commander (ReShade addon) is installed on this PC (checked once at start): otherwise its frame cap is greyed out
    int           fpsCapDefault { 0 };     // "Auto fps cap" given to the programs added to the detections (0 none, 1 RTSS, 2 Display Commander), Nbeam.app.cfg
    int           DefaultFpsCapMode() const { return (fpsCapDefault == 1 && rtssAvailable) || (fpsCapDefault == 2 && dcAvailable) || fpsCapDefault == 3 ? fpsCapDefault : 0; }
    std::string   profileOnAdd;          // profile given to the programs added to the detections ("" = none), Nbeam.app.cfg
    std::string   ProfileForNewProgram() const; // profileOnAdd if that profile still exists, otherwise ""
    int           fpsLimitDefault { 0 };   // "Frame limit" given to the programs added to the detections (0 = no limit, up to 1000), Nbeam.app.cfg
    int           rlFpsLimit { 0 };        // (old global ReLimiter frame limit: only read once to fill the frame limit of each program)
    int           dcFpsLimit { 0 };        // (old global Display Commander frame limit: the same)
    volatile bool emitterPresent { true }; // a 3D emitter is recognized (NVIDIA driver loaded, or a COM port emitter set); red note in the bottom bar otherwise
    std::string   defaultProfile; // old "Default profile" (read once to make the desktop settings from it, then always empty)
    struct ProfileLink
    {
        std::string exe;     // full path of the program, or just its file name
        std::string profile; // profile (.ini) loaded while that program runs
        int         delay { 0 }; // seconds the program must have been running before the profile is applied
        bool        enabled { true }; // unchecked = the link is ignored
        std::string path;             // full path of the program the last time it was seen running (when "exe" is only a file name): where the frame limit is written before the game starts
        std::string screenId;         // screen (KnownMonitor::id) this profile is for; up to 3 links of the same program, each on a different screen
        int         hz { 0 };         // Windows refresh rate of that screen while the program runs (0 = no change); every screen of the program changes at detection
        int         fpsCapMode { 0 }; // automatic game frame cap: 0 none, 1 RTSS (RivaTuner Statistics Server: half the 3D rate in 3D, the content rate with BFI / CRT, followed live),
                                      // 2 Display Commander (fpsLimit written in the game's DisplayCommander.ini), 3 ReLimiter (fpsLimit written in the game's relimiter.ini)
        int         fpsLimit { 0 };   // frame limit of this program on this screen for Display Commander / ReLimiter (0 = no limit, up to 1000): written when Nbeam starts
                                      // and when its screen changes, for every program at once, outside the start of the game
    };
    std::vector<ProfileLink> profileLinks;
    // profileLinks is edited by the HUD (render thread) and read by the detections (main thread): both lock this. Lock order: this
    // mutex first, then the monitors one. Never held while waiting for the render thread.
    mutable std::recursive_mutex linksMutex;
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
    // position of the screens in the list (Global settings: Screen 1, 2, 3...): the two screens exchange their places
    void                      SwapKnownMonitors(const std::string& idA, const std::string& idB);
    // forgets a screen that is not connected (false if it is connected or unknown)
    bool                      RemoveKnownMonitor(const std::string& id);
    int                       DisplayNoForScreen(const std::string& id, const std::vector<DisplayInfo>& displays) const; // index in displays of a connected screen, -1 if none
    std::string               ScreenIdOfDisplay(unsigned displayNo, const std::vector<DisplayInfo>& displays) const;
    static std::vector<int>  DisplayRefreshRates(HMONITOR monitor); // rates (Hz) the display supports at its current resolution
    static int               DisplayRefreshRate(HMONITOR monitor);  // current rate, 0 if unknown
    static bool              SetDisplayRefreshRate(HMONITOR monitor, int hz);
    HMONITOR                 OutputMonitor(const std::vector<DisplayInfo>& displays) const; // display the output is shown on
    void                     LoadApp();
    void                     SaveApp() const;
    static bool              AppRunAsAdmin();  // reads "runAsAdmin" in Nbeam.app.cfg (before anything else starts)
    static bool              IsElevated();     // this process runs with administrator rights
    void                     ApplyAutoStart() const; // adds / removes the Windows startup entry (current user)
    static bool              IsExeOnTaskbar(const std::string& pathOrName); // the program has a window shown on the taskbar (otherwise it counts as closed)
    HWND                      TaskbarWindowOfExe(const std::string& pathOrName) const;  // the program's window shown on the taskbar (NULL if none)
    std::string               ScreenIdOfExeWindow(const std::string& pathOrName) const; // screen where the program's taskbar window is ("" if unknown)
    static void              PeekProfile(const std::string& file, bool& stereo); // 3D of a profile, without loading it
    int                      setWinRect[4] { 0, 0, 0, 0 }; // window mode: position and size of the window (left, top, right, bottom; 0 = never saved)
    bool                     setWinMax { false };          // window mode: the window was maximized
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
    // Lossless Scaling: what is captured is its scaling layer (the full-screen window it draws the scaled game on), not its settings window
    static bool        IsScalerExe(const std::string& pathOrName); // losslessscaling.exe
    static bool        IsScalerLayer(HWND hWnd);                  // a topmost window covering (almost) a whole screen

    void Save(const ShaderManager& shaderManager) const;
    void Load(ShaderManager& shaderManager);
};

// Small trace of what happens around Lossless Scaling's layer (detections, restarts, raises of the overlay): Nbeam_ls.log in the log
// folder next to the exe, 256 KB at most. Only used to find out why a capture would start and stop in a loop.
void LsLog(const char* fmt, ...);
// last lines written to the log (formatted like in the file), `ms` milliseconds back: kept for the capture of the O key (see EmitterManager::DumpRecent)
std::vector<std::string> LsLogRecent(int ms);
// the O key (seen by the keyboard hook): QPC time of the press, taken by the glasses tracer at its next frame (0 = none)
extern std::atomic<long long> g_desyncRequestQpc;
// "Save the event log" (Application): off = nothing is recorded any more (log, O key capture, crash report, emitter reader log)
extern std::atomic<bool> g_logEnabled;
void DeleteEventLogs(); // deletes the files of the events (log folder, and next to the exe for the ones of older versions): Nbeam_ls.log, Nbeam_desync.log, Nbeam_crash.txt, Nbeam_emitter_input.log
void LsLogFlush(); // waits until every queued line is in the file (before leaving the process)
} // namespace Nbeam
