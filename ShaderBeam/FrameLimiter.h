/*
ShaderBeam: shader effect overlay -- game frame-rate sync through RivaTuner Statistics Server
MIT License

Anti-cheat friendly by design: ShaderBeam never opens, injects into, hooks or reads the game process. It only
  * looks the executable name up in the process list (Toolhelp snapshot: no handle to the game is opened) and
  * asks RTSS (the usual frame limiter that most anti-cheats tolerate) to change the frame limit of that program's
    RTSS profile, through the RTSS profile interface (RTSSHooks64.dll, the same one MSI Afterburner uses).
The limit that RTSS had before is restored when the option is switched off, the target changes, or ShaderBeam quits.
*/

#pragma once

#include <mutex>
#include <string>

namespace ShaderBeam
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

} // namespace ShaderBeam

namespace ShaderBeam
{

// Second, RTSS-free technique: the NVIDIA driver's own per-application "Max Frame Rate" (Frame Rate Limiter,
// the same setting as in the NVIDIA Control Panel), written through the official NVAPI driver-settings (DRS)
// interface. Nothing is injected into or read from the game; the driver does the limiting. Integer fps only.
// NVIDIA GPUs only. The previous value (or the profile that ShaderBeam created) is restored on release.
class NvDriverLimiter
{
public:
    void        Update(void* hwnd, double targetFps);
    void        Release();
    std::string Status() const;

private:
    void Set(const std::string& status);
    bool Restore();

    mutable std::mutex m_mutex;
    std::string        m_status { "off" };

    std::string m_exe;
    unsigned    m_fps { 0 };
    bool        m_createdProfile { false };
    bool        m_hadValue { false };
    unsigned    m_origValue { 0 };
    // opaque NVAPI handles of the profile being edited
};

NvDriverLimiter& GetNvDriverLimiter();

} // namespace ShaderBeam
