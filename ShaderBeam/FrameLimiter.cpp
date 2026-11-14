/*
ShaderBeam: shader effect overlay -- game frame-rate sync through RTSS (see FrameLimiter.h)
MIT License
*/

#include "stdafx.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <cmath>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include "FrameLimiter.h"

namespace ShaderBeam
{

namespace
{
using FnLoad   = void(__cdecl*)(const char*);
using FnSave   = void(__cdecl*)(const char*);
using FnUpdate = void(__cdecl*)();
using FnGet    = BOOL(__cdecl*)(const char*, unsigned char*, DWORD);
using FnSet    = BOOL(__cdecl*)(const char*, unsigned char*, DWORD);

struct Rtss
{
    HMODULE  dll { nullptr };
    FnLoad   load { nullptr };
    FnSave   save { nullptr };
    FnUpdate update { nullptr };
    FnGet    get { nullptr };
    FnSet    set { nullptr };
};

// executable name of a process id, from the process list (no handle to the process is opened)
std::string ExeOfPid(DWORD pid)
{
    std::string result;
    HANDLE      snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof(pe);
    for(BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    {
        if(pe.th32ProcessID == pid)
        {
            for(const wchar_t* c = pe.szExeFile; *c; c++)
                result.push_back(*c < 128 ? (char)*c : '?');
            break;
        }
    }
    CloseHandle(snap);
    return result;
}

// full path of RTSS.exe if it is running (RTSS is not protected, a limited query handle is fine)
std::wstring RunningRtssPath()
{
    std::wstring result;
    HANDLE       snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof(pe);
    for(BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    {
        if(_wcsicmp(pe.szExeFile, L"RTSS.exe") == 0)
        {
            if(HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID))
            {
                wchar_t path[MAX_PATH] {};
                DWORD   size = MAX_PATH;
                if(QueryFullProcessImageNameW(h, 0, path, &size))
                    result = path;
                CloseHandle(h);
            }
            else
                result = L"?"; // running, path unknown
            break;
        }
    }
    CloseHandle(snap);
    return result;
}

bool LoadRtss(Rtss& r, std::string& error)
{
    const auto exe = RunningRtssPath();
    if(exe.empty())
    {
        error = "RTSS is not running (start RivaTuner Statistics Server)";
        return false;
    }
    std::vector<std::filesystem::path> candidates;
    if(exe != L"?")
        candidates.push_back(std::filesystem::path(exe).parent_path() / L"RTSSHooks64.dll");
    candidates.push_back(L"C:\\Program Files (x86)\\RivaTuner Statistics Server\\RTSSHooks64.dll");
    candidates.push_back(L"C:\\Program Files\\RivaTuner Statistics Server\\RTSSHooks64.dll");
    for(const auto& p : candidates)
    {
        if(!std::filesystem::exists(p))
            continue;
        r.dll = LoadLibraryW(p.c_str());
        if(r.dll)
            break;
    }
    if(!r.dll)
    {
        error = "RTSSHooks64.dll not found";
        return false;
    }
    r.load   = (FnLoad)GetProcAddress(r.dll, "LoadProfile");
    r.save   = (FnSave)GetProcAddress(r.dll, "SaveProfile");
    r.update = (FnUpdate)GetProcAddress(r.dll, "UpdateProfiles");
    r.get    = (FnGet)GetProcAddress(r.dll, "GetProfileProperty");
    r.set    = (FnSet)GetProcAddress(r.dll, "SetProfileProperty");
    if(!r.load || !r.save || !r.update || !r.get || !r.set)
    {
        FreeLibrary(r.dll);
        r.dll = nullptr;
        error = "RTSS profile interface not available in this RTSS version";
        return false;
    }
    return true;
}

bool GetU32(const Rtss& r, const char* name, unsigned& v)
{
    unsigned tmp = 0;
    if(!r.get(name, (unsigned char*)&tmp, sizeof(tmp)))
        return false;
    v = tmp;
    return true;
}
bool SetU32(const Rtss& r, const char* name, unsigned v)
{
    return r.set(name, (unsigned char*)&v, sizeof(v)) != FALSE;
}
} // namespace

// path of RTSS.exe: the running one, the usual folders, then the install folder registered in the installed programs
std::wstring RtssExePath()
{
    std::wstring running = RunningRtssPath();
    if(!running.empty() && running != L"?")
        return running;
    for(const wchar_t* p : { L"C:\\Program Files (x86)\\RivaTuner Statistics Server\\RTSS.exe", L"C:\\Program Files\\RivaTuner Statistics Server\\RTSS.exe" })
        if(std::filesystem::exists(p))
            return p;
    for(REGSAM view : { (REGSAM)KEY_WOW64_32KEY, (REGSAM)KEY_WOW64_64KEY })
    {
        HKEY root = nullptr;
        if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &root) != ERROR_SUCCESS)
            continue;
        wchar_t name[256];
        for(DWORD i = 0; RegEnumKeyW(root, i, name, 256) == ERROR_SUCCESS; i++)
        {
            HKEY app = nullptr;
            if(RegOpenKeyExW(root, name, 0, KEY_READ | view, &app) != ERROR_SUCCESS)
                continue;
            wchar_t title[260] {}, dir[MAX_PATH] {};
            DWORD   size = sizeof(title) - sizeof(wchar_t);
            if(RegQueryValueExW(app, L"DisplayName", nullptr, nullptr, (LPBYTE)title, &size) == ERROR_SUCCESS && wcsstr(title, L"RivaTuner Statistics Server"))
            {
                size = sizeof(dir) - sizeof(wchar_t);
                if(RegQueryValueExW(app, L"InstallLocation", nullptr, nullptr, (LPBYTE)dir, &size) == ERROR_SUCCESS && dir[0])
                {
                    const auto p = std::filesystem::path(dir) / L"RTSS.exe";
                    if(std::filesystem::exists(p))
                    {
                        RegCloseKey(app);
                        RegCloseKey(root);
                        return p.wstring();
                    }
                }
            }
            RegCloseKey(app);
        }
        RegCloseKey(root);
    }
    return std::wstring();
}

