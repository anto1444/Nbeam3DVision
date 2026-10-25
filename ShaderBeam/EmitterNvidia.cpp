/*
Nbeam 3D Vision: NVIDIA 3D Vision USB emitter
Protocol details: see NvidiaProtocol.h.

What is implemented:
  * USB discovery (VID 0x0955, 11 known PID patterns), pipes PIPE02 / PIPE00
  * wake-up, the 6 programming packets (sync rate, open duration, W), 8-byte per-eye packets
What is NOT implemented:
  * Switching official 3D Vision monitors into their 3D mode through the undocumented NvAPI: put the monitor in its 3D mode yourself.
Requires the NV3DVisionUSB driver (extract it from the 425.31 NVIDIA package, or use 3dvisioncontrollerdriver.exe from WibbleWobble).
MIT License
*/

#include "stdafx.h"
#include <setupapi.h>
#include <cmath>
#include "Emitter.h"
#include "NvidiaProtocol.h"

#pragma comment(lib, "setupapi.lib")

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

class NvidiaEmitter : public IEmitter
{
    StereoEmitterSettings* m_s { nullptr };
    EmitterEnv             m_env;
    Nv::Timings            m_programmed {};
    ULONGLONG              m_lastProgramTick { 0 };
    HANDLE       m_control { INVALID_HANDLE_VALUE };
    HANDLE       m_sync { INVALID_HANDLE_VALUE };
    std::wstring m_base;

    HANDLE OpenPipe(const char* pipe) const
    {
        std::wstring path = m_base + L"\\" + std::wstring(pipe, pipe + strlen(pipe));
        return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
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
        for(const auto& packet : Nv::BuildProgrammingPackets(t))
            if(!Write(m_control, packet.data(), (DWORD)packet.size()))
                break;
        m_programmed = t; // even on failure: do not retry in a loop
    }

public:
    const char* Name() const override { return "NVIDIA 3D Vision"; }

    bool Init(StereoEmitterSettings& s, const EmitterEnv& env, std::string& status) override
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
        for(const auto& packet : Nv::BuildProgrammingPackets(t))
        {
            if(!Write(m_control, packet.data(), (DWORD)packet.size()))
            {
                status = "programming write failed (error " + std::to_string(GetLastError()) + ")";
                Deinit();
                return false;
            }
        }
        m_programmed      = t;
        m_lastProgramTick = GetTickCount64();
        Sleep(250); // let the emitter settle before the first sync

        char buf[160];
        snprintf(buf, sizeof(buf), "connected, %.2f Hz, open %.0f us", t.syncRateHz, t.openDurationUs);
        status = buf;
        return true;
    }

    void OnEnvChanged(const EmitterEnv& env) override { m_env = env; } // ReprogramIfChanged() then sends the new sync rate

    void Deinit() override
    {
        if(m_control != INVALID_HANDLE_VALUE)
            CloseHandle(m_control);
        if(m_sync != INVALID_HANDLE_VALUE)
            CloseHandle(m_sync);
        m_control = m_sync = INVALID_HANDLE_VALUE;
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
