/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include <fstream>
#include <filesystem>

#include "Helpers.h"

namespace Nbeam
{

HRESULT Helpers::CreateD3DDevice(D3D_DRIVER_TYPE const type, winrt::com_ptr<ID3D11Device>& device)
{
    WINRT_ASSERT(!device);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    return D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
}

HRESULT Helpers::CreateD3DDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE const type, winrt::com_ptr<ID3D11Device>& device)
{
    WINRT_ASSERT(!device);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    return D3D11CreateDevice(adapter, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
}

winrt::com_ptr<ID3D11Device> Helpers::CreateD3DDevice(IDXGIAdapter* adapter)
{
    if(adapter == nullptr)
        return CreateD3DDevice();

    winrt::com_ptr<ID3D11Device> device;
    HRESULT                      hr = CreateD3DDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, device);
    if(DXGI_ERROR_UNSUPPORTED == hr)
    {
        hr = CreateD3DDevice(nullptr, D3D_DRIVER_TYPE_WARP, device);
    }

    winrt::check_hresult(hr);
    return device;
}

winrt::com_ptr<ID3D11Device> Helpers::CreateD3DDevice()
{
    winrt::com_ptr<ID3D11Device> device;
    HRESULT                      hr = CreateD3DDevice(D3D_DRIVER_TYPE_HARDWARE, device);
    if(DXGI_ERROR_UNSUPPORTED == hr)
    {
        hr = CreateD3DDevice(D3D_DRIVER_TYPE_WARP, device);
    }

    winrt::check_hresult(hr);
    return device;
}

static LARGE_INTEGER freq { .QuadPart = 0 };
static LARGE_INTEGER startTicks { .QuadPart = 0 };

void Helpers::InitQPC()
{
    QueryPerformanceFrequency(&freq);
    if(!QueryPerformanceCounter(&startTicks))
    {
        throw std::runtime_error("Unable to query performance counter");
    }
}

float Helpers::GetTicks()
{
    LARGE_INTEGER ticks;
    if(!QueryPerformanceCounter(&ticks) || freq.QuadPart == 0)
    {
        throw std::runtime_error("Unable to query performance counter");
    }

    return (float)((ticks.QuadPart - startTicks.QuadPart) / (freq.QuadPart / TICKS_PER_SEC));
}

ULONGLONG Helpers::GetQPC()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

float Helpers::QPCToTicks(ULONGLONG qpc)
{
    return (float)((qpc - startTicks.QuadPart) / (freq.QuadPart / TICKS_PER_SEC));
}

float Helpers::QPCToDeltaMs(ULONGLONG qpc)
{
    return (float)(qpc / (freq.QuadPart / TICKS_PER_SEC));
}

static std::filesystem::path ShaderCacheDir()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return std::filesystem::path(path).parent_path() / L"ShaderCache";
}

HRESULT Helpers::CompileCached(const char* source, size_t length, const char* name, const D3D_SHADER_MACRO* macros, const char* entry, const char* profile, UINT flags,
                               winrt::com_ptr<ID3DBlob>& blob, std::string& errors)
{
    // 64-bit FNV-1a over everything that changes the bytecode
    uint64_t hash = 1469598103934665603ULL;
    auto     mix  = [&](const void* data, size_t n) {
        const auto* p = (const unsigned char*)data;
        for(size_t i = 0; i < n; i++)
        {
            hash ^= p[i];
            hash *= 1099511628211ULL;
        }
    };
    auto mixStr = [&](const char* s) {
        if(s)
            mix(s, strlen(s));
        mix("\0", 1);
    };
    mixStr("NbeamShaderCache1");
    mixStr(entry);
    mixStr(profile);
    mixStr(name);
    mix(&flags, sizeof(flags));
    for(auto m = macros; m && m->Name; m++)
    {
        mixStr(m->Name);
        mixStr(m->Definition);
    }
    mix(source, length);

    char file[40];
    snprintf(file, sizeof(file), "%016llx.cso", (unsigned long long)hash);
    std::error_code ec;
    const auto      dir  = ShaderCacheDir();
    const auto      path = dir / file;

    // cache hit
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if(in)
        {
            const auto size = (size_t)in.tellg();
            if(size > 16 && size < (64u << 20))
            {
                winrt::com_ptr<ID3DBlob> cached;
                if(SUCCEEDED(D3DCreateBlob(size, cached.put())))
                {
                    in.seekg(0);
                    in.read((char*)cached->GetBufferPointer(), (std::streamsize)size);
                    if(in.gcount() == (std::streamsize)size)
                    {
                        blob = cached;
                        return S_OK;
                    }
                }
            }
        }
    }

    // compile, then store (a failure to write the cache is ignored)
    winrt::com_ptr<ID3DBlob> err;
    HRESULT                  hr = D3DCompile(source, length, name, macros, nullptr, entry, profile, flags, 0, blob.put(), err.put());
    if(FAILED(hr))
    {
        errors = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : std::string();
        return hr;
    }
    std::filesystem::create_directories(dir, ec);
    const auto tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if(out)
            out.write((const char*)blob->GetBufferPointer(), (std::streamsize)blob->GetBufferSize());
    }
    std::filesystem::rename(tmp, path, ec);
    if(ec)
        std::filesystem::remove(tmp, ec);
    return hr;
}