bool RtssInstalled()
{
    return !RtssExePath().empty();
}

bool RtssRunning()
{
    return !RunningRtssPath().empty();
}

bool StartRtssMinimized()
{
    const auto exe = RtssExePath();
    if(exe.empty())
        return false;
    const auto dir = std::filesystem::path(exe).parent_path().wstring();
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWMINNOACTIVE) > 32;
}

bool DcInstalled()
{
    wchar_t* base = nullptr;
    size_t   len  = 0;
    bool     ok   = false;
    if(_wdupenv_s(&base, &len, L"LOCALAPPDATA") == 0 && base)
    {
        std::error_code ec;
        ok = std::filesystem::is_directory(std::filesystem::path(base) / L"Programs" / L"Display_Commander", ec) && !ec;
        free(base);
    }
    return ok;
}

std::wstring ProgramPathOfWindow(void* hwnd, unsigned long* pid)
{
    DWORD id = 0;
    GetWindowThreadProcessId((HWND)hwnd, &id);
    if(pid)
        *pid = id;
    if(!id)
        return std::wstring();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if(!process)
        return std::wstring();
    wchar_t      path[MAX_PATH * 2] = {};
    DWORD        size               = MAX_PATH * 2;
    std::wstring result;
    if(QueryFullProcessImageNameW(process, 0, path, &size))
        result.assign(path, size);
    CloseHandle(process);
    return result;
}

bool ProgramRunning(unsigned long pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!process)
        return false; // gone (or no longer reachable)
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

