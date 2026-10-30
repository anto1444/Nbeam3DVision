#include "stdafx.h"

#include "Common.h"
#include "ShaderManager.h"
#include "Lang.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <map>
#include <sstream>
#include "ini.h"
#include "Lang.h"
#include "EmitterIni.h"
#include <tlhelp32.h>
#include <dwmapi.h>

#define LOAD_BOOL(ini, var) var = ini.has(#var) ? ini.get(#var) == "1" : var;
#define LOAD_INT(ini, var) var = ini.has(#var) ? std::stoi(ini.get(#var)) : var;

#define SAVE_VERSION(ini) ini["version"] = "2";
#define SAVE_BOOL(ini, var) ini[#var] = var ? "1" : "0";
#define SAVE_INT(ini, var) ini[#var] = std::to_string(var);

#define ACTIVE_PROFILE_FILE "Nbeam3DVision.profile.txt"

namespace ShaderBeam
{
static std::mutex& MonitorsMutex()
{
    static std::mutex m;
    return m;
}

static std::string Narrow(const std::wstring& w)
{
    if(w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Widen(const std::string& s)
{
    if(s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Options::ProcessNameOfWindow(HWND hWnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if(!pid)
        return std::string();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!process)
        return std::string();
    wchar_t path[MAX_PATH * 2] = {};
    DWORD   size               = MAX_PATH * 2;
    std::string result;
    if(QueryFullProcessImageNameW(process, 0, path, &size))
    {
        std::wstring p(path, size);
        const auto   slash = p.find_last_of(L"\\/");
        if(slash != std::wstring::npos)
            p = p.substr(slash + 1);
        for(auto& c : p)
            c = (wchar_t)towlower(c);
        result = Narrow(p);
    }
    CloseHandle(process);
    return result;
}

namespace
{
struct FindCtx
{
    const Options* options;
    std::string    process;
    std::wstring   title;
    HWND           best { 0 };
    int            bestScore { -1 };
};

BOOL CALLBACK FindProc(HWND hWnd, LPARAM lParam)
{
    auto* c = (FindCtx*)lParam;
    if(hWnd == c->options->outputWindow || !IsWindowVisible(hWnd) || GetWindow(hWnd, GW_OWNER) != NULL)
        return TRUE;
    wchar_t title[256] = {};
    if(!GetWindowTextW(hWnd, title, 255))
        return TRUE;
    if(Options::ProcessNameOfWindow(hWnd) != c->process)
        return TRUE;
    RECT r {};
    GetWindowRect(hWnd, &r);
    const long area  = (r.right - r.left) * (r.bottom - r.top);
    int        score = (std::wstring(title) == c->title ? 1 << 30 : 0) + (int)(std::min)(area / 64, 1L << 20);
    if(score > c->bestScore)
    {
        c->bestScore = score;
        c->best      = hWnd;
    }
    return TRUE;
}
} // namespace

std::string Options::NormalizeExeName(const std::string& text)
{
    std::string t = text;
    while(!t.empty() && (t.back() == ' ' || t.back() == '"' || t.back() == '\t'))
        t.pop_back();
    size_t b = 0;
    while(b < t.size() && (t[b] == ' ' || t[b] == '"' || t[b] == '\t'))
        b++;
    t = t.substr(b);
    const auto slash = t.find_last_of("\\/");
    if(slash != std::string::npos)
        t = t.substr(slash + 1);
    for(auto& c : t)
        c = (char)tolower((unsigned char)c);
    return t;
}

bool Options::ProcessRect(RECT& r) const
{
    if(!outputOnProcess || !captureWindow || !IsWindow(captureWindow) || IsIconic(captureWindow))
        return false;
    if(DwmGetWindowAttribute(captureWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(RECT)) != S_OK && !GetWindowRect(captureWindow, &r))
        return false;
    return r.right - r.left >= 64 && r.bottom - r.top >= 64;
}

HWND Options::FindCaptureWindow() const
{
    if(captureProcess.empty())
        return NULL;
    FindCtx ctx;
    ctx.options = this;
    ctx.process = captureProcess;
    ctx.title   = Widen(captureTitle);
    EnumWindows(FindProc, (LPARAM)&ctx);
    return ctx.best;
}

void Options::BringToForeground(HWND hWnd)
{
    if(!hWnd || !IsWindow(hWnd))
        return;
    if(IsIconic(hWnd))
        ShowWindow(hWnd, SW_RESTORE);
    HWND  fg  = GetForegroundWindow();
    DWORD ft  = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD me  = GetCurrentThreadId();
    const bool attach = ft && ft != me;
    if(attach)
        AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(hWnd);
    BringWindowToTop(hWnd);
    if(attach)
        AttachThreadInput(me, ft, FALSE);
}

const char* Options::HotkeyActionName(int id)
{
    switch(id)
    {
    case HOTKEY_TOGGLEUI: return "Toggle UI";
    case HOTKEY_RESTART: return "Restart";
    case HOTKEY_QUIT: return "Quit";
    case HOTKEY_SWAPEYES: return "Swap eyes";
    case HOTKEY_EFFECTS: return "Show/Hide effects";
    case HOTKEY_DETECTION: return "Profile detection on/off";
    case HOTKEY_ADDEXE: return "Add the program to the profiles";
    case HOTKEY_SUBFRAMES_DEC: return "SubFrames -";
    case HOTKEY_SUBFRAMES_INC: return "SubFrames +";
    case HOTKEY_CLEAR_DEC: return "Clear frames -";
    case HOTKEY_CLEAR_INC: return "Clear frames +";
    case HOTKEY_SYNCOFFSET_DEC: return "Sync offset -";
    case HOTKEY_SYNCOFFSET_INC: return "Sync offset +";
    case HOTKEY_DURATION_DEC: return "Open duration -";
    case HOTKEY_DURATION_INC: return "Open duration +";
    }
    return "";
}

std::string Options::HotkeyText(const Hotkey& h)
{
    if(!h.vk)
        return Tr("None");
    std::string s;
    if(h.mods & MOD_CONTROL)
        s += "Ctrl+";
    if(h.mods & MOD_ALT)
        s += "Alt+";
    if(h.mods & MOD_SHIFT)
        s += "Shift+";
    if(h.mods & MOD_WIN)
        s += "Win+";
    char name[64] {};
    UINT scan = MapVirtualKeyA(h.vk, MAPVK_VK_TO_VSC);
    LONG lp   = (LONG)(scan << 16);
    switch(h.vk) // extended keys need bit 24 for a correct name
    {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE:
        lp |= 1 << 24;
    }
    if(GetKeyNameTextA(lp, name, sizeof(name)) > 0)
        s += name;
    else
        s += "VK" + std::to_string(h.vk);
    return s;
}

bool Options::RegisterHotkey(int id) const
{
    if(id < 0 || id >= HOTKEY_COUNT || !hotkeys[id].vk || !outputWindow)
        return false;
    // Not registered with RegisterHotKey on purpose: that would swallow the key (pressing "M" would no longer type "M").
    // Hotkeys are detected by keyboard polling (HotkeyPoll / AdjustPoll) and the keystroke still reaches the other program.
    UnregisterHotKey(outputWindow, id);
    hotkeyOk[id] = false;
    return false;
}

void Options::UnregisterHotkey(int id) const
{
    if(id >= 0 && id < HOTKEY_COUNT && outputWindow)
    {
        UnregisterHotKey(outputWindow, id);
        hotkeyOk[id] = false;
    }
}

std::filesystem::path Options::ExeDir()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path Options::ProfilesDir()
{
    return ExeDir() / "Profils";
}

static bool s_firstRun = false;

bool Options::FirstRun()
{
    return s_firstRun;
}

void Options::EnsureProfilesDir()
{
    std::error_code ec;
    const auto      dir = ProfilesDir();
    if(std::filesystem::create_directories(dir, ec))
        s_firstRun = true;
    // profiles of earlier versions were next to the exe: moved once (imgui.ini stays where it is)
    try
    {
        std::vector<std::filesystem::path> old;
        for(const auto& e : std::filesystem::directory_iterator(ExeDir(), ec))
        {
            if(!e.is_regular_file() || e.path().extension() != ".ini")
                continue;
            std::string lower = e.path().filename().string();
            for(auto& c : lower)
                c = (char)tolower((unsigned char)c);
            if(lower != "imgui.ini")
                old.push_back(e.path());
        }
        for(const auto& p : old)
            if(!std::filesystem::exists(dir / p.filename(), ec))
                std::filesystem::rename(p, dir / p.filename(), ec);
    }
    catch(...)
    { }
}

std::vector<std::string> Options::ListProfiles()
{
    std::vector<std::string> list;
    try
    {
        for(const auto& e : std::filesystem::directory_iterator(ProfilesDir()))
        {
            if(!e.is_regular_file() || e.path().extension() != ".ini")
                continue;
            auto name = e.path().filename().string();
            std::string lower = name;
            for(auto& c : lower)
                c = (char)tolower((unsigned char)c);
            if(lower == "imgui.ini")
                continue;
            list.push_back(name);
        }
    }
    catch(...)
    { }
    std::sort(list.begin(), list.end());
    return list;
}

#define LANGUAGE_FILE "Nbeam3DVision.language.txt"

// path of a settings file next to the exe; a file left by the previous name is renamed once
static std::filesystem::path DataFile(const char* name, const char* legacyName)
{
    const auto      dir = Options::ExeDir();
    std::error_code ec;
    if(!std::filesystem::exists(dir / name, ec) && std::filesystem::exists(dir / legacyName, ec))
        std::filesystem::rename(dir / legacyName, dir / name, ec);
    return dir / name;
}

void Options::SaveLanguage(int lang)
{
    try
    {
        std::ofstream f(DataFile(LANGUAGE_FILE, "ShaderBeam.language.txt"), std::ios::trunc);
        f << (lang == LANG_EN ? "en" : "fr") << "\n";
    }
    catch(...)
    { }
}

void Options::InitProfile()
{
    try
    {
        std::ifstream lf(DataFile(LANGUAGE_FILE, "ShaderBeam.language.txt"));
        std::string   code;
        if(lf && std::getline(lf, code))
            SetLanguage(code.rfind("en", 0) == 0 ? LANG_EN : LANG_FR);
    }
    catch(...)
    { }
    EnsureProfilesDir();
    // no profile at all (first start): Defaut.ini is created as the example profile
    try
    {
        if(ListProfiles().empty())
        {
            // example profile written at the very first start (what is not listed keeps its program default: hotkeys are global, the
            // screens are picked at run time, the AI settings only matter with the 2D to 3D source)
            static const char* kDefaultProfile = R"INI([settings]
version=2
remembersettings=1
shaderprofileno=1
captureadapterno=0
shaderadapterno=0
capturedisplayno=0
shaderdisplayno=0
capturemethod=0
splitscreen=0
hardwaresrgb=0
monitortype=1
usehdr=1
maxqueuedframes=0
tearingline=0
framebuffer=0
subframessetting=2
subframesmode=0
lowfpsthreshold=48.000000
lowfpsmode=0
autointerval=0.000000
outputonprocess=0
[emitter]
sourceformat=0
syncauto=1
syncoffsetus=260
swapeyes=1
comportno=-1
nvopendurationus=5785.000000
)INI";
            std::ofstream      df(ProfilesDir() / "Defaut.ini", std::ios::binary);
            df << kDefaultProfile;
            df.close();
            profileFile = "Defaut.ini";
            RememberProfile();
        }
    }
    catch(...)
    { }
    try
    {
        std::ifstream f(DataFile(ACTIVE_PROFILE_FILE, "ShaderBeam.profile.txt"));
        std::string   name;
        if(f && std::getline(f, name))
        {
            while(!name.empty() && (name.back() == '\r' || name.back() == ' '))
                name.pop_back();
            // plain file name only, and it must still exist
            if(!name.empty() && name.find_first_of("\\/:") == std::string::npos && std::filesystem::exists(ProfilesDir() / name))
                profileFile = name;
        }
    }
    catch(...)
    { }
    LoadApp();
}

// [hotkeys] "mods,vk" ("0,0" = no shortcut) and [gamepad] button bits, keyed by the action name
template<class Section> void Options::ReadHotkeys(Section& hk, Section* gp)
{
    for(int id = 0; id < HOTKEY_COUNT; id++)
    {
        if(!hk.has(HotkeyActionName(id)))
            continue;
        unsigned m = 0, v = 0;
        if(sscanf_s(hk.get(HotkeyActionName(id)).c_str(), "%u,%u", &m, &v) == 2 && v < 255)
        {
            hotkeys[id].mods = v ? (m & 0xF) : 0;
            hotkeys[id].vk   = v;
        }
    }
    if(gp)
        for(int id = 0; id < HOTKEY_COUNT; id++)
        {
            if(!gp->has(HotkeyActionName(id)))
                continue;
            try
            {
                hotkeys[id].pad = (unsigned)std::stoul(gp->get(HotkeyActionName(id))) & 0x3F3FFu;
            }
            catch(...)
            { }
        }
    // the HUD and the effects must always be reachable
    Options defaults;
    for(int id : { HOTKEY_TOGGLEUI, HOTKEY_EFFECTS })
        if(!hotkeys[id].vk)
            hotkeys[id].mods = defaults.hotkeys[id].mods, hotkeys[id].vk = defaults.hotkeys[id].vk;
}

#define APP_FILE "Nbeam.app.cfg"
#define AUTOSTART_VALUE L"Nbeam3DVision"

void Options::LoadApp()
{
    try
    {
        mINI::INIFile      file(ExeDir() / APP_FILE);
        mINI::INIStructure ini;
        if(!file.read(ini))
            return;
        if(ini.has("app"))
        {
            auto& a = ini["app"];
            LOAD_BOOL(a, startWithWindows)
            LOAD_BOOL(a, startMinimized)
            if(a.has("defaultProfile"))
                defaultProfile = a.get("defaultProfile");
            if(a.has("hzSwitchMs"))
                try { hzSwitchMs = std::clamp(std::stod(a.get("hzSwitchMs")), 100.0, 10000.0); } catch(...) { }
            if(a.has("hzSwitchN"))
                hzSwitchCount = std::clamp(atoi(a.get("hzSwitchN").c_str()), 0, 1000);
        }
        if(ini.has("hud"))
        {
            auto h = ini.get("hud");
            auto f = [&](const char* k, float& v, float lo, float hi) {
                if(h.has(k))
                    try { v = std::clamp(std::stof(h.get(k)), lo, hi); } catch(...) { }
            };
            f("r", hud.r, 0.0f, 1.0f);
            f("g", hud.g, 0.0f, 1.0f);
            f("b", hud.b, 0.0f, 1.0f);
            f("contrast", hud.contrast, 0.5f, 1.8f);
            f("transparency", hud.transparency, 0.0f, 0.8f);
            f("hdrBlackNits", hud.hdrBlackNits, 0.0f, 20.0f);
            f("hdrGrey", hud.hdrGrey, 0.5f, 2.0f);
            f("hdrNits", hud.hdrNits, -1000.0f, 1000.0f);
            f("sdrBrightness", hud.sdrBrightness, 20.0f, 100.0f);
            float bs = (float)hud.bannerSeconds, bc = (float)hud.bannerCorner;
            f("bannerSeconds", bs, 0.0f, 11.0f);
            f("bannerCorner", bc, 0.0f, 3.0f);
            hud.bannerSeconds = (int)bs;
            hud.bannerCorner  = (int)bc;
            f("scale", hud.scale, 0.6f, 2.0f);
        }
        std::lock_guard<std::recursive_mutex> linksLock(linksMutex);
        profileLinks.clear();
        for(int i = 0; i < 64 && ini.has("link" + std::to_string(i)); i++)
        {
            auto&       l = ini["link" + std::to_string(i)];
            ProfileLink link;
            link.exe     = l.has("exe") ? l.get("exe") : "";
            link.profile = l.has("profile") ? l.get("profile") : "";
            if(l.has("delay"))
                link.delay = std::clamp(atoi(l.get("delay").c_str()), 0, 600);
            if(l.has("enabled"))
                link.enabled = l.get("enabled") != "0";
            link.screenId = l.has("screen") ? l.get("screen") : "";
            if(l.has("hz"))
                link.hz = std::clamp(atoi(l.get("hz").c_str()), 0, 1000);
            profileLinks.push_back(link);
        }
        if(ini.has("hotkeys"))
        {
            // global hotkeys (the same whatever the profile, also while a detected program runs)
            auto hk = ini.get("hotkeys");
            auto gp = ini.get("gamepad");
            ReadHotkeys(hk, ini.has("gamepad") ? &gp : nullptr);
            appHotkeys = true;
        }
        knownMonitors.clear();
        for(int i = 0; i < 32 && ini.has("monitor" + std::to_string(i)); i++)
        {
            auto&        m = ini["monitor" + std::to_string(i)];
            KnownMonitor km;
            km.id   = m.has("id") ? m.get("id") : "";
            km.name = m.has("name") ? m.get("name") : "";
            if(m.has("hz"))
                km.hz = std::clamp(atoi(m.get("hz").c_str()), 0, 1000);
            if(m.has("rates"))
            {
                std::stringstream ss(m.get("rates"));
                std::string       tok;
                while(std::getline(ss, tok, ','))
                {
                    const int r = atoi(tok.c_str());
                    if(r > 1 && r <= 1000)
                        km.rates.push_back(r);
                }
            }
            if(!km.id.empty())
                knownMonitors.push_back(km);
        }
    }
    catch(...)
    { }
}

void Options::SaveApp() const
{
    std::lock_guard<std::recursive_mutex> linksLock(linksMutex); // the detections are written below
    try
    {
        mINI::INIFile      file(ExeDir() / APP_FILE);
        mINI::INIStructure ini;
        auto&              a = ini["app"];
        SAVE_BOOL(a, startWithWindows)
        SAVE_BOOL(a, startMinimized)
        a["defaultProfile"] = defaultProfile;
        a["hzSwitchMs"] = std::to_string((int)hzSwitchMs);
        a["hzSwitchN"]  = std::to_string(hzSwitchCount);
        auto& h = ini["hud"];
        h["r"] = std::to_string(hud.r);
        h["g"] = std::to_string(hud.g);
        h["b"] = std::to_string(hud.b);
        h["contrast"] = std::to_string(hud.contrast);
        h["transparency"] = std::to_string(hud.transparency);
        h["hdrBlackNits"] = std::to_string(hud.hdrBlackNits);
        h["hdrGrey"]      = std::to_string(hud.hdrGrey);
        h["hdrNits"]      = std::to_string(hud.hdrNits);
        h["sdrBrightness"] = std::to_string(hud.sdrBrightness);
        h["bannerSeconds"] = std::to_string(hud.bannerSeconds);
        h["bannerCorner"]  = std::to_string(hud.bannerCorner);
        h["scale"]         = std::to_string(hud.scale);
        {
            // one section at a time: creating a section moves the others in memory, so a reference kept to "hotkeys" while "gamepad"
            // is created points to freed memory (that crashed Nbeam on every save of the application settings)
            auto& hk = ini["hotkeys"];
            for(int id = 0; id < HOTKEY_COUNT; id++)
                hk[HotkeyActionName(id)] = std::to_string(hotkeys[id].mods) + "," + std::to_string(hotkeys[id].vk);
        }
        {
            auto& gp = ini["gamepad"];
            for(int id = 0; id < HOTKEY_COUNT; id++)
                gp[HotkeyActionName(id)] = std::to_string(hotkeys[id].pad);
        }
        for(size_t i = 0; i < profileLinks.size() && i < 64; i++)
        {
            auto& l       = ini["link" + std::to_string(i)];
            l["exe"]      = profileLinks[i].exe;
            l["profile"]  = profileLinks[i].profile;
            l["delay"]    = std::to_string(profileLinks[i].delay);
            l["enabled"]   = profileLinks[i].enabled ? "1" : "0";
            l["screen"]    = profileLinks[i].screenId;
            l["hz"]        = std::to_string(profileLinks[i].hz);
        }
        {
            std::lock_guard<std::mutex> lock(MonitorsMutex());
            for(size_t i = 0; i < knownMonitors.size() && i < 32; i++)
            {
                auto&       m = ini["monitor" + std::to_string(i)];
                std::string rates;
                for(int r : knownMonitors[i].rates)
                    rates += (rates.empty() ? "" : ",") + std::to_string(r);
                m["id"]    = knownMonitors[i].id;
                m["name"]  = knownMonitors[i].name;
                m["hz"]    = std::to_string(knownMonitors[i].hz);
                m["rates"] = rates;
            }
        }
        file.write(ini);
    }
    catch(...)
    { }
}

static bool MonitorDevice(HMONITOR monitor, wchar_t (&device)[CCHDEVICENAME])
{
    MONITORINFOEXW info = {};
    info.cbSize         = sizeof(info);
    if(!monitor || !GetMonitorInfoW(monitor, &info))
        return false;
    wcsncpy_s(device, info.szDevice, CCHDEVICENAME - 1);
    return true;
}

std::vector<int> Options::DisplayRefreshRates(HMONITOR monitor)
{
    std::vector<int> rates;
    wchar_t          dev[CCHDEVICENAME] = {};
    DEVMODEW         cur                = {};
    cur.dmSize                          = sizeof(cur);
    if(!MonitorDevice(monitor, dev) || !EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &cur))
        return rates;
    DEVMODEW m = {};
    m.dmSize   = sizeof(m);
    for(DWORD i = 0; EnumDisplaySettingsW(dev, i, &m); i++)
        if(m.dmPelsWidth == cur.dmPelsWidth && m.dmPelsHeight == cur.dmPelsHeight && m.dmBitsPerPel == cur.dmBitsPerPel && m.dmDisplayFrequency > 1 &&
           std::find(rates.begin(), rates.end(), (int)m.dmDisplayFrequency) == rates.end())
            rates.push_back((int)m.dmDisplayFrequency);
    std::sort(rates.begin(), rates.end());
    return rates;
}

int Options::DisplayRefreshRate(HMONITOR monitor)
{
    wchar_t  dev[CCHDEVICENAME] = {};
    DEVMODEW cur                = {};
    cur.dmSize                  = sizeof(cur);
    if(!MonitorDevice(monitor, dev) || !EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &cur))
        return 0;
    return (int)cur.dmDisplayFrequency;
}

// Changes only the refresh rate (same resolution and depth). CDS_FULLSCREEN = temporary: Windows restores the registry mode when Nbeam exits.
bool Options::SetDisplayRefreshRate(HMONITOR monitor, int hz)
{
    wchar_t  dev[CCHDEVICENAME] = {};
    DEVMODEW m                  = {};
    m.dmSize                    = sizeof(m);
    if(hz <= 1 || !MonitorDevice(monitor, dev) || !EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &m))
        return false;
    if((int)m.dmDisplayFrequency == hz)
        return true;
    m.dmDisplayFrequency = (DWORD)hz;
    m.dmFields           = DM_DISPLAYFREQUENCY | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    return ChangeDisplaySettingsExW(dev, &m, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL;
}

struct MonProbe
{
    HMONITOR     monitor;
    std::wstring gdi;
    bool         primary;
};

static BOOL CALLBACK EnumMonProc(HMONITOR m, HDC, LPRECT, LPARAM lp)
{
    MONITORINFOEXW mi = {};
    mi.cbSize         = sizeof(mi);
    if(GetMonitorInfoW(m, &mi))
        ((std::vector<MonProbe>*)lp)->push_back({ m, mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
    return TRUE;
}

static std::string UpperAscii(std::string s)
{
    for(auto& ch : s)
        ch = (char)toupper((unsigned char)ch);
    return s;
}

// Connected screens, identified by the device path of the monitor (display configuration), with their friendly (EDID) name. The
// screens are remembered (with their refresh rates) so a profile can be tied to a screen that is not connected right now.
void Options::RefreshMonitors(bool keepHz)
{
    std::vector<MonProbe> probes;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&probes);

    std::map<std::wstring, std::pair<std::wstring, std::wstring>> info; // GDI device name -> (monitor device path, friendly name)
    UINT32                                                        np = 0, nm = 0;
    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np > 0)
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS)
        {
            for(UINT32 i = 0; i < np; i++)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                src.header.type                      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size                      = sizeof(src);
                src.header.adapterId                 = paths[i].sourceInfo.adapterId;
                src.header.id                        = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
                tgt.header.type                      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size                      = sizeof(tgt);
                tgt.header.adapterId                 = paths[i].targetInfo.adapterId;
                tgt.header.id                        = paths[i].targetInfo.id;
                if(DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS)
                    info[src.viewGdiDeviceName] = { tgt.monitorDevicePath, tgt.monitorFriendlyDeviceName };
            }
        }
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(MonitorsMutex());
        for(auto& k : knownMonitors)
        {
            k.connected = false;
            k.primary   = false;
            k.gdi.clear();
        }
        for(const auto& p : probes)
        {
            std::string id, name;
            auto        it = info.find(p.gdi);
            if(it != info.end())
            {
                id   = UpperAscii(Narrow(it->second.first));
                name = Narrow(it->second.second);
            }
            if(id.empty())
                id = UpperAscii(Narrow(p.gdi));
            if(name.empty())
                name = "Monitor";
            auto k = std::find_if(knownMonitors.begin(), knownMonitors.end(), [&](const KnownMonitor& m) { return m.id == id; });
            if(k == knownMonitors.end())
            {
                if(knownMonitors.size() >= 32)
                {
                    auto old = std::find_if(knownMonitors.begin(), knownMonitors.end(), [](const KnownMonitor& m) { return !m.connected; });
                    if(old == knownMonitors.end())
                        continue;
                    knownMonitors.erase(old);
                }
                KnownMonitor nk;
                nk.id = id;
                knownMonitors.push_back(nk);
                k       = knownMonitors.end() - 1;
                changed = true;
            }
            k->connected = true;
            k->primary   = p.primary;
            k->gdi       = Narrow(p.gdi);
            if(k->name != name)
            {
                k->name = name;
                changed = true;
            }
            const int cur = DisplayRefreshRate(p.monitor);
            auto      r   = DisplayRefreshRates(p.monitor);
            if(!keepHz && cur > 0 && k->hz != cur)
            {
                k->hz   = cur;
                changed = true;
            }
            if(!r.empty() && r != k->rates)
            {
                k->rates = r;
                changed  = true;
            }
        }
    }
    if(changed)
        SaveApp();
}

