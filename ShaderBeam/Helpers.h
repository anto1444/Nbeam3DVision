/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include <d3dcompiler.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

#define THROW(hr, msg) Helpers::Throw(hr, msg)

#define TICKS_PER_SEC 1000.0f

namespace ShaderBeam
{

class Helpers
{
public:
    static winrt::com_ptr<ID3D11Device> CreateD3DDevice();
    static winrt::com_ptr<ID3D11Device> CreateD3DDevice(IDXGIAdapter* adapter);
    static void                         InitQPC();
    static float                        GetTicks();
    static ULONGLONG                    GetQPC();
    static float                        QPCToTicks(ULONGLONG qpc);
    static float                        QPCToDeltaMs(ULONGLONG qpc);
    static void                         Throw(HRESULT hr, const char* action);
    static std::string                  WCharToString(const wchar_t* text);
    // D3DCompile with an on-disk cache (ShaderCache folder next to the exe, keyed by a hash of the source and options):
    // the first launch compiles, the next ones just load the bytecode, which makes startup and restarts much faster.
    static HRESULT CompileCached(const char* source, size_t length, const char* name, const D3D_SHADER_MACRO* macros, const char* entry, const char* profile, UINT flags,
                                 winrt::com_ptr<ID3DBlob>& blob, std::string& errors);

private:
    static HRESULT CreateD3DDevice(D3D_DRIVER_TYPE const type, winrt::com_ptr<ID3D11Device>& device);
    static HRESULT CreateD3DDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE const type, winrt::com_ptr<ID3D11Device>& device);
};

// The calling thread joins the Windows multimedia scheduler (MMCSS, "Games" task, high priority) for its lifetime: it keeps getting
// CPU time when a game takes every core (shader compilation, loading screens). Nothing happens if the service is not available.
class MmcssGames
{
public:
    MmcssGames()
    {
        DWORD index = 0;
        m_handle    = AvSetMmThreadCharacteristicsW(L"Games", &index);
        if(m_handle)
            AvSetMmThreadPriority(m_handle, AVRT_PRIORITY_HIGH);
    }
    ~MmcssGames()
    {
        if(m_handle)
            AvRevertMmThreadCharacteristics(m_handle);
    }
    MmcssGames(const MmcssGames&)            = delete;
    MmcssGames& operator=(const MmcssGames&) = delete;

private:
    HANDLE m_handle { nullptr };
};
} // namespace ShaderBeam
