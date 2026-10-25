/*
ShaderBeam: shader effect overlay -- game frame-rate sync through RTSS (see FrameLimiter.h)
MIT License
*/

#include "stdafx.h"
#include <tlhelp32.h>
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

    bool ok;
    if(!m_denomProp.empty())
    {
        // fractional limit: fps = limit / denominator
        // 4 decimals (denominator 10000); if this RTSS does not keep it, fall back to 3 decimals
        ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps * 10000.0)) && SetU32(r, m_denomProp.c_str(), 10000);
        unsigned rl = 0, rd = 0;
        if(!ok || !GetU32(r, m_limitProp.c_str(), rl) || !GetU32(r, m_denomProp.c_str(), rd) || rd == 0 || std::fabs((double)rl / rd - fps) > 0.0002)
            ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps * 1000.0)) && SetU32(r, m_denomProp.c_str(), 1000);
    }
    else
        ok = SetU32(r, m_limitProp.c_str(), (unsigned)std::llround(fps));
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
        Set("RTSS refused the new frame limit");
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