namespace
{
// Where a ReShade addon keeps the frame limit it reads when the game starts
struct LimitIni
{
    const wchar_t* label;      // for the log
    const wchar_t* iniName;    // settings file next to the game exe
    const wchar_t* addon64;    // addon files (the program is only touched when one of them, or the settings file, is next to the exe)
    const wchar_t* addon32;
    const char*    section;    // [section] of the file
    const char*    key;        // key of the limit
    bool           decimals;   // value written as "60.000000" (Display Commander) or "60" (ReLimiter)
    const char*    enableKey;  // key to switch on when a limit is wanted (nullptr: none)
    int            minFps;     // lowest limit the addon accepts (below it, up to this value; 0 stays 0 = no limit)
};

std::string WriteLimitIni(const std::wstring& exePath, int fps, const LimitIni& spec, bool& ok)
{
    ok = false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string name(spec.label, spec.label + wcslen(spec.label));
    const fs::path    dir = fs::path(exePath).parent_path();
    const fs::path    ini = dir / spec.iniName;
    if(dir.empty() || !fs::is_regular_file(ini, ec))
        return name + " settings file not found next to the program (ignored)"; // only an existing file is rewritten
    fps = std::clamp(fps, 0, 1000);
    if(fps > 0 && fps < spec.minFps)
        fps = spec.minFps;
    std::string text;
    {
        std::ifstream in(ini, std::ios::binary);
        if(in)
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const char* nl = text.find("\r\n") != std::string::npos ? "\r\n" : (text.empty() ? "\r\n" : "\n");
    // lines of the file, each without its end of line
    std::vector<std::string> lines;
    for(size_t i = 0; i < text.size();)
    {
        size_t e = text.find('\n', i);
        if(e == std::string::npos)
            e = text.size();
        size_t t = e;
        if(t > i && text[t - 1] == '\r')
            t--;
        lines.push_back(text.substr(i, t - i));
        i = e + 1;
    }
    auto lower = [](std::string v) {
        for(auto& c : v)
            c = (char)tolower((unsigned char)c);
        return v;
    };
    const std::string value    = std::to_string(fps) + (spec.decimals ? ".000000" : "");
    const std::string keyLine  = std::string(spec.key) + "=" + value;
    const std::string keyLower = lower(std::string(spec.key)) + "=";
    const std::string secLower = "[" + lower(std::string(spec.section)) + "]";
    const std::string enLower  = spec.enableKey ? lower(std::string(spec.enableKey)) + "=" : std::string();
    size_t            secStart = std::string::npos, secEnd = lines.size();
    for(size_t i = 0; i < lines.size(); i++)
    {
        const std::string l = lower(lines[i]);
        if(secStart == std::string::npos)
        {
            if(l == secLower)
                secStart = i;
        }
        else if(!l.empty() && l[0] == '[')
        {
            secEnd = i;
            break;
        }
    }
    bool wrote = false;
    if(secStart == std::string::npos)
    {
        lines.insert(lines.begin(), { "[" + std::string(spec.section) + "]", keyLine, "" });
        wrote = true;
    }
    else
    {
        bool found = false;
        for(size_t i = secStart + 1; i < secEnd; i++)
        {
            const std::string l = lower(lines[i]);
            if(l.rfind(keyLower, 0) == 0)
            {
                found = true;
                if(lines[i] != keyLine)
                {
                    lines[i] = keyLine;
                    wrote    = true;
                }
            }
            else if(fps > 0 && !enLower.empty() && l.rfind(enLower, 0) == 0 && (l.substr(enLower.size()) == "0" || l.substr(enLower.size()) == "false"))
            {
                lines[i] = std::string(spec.enableKey) + "=1"; // a limit is wanted: the limiter must not be switched off
                wrote    = true;
            }
        }
        if(!found)
        {
            lines.insert(lines.begin() + secStart + 1, keyLine);
            wrote = true;
        }
    }
    if(!wrote)
    {
        ok = true;
        return name + " already at " + std::to_string(fps) + " fps";
    }
    std::string out;
    for(const auto& l : lines)
        out += l + nl;
    const fs::path tmp = dir / (std::wstring(spec.iniName) + L".nbeam");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if(!o)
            return name + ": the folder of the program refuses writing";
        o.write(out.data(), (std::streamsize)out.size());
        if(!o)
            return name + ": writing failed";
    }
    if(!MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        fs::remove(tmp, ec);
        return name + ": the settings file is busy";
    }
    ok = true;
    return name + ": limit " + std::to_string(fps) + " fps written (applies when the program starts)";
}
} // namespace

std::string DcWriteLimit(const std::wstring& exePath, int fps, bool& ok)
{
    static const LimitIni spec { L"Display Commander", L"DisplayCommander.ini", L"zzz_display_commander.addon64", L"zzz_display_commander.addon32", "DisplayCommander", "FpsLimiter.TargetFps", true, "FpsLimiter.Enabled", 0 };
    return WriteLimitIni(exePath, fps, spec, ok);
}

std::string RlWriteLimit(const std::wstring& exePath, int fps, bool& ok)
{
    static const LimitIni spec { L"ReLimiter", L"relimiter.ini", L"relimiter.addon64", L"relimiter.addon32", "FrameLimiter", "target_fps", false, nullptr, 30 };
    return WriteLimitIni(exePath, fps, spec, ok);
}

FrameLimiter& GetFrameLimiter()
{
    static FrameLimiter limiter;
    return limiter;
}

void FrameLimiter::Set(const std::string& status)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_status = status;
}

