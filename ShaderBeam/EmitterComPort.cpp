/*
ShaderBeam: shader effect overlay -- ComPort emitter (Arduino VESA / IR sender)
Protocol (from WW's WWVESASender.ino / WWIRSender.ino): 115200 baud, one ASCII byte per eye: '0' = left, '1' = right.
  IR sketch  : '1' -> open right / close left, anything else -> open left / close right
  VESA sketch: '0' -> sync pin HIGH, anything else -> LOW
MIT License
*/

#include "stdafx.h"
#include "Emitter.h"

namespace ShaderBeam
{

std::vector<int> EnumerateComPorts()
{
    std::vector<int> ports;
    HKEY             key;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        for(DWORD i = 0;; i++)
        {
            wchar_t name[256], value[64];
            DWORD   nameLen = 256, valueBytes = sizeof(value) - sizeof(wchar_t), type = 0;
            if(RegEnumValueW(key, i, name, &nameLen, nullptr, &type, (LPBYTE)value, &valueBytes) != ERROR_SUCCESS)
                break;
            value[valueBytes / sizeof(wchar_t)] = 0;
            if(type == REG_SZ && wcsncmp(value, L"COM", 3) == 0)
                ports.push_back(_wtoi(value + 3));
        }
        RegCloseKey(key);
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

class ComPortEmitter : public IEmitter
{
    HANDLE m_h { INVALID_HANDLE_VALUE };

public:
    const char* Name() const override { return "ComPort"; }

    bool Init(StereoEmitterSettings& s, const EmitterEnv&, std::string& status) override
    {
        if(s.comPort.comPortNo < 0)
        {
            status = "no port selected";
            return false;
        }
        wchar_t path[32];
        swprintf_s(path, L"\\\\.\\COM%d", s.comPort.comPortNo);
        m_h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if(m_h == INVALID_HANDLE_VALUE)
        {
            status = "cannot open COM" + std::to_string(s.comPort.comPortNo) + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        DCB dcb {};
        dcb.DCBlength = sizeof(dcb);
        GetCommState(m_h, &dcb);
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        SetCommState(m_h, &dcb);
        COMMTIMEOUTS to {};
        to.WriteTotalTimeoutConstant = 5;
        SetCommTimeouts(m_h, &to);
        status = "COM" + std::to_string(s.comPort.comPortNo);
        return true;
    }

    void Deinit() override
    {
        if(m_h != INVALID_HANDLE_VALUE)
            CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }

    void Sync(Eye eye) override
    {
        const char c = eye == Eye::Left ? '0' : '1';
        DWORD      n = 0;
        WriteFile(m_h, &c, 1, &n, nullptr);
    }
};

std::unique_ptr<IEmitter> CreateComPortEmitter()
{
    return std::make_unique<ComPortEmitter>();
}

} // namespace ShaderBeam
