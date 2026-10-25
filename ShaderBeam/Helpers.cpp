/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include <fstream>
#include <filesystem>

#include "Helpers.h"

namespace ShaderBeam
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

} // namespace ShaderBeam