std::string FrameLimiter::Status() const
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_status;
}

void FrameLimiter::Update(void* hwnd, double targetFps)
{
    if(!hwnd || targetFps <= 0.0)
    {
        if(!m_exe.empty())
            Restore();
        Set(hwnd ? "off" : "select a window as Capture Input (a whole display has no process to limit)");
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId((HWND)hwnd, &pid);
    const std::string exe = pid ? ExeOfPid(pid) : std::string();
    if(exe.empty())
    {
        Set("captured window's program not found");
        return;
    }
    if(exe == m_exe && std::fabs(targetFps - m_fps) < 0.00008)
        return; // already applied
    if(!m_exe.empty() && exe != m_exe)
        Restore();
    Apply(exe, targetFps);
}

void FrameLimiter::Apply(const std::string& exe, double fps)
{
    Rtss        r;
    std::string error;
    if(!LoadRtss(r, error))
    {
        Set(error);
        return;
    }
    r.load(exe.c_str());

    if(!m_haveOrig || exe != m_exe)
    {
        // find the property names this RTSS version uses, remember what was there
        static const char* limitNames[] = { "FramerateLimit", "Limit" };
        static const char* denomNames[] = { "FramerateLimitDenominator", "LimitDenominator" };
        m_limitProp.clear();
        m_denomProp.clear();
        for(auto n : limitNames)
            if(GetU32(r, n, m_origLimit))
            {
                m_limitProp = n;
                break;
            }
        for(auto n : denomNames)
            if(GetU32(r, n, m_origDenominator))
            {
                m_denomProp = n;
                break;
            }
        if(m_limitProp.empty())
        {
            Set("RTSS frame limit property not found (unsupported RTSS version)");
            FreeLibrary(r.dll);
            return;
        }
        if(m_denomProp.empty())
            m_origDenominator = 1;
        m_haveOrig = true;
    }

    bool ok = false;
    if(!m_denomProp.empty())
    {
        if(std::fabs(fps - std::round(fps)) < 0.0001)
            ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps)) && SetU32(r, m_denomProp.c_str(), 1); // whole fps: the plain RTSS format
        if(!ok)
        {
            // fractional limit: fps = limit / denominator, 4 decimals (denominator 10000), then 3 decimals
            ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps * 10000.0)) && SetU32(r, m_denomProp.c_str(), 10000);
            unsigned rl = 0, rd = 0;
            if(!ok || !GetU32(r, m_limitProp.c_str(), rl) || !GetU32(r, m_denomProp.c_str(), rd) || rd == 0 || std::fabs((double)rl / rd - fps) > 0.0002)
                ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps * 1000.0)) && SetU32(r, m_denomProp.c_str(), 1000);
        }
    }
    if(!ok)
        ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps)); // (no denominator, or it was refused: the limit alone)
    if(ok)
    {
        r.save(exe.c_str());
        r.update();
        m_exe = exe;
        m_fps = fps;
        char buf[200];
        snprintf(buf, sizeof(buf), "RTSS: %s limited to %.4f fps", exe.c_str(), fps);
        Set(buf);
    }
    else
    {
        char buf[240];
        snprintf(buf, sizeof(buf), "RTSS refused the new frame limit (%s: '%s' was %u, '%s' %s)", exe.c_str(), m_limitProp.c_str(), m_origLimit,
                 m_denomProp.empty() ? "no denominator" : m_denomProp.c_str(), m_denomProp.empty() ? "" : "found");
        Set(buf);
    }
    FreeLibrary(r.dll);
}

bool FrameLimiter::Restore()
{
    if(m_exe.empty() || !m_haveOrig)
    {
        m_exe.clear();
        return false;
    }
    Rtss        r;
    std::string error;
    bool        ok = false;
    if(LoadRtss(r, error))
    {
        r.load(m_exe.c_str());
        ok = SetU32(r, m_limitProp.c_str(), m_origLimit);
        if(!m_denomProp.empty())
            SetU32(r, m_denomProp.c_str(), m_origDenominator);
        if(ok)
        {
            r.save(m_exe.c_str());
            r.update();
        }
        FreeLibrary(r.dll);
    }
    m_exe.clear();
    m_fps      = 0;
    m_haveOrig = false;
    return ok;
}

void FrameLimiter::Release()
{
    if(!m_exe.empty())
        Restore();
    Set("off");
}

} // namespace ShaderBeam
