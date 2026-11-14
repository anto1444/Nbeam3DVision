/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"

#include "CaptureWGC.h"
#include "Helpers.h"

#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.interop.h>

namespace ShaderBeam
{

CaptureWGC::CaptureWGC(Watcher& watcher, const Options& options) : CaptureBase(watcher, options)
{
    m_name = "Windows Graphics Capture";
}

bool CaptureWGC::IsSupported()
{
    return HasCaptureAPI();
}

bool CaptureWGC::SupportsWindowCapture()
{
    return true;
}

void CaptureWGC::InternalStart()
{
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item { 0 };

    if(m_options.captureWindow)
    {
        item            = CreateCaptureItemForWindow(m_options.captureWindow);
        m_captureWindow = m_options.captureWindow;
    }
    else
    {
        item            = CreateCaptureItemForMonitor(m_options.captureMonitor);
        m_captureWindow = NULL;
    }

    auto format =
        m_options.useHdr ? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float : winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    auto contentSize = item.Size();

    m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(CreateDirect3DDevice(m_captureDevice.get()), format, m_options.wgcBuffers, contentSize);

    if(m_options.gpuThreadPriority)
        m_captureDevice->SetGPUThreadPriority(m_options.gpuThreadPriority | 0x40000000);

    m_poolSize = contentSize;

    m_session = m_framePool.CreateCaptureSession(item);

    if(CanDisableBorder())
        m_session.IsBorderRequired(false);

    if(CanSetCaptureRate())
    {
        // workaround for an older bug
        m_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(std::chrono::milliseconds(1)));
        m_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(0));
    }

    if(CanUpdateCursor())
        m_session.IsCursorCaptureEnabled(m_options.captureDisplayNo != m_options.shaderDisplayNo);

    m_session.StartCapture();
}

void CaptureWGC::InternalStop()
{
    try
    {
        if(m_session)
            m_session.Close();
        if(m_framePool)
            m_framePool.Close();
    }
    catch(...)
    { }
    m_session   = nullptr;
    m_framePool = nullptr;
}

bool CaptureWGC::InternalPoll(const winrt::com_ptr<ID3D11Texture2D>& outputTexture)
{
    if(m_captureWindow && !IsWindow(m_captureWindow))
    {
        // window closed, restart capture with desktop: asked once (it was posted on every frame, hundreds of restarts a second)
        PostMessage(m_options.outputWindow, WM_USER_NOWINDOW, 0, 0);
        m_captureWindow = NULL;
        return false;
    }
    if(!m_framePool)
        return false; // the capture could not start (window gone at the wrong moment...): nothing to read

    auto frame = m_framePool.TryGetNextFrame();
    if(frame)
    {
        // take everything queued
        auto newerFrame = m_framePool.TryGetNextFrame();
        while(newerFrame)
        {
            swap(frame, newerFrame);
            auto timestamp = frame.SystemRelativeTime();
            m_watcher.FrameReceived(Helpers::QPCToTicks(timestamp.count()));
            newerFrame = m_framePool.TryGetNextFrame();
        }

        auto texture   = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
        auto size      = frame.ContentSize();
        auto timestamp = frame.SystemRelativeTime();
        m_watcher.FrameReceived(Helpers::QPCToTicks(timestamp.count()));
        if(size.Width != m_poolSize.Width || size.Height != m_poolSize.Height)
        {
            // the captured window changed size (e.g. resized to twice the width for Full SBS): the pool buffers keep their old size and Windows would
            // squeeze the new picture into them, so the pool is made again at the new size (this frame is the squeezed one: skipped)
            try
            {
                auto format = m_options.useHdr ? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float : winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                m_framePool.Recreate(CreateDirect3DDevice(m_captureDevice.get()), format, m_options.wgcBuffers, size);
                m_poolSize = size;
            }
            catch(...)
            { }
            return false;
        }
        {
            // age of this frame now (SystemRelativeTime: the performance counter, in 100 ns units), for the BFI phase lock
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const double nowHns = (double)now.QuadPart * 1.0e7 / (double)freq.QuadPart;
            m_lastFrameAgeMs    = (nowHns - (double)timestamp.count()) / 1.0e4;
        }
        // window capture: only the game (the client area of its window), without the title bar, the borders and the shadow, so that nothing
        // shifts or squeezes the picture (a 3D format would be split in the wrong place)
        int cx = 0, cy = 0, cw = size.Width, ch = size.Height;
        if(m_captureWindow)
            ClientPart(size.Width, size.Height, cx, cy, cw, ch);
        CopyToOutput(texture, cw, ch, outputTexture, cx, cy);
        return true;
    }
    return false;
}

// the client area of the captured window inside the frame (the frame covers the visible window: DWM extended frame bounds). Read again every
// 100 ms or when the frame size changes; while the window and the frame do not have the same size (during a resize) the whole frame is used.
void CaptureWGC::ClientPart(int W, int H, int& x, int& y, int& w, int& h)
{
    const ULONGLONG now = GetTickCount64();
    if(W != m_cropFrameW || H != m_cropFrameH || now - m_cropAt >= 100)
    {
        m_cropAt     = now;
        m_cropFrameW = W;
        m_cropFrameH = H;
        m_cropOk     = false;
        RECT  efb {}, cr {};
        POINT o { 0, 0 };
        if(DwmGetWindowAttribute(m_captureWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof(efb)) == S_OK && GetClientRect(m_captureWindow, &cr) &&
           ClientToScreen(m_captureWindow, &o) && abs((efb.right - efb.left) - W) <= 2 && abs((efb.bottom - efb.top) - H) <= 2)
        {
            int cx = (std::max)(0, (int)(o.x - efb.left)), cy = (std::max)(0, (int)(o.y - efb.top));
            int cw = (std::min)((int)cr.right, W - cx), ch = (std::min)((int)cr.bottom, H - cy);
            if(cw >= 64 && ch >= 64)
            {
                m_crop[0] = cx, m_crop[1] = cy, m_crop[2] = cw, m_crop[3] = ch;
                m_cropOk  = true;
            }
        }
    }
    if(m_cropOk)
        x = m_crop[0], y = m_crop[1], w = m_crop[2], h = m_crop[3];
}

template <typename T>
winrt::com_ptr<T> CaptureWGC::GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object)
{
    auto              access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
    return result;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureWGC::CreateCaptureItemForMonitor(HMONITOR hmon)
{
    auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = { nullptr };
    winrt::check_hresult(interop_factory->CreateForMonitor(hmon, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureWGC::CreateCaptureItemForWindow(HWND hwnd)
{
    auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = { nullptr };
    winrt::check_hresult(interop_factory->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CaptureWGC::CreateDirect3DDevice(IDXGIDevice* dxgi_device)
{
    winrt::com_ptr<::IInspectable> d3d_device;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device, d3d_device.put()));
    return d3d_device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

bool CaptureWGC::HasCaptureAPI()
{
    try
    {
        return winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureItem");
    }
    catch(...)
    {
        return false;
    }
}

bool CaptureWGC::CanDisableBorder()
{
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
}

bool CaptureWGC::CanSetCaptureRate()
{
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval");
}

bool CaptureWGC::CanUpdateCursor()
{
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
}
} // namespace ShaderBeam