std::vector<Options::KnownMonitor> Options::MonitorsSnapshot() const
{
    std::lock_guard<std::mutex> lock(MonitorsMutex());
    return knownMonitors;
}

static std::string DisplayGdi(const std::vector<DisplayInfo>& displays, size_t i)
{
    MONITORINFOEXW mi = {};
    mi.cbSize         = sizeof(mi);
    if(i >= displays.size() || !displays[i].monitor || !GetMonitorInfoW(displays[i].monitor, &mi))
        return "";
    return Narrow(mi.szDevice);
}

int Options::DisplayNoForScreen(const std::string& id, const std::vector<DisplayInfo>& displays) const
{
    std::string gdi;
    {
        std::lock_guard<std::mutex> lock(MonitorsMutex());
        for(const auto& k : knownMonitors)
            if(k.id == id && k.connected)
                gdi = k.gdi;
    }
    if(gdi.empty())
        return -1;
    for(size_t i = 0; i < displays.size(); i++)
        if(DisplayGdi(displays, i) == gdi)
            return (int)i;
    return -1;
}

std::string Options::ScreenIdOfDisplay(unsigned displayNo, const std::vector<DisplayInfo>& displays) const
{
    const std::string gdi = DisplayGdi(displays, displayNo);
    if(gdi.empty())
        return "";
    std::lock_guard<std::mutex> lock(MonitorsMutex());
    for(const auto& k : knownMonitors)
        if(k.connected && k.gdi == gdi)
            return k.id;
    return "";
}