void Helpers::Throw(HRESULT hr, const char* action)
{
    if(FAILED(hr))
    {
        _com_error err(hr);
        auto       error = err.ErrorMessage();        
        throw std::runtime_error(action + std::string("\r\n") + Helpers::WCharToString(error));
    }
}

std::string Helpers::WCharToString(const wchar_t* text)
{
    char utfString[256];
    utfString[255] = 0;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utfString, 255, NULL, NULL);
    return std::string(utfString);
}

} // namespace Nbeam

// ---- inaudible sound that keeps Windows spatial audio (Dolby Atmos) active ----
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <cstring>
namespace Nbeam
{
namespace
{
std::atomic<bool> s_akStop { true };
std::thread       s_akThread;
HANDLE            s_akEvent = nullptr;

template<class T> void SafeRelease(T*& p)
{
    if(p)
    {
        p->Release();
        p = nullptr;
    }
}

// one device from start to loss: false = try again later
void AudioKeepAliveRun()
{
    IMMDeviceEnumerator* en  = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        cl  = nullptr;
    IAudioRenderClient*  rc  = nullptr;
    WAVEFORMATEX*        fmt = nullptr;
    LPWSTR               id  = nullptr;
    std::wstring         myId;
    do
    {
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
            break;
        if(FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            break;
        if(SUCCEEDED(dev->GetId(&id)) && id)
            myId = id;
        if(FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl)))
            break;
        if(FAILED(cl->GetMixFormat(&fmt)) || !fmt)
            break;
        const REFERENCE_TIME buf = 2000000; // 200 ms
        if(FAILED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf, 0, fmt, nullptr)))
            break;
        UINT32 frames = 0;
        if(FAILED(cl->GetBufferSize(&frames)) || FAILED(cl->GetService(__uuidof(IAudioRenderClient), (void**)&rc)))
            break;
        bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT; // (the sub format starts with the format tag)
        const int      bytes = fmt->wBitsPerSample / 8, ch = fmt->nChannels;
        unsigned       rng   = 12345u;
        auto           fill  = [&](UINT32 n) {
            BYTE* data = nullptr;
            if(FAILED(rc->GetBuffer(n, &data)) || !data)
                return false;
            for(UINT32 f = 0; f < n; f++)
                for(int c = 0; c < ch; c++)
                {
                    rng           = rng * 1664525u + 1013904223u;
                    const int r   = (int)(rng >> 16) & 3; // 0..3: a tiny noise, about -90 dBFS
                    BYTE*     p   = data + ((size_t)f * ch + c) * bytes;
                    if(isFloat && bytes == 4)
                        *(float*)p = ((float)r - 1.5f) * 1.0e-5f;
                    else if(bytes == 2)
                        *(short*)p = (short)(r - 1);
                    else if(bytes == 4)
                        *(int*)p = (r - 1) << 16; // 32 bits: +-1 least significant bit of 16 bits
                    else if(bytes == 3)
                    {
                        const int v = (r - 1) << 8; // 24 bits (little endian)
                        p[0]        = (BYTE)(v & 0xFF);
                        p[1]        = (BYTE)((v >> 8) & 0xFF);
                        p[2]        = (BYTE)((v >> 16) & 0xFF);
                    }
                    else
                        memset(p, 0, bytes);
                }
            rc->ReleaseBuffer(n, 0);
            return true;
        };
        if(!fill(frames) || FAILED(cl->Start()))
            break;
        int ticks = 0;
        while(!s_akStop.load())
        {
            if(WaitForSingleObject(s_akEvent, 50) == WAIT_OBJECT_0)
                break;
            UINT32 pad = 0;
            if(FAILED(cl->GetCurrentPadding(&pad)))
                break; // device lost / changed
            if(frames > pad && !fill(frames - pad))
                break;
            if(++ticks % 20 == 0)
            {
                // the default device changed: start again on the new one
                IMMDevice* now = nullptr;
                LPWSTR     nid = nullptr;
                bool       same = true;
                if(SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &now)) && now && SUCCEEDED(now->GetId(&nid)) && nid)
                    same = myId == nid;
                if(nid)
                    CoTaskMemFree(nid);
                SafeRelease(now);
                if(!same)
                    break;
            }
        }
        cl->Stop();
    } while(false);
    if(id)
        CoTaskMemFree(id);
    if(fmt)
        CoTaskMemFree(fmt);
    SafeRelease(rc);
    SafeRelease(cl);
    SafeRelease(dev);
    SafeRelease(en);
}
} // namespace

void AudioKeepAliveSet(bool on)
{
    if(on == !s_akStop.load())
        return;
    if(!on)
    {
        s_akStop = true;
        if(s_akEvent)
            SetEvent(s_akEvent);
        if(s_akThread.joinable())
            s_akThread.join();
        if(s_akEvent)
        {
            CloseHandle(s_akEvent);
            s_akEvent = nullptr;
        }
        return;
    }
    s_akStop  = false;
    s_akEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_akThread = std::thread([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while(!s_akStop.load())
        {
            AudioKeepAliveRun();
            if(!s_akStop.load())
                WaitForSingleObject(s_akEvent, 2000); // (device change or loss: tries again after a short pause)
        }
        if(SUCCEEDED(hr))
            CoUninitialize();
    });
}
} // namespace Nbeam
