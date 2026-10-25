/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"

#include "CaptureBase.h"
#include "Helpers.h"
#include <map>

namespace ShaderBeam
{

namespace
{
// Resolution fix: when the captured screen and the output screen have different sizes the picture is scaled to the output
// (upscaled when the output is bigger, downscaled when it is smaller), keeping the aspect ratio (black bars if needed).
struct Scaler
{
    winrt::com_ptr<ID3D11Device>             device;
    winrt::com_ptr<ID3D11VertexShader>       vs;
    winrt::com_ptr<ID3D11PixelShader>        ps;
    winrt::com_ptr<ID3D11Buffer>             cb;
    winrt::com_ptr<ID3D11SamplerState>       samp;
    winrt::com_ptr<ID3D11Texture2D>          srcCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> srcSrv;
    std::map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>> rtvs; // one per output texture (the buffer ring has several)
    UINT                                     srcW = 0, srcH = 0;
    DXGI_FORMAT                              srcFmt = DXGI_FORMAT_UNKNOWN;
};
Scaler g_scaler;

const char kScaleHlsl[] = R"HLSL(
cbuffer Params : register(b0) { float4 a; float4 b; float4 c; }; // c = viewport origin ; a = src w,h, dst w,h ; b = taps x, taps y, srgb out, 0
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);
float4 VSmain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float3 Dec(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 PSmain(float4 pos : SV_Position) : SV_Target
{
    // pos is inside the viewport (the fitted rectangle): uv from the viewport origin handled by the caller through a.zw
    float2 uv = (pos.xy - c.xy) / a.zw;
    float2 stepUv = float2(1.0 / a.x, 1.0 / a.y);
    float  tx = b.x, ty = b.y;
    float2 foot = float2(a.x / a.z, a.y / a.w); // source pixels per output pixel
    float4 acc = 0;
    for(int j = 0; j < (int)ty; j++)
        for(int i = 0; i < (int)tx; i++)
        {
            float2 o = (float2(i + 0.5, j + 0.5) / float2(tx, ty) - 0.5) * max(foot, 1.0) * stepUv;
            acc += src.SampleLevel(smp, uv + o, 0);
        }
    acc /= tx * ty;
    if(b.z > 0.5)
        acc.rgb = Dec(saturate(acc.rgb));
    return acc;
}
)HLSL";
void g_scalerReset() { g_scaler = Scaler {}; }
} // namespace

CaptureBase::CaptureBase(Watcher& watcher, const Options& options) : m_watcher(watcher), m_options(options), m_name() { }