HMONITOR Options::OutputMonitor(const std::vector<DisplayInfo>& displays) const
{
    const size_t no = outputOnProcess ? (size_t)captureDisplayNo : (size_t)shaderDisplayNo;
    return no < displays.size() ? displays[no].monitor : (HMONITOR)0;
}

// Windows startup entry for the current user.
void Options::ApplyAutoStart() const
{
    HKEY key = nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if(startWithWindows)
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        const std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, AUTOSTART_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
        RegDeleteValueW(key, AUTOSTART_VALUE);
    RegCloseKey(key);
}

static std::wstring NormalizePath(std::wstring p)
{
    for(auto& c : p)
    {
        c = (wchar_t)towlower(c);
        if(c == L'/')
            c = L'\\';
    }
    return p;
}

// Running processes (lower-case names), read at most every 150 ms: the detections ask several times per tick and every ask used to
// take a new snapshot of the whole process list, which made the HUD stutter.
struct ProcEntry
{
    DWORD        pid;
    std::wstring name;
};

static std::vector<ProcEntry> ProcessList()
{
    static std::mutex             mtx;
    static std::vector<ProcEntry> cache;
    static ULONGLONG              stamp = 0;
    std::lock_guard<std::mutex>   lock(mtx);
    const ULONGLONG               now = GetTickCount64();
    if(stamp != 0 && now - stamp < 150)
        return cache;
    cache.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE)
        return cache;
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof(pe);
    for(BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        cache.push_back({ pe.th32ProcessID, NormalizePath(pe.szExeFile) });
    CloseHandle(snap);
    stamp = now;
    return cache;
}

