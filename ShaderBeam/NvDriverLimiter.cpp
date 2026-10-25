/*
ShaderBeam: shader effect overlay -- game frame cap through the NVIDIA driver profile (see FrameLimiter.h)
MIT License
Uses the MIT-licensed NVAPI headers from https://github.com/NVIDIA/nvapi (folder nvapi/), loaded dynamically
from the driver's nvapi64.dll through nvapi_QueryInterface: no import library is needed.
*/

#include "stdafx.h"
#include <tlhelp32.h>
#include "nvapi/nvapi.h"
#include "nvapi/NvApiDriverSettings.h"
#include "FrameLimiter.h"

namespace ShaderBeam
{

namespace
{
using QueryInterfaceFn = void*(__cdecl*)(unsigned int);
using FnInit           = NvAPI_Status(__cdecl*)();
using FnCreateSession  = NvAPI_Status(__cdecl*)(NvDRSSessionHandle*);
using FnDestroySession = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
using FnLoad           = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
using FnSave           = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
using FnFindApp        = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*, NVDRS_APPLICATION*);
using FnCreateProfile  = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NVDRS_PROFILE*, NvDRSProfileHandle*);
using FnDeleteProfile  = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle);
using FnCreateApp      = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION*);
using FnSetSetting     = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*);
using FnGetSetting     = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING*);
using FnRestore        = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32);

struct Api
{
    HMODULE          dll { nullptr };
    FnInit           init { nullptr };
    FnCreateSession  createSession { nullptr };
    FnDestroySession destroySession { nullptr };
    FnLoad           load { nullptr };
    FnSave           save { nullptr };
    FnFindApp        findApp { nullptr };
    FnCreateProfile  createProfile { nullptr };
    FnDeleteProfile  deleteProfile { nullptr };
    FnCreateApp      createApp { nullptr };
    FnSetSetting     setSetting { nullptr };
    FnGetSetting     getSetting { nullptr };
    FnRestore        restore { nullptr };
    bool             ok { false };
};

Api& GetApi()
{
    static Api api;
    static bool tried = false;
    if(tried)
        return api;
    tried   = true;
    api.dll = LoadLibraryW(L"nvapi64.dll");
    if(!api.dll)
        return api;
    auto qi = (QueryInterfaceFn)GetProcAddress(api.dll, "nvapi_QueryInterface");
    if(!qi)
        return api;
    // function ids from nvapi_interface.h of the NVAPI SDK
    api.init           = (FnInit)qi(0x0150e828);
    api.createSession  = (FnCreateSession)qi(0x0694d52e);
    api.destroySession = (FnDestroySession)qi(0xdad9cff8);
    api.load           = (FnLoad)qi(0x375dbd6b);
    api.save           = (FnSave)qi(0xfcbc7e14);
    api.findApp        = (FnFindApp)qi(0xeee566b2);
    api.createProfile  = (FnCreateProfile)qi(0xcc176068);
    api.deleteProfile  = (FnDeleteProfile)qi(0x17093206);
    api.createApp      = (FnCreateApp)qi(0x4347a9de);
    api.setSetting     = (FnSetSetting)qi(0x577dd202);
    api.getSetting     = (FnGetSetting)qi(0x73bf8338);
    api.restore        = (FnRestore)qi(0x53f0381e);
    api.ok = api.init && api.createSession && api.destroySession && api.load && api.save && api.findApp && api.createProfile && api.deleteProfile &&
             api.createApp && api.setSetting && api.getSetting && api.restore && api.init() == NVAPI_OK;
    return api;
}

// RAII DRS session
struct Session
{
    Api&                api;
    NvDRSSessionHandle  h { nullptr };
    bool                valid { false };
    explicit Session(Api& a) : api(a)
    {
        valid = api.createSession(&h) == NVAPI_OK && api.load(h) == NVAPI_OK;
    }
    ~Session()
    {
        if(h)
            api.destroySession(h);
    }
};

void ToWide(const std::string& s, NvU16* out)
{
    size_t i = 0;
    for(; i < s.size() && i < NVAPI_UNICODE_STRING_MAX - 1; i++)
        out[i] = (NvU16)(unsigned char)s[i];
    out[i] = 0;
}

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
} // namespace

NvDriverLimiter& GetNvDriverLimiter()
{
    static NvDriverLimiter l;
    return l;
}

void NvDriverLimiter::Set(const std::string& status)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_status = status;
}

std::string NvDriverLimiter::Status() const
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_status;
}