void CaptureBase::Start(winrt::com_ptr<IDXGIDevice> captureDevice, winrt::com_ptr<ID3D11DeviceContext> outputContext)
{
    m_captureDevice       = captureDevice;
    m_outputContext       = outputContext;
    m_stopping            = false;
    m_stagingCopyRequired = false;
    m_windowX             = 0;
    m_windowY             = 0;

    if(m_options.captureWindow && !m_options.processOutput ) // (output "Process": the output is the window itself, no offset)
    {
        RECT captureRect;
        if(DwmGetWindowAttribute(m_options.captureWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &captureRect, sizeof(RECT)) == S_OK)
        {
            auto        monitor = MonitorFromWindow(m_options.captureWindow, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo;
            monitorInfo.cbSize = sizeof(monitorInfo);
            if(GetMonitorInfo(monitor, &monitorInfo))
            {
                m_windowX = captureRect.left - monitorInfo.rcMonitor.left;
                m_windowY = captureRect.top - monitorInfo.rcMonitor.top;
            }
        }
    }

    InternalStart();
}

void CaptureBase::BenchmarkCopy(const winrt::com_ptr<ID3D11Texture2D>& outputTexture)
{
    if(m_options.crossAdapter)
        CopyStagingToOutput(outputTexture);
}

void CaptureBase::Stop()
{
    m_stopping = true;
    g_scalerReset();

    InternalStop();

    m_stagingFrame        = nullptr;
    m_stagingContext      = nullptr;
    m_stagingDevice       = nullptr;
    m_outputContext       = nullptr;
    m_captureDevice       = nullptr;
    m_stagingCopyRequired = false;
}

bool CaptureBase::Poll(const winrt::com_ptr<ID3D11Texture2D>& outputTexture)
{
    if(m_stopping || !outputTexture)
        return false;

    bool newFrame = InternalPoll(outputTexture);

    // we do this separately so that capture API can get its resource back asap
    if(m_stagingCopyRequired)
    {
        CopyStagingToOutput(outputTexture);
    }

    return newFrame;
}

void CaptureBase::CopyToOutput(const winrt::com_ptr<ID3D11Texture2D>& capturedFrame, int width, int height, const winrt::com_ptr<ID3D11Texture2D>& outputTexture)
{
    if(m_stopping || !capturedFrame || width == 0 || height == 0)
        return;

    if(m_options.crossAdapter)
    {
        // capture and render are different adapters, we need to copy the texture via CPU
        if(!m_stagingDevice)
        {
            m_stagingDevice = m_captureDevice.as<ID3D11Device>();
            m_stagingDevice->GetImmediateContext(m_stagingContext.put());
        }
        if(!m_stagingFrame)
        {
            D3D11_TEXTURE2D_DESC stagingDesc {};
            stagingDesc.Width              = m_options.outputWidth;
            stagingDesc.Height             = m_options.outputHeight;
            stagingDesc.ArraySize          = 1;
            stagingDesc.Format             = m_options.format;
            stagingDesc.SampleDesc.Count   = 1;
            stagingDesc.SampleDesc.Quality = 0;
            stagingDesc.MipLevels          = 1;
            stagingDesc.MiscFlags          = 0;
            stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
            stagingDesc.Usage              = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags          = 0;
            THROW(m_stagingDevice->CreateTexture2D(&stagingDesc, nullptr, m_stagingFrame.put()), "Unable to create staging texture");
        }

        // copy captured frame to staging frame (because capturedFrame wont't have CPU_ACCESS_READ)
        CopyTrimToOutputSize(m_stagingContext.get(), m_stagingFrame.get(), capturedFrame.get(), width, height);

        m_stagingCopyRequired = true;
    }
    else
    {
        // same adapter, just copy directly
        CopyTrimToOutputSize(m_outputContext.get(), outputTexture.get(), capturedFrame.get(), width, height);
    }
}

void CaptureBase::CopyStagingToOutput(const winrt::com_ptr<ID3D11Texture2D>& outputTexture)
{
    m_stagingCopyRequired = false;

    // map both textures to CPU memory and copy between
    // we can do this because we are polling so render frame is not used by another thread
    D3D11_MAPPED_SUBRESOURCE stagingResource;
    THROW(m_stagingContext->Map(m_stagingFrame.get(), 0, D3D11_MAP_READ, 0, &stagingResource), "Unable to map staging texture");

    D3D11_MAPPED_SUBRESOURCE renderResource;
    THROW(m_outputContext->Map(outputTexture.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &renderResource), "Unable to map render texture");

    if(stagingResource.DepthPitch == renderResource.DepthPitch)
    {
        // copy between mapped textures
        memcpy(renderResource.pData, stagingResource.pData, stagingResource.DepthPitch);
    }

    m_stagingContext->Unmap(m_stagingFrame.get(), 0);
    m_outputContext->Unmap(outputTexture.get(), 0);
}

void CaptureBase::CopyTrimToOutputSize(ID3D11DeviceContext* context, ID3D11Texture2D* output, ID3D11Texture2D* source, int width, int height)
{
    D3D11_TEXTURE2D_DESC outDesc {};
    output->GetDesc(&outDesc); // (3D "Full" modes capture into a texture twice as large as the shader display)
    if(width == (int)outDesc.Width && height == (int)outDesc.Height && m_windowX == 0 && m_windowY == 0)
    {
        context->CopyResource(output, source);
        return;
    }

    const bool sameAdapterOutput = context == m_outputContext.get();
    if(sameAdapterOutput && m_windowX == 0 && m_windowY == 0 && (outDesc.BindFlags & D3D11_BIND_RENDER_TARGET))
    {
        try
        {
            auto& S = g_scaler;
            winrt::com_ptr<ID3D11Device> dev;
            context->GetDevice(dev.put());
            if(S.device.get() != dev.get())
            {
                S = Scaler {};
                S.device = dev;
            }
            if(!S.ps)
            {
                winrt::com_ptr<ID3DBlob> vsb, psb;
                std::string              errors;
                if(FAILED(Helpers::CompileCached(kScaleHlsl, sizeof(kScaleHlsl) - 1, "CaptureScale", nullptr, "VSmain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, vsb, errors))
                   || FAILED(Helpers::CompileCached(kScaleHlsl, sizeof(kScaleHlsl) - 1, "CaptureScale", nullptr, "PSmain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, psb, errors)))
                    throw std::runtime_error("Unable to compile the scaling shader: " + errors);
                THROW(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, S.vs.put()), "Unable to create scaling vertex shader");
                THROW(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, S.ps.put()), "Unable to create scaling pixel shader");
                D3D11_BUFFER_DESC cbd {};
                cbd.ByteWidth      = 48;
                cbd.Usage          = D3D11_USAGE_DYNAMIC;
                cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
                cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                THROW(dev->CreateBuffer(&cbd, nullptr, S.cb.put()), "Unable to create scaling constant buffer");
                D3D11_SAMPLER_DESC sd {};
                sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.MaxLOD         = D3D11_FLOAT32_MAX;
                THROW(dev->CreateSamplerState(&sd, S.samp.put()), "Unable to create scaling sampler");
            }
            D3D11_TEXTURE2D_DESC sd {};
            source->GetDesc(&sd);
            if(!S.srcCopy || S.srcW != sd.Width || S.srcH != sd.Height || S.srcFmt != sd.Format)
            {
                S.srcSrv  = nullptr;
                S.srcCopy = nullptr;
                sd.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
                sd.MiscFlags      = 0;
                sd.CPUAccessFlags = 0;
                sd.Usage          = D3D11_USAGE_DEFAULT;
                sd.MipLevels      = 1;
                THROW(dev->CreateTexture2D(&sd, nullptr, S.srcCopy.put()), "Unable to create scaling source texture");
                THROW(dev->CreateShaderResourceView(S.srcCopy.get(), nullptr, S.srcSrv.put()), "Unable to create scaling source view");
                S.srcW = sd.Width;
                S.srcH = sd.Height;
                S.srcFmt = sd.Format;
            }
            if(S.rtvs.size() > 16)
                S.rtvs.clear();
            auto& rtvRef = S.rtvs[output];
            if(!rtvRef)
                THROW(dev->CreateRenderTargetView(output, nullptr, rtvRef.put()), "Unable to create scaling target");
            context->CopySubresourceRegion(S.srcCopy.get(), 0, 0, 0, 0, source, 0, nullptr);

            // fitted rectangle (aspect ratio kept)
            const float sw = (float)width, sh = (float)height, dw = (float)outDesc.Width, dh = (float)outDesc.Height;
            const float k  = (std::min)(dw / sw, dh / sh);
            const float fw = (float)(int)(sw * k + 0.5f), fh = (float)(int)(sh * k + 0.5f);
            const float fx = (float)(int)((dw - fw) * 0.5f), fy = (float)(int)((dh - fh) * 0.5f);

            D3D11_MAPPED_SUBRESOURCE mapped {};
            if(SUCCEEDED(context->Map(S.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                const float tx = (std::min)(4.0f, (std::max)(1.0f, ceilf(sw / fw)));
                const float ty = (std::min)(4.0f, (std::max)(1.0f, ceilf(sh / fh)));
                const bool  srgbOut = outDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || outDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                // pixel position inside the viewport: SV_Position is in render-target pixels, so shift by the fitted origin
                const float v[12] = { sw, sh, fw, fh, tx, ty, srgbOut ? 1.0f : 0.0f, 0.0f, fx, fy, 0.0f, 0.0f };
                memcpy(mapped.pData, v, sizeof(v));
                context->Unmap(S.cb.get(), 0);
            }
            ID3D11RenderTargetView* rtv = rtvRef.get();
            float                   black[4] = { 0, 0, 0, 1 };
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->ClearRenderTargetView(rtv, black);
            D3D11_VIEWPORT vp = { fx, fy, fw, fh, 0.0f, 1.0f };
            context->RSSetViewports(1, &vp);
            context->RSSetState(nullptr);
            context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(S.vs.get(), nullptr, 0);
            context->PSSetShader(S.ps.get(), nullptr, 0);
            ID3D11Buffer* cb = S.cb.get();
            context->PSSetConstantBuffers(0, 1, &cb);
            ID3D11SamplerState* smp = S.samp.get();
            context->PSSetSamplers(0, 1, &smp);
            ID3D11ShaderResourceView* srv = S.srcSrv.get();
            context->PSSetShaderResources(0, 1, &srv);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            context->PSSetShaderResources(0, 1, &nullSrv);
            ID3D11RenderTargetView* nullRtv = nullptr;
            context->OMSetRenderTargets(1, &nullRtv, nullptr);
            return;
        }
        catch(...)
        {
            // fall back to the plain copy below
        }
    }

    // truncate (window capture offset, cross adapter)
    D3D11_BOX box;
    box.left   = 0;
    box.top    = 0;
    box.right  = min(outDesc.Width - m_windowX, (unsigned)width);
    box.bottom = min(outDesc.Height - m_windowY, (unsigned)height);
    box.front  = 0;
    box.back   = 1;
    context->CopySubresourceRegion(output, 0, m_windowX, m_windowY, 0, source, 0, &box);
}

} // namespace ShaderBeam