// Process ids running the given program: a full path matches the image path, a bare file name matches the process name.
static void MatchingPids(const std::string& pathOrName, std::vector<DWORD>& pids)
{
    if(pathOrName.empty())
        return;
    const std::wstring spec     = NormalizePath(Widen(pathOrName));
    const bool         fullPath = spec.find(L'\\') != std::wstring::npos;
    const auto         slash    = spec.find_last_of(L'\\');
    const std::wstring name     = slash == std::wstring::npos ? spec : spec.substr(slash + 1);
    for(const auto& e : ProcessList())
    {
        if(e.name != name)
            continue;
        if(!fullPath)
        {
            pids.push_back(e.pid);
            continue;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid);
        if(!process)
            continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD   size               = MAX_PATH * 2;
        if(QueryFullProcessImageNameW(process, 0, path, &size) && NormalizePath(std::wstring(path, size)) == spec)
            pids.push_back(e.pid);
        CloseHandle(process);
    }
}

struct TaskbarProbe
{
    const std::vector<DWORD>* pids;
    bool                      found;
    HWND                      hwnd { nullptr };
};

static BOOL CALLBACK TaskbarWindowProc(HWND w, LPARAM lp)
{
    auto* p = (TaskbarProbe*)lp;
    if(!IsWindowVisible(w))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if(std::find(p->pids->begin(), p->pids->end(), pid) == p->pids->end())
        return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if(!(ex & WS_EX_APPWINDOW) && ((ex & WS_EX_TOOLWINDOW) || GetWindow(w, GW_OWNER)))
        return TRUE; // not a taskbar window
    DWORD cloaked = 0;
    if(SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;
    p->found = true;
    p->hwnd  = w;
    return FALSE;
}

// A program counts as running while one of its windows is shown on the taskbar; once it is no longer there (closed, or only a
// background process / tray icon left) it is considered closed.
bool Options::IsExeOnTaskbar(const std::string& pathOrName)
{
    std::vector<DWORD> pids;
    MatchingPids(pathOrName, pids);
    if(pids.empty())
        return false;
    TaskbarProbe probe { &pids, false, nullptr };
    EnumWindows(TaskbarWindowProc, (LPARAM)&probe);
    return probe.found;
}

// The screen on which the program's window is (known screens, by identification), so the detection of that screen is chosen.
std::string Options::ScreenIdOfExeWindow(const std::string& pathOrName) const
{
    std::vector<DWORD> pids;
    MatchingPids(pathOrName, pids);
    if(pids.empty())
        return "";
    TaskbarProbe probe { &pids, false, nullptr };
    EnumWindows(TaskbarWindowProc, (LPARAM)&probe);
    if(!probe.found || !probe.hwnd)
        return "";
    MONITORINFOEXW mi = {};
    mi.cbSize         = sizeof(mi);
    HMONITOR mon      = MonitorFromWindow(probe.hwnd, MONITOR_DEFAULTTONULL);
    if(!mon || !GetMonitorInfoW(mon, &mi))
        return "";
    const std::string gdi = Narrow(mi.szDevice);
    std::lock_guard<std::mutex> lock(MonitorsMutex());
    for(const auto& k : knownMonitors)
        if(k.connected && k.gdi == gdi)
            return k.id;
    return "";
}

// Refresh rate and 3D of a profile file, read without loading it (the profile is not applied yet)
void Options::PeekProfile(const std::string& file, bool& stereo)
{
    stereo = false;
    try
    {
        mINI::INIFile      f(ProfilesDir() / file);
        mINI::INIStructure ini;
        if(!f.read(ini))
            return;
        if(ini.has("emitter") && ini.get("emitter").has("sourceFormat"))
        {
            const int src = atoi(ini.get("emitter").get("sourceFormat").c_str());
            stereo        = src >= 1 && src <= 4;
        }
    }
    catch(...)
    { }
}

void Options::RememberProfile() const
{
    try
    {
        std::ofstream f(DataFile(ACTIVE_PROFILE_FILE, "ShaderBeam.profile.txt"), std::ios::trunc);
        f << profileFile << "\n";
    }
    catch(...)
    { }
}

// 3D: the eye switch rate of the emitter and SubFrames are tied: SubFrames = display Hz / emitter rate (480 Hz, 120 Hz emitter -> 4;
// 100 Hz, 100 Hz emitter -> 1). Auto picks the rate among display Hz / n between 80 and 165 Hz (closest to 120); out of that range
// SubFrames is left alone and the emitter is switched off.
int Options::StereoSubFrames() const
{
    emitter.rateBlocked = false;
    if(emitter.sourceFormat == StereoSource::Off || emitter.hold || vsyncRate <= 1.0f)
        return 0;
    if(emitter.syncAuto)
    {
        int    best  = 0;
        double bestD = 1e9;
        for(int n = MIN_SUBFRAMES; n <= MAX_SUBFRAMES; n++)
        {
            const double hz = vsyncRate / n;
            if(hz < 80.0 - 0.5 || hz > 165.0 + 0.5)
                continue;
            const double d = fabs(hz - 120.0);
            if(d < bestD)
            {
                bestD = d;
                best  = n;
            }
        }
        emitter.rateBlocked = best == 0;
        return best;
    }
    if(emitter.syncRateHz < 1.0f)
        return 0;
    return std::clamp((int)roundf(vsyncRate / emitter.syncRateHz), (int)MIN_SUBFRAMES, (int)MAX_SUBFRAMES);
}

bool Options::StereoLinked() const
{
    return emitter.sourceFormat != StereoSource::Off && !emitter.hold && StereoSubFrames() > 0;
}

int Options::ManualSubFrames() const
{
    const int n = StereoSubFrames();
    return n > 0 ? n : ResolveSubFrames();
}

int Options::ResolveSubFrames() const
{
    if(subFramesSetting >= MIN_SUBFRAMES && subFramesSetting <= MAX_SUBFRAMES)
        return subFramesSetting;
    return std::clamp((int)roundf(vsyncRate / 60), 1, MAX_SUBFRAMES); // never set (or old "0 = auto" profile): display Hz / 60
}

void Options::Save(const ShaderManager& shaderManager) const
{
    try
    {
        mINI::INIFile      file(ProfilesDir() / profileFile);
        mINI::INIStructure ini;

        if(!rememberSettings)
        {
            ini.remove("settings");
            ini.remove("shader");
        }

        auto& s = ini["settings"];
        SAVE_VERSION(s)

        SAVE_BOOL(s, rememberSettings);
        if(rememberSettings)
        {
            SAVE_INT(s, shaderProfileNo)
            SAVE_INT(s, captureAdapterNo)
            SAVE_INT(s, shaderAdapterNo)
            SAVE_INT(s, captureDisplayNo)
            SAVE_INT(s, shaderDisplayNo)
            s["shaderScreen"]  = !shaderScreenId.empty() ? shaderScreenId : (shaderDisplayNo >= 0 && (size_t)shaderDisplayNo < displayIds.size() ? displayIds[shaderDisplayNo] : std::string());
            s["captureScreen"] = captureDisplayNo >= 0 && (size_t)captureDisplayNo < displayIds.size() && !displayIds[captureDisplayNo].empty() ? displayIds[captureDisplayNo] : captureScreenId;
            SAVE_BOOL(s, outputOnProcess)
            SAVE_INT(s, captureMethod)
            SAVE_INT(s, splitScreen)
            SAVE_BOOL(s, hardwareSrgb)
            SAVE_INT(s, monitorType)
            SAVE_INT(s, frameBuffer)
            SAVE_BOOL(s, useHdr)
            SAVE_INT(s, maxQueuedFrames)
            SAVE_INT(s, tearingLine)
            SAVE_BOOL(s, beamRacing)
            SAVE_INT(s, beamLead)
            SAVE_INT(s, subFramesSetting)
            SAVE_INT(s, subFramesMode)
            s["lowFpsThreshold"] = std::to_string(lowFps.threshold);
            s["lowFpsMode"]      = std::to_string(lowFps.mode);
            s["autoInterval"]    = std::to_string(autoSub.intervalSec);

            auto& shader = ini["shader"];
            for(const auto& param : shaderManager.GetParameterInfos())
            {
                switch(param.type)
                {
                case ParamInt:
                    shader[param.name] = std::to_string(*param.p.ip.value); // always written (a default value was skipped and then lost)
                    break;
                case ParamFloat:
                    shader[param.name] = std::to_string(*param.p.fp.value);
                    break;
                }
            }

            SaveEmitterSettings(ini, emitter);

            auto& hk = ini["hotkeys"];
            for(int id = 0; id < HOTKEY_COUNT; id++)
                hk[HotkeyActionName(id)] = std::to_string(hotkeys[id].mods) + "," + std::to_string(hotkeys[id].vk);
            auto& gp = ini["gamepad"];
            for(int id = 0; id < HOTKEY_COUNT; id++)
                gp[HotkeyActionName(id)] = std::to_string(hotkeys[id].pad);
        }

        file.write(ini);
    }
    catch(...)
    { }
}

void Options::Load(ShaderManager& shaderManager)
{
    try
    {
        mINI::INIFile      file(ProfilesDir() / profileFile);
        mINI::INIStructure ini;

        file.read(ini);
        if(ini.has("settings"))
        {
            auto s = ini.get("settings");
            LOAD_BOOL(s, rememberSettings);
            if(rememberSettings)
            {
                LOAD_INT(s, shaderProfileNo)
                LOAD_INT(s, captureAdapterNo)
                LOAD_INT(s, shaderAdapterNo)
                LOAD_INT(s, captureDisplayNo)
                LOAD_INT(s, shaderDisplayNo)
                shaderScreenId  = s.has("shaderScreen") ? s.get("shaderScreen") : std::string();
                captureScreenId = s.has("captureScreen") ? s.get("captureScreen") : std::string();
                LOAD_BOOL(s, outputOnProcess)
                LOAD_INT(s, captureMethod)
                LOAD_INT(s, splitScreen)
                LOAD_BOOL(s, hardwareSrgb)
                LOAD_INT(s, monitorType)
                LOAD_INT(s, frameBuffer)
                frameBuffer = std::clamp(frameBuffer, -1, 5);
                LOAD_BOOL(s, useHdr)
                LOAD_INT(s, maxQueuedFrames)
                maxQueuedFrames = std::clamp(maxQueuedFrames, 0, QUEUE_TEARING);
                if(maxQueuedFrames == 3)
                    maxQueuedFrames = 0; // 3 is the default now (shown as "3 (default)")
                LOAD_INT(s, tearingLine)
                tearingLine = std::clamp(tearingLine, 0, 100);
                LOAD_BOOL(s, beamRacing)
                LOAD_INT(s, beamLead)
                beamLead = std::clamp(beamLead, 0, 20);
                LOAD_INT(s, subFramesSetting)
                subFramesSetting = std::clamp(subFramesSetting, 0, MAX_SUBFRAMES);
                LOAD_INT(s, subFramesMode)
                if(subFramesMode != SF_AUTO)
                    subFramesMode = SF_MANUAL; // (the removed Adaptive mode of earlier builds falls back to Manual)
                try
                {
                    if(s.has("lowFpsThreshold"))
                        lowFps.threshold = std::clamp(std::stof(s.get("lowFpsThreshold")), 10.0f, 120.0f);
                    if(s.has("lowFpsMode"))
                        lowFps.mode = std::clamp(std::stoi(s.get("lowFpsMode")), 0, 1);
                    // the captured program comes from the active profile link (captureProcess is not stored in profiles)
                    captureWindow = captureProcess.empty() ? NULL : FindCaptureWindow();
                    if(s.has("autoInterval"))
                        autoSub.intervalSec = std::clamp(std::stof(s.get("autoInterval")), 0.0f, 5.0f);
                }
                catch(...)
                { }
                LoadEmitterSettings(ini, emitter);

                if(!appHotkeys && ini.has("hotkeys"))
                {
                    // old settings: the hotkeys were kept in the profiles; from now on they live in Nbeam.app.cfg
                    auto hk = ini.get("hotkeys");
                    auto gp = ini.get("gamepad");
                    ReadHotkeys(hk, ini.has("gamepad") ? &gp : nullptr);
                }
            }

            if(ini.has("shader") && shaderProfileNo <= shaderManager.GetShaders().size())
            {
                auto& shader = ini["shader"];
                for(const auto& param : shaderManager.GetParameterInfos(shaderProfileNo))
                {
                    // not in this profile: its default value (not the value left by the previous profile)
                    if(param.type == ParamInt)
                        *param.p.ip.value = param.p.ip.def;
                    else if(param.type == ParamFloat)
                        *param.p.fp.value = param.p.fp.def;
                    if(shader.has(param.name))
                    {
                        switch(param.type)
                        {
                        case ParamInt:
                            *param.p.ip.value = std::stoi(shader[param.name]);
                            break;
                        case ParamFloat:
                            *param.p.fp.value = std::stof(shader[param.name]);
                            break;
                        }
                    }
                }
            }
        }
    }
    catch(...)
    { }
}
} // namespace ShaderBeam