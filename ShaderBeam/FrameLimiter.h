/*
Nbeam 3D Vision: shader effect overlay -- game frame-rate sync through RivaTuner Statistics Server
MIT License

Anti-cheat friendly by design: Nbeam never opens, injects into, hooks or reads the game process. It only
  * looks the executable name up in the process list (Toolhelp snapshot: no handle to the game is opened) and
  * asks RTSS (the usual frame limiter that most anti-cheats tolerate) to change the frame limit of that program's
    RTSS profile, through the RTSS profile interface (RTSSHooks64.dll, the same one MSI Afterburner uses).
The limit that RTSS had before is restored when the option is switched off, the target changes, or Nbeam quits.
*/

#pragma once

#include <mutex>
#include <string>

namespace Nbeam
{

class FrameLimiter
{
public:
    // targetFps <= 0 or hwnd == nullptr: no limit wanted (restores the previous one)
    void Update(void* hwnd, double targetFps);
    void Release(); // restore RTSS's previous limit
    std::string Status() const;

private:
    void Set(const std::string& status);
    void Apply(const std::string& exe, double fps);
    bool Restore();

    mutable std::mutex m_mutex;
    std::string        m_status { "off" };

    std::string m_exe;        // program currently limited
    double      m_fps { 0 };  // limit currently applied
    bool        m_haveOrig { false };
    unsigned    m_origLimit { 0 };
    unsigned    m_origDenominator { 1 };
    std::string m_limitProp, m_denomProp;
};

FrameLimiter& GetFrameLimiter();

// RTSS itself: installed on this PC (running, or found in its usual folders / the installed programs), running, and started minimized
bool RtssInstalled();
bool RtssRunning();
bool StartRtssMinimized(); // false when it could not be started (not installed, refused...)

// Display Commander (ReShade addon): installed on this PC (its folder in %LocalAppData%\Programs). It reads its frame limit from the game's
// DisplayCommander.ini (key FpsLimiter.TargetFps, next to the game exe) only when the game starts, so the limit is written there each time the
// program is detected and when it closes. Nothing is injected or hooked.
bool DcInstalled();
// The path of the program running in that window (empty when unknown); pid receives its process id
std::wstring ProgramPathOfWindow(void* hwnd, unsigned long* pid);
bool         ProgramRunning(unsigned long pid);
// Writes the limit (0 = no limit, up to 1000) in DisplayCommander.ini next to the game exe. ok = false: the addon was not found next to the exe or
// the file could not be written. Returns a short text for the log.
std::string DcWriteLimit(const std::wstring& exePath, int fps, bool& ok);
// Same for ReLimiter (ReShade addon, relimiter.ini next to the game exe, key target_fps under [FrameLimiter]; 0 = no limit, otherwise 30 to 1000).
// ReLimiter has no installation folder to look for: it is only used for a game that has its addon (or its settings file) next to the exe.
std::string RlWriteLimit(const std::wstring& exePath, int fps, bool& ok);

} // namespace Nbeam
