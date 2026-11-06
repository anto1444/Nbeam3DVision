/*
Nbeam 3D Vision: NVIDIA 3D Vision USB emitter
Protocol details: see NvidiaProtocol.h.

What is implemented:
  * USB discovery (VID 0x0955, 11 known PID patterns), pipes PIPE02 / PIPE00
  * wake-up, the 6 programming packets (sync rate, open duration, W), 8-byte per-eye packets
  * the wheel and the button of the emitter, read all the time (also without 3D) and used by the 3D mini menu: every 50 ms the emitter is
    asked for the 3 bytes at 0x201F (read + clear, the request of the libnvstusb project) and answers on PIPE03. With the file
    Nbeam_emitter_probe.txt next to the exe, what is read is also written to Nbeam_emitter_input.log.
What is NOT implemented:
  * Switching official 3D Vision monitors into their 3D mode through the undocumented NvAPI: put the monitor in its 3D mode yourself.
Requires the NV3DVisionUSB driver (extract it from the 425.31 NVIDIA package, or use 3dvisioncontrollerdriver.exe from WibbleWobble).
MIT License
*/

#include "stdafx.h"
#include <setupapi.h>
#include <cmath>
#include <atomic>
#include <thread>
#include <mutex>
#include "Emitter.h"
#include "Gamepad.h"
#include "NvidiaProtocol.h"