void NvDriverLimiter::Update(void* hwnd, double targetFps)
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
    unsigned fps = (unsigned)std::llround(targetFps);
    if(fps < 1)
        fps = 1;
    if(fps > FRL_FPS_MAX)
        fps = FRL_FPS_MAX;
    if(exe == m_exe && fps == m_fps)
        return;
    if(!m_exe.empty() && exe != m_exe)
        Restore();

    Api& api = GetApi();
    if(!api.ok)
    {
        Set("NVIDIA driver interface (nvapi64.dll) not available: NVIDIA GPU and driver required");
        return;
    }
    Session s(api);
    if(!s.valid)
    {
        Set("cannot open the NVIDIA driver settings");
        return;
    }

    NvDRSProfileHandle profile = nullptr;
    if(m_exe.empty())
    {
        // first time for this program: find its driver profile, or create one just for it
        NVDRS_APPLICATION app {};
        app.version = NVDRS_APPLICATION_VER;
        std::vector<NvU16> wname(NVAPI_UNICODE_STRING_MAX, 0);
        ToWide(exe, wname.data());
        const auto st = api.findApp(s.h, wname.data(), &profile, &app);
        m_createdProfile = false;
        if(st != NVAPI_OK)
        {
            NVDRS_PROFILE p {};
            p.version = NVDRS_PROFILE_VER;
            ToWide("ShaderBeam - " + exe, (NvU16*)p.profileName);
            if(api.createProfile(s.h, &p, &profile) != NVAPI_OK)
            {
                Set("cannot create a driver profile (try running ShaderBeam as administrator)");
                return;
            }
            NVDRS_APPLICATION a {};
            a.version = NVDRS_APPLICATION_VER;
            ToWide(exe, (NvU16*)a.appName);
            if(api.createApp(s.h, profile, &a) != NVAPI_OK)
            {
                api.deleteProfile(s.h, profile);
                Set("cannot register the program in the driver profile");
                return;
            }
            m_createdProfile = true;
        }
        // remember what was there
        NVDRS_SETTING cur {};
        cur.version = NVDRS_SETTING_VER;
        m_hadValue  = api.getSetting(s.h, profile, FRL_FPS_ID, &cur) == NVAPI_OK && cur.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION;
        m_origValue = m_hadValue ? cur.u32CurrentValue : 0;
    }
    else
    {
        // same program, only the number changed: look the profile up again (handles are per session)
        NVDRS_APPLICATION app {};
        app.version = NVDRS_APPLICATION_VER;
        std::vector<NvU16> wname(NVAPI_UNICODE_STRING_MAX, 0);
        ToWide(exe, wname.data());
        if(api.findApp(s.h, wname.data(), &profile, &app) != NVAPI_OK)
        {
            Set("driver profile lost");
            m_exe.clear();
            return;
        }
    }

    NVDRS_SETTING setting {};
    setting.version         = NVDRS_SETTING_VER;
    setting.settingId       = FRL_FPS_ID;
    setting.settingType     = NVDRS_DWORD_TYPE;
    setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    setting.u32CurrentValue = fps;
    if(api.setSetting(s.h, profile, &setting) != NVAPI_OK || api.save(s.h) != NVAPI_OK)
    {
        Set("the NVIDIA driver refused the frame limit (try running ShaderBeam as administrator)");
        return;
    }
    m_exe = exe;
    m_fps = fps;
    char buf[200];
    snprintf(buf, sizeof(buf), "NVIDIA driver: %s capped at %u fps", exe.c_str(), fps);
    Set(buf);
}

bool NvDriverLimiter::Restore()
{
    if(m_exe.empty())
        return false;
    bool ok  = false;
    Api& api = GetApi();
    if(api.ok)
    {
        Session s(api);
        if(s.valid)
        {
            NVDRS_APPLICATION  app {};
            app.version = NVDRS_APPLICATION_VER;
            NvDRSProfileHandle profile = nullptr;
            std::vector<NvU16> wname(NVAPI_UNICODE_STRING_MAX, 0);
            ToWide(m_exe, wname.data());
            if(api.findApp(s.h, wname.data(), &profile, &app) == NVAPI_OK)
            {
                if(m_createdProfile)
                    ok = api.deleteProfile(s.h, profile) == NVAPI_OK;
                else if(m_hadValue)
                {
                    NVDRS_SETTING setting {};
                    setting.version         = NVDRS_SETTING_VER;
                    setting.settingId       = FRL_FPS_ID;
                    setting.settingType     = NVDRS_DWORD_TYPE;
                    setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
                    setting.u32CurrentValue = m_origValue;
                    ok                      = api.setSetting(s.h, profile, &setting) == NVAPI_OK;
                }
                else
                    ok = api.restore(s.h, profile, FRL_FPS_ID) == NVAPI_OK;
                if(ok)
                    api.save(s.h);
            }
        }
    }
    m_exe.clear();
    m_fps            = 0;
    m_createdProfile = false;
    return ok;
}

void NvDriverLimiter::Release()
{
    if(!m_exe.empty())
        Restore();
    Set("off");
}

} // namespace ShaderBeam