#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace ShaderBeam
{

// GUID_DEVINTERFACE_USB_DEVICE
static const GUID kUsbDeviceGuid = { 0xa5dcbf10, 0x6530, 0x11d2, { 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed } };

static std::string ToAscii(const std::wstring& w)
{
    std::string s;
    for(auto c : w)
        s.push_back(c < 128 ? (char)tolower((int)c) : '?');
    return s;
}

// device interface path of the first 3D Vision emitter found, empty if none
static std::wstring FindEmitterPath()
{
    std::wstring found;
    HDEVINFO     set = SetupDiGetClassDevsW(&kUsbDeviceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if(set == INVALID_HANDLE_VALUE)
        return found;
    SP_DEVICE_INTERFACE_DATA ifd {};
    ifd.cbSize = sizeof(ifd);
    for(DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &kUsbDeviceGuid, i, &ifd); i++)
    {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if(need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;
        std::vector<BYTE> buf(need);
        auto*             detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        detail->cbSize           = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr))
            continue;
        const std::string path = ToAscii(detail->DevicePath);
        for(auto pattern : Nv::kUsbPathPatterns)
        {
            if(path.find(pattern) != std::string::npos)
            {
                found = detail->DevicePath;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Startup check (3D menu, warning): a present USB device with an emitter id whose driver is loaded without problem.
// The device instance id (USB\VID_0955&PID_0007\...) is matched like the interface paths (usb#vid_0955&pid_0007...).
bool NvidiaEmitterReady()
{
    bool     ready = false;
    HDEVINFO set   = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if(set == INVALID_HANDLE_VALUE)
        return false;
    SP_DEVINFO_DATA dd {};
    dd.cbSize = sizeof(dd);
    for(DWORD i = 0; !ready && SetupDiEnumDeviceInfo(set, i, &dd); i++)
    {
        wchar_t id[512] {};
        if(!SetupDiGetDeviceInstanceIdW(set, &dd, id, 511, nullptr))
            continue;
        std::string s = ToAscii(id);
        for(auto& c : s)
            if(c == '\\')
                c = '#';
        bool match = false;
        for(auto pattern : Nv::kUsbPathPatterns)
            match = match || s.rfind(pattern, 0) == 0;
        if(!match)
            continue;
        ULONG status = 0, problem = 0;
        if(CM_Get_DevNode_Status(&status, &problem, dd.DevInst, 0) == CR_SUCCESS && (status & DN_DRIVER_LOADED) && !(status & DN_HAS_PROBLEM))
            ready = true;
    }
    SetupDiDestroyDeviceInfoList(set);
    return ready;
}

// ---- wheel and button of the emitter ------------------------------------------------------------------------------------
static std::mutex g_controlMutex; // every write to the control pipe (programming of the emitter, request of the keys)

static HANDLE OpenPipeAt(const std::wstring& base, const char* pipe)
{
    std::wstring path = base + L"\\" + std::wstring(pipe, pipe + strlen(pipe));
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// what the emitter reader found, written to Nbeam_emitter_input.log only when Nbeam_emitter_probe.txt exists next to the exe
static void KeyLog(const std::string& text)
{
    static std::mutex m;
    static std::wstring path;
    static int enabled = -1;
    std::lock_guard<std::mutex> lock(m);
    if(enabled < 0)
    {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring dir = exe;
        dir.resize(dir.find_last_of(L'\\') + 1);
        enabled = GetFileAttributesW((dir + L"Nbeam_emitter_probe.txt").c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        path    = dir + L"Nbeam_emitter_input.log";
    }
    if(!enabled)
        return;
    FILE* f = nullptr;
    if(_wfopen_s(&f, path.c_str(), L"ab") == 0 && f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text.c_str());
        fclose(f);
    }
}

// answer to the request of the keys: 7 bytes = offset 0x18, count 3, bytes sent (2), wheel, wheel + button, button bits
static void HandleKeysReport(const uint8_t* buf, DWORD got)
{
    if(got != 7 || buf[0] != 0x18 || buf[1] != 0x03)
        return;
    const int  wheel   = (int8_t)buf[4];
    const int  held    = (int8_t)buf[5]; // wheel turned while the button is held down
    const bool button  = (buf[6] & 0x01) != 0;
    if(!wheel && !held && !button && !(buf[6] & ~0x05)) // (bits other than 0 and 2 of the last byte are logged too: a sign of the release of the button?)
        return;
    auto& e = Pad::Emitter();
    if(wheel)
        e.menuWheel += wheel;
    if(held)
        e.menuHeld += held;
    // the emitter reports the button when it is released (also at the end of a turn made with the button held): the menu sorts it out
    if(button)
        e.menuClicks++;
    KeyLog("keys: wheel " + std::to_string(wheel) + ", button held + wheel " + std::to_string(held) + ", button " + std::to_string((int)button) + " [bytes " + std::to_string((int)buf[4]) + " " + std::to_string((int)buf[5]) + " " + std::to_string((int)buf[6]) + "]");
}

// Reads the answers on PIPE03 and asks for the keys every 50 ms on the control pipe (borrowed from the 3D emitter or owned by the service)
class KeysLink
{
    HANDLE            m_in { INVALID_HANDLE_VALUE };
    HANDLE            m_control { INVALID_HANDLE_VALUE };
    std::thread       m_reader, m_writer;
    std::atomic<bool> m_run { false }, m_failed { false }, m_readerDone { true };

public:
    bool Start(const std::wstring& base, HANDLE control)
    {
        m_in = OpenPipeAt(base, "PIPE03");
        if(m_in == INVALID_HANDLE_VALUE)
        {
            KeyLog("PIPE03: cannot open (error " + std::to_string(GetLastError()) + ")");
            return false;
        }
        m_control    = control;
        m_failed     = false;
        m_run        = true;
        m_readerDone = false;
        m_reader     = std::thread([this]() {
            int errors = 0;
            while(m_run)
            {
                uint8_t buf[64] = {};
                DWORD   got     = 0;
                if(!ReadFile(m_in, buf, sizeof(buf), &got, nullptr))
                {
                    const DWORD err = GetLastError();
                    if(!m_run || err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE)
                        break;
                    if(++errors >= 3)
                    {
                        KeyLog("PIPE03: read error " + std::to_string(err));
                        m_failed = true;
                        break;
                    }
                    Sleep(200);
                    continue;
                }
                errors = 0;
                HandleKeysReport(buf, got);
            }
            m_readerDone = true;
        });
        m_writer = std::thread([this]() {
            const uint8_t readKeys[4] = { 0x42, 0x18, 0x03, 0x00 }; // read + clear, 3 bytes from 0x201F
            int           errors      = 0;
            while(m_run && !m_failed)
            {
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(g_controlMutex);
                    DWORD                       written = 0;
                    ok                                  = WriteFile(m_control, readKeys, sizeof(readKeys), &written, nullptr) && written == sizeof(readKeys);
                }
                if(ok)
                    errors = 0;
                else if(++errors >= 5)
                {
                    KeyLog("control pipe: write error " + std::to_string(GetLastError()));
                    m_failed = true;
                    break;
                }
                Sleep(50);
            }
        });
        KeyLog("emitter keys: started");
        return true;
    }

    bool Failed() const { return m_failed; }

    void Stop()
    {
        if(!m_run && !m_reader.joinable() && !m_writer.joinable())
            return;
        m_run = false;
        if(m_writer.joinable())
            m_writer.join();
        for(int i = 0; i < 200 && !m_readerDone; i++) // the reader may be waiting in ReadFile: cancel until it leaves
        {
            CancelIoEx(m_in, nullptr);
            Sleep(10);
        }
        if(m_reader.joinable())
            m_reader.join();
        if(m_in != INVALID_HANDLE_VALUE)
            CloseHandle(m_in);
        m_in      = INVALID_HANDLE_VALUE;
        m_control = INVALID_HANDLE_VALUE;
    }
};

// Background reader used while the 3D emitter is not running (the 3D emitter reads the keys itself and the service waits).
static std::thread       s_svcThread;
static std::atomic<bool> s_svcRun { false }, s_svcPause { false }, s_svcPaused { true };

static void KeysServiceLoop()
{
    KeysLink     link;
    HANDLE       control   = INVALID_HANDLE_VALUE;
    bool         connected = false;
    auto         drop      = [&]() {
        link.Stop();
        if(control != INVALID_HANDLE_VALUE)
            CloseHandle(control);
        control   = INVALID_HANDLE_VALUE;
        connected = false;
    };
    // sleeps in short steps so that pausing / stopping is quick
    auto wait = [&](int ms) {
        for(int i = 0; i < ms / 20 && s_svcRun && !s_svcPause; i++)
            Sleep(20);
    };
    while(s_svcRun)
    {
        if(s_svcPause)
        {
            if(connected)
                drop();
            s_svcPaused = true;
            Sleep(20);
            continue;
        }
        s_svcPaused = false;
        if(connected)
        {
            if(link.Failed())
            {
                drop();
                wait(2000);
            }
            else
                wait(100);
            continue;
        }
        const std::wstring base = FindEmitterPath();
        if(base.empty())
        {
            wait(5000);
            continue;
        }
        HANDLE wake = OpenPipeAt(base, Nv::kWakePipe); // wake the emitter (open + close), as the 3D emitter does
        if(wake == INVALID_HANDLE_VALUE)
        {
            wait(5000);
            continue;
        }
        Sleep(100);
        CloseHandle(wake);
        Sleep(100);
        control = OpenPipeAt(base, Nv::kControlPipe);
        if(control == INVALID_HANDLE_VALUE || !link.Start(base, control))
        {
            drop();
            wait(5000);
            continue;
        }
        connected = true;
    }
    drop();
    s_svcPaused = true;
}

void NvidiaKeysService(bool run)
{
    if(run && !s_svcRun)
    {
        s_svcRun    = true;
        s_svcThread = std::thread(KeysServiceLoop);
    }
    else if(!run && s_svcRun)
    {
        s_svcRun = false;
        if(s_svcThread.joinable())
            s_svcThread.join();
    }
}

// stops the service when the program ends (a std::thread still joinable at exit would end Nbeam with std::terminate); built after s_svcThread and
// the objects the service uses, so it is destroyed before them
static struct KeysServiceGuard
{
    KeysServiceGuard() { (void)Pad::Emitter(); }
    ~KeysServiceGuard() { NvidiaKeysService(false); }
} s_keysServiceGuard;

// the 3D emitter needs the pipes for itself: the service lets go of them (and comes back afterwards)
void NvidiaKeysPause(bool pause)
{
    s_svcPause = pause;
    if(pause && s_svcRun)
        for(int i = 0; i < 300 && !s_svcPaused; i++)
            Sleep(10);
}

class NvidiaEmitter : public IEmitter
{
    StereoEmitterSettings* m_s { nullptr };
    EmitterEnv             m_env;
    Nv::Timings            m_programmed {};
    ULONGLONG              m_lastProgramTick { 0 };
    HANDLE       m_control { INVALID_HANDLE_VALUE };
    HANDLE       m_sync { INVALID_HANDLE_VALUE };
    std::wstring m_base;
    KeysLink     m_keys;
    bool         m_paused { false };

    HANDLE OpenPipe(const char* pipe) const
    {
        return OpenPipeAt(m_base, pipe);
    }
    static bool Write(HANDLE h, const void* data, DWORD size)
    {
        DWORD written = 0;
        return WriteFile(h, data, size, &written, nullptr) && written == size;
    }

    Nv::Timings CurrentTimings() const
    {
        const auto& cfg = m_s->nvidia;
        return Nv::Timings { (float)m_env.SyncRateHz(*m_s), cfg.openDurationUs };
    }

    // Live tuning: when Open Duration (or the sync rate) changed in the UI, send the programming
    // packets again. Runs on the emitter sync thread (the control pipe is only ever written from here after Init),
    // at most every 50 ms so that dragging a slider does not flood the USB pipe.
    void ReprogramIfChanged()
    {
        const Nv::Timings t = CurrentTimings();
        auto differs = [](float a, float b) { return std::fabs(a - b) > 0.001f; };
        if(!differs(t.syncRateHz, m_programmed.syncRateHz) && !differs(t.openDurationUs, m_programmed.openDurationUs))
            return;
        const ULONGLONG now = GetTickCount64();
        if(now - m_lastProgramTick < 50)
            return;
        m_lastProgramTick = now;
        std::lock_guard<std::mutex> controlLock(g_controlMutex);
        for(const auto& packet : Nv::BuildProgrammingPackets(t))
            if(!Write(m_control, packet.data(), (DWORD)packet.size()))
                break;
        m_programmed = t; // even on failure: do not retry in a loop
    }

public:
    const char* Name() const override { return "NVIDIA 3D Vision"; }

    bool Init(StereoEmitterSettings& s, const EmitterEnv& env, std::string& status) override
    {
        NvidiaKeysPause(true); // the background reader lets go of the emitter while the 3D runs
        m_paused = true;
        const bool ok = InitEmitter(s, env, status);
        if(!ok && m_paused)
        {
            NvidiaKeysPause(false);
            m_paused = false;
        }
        return ok;
    }

    bool InitEmitter(StereoEmitterSettings& s, const EmitterEnv& env, std::string& status)
    {
        auto& cfg = s.nvidia;
        m_s       = &s;
        m_env     = env;

        // no automatic timing database: a sensible default duration until the sliders are used
        if(cfg.openDurationUs <= 0.0f)
            cfg.openDurationUs = 2000.0f;

        // 1. find the emitter
        m_base = FindEmitterPath();
        if(m_base.empty())
        {
            status = "no 3D Vision USB emitter found (VID 0955) - is the NV3DVisionUSB driver installed?";
            return false;
        }

        // 2. wake it up (open + close PIPE02), then open the control and sync pipes
        HANDLE wake = OpenPipe(Nv::kWakePipe);
        if(wake == INVALID_HANDLE_VALUE)
        {
            status = "emitter absent or asleep (cannot open PIPE02, error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        Sleep(100);
        CloseHandle(wake);
        Sleep(100);

        m_control = OpenPipe(Nv::kControlPipe);
        m_sync    = OpenPipe(Nv::kSyncPipe);
        if(m_control == INVALID_HANDLE_VALUE || m_sync == INVALID_HANDLE_VALUE)
        {
            status = "cannot open emitter pipes (error " + std::to_string(GetLastError()) + ")";
            Deinit();
            return false;
        }

        // 4. program timings
        const Nv::Timings t = CurrentTimings();
        std::unique_lock<std::mutex> controlLock(g_controlMutex);
        for(const auto& packet : Nv::BuildProgrammingPackets(t))
        {
            if(!Write(m_control, packet.data(), (DWORD)packet.size()))
            {
                controlLock.unlock();
                status = "programming write failed (error " + std::to_string(GetLastError()) + ")";
                Deinit();
                return false;
            }
        }
        controlLock.unlock();
        m_programmed      = t;
        m_lastProgramTick = GetTickCount64();
        Sleep(250); // let the emitter settle before the first sync
        m_keys.Start(m_base, m_control); // wheel and button (a failure here does not matter for the 3D)

        char buf[160];
        snprintf(buf, sizeof(buf), "connected, %.2f Hz, open %.0f us", t.syncRateHz, t.openDurationUs);
        status = buf;
        return true;
    }

    void OnEnvChanged(const EmitterEnv& env) override { m_env = env; } // ReprogramIfChanged() then sends the new sync rate

    void Deinit() override
    {
        m_keys.Stop();
        if(m_control != INVALID_HANDLE_VALUE)
            CloseHandle(m_control);
        if(m_sync != INVALID_HANDLE_VALUE)
            CloseHandle(m_sync);
        m_control = m_sync = INVALID_HANDLE_VALUE;
        if(m_paused)
        {
            m_paused = false;
            NvidiaKeysPause(false); // the background reader takes the emitter back
        }
    }

    void Sync(Eye eye) override
    {
        if(m_sync != INVALID_HANDLE_VALUE && m_control != INVALID_HANDLE_VALUE && m_s)
            ReprogramIfChanged();
        if(m_sync != INVALID_HANDLE_VALUE)
            Write(m_sync, Nv::kEyePacket[eye == Eye::Left ? 0 : 1], Nv::kEyePacketSize);
    }
};

std::unique_ptr<IEmitter> CreateNvidiaEmitter()
{
    return std::make_unique<NvidiaEmitter>();
}

} // namespace ShaderBeam
