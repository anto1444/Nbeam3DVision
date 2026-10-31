/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"
#include "Renderer.h"
#include "Helpers.h"
#include "CaptureBase.h"

namespace ShaderBeam
{

static double NowMs()
{
    static const double freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq;
}

Renderer::Renderer(Options& options, UI& ui, Watcher& watcher, ShaderManager& shaderManager) :
    m_options(options), m_ui(ui), m_watcher(watcher), m_shaderManager(shaderManager), m_charts(m_options, m_watcher), m_renderContext(m_options)
{ }

void Renderer::Start(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context)
{
    m_deviceLost = false;
    m_renderContext.device        = device;
    m_renderContext.deviceContext = context;

    Create();
    m_charts.Create(m_renderContext.device, m_renderContext.deviceContext);
    m_shaderManager.Create(m_renderContext, m_options.shaderProfileNo);
    CreateInputs();
    m_appliedSubFrames = m_options.subFrames;

    ResetModeState();

    StartEmitters();
}

void Renderer::ResetModeState()
{
    m_mode = m_options.EffectiveSubFramesMode();
    m_auto.Reset();
    m_bypass     = false;
    m_ui.m_lowFps = false;
    m_ui.m_lowBypass = false;
    m_ui.m_flickerHz = 0;
    m_ui.m_gameFps              = 0;
}

// Shader type, 3D source or emitter settings changed: rebuild what depends on them without restarting the renderer, the capture,
// the swap chain or the HUD (a restart makes the picture stutter).
void Renderer::ReloadLive()
{
    m_emitters.Stop();
    WaitTillIdle();
    m_shaderManager.Destroy();
    DestroyInputs();
    m_shaderManager.Create(m_renderContext, m_options.shaderProfileNo);
    CreateInputs();
    if(m_options.EffectiveSubFramesMode() == SF_MANUAL)
        m_options.subFrames = m_options.ManualSubFrames();
    m_appliedSubFrames         = m_options.subFrames;
    m_renderContext.frameNo    = 0;
    m_renderContext.subFrameNo = 0;
    ResetModeState();
    StartEmitters();
    m_ui.ShowBanner();
}

void Renderer::ApplyLiveChanges()
{
    if(m_options.liveLatency)
    {
        m_options.liveLatency = false;
        try
        {
            m_renderContext.device.as<IDXGIDevice1>()->SetMaximumFrameLatency(m_options.Tearing() ? 1 : m_options.maxQueuedFrames);
        }
        catch(...)
        { }
    }
    if(m_options.liveEmitters && !m_options.liveReload)
    {
        m_options.liveEmitters = false;
        m_emitters.Stop();
        StartEmitters();
        m_ui.ShowBanner();
    }
    if(m_options.liveReload)
    {
        m_options.liveReload   = false;
        m_options.liveEmitters = false;
        try
        {
            ReloadLive();
        }
        catch(std::exception& ex)
        {
            m_ui.SetError(ex.what());
        }
    }
}

void Renderer::StartEmitters()
{
    if(!m_options.StereoAllowed())
    {
        // 3D only exists in Manual SubFrames mode
        m_emitters.Stop();
        m_ui.m_emitterStatus.clear();
        return;
    }
    EmitterEnv env;
    env.outputWindow = m_options.outputWindow;
    env.width        = m_options.outputWidth;
    env.height       = m_options.outputHeight;
    env.vsyncRateHz  = m_options.vsyncRate;
    env.subFrames    = m_options.subFrames;
    env.swapChain    = m_swapChain.get();
    env.queuedFrames = m_options.Tearing() ? 0 : (m_options.maxQueuedFrames > 0 ? m_options.maxQueuedFrames : 3); // 0 = driver default (3)
    try
    {
        winrt::com_ptr<IDXGIAdapter> adapter;
        if(SUCCEEDED(m_renderContext.device.as<IDXGIDevice>()->GetAdapter(adapter.put())))
        {
            DXGI_ADAPTER_DESC desc {};
            if(SUCCEEDED(adapter->GetDesc(&desc)))
                env.adapterLuid = desc.AdapterLuid;
        }
    }
    catch(...)
    { }
    m_emitters.Start(&m_options.emitter, env);
    m_ui.m_emitterStatus = m_emitters.Status();
}

void Renderer::Stop()
{
    m_emitters.Stop();
    m_shaderManager.Destroy();
    m_charts.Destroy();
    Destroy();
}

void Renderer::ApplySubFramesLive()
{
    // SubFrames edited in the UI: switch without recreating devices / capture / swap chain (no hitch)
    m_appliedSubFrames         = m_options.subFrames;
    m_renderContext.frameNo    = 0;
    m_renderContext.subFrameNo = 0;
    m_shaderManager.SubFramesChanged(m_renderContext);
    m_emitters.SetSubFrames(m_options.subFrames);
}

void Renderer::Render(bool present, bool ui)
{
    if(m_options.subFrames != m_appliedSubFrames && m_options.subFrames > 0)
        ApplySubFramesLive();

    // eye of the content frame being rendered (before the frame counters advance)
    const int  emitFrameNo    = m_renderContext.frameNo;
    const int  emitSubFrameNo = m_renderContext.subFrameNo;
    const bool emit           = present && m_emitters.Active() && !EffectsHidden();
    Eye        eye            = Eye::Left;
    if(emit)
        eye = m_emitters.NextEye(emitFrameNo, emitSubFrameNo);

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_options.outputWidth, (float)m_options.outputHeight };
    m_renderContext.deviceContext->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* targets[1] = { m_renderContext.outputTargetView.get() };
    m_renderContext.deviceContext->OMSetRenderTargets(1, targets, NULL);

    {
        // split-screen can be changed live
        D3D11_RECT scissor {};
        scissor.right  = m_options.splitScreen == 1 ? m_options.outputWidth / 2 : m_options.outputWidth;
        scissor.bottom = m_options.splitScreen == 2 ? m_options.outputHeight / 2 : m_options.outputHeight;
        m_renderContext.deviceContext->RSSetScissorRects(1, &scissor);
    }

    // one subframe per content frame: nothing to simulate, the picture goes through untouched (no gamma / gain / blur of the shader)
    if(Bypassing() || (m_options.subFrames <= 1 && !m_stereoActive))
    {
        m_renderContext.deviceContext->CopyResource(m_renderContext.outputTexture.get(), (m_stereoActive && EffectsOff() ? m_stereoRaw : m_renderContext.inputTextures[m_renderContext.inputSlots[0]]).get());
    }
    else
        m_shaderManager.Render(m_renderContext);

    if(m_options.splitScreen)
    {
        D3D11_BOX box {};
        UINT      destX = m_options.splitScreen == 1 ? m_options.outputWidth / 2 : 0;
        UINT      destY = m_options.splitScreen == 2 ? m_options.outputHeight / 2 : 0;
        box.left        = destX;
        box.top         = destY;
        box.right       = m_options.outputWidth;
        box.bottom      = m_options.outputHeight;
        box.front       = 0;
        box.back        = 1;
        m_renderContext.deviceContext->CopySubresourceRegion(
            m_renderContext.outputTexture.get(), 0, destX, destY, 0, m_renderContext.inputTextures[m_renderContext.inputSlots[0]].get(), 0, &box);
    }

    ApplyCaptureTone();

    if(ui && m_options.charts)
        m_charts.Render(m_renderContext.outputTexture, m_options.outputHeight);

    if(ui && m_ui.RenderRequired())
        RenderHud();
    else
        m_hudLast = {}; // shown again: drawn at once

    if(emit)
    {
        // on-screen emitters go on top of everything, including the UI
        EmitterFrame frame;
        frame.eye           = eye;
        frame.frameNo       = emitFrameNo;
        frame.subFrameNo    = emitSubFrameNo;
        frame.deviceContext = m_renderContext.deviceContext.get();
        frame.outputView    = m_renderContext.outputTargetView.get();
        frame.width         = m_options.outputWidth;
        frame.height        = m_options.outputHeight;
        m_emitters.DrawOverlay(frame);
    }

    ID3D11RenderTargetView* null[] = { nullptr };
    m_renderContext.deviceContext->OMSetRenderTargets(1, null, NULL);

    m_renderContext.subFrameNo++;
    if(m_renderContext.subFrameNo == m_options.subFrames)
    {
        m_renderContext.frameNo++;
        m_renderContext.subFrameNo = 0;
    }

    // beam racing: where the tear may land in this frame (CRT bands); full-frame effects, 3D, split screen and on-screen emitters: vblank
    m_tearSafe = 0.0f;
    m_beamOk   = m_tearing && m_options.beamRacing && !m_stereoActive && !m_emitters.Active();
    if(m_tearing && m_options.beamRacing && (m_stereoActive || m_emitters.Active()))
        m_options.beamRacingState = 2; // 3D: not used
    if(m_beamOk && !emit && !m_options.splitScreen && !Bypassing() && m_options.subFrames > 1)
        m_tearSafe = m_shaderManager.TearSafeLimit(m_renderContext);

    if(present)
    {
        Present(true);
        if(emit)
            m_emitters.AfterPresent(eye, emitSubFrameNo);
    }
    m_watcher.FrameSubmitted();
}

// HUD: ImGui draws into a transparent texture (premultiplied alpha) 30 times a second (full rate while it is dragged); every frame the texture is laid over the
// picture with one full-screen draw. Falls back to drawing the HUD straight into the picture if the texture cannot be created.
void Renderer::RenderHud()
{
    auto* dev = m_renderContext.device.get();
    auto* ctx = m_renderContext.deviceContext.get();
    if(!m_hudFailed)
    {
        try
        {
            if(!m_hudPs)
            {
                static const char hlsl[] = R"HLSL(
Texture2D<float4> hud : register(t0);
float4 VSmain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PShud(float4 pos : SV_Position) : SV_Target
{
    return hud.Load(int3(int2(pos.xy), 0));
}
)HLSL";
                winrt::com_ptr<ID3DBlob> vs, ps;
                std::string              errors;
                if(FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "HudOverlay", nullptr, "VSmain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, vs, errors))
                   || FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "HudOverlay", nullptr, "PShud", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, ps, errors)))
                    throw std::runtime_error("HUD overlay shader");
                THROW(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_hudVs.put()), "HUD overlay vertex shader");
                THROW(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_hudPs.put()), "HUD overlay pixel shader");
                D3D11_BLEND_DESC bd {};
                bd.RenderTarget[0].BlendEnable           = TRUE;
                bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE; // premultiplied
                bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                THROW(dev->CreateBlendState(&bd, m_hudBlend.put()), "HUD overlay blend state");
            }
            D3D11_TEXTURE2D_DESC od {};
            m_renderContext.outputTexture->GetDesc(&od);
            if(!m_hudTex || m_hudW != od.Width || m_hudH != od.Height)
            {
                m_hudSrv = nullptr;
                m_hudRtv = nullptr;
                m_hudTex = nullptr;
                D3D11_TEXTURE2D_DESC td {};
                td.Width            = od.Width;
                td.Height           = od.Height;
                td.MipLevels        = 1;
                td.ArraySize        = 1;
                td.Format           = m_options.format; // same as the HUD render target of the picture
                td.SampleDesc.Count = 1;
                td.Usage            = D3D11_USAGE_DEFAULT;
                td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                THROW(dev->CreateTexture2D(&td, nullptr, m_hudTex.put()), "HUD texture");
                THROW(dev->CreateRenderTargetView(m_hudTex.get(), nullptr, m_hudRtv.put()), "HUD texture target");
                THROW(dev->CreateShaderResourceView(m_hudTex.get(), nullptr, m_hudSrv.put()), "HUD texture view");
                m_hudW    = od.Width;
                m_hudH    = od.Height;
                m_hudLast = {};
            }
        }
        catch(...)
        {
            m_hudFailed = true;
        }
    }
    if(m_hudFailed)
    {
        ID3D11RenderTargetView* t = m_uiTargetView.get();
        ctx->OMSetRenderTargets(1, &t, nullptr);
        m_ui.Render();
        return;
    }

    // 1. the HUD itself, 30 times a second (every frame while a HUD window is dragged, so it follows the mouse smoothly)
    const auto now = std::chrono::steady_clock::now();
    if(m_hudLast.time_since_epoch().count() == 0 || m_ui.HudMoving() || now - m_hudLast >= std::chrono::microseconds(33300))
    {
        m_hudLast             = now;
        const float clear[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->ClearRenderTargetView(m_hudRtv.get(), clear);
        ID3D11RenderTargetView* t = m_hudRtv.get();
        ctx->OMSetRenderTargets(1, &t, nullptr);
        m_ui.Render();
        ID3D11RenderTargetView* none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);
        RECT r {};
        m_hudEmpty = !m_ui.HudBounds(r);
        if(!m_hudEmpty)
        {
            // the area of the HUD windows of this redraw (the texture is transparent everywhere else)
            m_hudArea.left   = std::clamp<LONG>(r.left, 0, (LONG)m_hudW);
            m_hudArea.top    = std::clamp<LONG>(r.top, 0, (LONG)m_hudH);
            m_hudArea.right  = std::clamp<LONG>(r.right, 0, (LONG)m_hudW);
            m_hudArea.bottom = std::clamp<LONG>(r.bottom, 0, (LONG)m_hudH);
        }
    }
    if(m_hudEmpty || m_hudArea.right <= m_hudArea.left || m_hudArea.bottom <= m_hudArea.top)
        return; // nothing to lay over

    // 2. laid over the picture every frame, only where the HUD windows are (the rest of the texture is transparent)
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_hudW, (float)m_hudH, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &m_hudArea);
    ctx->RSSetState(m_rasterizerState.get());
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_hudVs.get(), nullptr, 0);
    ctx->PSSetShader(m_hudPs.get(), nullptr, 0);
    ID3D11RenderTargetView* t = m_uiTargetView.get();
    ctx->OMSetRenderTargets(1, &t, nullptr);
    ctx->OMSetBlendState(m_hudBlend.get(), nullptr, 0xffffffff);
    ID3D11ShaderResourceView* srv = m_hudSrv.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    D3D11_RECT full { 0, 0, (LONG)m_hudW, (LONG)m_hudH };
    ctx->RSSetScissorRects(1, &full); // the on-screen emitter overlay drawn next covers the whole picture
}

// Windows HDR only: black level / grey level of the captured picture, applied right after the effects, before the charts, the HUD
// and the emitter overlay. Pure black stays pure black (0 -> 0), so BFI black frames stay black. The picture is copied once and a
// pixel shader writes the corrected result.
void Renderer::ApplyCaptureTone()
{
    const float black = m_options.captureBlack;
    const float grey  = m_options.captureGrey;
    const float gain  = m_options.captureGain * m_uniformGain; // (Auto: Uniform brightness)
    const bool  tone  = black > 0.0005f || fabsf(grey - 1.0f) > 0.005f || fabsf(gain - 1.0f) > 0.0005f;
    if(!tone)
        return;
    auto* dev = m_renderContext.device.get();
    auto* ctx = m_renderContext.deviceContext.get();
    if(!m_tonePs)
    {
        static const char hlsl[] = R"HLSL(
cbuffer Params : register(b0) { float4 p; }; // x = gain, y = black level, z = grey (gamma)
Texture2D<float4> src : register(t0);
float4 VSmain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PStone(float4 pos : SV_Position) : SV_Target
{
    float4 c = src.Load(int3(int2(pos.xy), 0));
    float3 x = max(c.rgb - p.y, 0.0);
    float3 lo = pow(min(x, 1.0), p.z);
    x = x <= 1.0 ? lo : x;
    return float4(x * p.x, c.a);
}
)HLSL";
        winrt::com_ptr<ID3DBlob> vs, pt;
        std::string              errors;
        if(FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "CaptureTone", nullptr, "VSmain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, vs, errors))
           || FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "CaptureTone", nullptr, "PStone", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, pt, errors)))
            throw std::runtime_error("Unable to compile the capture tone shader: " + errors);
        THROW(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_toneVs.put()), "Unable to create capture tone vertex shader");
        THROW(dev->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, m_tonePs.put()), "Unable to create capture tone pixel shader");
        D3D11_BUFFER_DESC cbd {};
        cbd.ByteWidth      = 16;
        cbd.Usage          = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        THROW(dev->CreateBuffer(&cbd, nullptr, m_toneCb.put()), "Unable to create capture tone constant buffer");
    }
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_options.outputWidth, (float)m_options.outputHeight, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_toneVs.get(), nullptr, 0);
    D3D11_TEXTURE2D_DESC td {};
    m_renderContext.outputTexture->GetDesc(&td);
    if(!m_toneCopy || m_toneW != td.Width || m_toneH != td.Height)
    {
        m_toneCopySrv = nullptr;
        m_toneCopy    = nullptr;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags  = 0;
        THROW(dev->CreateTexture2D(&td, nullptr, m_toneCopy.put()), "Unable to create capture tone texture");
        THROW(dev->CreateShaderResourceView(m_toneCopy.get(), nullptr, m_toneCopySrv.put()), "Unable to create capture tone view");
        m_toneW = td.Width;
        m_toneH = td.Height;
    }
    ctx->CopyResource(m_toneCopy.get(), m_renderContext.outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if(SUCCEEDED(ctx->Map(m_toneCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const float v[4] = { gain, black, grey, 0.0f };
        memcpy(mapped.pData, v, sizeof(v));
        ctx->Unmap(m_toneCb.get(), 0);
    }
    ID3D11RenderTargetView* rtv = m_renderContext.outputTargetView.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->PSSetShader(m_tonePs.get(), nullptr, 0);
    ID3D11Buffer* cb = m_toneCb.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = m_toneCopySrv.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
}

// ---- beam racing ------------------------------------------------------------------------------------------------------------
// With tearing (queue 0) the flip happens at once, wherever the scanout is: the tear lands there. Beam racing reads the real scanout
// line (D3DKMTGetScanLine, gdi32) and sends the flip only when the tear will be invisible for the current effect:
//  - BFI, 3D, plain picture: in the vertical blank (the flip is sent `beamLead` % of a refresh before it, to make up for its delay);
//  - CRT Beam Simulator (vertical scan): anywhere above the bands lit by the previous and the new frame (lower latency), else the vblank.
// One flip per refresh. If the scanout cannot be read, the normal tearing path is used.
namespace
{
struct KmtOpenFromHdc
{
    HDC      hDc;
    unsigned hAdapter;
    LUID     AdapterLuid;
    unsigned VidPnSourceId;
};
struct KmtGetScanLine
{
    unsigned hAdapter;
    unsigned VidPnSourceId;
    BOOLEAN  InVerticalBlank;
    unsigned ScanLine;
};
struct KmtClose
{
    unsigned hAdapter;
};
typedef LONG(APIENTRY* PfnKmtOpen)(KmtOpenFromHdc*);
typedef LONG(APIENTRY* PfnKmtScan)(KmtGetScanLine*);
typedef LONG(APIENTRY* PfnKmtClose)(KmtClose*);
PfnKmtOpen  s_kmtOpen  = nullptr;
PfnKmtScan  s_kmtScan  = nullptr;
PfnKmtClose s_kmtClose = nullptr;
bool LoadKmt()
{
    static bool loaded = false;
    if(!loaded)
    {
        loaded = true;
        if(HMODULE g = GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll") : LoadLibraryW(L"gdi32.dll"))
        {
            s_kmtOpen  = (PfnKmtOpen)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc");
            s_kmtScan  = (PfnKmtScan)GetProcAddress(g, "D3DKMTGetScanLine");
            s_kmtClose = (PfnKmtClose)GetProcAddress(g, "D3DKMTCloseAdapter");
        }
    }
    return s_kmtOpen && s_kmtScan && s_kmtClose;
}
} // namespace

void Renderer::OpenScanline()
{
    CloseScanline();
    m_options.beamRacingState = -1;
    if(!m_tearOutput || !LoadKmt())
        return;
    DXGI_OUTPUT_DESC od {};
    if(FAILED(m_tearOutput->GetDesc(&od)))
        return;
    DEVMODEW dm {};
    dm.dmSize = sizeof(dm);
    if(!EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) || dm.dmPelsHeight == 0)
        return;
    HDC dc = CreateDCW(od.DeviceName, nullptr, nullptr, nullptr);
    if(!dc)
        return;
    KmtOpenFromHdc o {};
    o.hDc = dc;
    if(s_kmtOpen(&o) == 0)
    {
        m_kmtAdapter = o.hAdapter;
        m_kmtSource  = o.VidPnSourceId;
        m_kmtLines   = (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270) ? dm.dmPelsWidth : dm.dmPelsHeight;
        KmtGetScanLine t {};
        t.hAdapter      = m_kmtAdapter;
        t.VidPnSourceId = m_kmtSource;
        if(s_kmtScan(&t) == 0)
            m_options.beamRacingState = 1;
        else
            CloseScanline();
    }
    DeleteDC(dc);
    D3D11_QUERY_DESC qd { D3D11_QUERY_EVENT, 0 };
    m_gpuDone = nullptr;
    m_renderContext.device->CreateQuery(&qd, m_gpuDone.put());
    m_brLastF      = 1.0;
    m_brLastVbl    = true;
    m_brNewRefresh = true;
}

void Renderer::CloseScanline()
{
    if(m_kmtAdapter && s_kmtClose)
    {
        KmtClose c { m_kmtAdapter };
        s_kmtClose(&c);
    }
    m_kmtAdapter = 0;
    m_gpuDone    = nullptr;
}

bool Renderer::BeamRacePresent()
{
    if(!m_kmtAdapter || !m_gpuDone || !m_kmtLines)
        return false;
    auto* ctx = m_renderContext.deviceContext.get();
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t0, t;
    auto elapsedMs = [&] { QueryPerformanceCounter(&t); return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart; };

    // 1. the frame is fully drawn (otherwise the flip waits for the GPU and lands anywhere)
    ctx->End(m_gpuDone.get());
    ctx->Flush();
    QueryPerformanceCounter(&t0);
    while(ctx->GetData(m_gpuDone.get(), nullptr, 0, 0) == S_FALSE && elapsedMs() < 100.0)
        YieldProcessor();

    // 2. wait for a place where the tear does not show, in a new refresh
    const double lead  = std::clamp(m_options.beamLead, 0, 20) / 100.0;
    const double safe  = m_tearSafe;
    const double limit = (std::max)(2.0, m_options.vsyncDuration * 2.5);
    KmtGetScanLine s {};
    s.hAdapter      = m_kmtAdapter;
    s.VidPnSourceId = m_kmtSource;
    double f        = 1.0;
    bool   vbl      = true;
    QueryPerformanceCounter(&t0);
    for(;;)
    {
        if(s_kmtScan(&s) != 0)
        {
            m_options.beamRacingState = -1;
            return false; // nothing sent yet: the normal path presents this frame
        }
        vbl = s.InVerticalBlank != 0;
        f   = vbl ? 1.0 : (std::min)(1.0, s.ScanLine / (double)m_kmtLines);
        if(!m_brNewRefresh && ((vbl && !m_brLastVbl) || (!vbl && f < m_brLastF - 0.005)))
            m_brNewRefresh = true; // the scanout went through a vblank since the last flip
        if(m_brNewRefresh && (vbl || f >= 1.0 - lead || f + lead <= safe))
            break;
        if(elapsedMs() > limit)
            break;
        YieldProcessor();
    }
    DXGI_PRESENT_PARAMETERS pp {};
    CheckPresent(m_swapChain->Present1(0, DXGI_PRESENT_ALLOW_TEARING, &pp));
    const bool atEnd = vbl || f >= 1.0 - lead;
    m_brLastF        = atEnd ? 1.0 : f;
    m_brLastVbl      = atEnd;
    m_brNewRefresh   = false;
    m_options.beamRacingState = 1;
    return true;
}

void Renderer::Present(bool vsync)
{
    DXGI_PRESENT_PARAMETERS pp {};
    if(vsync && m_tearing && m_options.beamRacing && m_beamOk && BeamRacePresent())
        return;
    if(vsync && m_tearing)
    {
        // Tearing allowed: no queue, the flip happens at once. The thread is paced on the vblank; "tearing control" delays the flip so the
        // tear lands lower in the picture (0 = right after vblank: at the very top, hidden)
        if(m_tearOutput)
            m_tearOutput->WaitForVBlank();
        const int line = std::clamp(m_options.tearingLine, 0, 100);
        if(line > 0)
        {
            static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
            LARGE_INTEGER        t0, t;
            QueryPerformanceCounter(&t0);
            const double waitMs = m_options.vsyncDuration * line / 100.0 * 0.95; // never reach the next vblank
            do
            {
                QueryPerformanceCounter(&t);
            } while((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart < waitMs);
        }
        CheckPresent(m_swapChain->Present1(0, DXGI_PRESENT_ALLOW_TEARING, &pp));
        return;
    }
    CheckPresent(m_swapChain->Present1(vsync ? 1 : 0, 0, &pp));
}

// The graphics card was lost (driver update or driver crash): every Present fails at once, the render thread would spin at 100 % CPU on a
// frozen picture. Noted here; the render thread then asks for a restart (new devices) once.
void Renderer::CheckPresent(HRESULT hr)
{
    if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        m_deviceLost = true;
}

// Auto SubFrames: every second the measured game frame rate gives SubFrames = round(display Hz / fps); Render() applies it live.
// Below the low fps threshold the BFI is doubled (or disabled), see LowFpsSettings.
void Renderer::UpdateSubFramesMode()
{
    const int effective = m_options.EffectiveSubFramesMode();
    if(effective == SF_MANUAL && m_mode == SF_MANUAL && m_options.emitter.sourceFormat != StereoSource::Off)
    {
        // 3D: SubFrames follows the emitter rate (live, also when the display rate changes)
        const int n = m_options.StereoSubFrames();
        if(n > 0 && n != m_options.subFrames)
            m_options.subFrames = n;
    }
    if(effective != m_mode)
    {
        // Manual <-> Auto switched live
        ResetModeState();
        if(effective == SF_MANUAL)
            m_options.subFrames = m_options.ManualSubFrames();
    }
    if(m_mode != SF_AUTO)
    {
        m_uniformGain = 1.0f;
        return;
    }
    const auto& low     = m_options.lowFps;
    const int   current = m_options.subFrames;
    const int   next    = m_auto.Update(NowMs(), m_watcher.TotalInputFrames(), m_options.vsyncRate, current, low, m_options.autoSub.intervalSec);
    m_bypass            = m_auto.Bypass(low);
    m_ui.m_gameFps      = m_auto.GameFps();
    m_ui.m_lowFps       = m_auto.LowFps();
    m_ui.m_lowBypass    = m_bypass;
    m_ui.m_flickerHz    = m_bypass ? m_auto.GameFps() : (float)(m_options.vsyncRate / (std::max)(1, m_options.subFrames));
    if(next != current && next >= MIN_SUBFRAMES && next <= MAX_SUBFRAMES)
        m_options.subFrames = next;

    // Uniform brightness (Simple BFI): the brightness of the BFI is (clear + dark x (1 - gain vs blur)) / SubFrames, so it changes with
    // the SubFrames Auto picks. The picture is kept at the brightness the BFI has at the low fps threshold (the darkest point of
    // the range: the lower the threshold, the darker), by dimming every other case (more fps, doubled or disabled BFI below it).
    m_uniformGain = 1.0f;
    if(m_options.uniformBrightness && !EffectsOff()) // effects hidden: the picture as it is
    {
        int   clearFrames = -1;
        float darkGain    = 1.0f;
        for(const auto& p : m_shaderManager.GetParameterInfos())
        {
            if(p.type == ParameterType::ParamInt && p.name == "Clear Frames")
                clearFrames = *p.p.ip.value;
            else if(p.type == ParameterType::ParamFloat && p.name == "Gain vs Blur")
                darkGain = *p.p.fp.value;
        }
        if(clearFrames > 0 && m_options.vsyncRate > 1.0f)
        {
            auto brightness = [&](int sf) {
                if(sf <= 1)
                    return 1.0f; // one subframe: the picture as it is
                const int c = (std::min)(clearFrames, sf);
                return ((float)c + (float)(sf - c) * (1.0f - std::clamp(darkGain, 0.0f, 1.0f))) / (float)sf;
            };
            const int   sfLow  = std::clamp((int)std::lround(m_options.vsyncRate / LowFpsGate::Threshold(low)), 1, AutoSubFrames::kMax);
            const float target = brightness(sfLow);
            const float now    = m_bypass ? 1.0f : brightness(m_options.subFrames);
            if(now > 0.0f)
                m_uniformGain = std::clamp(target / now, 0.0f, 1.0f);
        }
    }
}

int Renderer::GetNextSlot() const
{
    // return either a free slot, or the oldest one to receive new input
    bool inputUsed[MAX_INPUTS];
    ZeroMemory(inputUsed, sizeof(inputUsed));
    for(auto slot : m_renderContext.inputSlots)
    {
        inputUsed[slot] = true;
    }
    for(auto t = 0; t < m_renderContext.inputTextures.size(); t++)
    {
        if(!inputUsed[t])
            return t;
    }
    return m_renderContext.inputSlots.back();
}

const winrt::com_ptr<ID3D11Texture2D>& Renderer::GetNextInput() const
{
    if(m_stereoActive)
        return m_stereoRaw; // the capture writes the full 3D frame here; ConvertStereoInput() then extracts one eye
    return m_renderContext.inputTextures[GetNextSlot()];
}

Eye Renderer::StereoImageEye()
{
    // eye of the content frame about to be shown. Swap Eyes is applied to the emitter only (it reverses emitter vs image).
    const int frameNo = m_renderContext.frameNo;
    if(m_emitters.Active() && !EffectsHidden())
        return m_emitters.NextEye(frameNo, 0, false);
    return (((frameNo & 1) != 0) != m_options.emitter.swapEyes) ? Eye::Right : Eye::Left;
}

void Renderer::ConvertStereoInput(bool newFrame)
{
    if(!m_stereoActive)
        return;
    if(newFrame)
        m_stereoHaveRaw = true;
    if(!m_stereoHaveRaw)
        return;
    if(EffectsOff())
        return; // effects suspended: the captured frame is shown as it is, without 3D decoding
    // new frame: goes to the slot RollInput() will make current; no new frame: refresh the current slot with the other eye
    const int slot = newFrame ? GetNextSlotForStereo() : m_renderContext.inputSlots.front();
    if(m_options.emitter.sourceFormat == StereoSource::AiDepth)
        UpdateDepth(newFrame);
    m_stereo.Convert(m_renderContext.deviceContext.get(),
                     m_stereoRawView.get(),
                     m_stereoTargets[slot].get(),
                     m_options.emitter.sourceFormat,
                     StereoImageEye(),
                     m_options.outputWidth,
                     m_options.outputHeight,
                     m_depthGpuSrv.get(),
                     &m_options.emitter.ai);
}

void Renderer::CreateDepthResources()
{
    const auto&  ai   = m_options.emitter.ai;
    const double outW = (std::max)(1u, (unsigned)m_options.outputWidth);
    const double outH = (std::max)(1u, (unsigned)m_options.outputHeight);
    const int    w    = std::clamp(((ai.modelWidth + 7) / 14) * 14, 126, 1008);
    const int    h    = (std::max)(14, (int)std::lround(w * outH / outW / 14.0) * 14);
    m_depthW          = w;
    m_depthH          = h;
    auto* dev         = m_renderContext.device.get();

    D3D11_TEXTURE2D_DESC d {};
    d.Width              = w;
    d.Height             = h;
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count   = 1;
    d.Usage              = D3D11_USAGE_DEFAULT;
    d.BindFlags          = D3D11_BIND_RENDER_TARGET;
    THROW(dev->CreateTexture2D(&d, nullptr, m_depthSmall.put()), "Unable to create AI depth input");
    THROW(dev->CreateRenderTargetView(m_depthSmall.get(), nullptr, m_depthSmallRtv.put()), "Unable to create AI depth input target");
    d.Usage          = D3D11_USAGE_STAGING;
    d.BindFlags      = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    THROW(dev->CreateTexture2D(&d, nullptr, m_depthStaging.put()), "Unable to create AI depth readback texture");

    // depth map seen by the shader: starts flat (0.5) until the first estimation arrives
    std::vector<uint16_t> init((size_t)w * h, 32768);
    D3D11_TEXTURE2D_DESC  g {};
    g.Width              = w;
    g.Height             = h;
    g.MipLevels          = 1;
    g.ArraySize          = 1;
    g.Format             = DXGI_FORMAT_R16_UNORM;
    g.SampleDesc.Count   = 1;
    g.Usage              = D3D11_USAGE_DYNAMIC;
    g.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    g.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA sd {};
    sd.pSysMem     = init.data();
    sd.SysMemPitch = w * sizeof(uint16_t);
    THROW(dev->CreateTexture2D(&g, &sd, m_depthGpu.put()), "Unable to create AI depth map texture");
    THROW(dev->CreateShaderResourceView(m_depthGpu.get(), nullptr, m_depthGpuSrv.put()), "Unable to create AI depth map view");

    m_depthCopyPending = false;
    m_depthModelWidth  = ai.modelWidth;
    m_depthUseGpu      = ai.useGpu;
    m_depth.Start(Options::ExeDir(), w, h, ai.useGpu);
}

void Renderer::DestroyDepthResources()
{
    m_depth.Stop();
    m_depthGpuSrv      = nullptr;
    m_depthGpu         = nullptr;
    m_depthStaging     = nullptr;
    m_depthSmallRtv    = nullptr;
    m_depthSmall       = nullptr;
    m_depthCopyPending = false;
    m_ui.m_aiStatus.clear();
}

void Renderer::UpdateDepth(bool newFrame)
{
    auto*       ctx = m_renderContext.deviceContext.get();
    const auto& ai  = m_options.emitter.ai;
    try
    {
        // a different model size / device needs a new session
        if(m_depthSmall && (ai.modelWidth != m_depthModelWidth || ai.useGpu != m_depthUseGpu))
        {
            DestroyDepthResources();
            CreateDepthResources();
        }
    }
    catch(const std::exception& e)
    {
        DestroyDepthResources();
        m_ui.m_aiStatus = std::string("AI depth: ") + e.what();
        return;
    }
    if(!m_depthSmall)
        return;

    if((m_depthTick++ % 15) == 0)
    {
        m_ui.m_aiStatus  = m_depth.Status();
        m_ui.m_aiInferMs = m_depth.InferMs();
        m_ui.m_aiFps     = m_depth.DepthFps();
        m_ui.m_aiLatencyMs = (float)m_depthLatencyMs;
    }

    // a new depth map from the worker goes to the GPU
    if(m_depth.TryGetDepth(m_depthBuf) && m_depthBuf.size() == (size_t)m_depthW * m_depthH)
    {
        if(m_depthSubmitStamp.time_since_epoch().count() != 0)
        {
            // time between the frame the map was computed from and the map being ready: the delay the image needs to match it
            const double ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_depthSubmitStamp).count();
            m_depthLatencyMs = m_depthLatencyMs <= 0.0 ? ms : m_depthLatencyMs * 0.7 + ms * 0.3;
        }
        D3D11_MAPPED_SUBRESOURCE m {};
        if(SUCCEEDED(ctx->Map(m_depthGpu.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            for(int y = 0; y < m_depthH; y++)
                memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, &m_depthBuf[(size_t)y * m_depthW], (size_t)m_depthW * sizeof(uint16_t));
            ctx->Unmap(m_depthGpu.get(), 0);
        }
    }

    // the reduced frame copied earlier goes to the worker once the GPU has finished with it (never waits)
    if(m_depthCopyPending)
    {
        D3D11_MAPPED_SUBRESOURCE m {};
        const HRESULT            hr = ctx->Map(m_depthStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if(SUCCEEDED(hr))
        {
            DepthParams p;
            p.smoothing = ai.smoothing * 0.01f;
            p.invert    = m_options.emitter.swapEyes; // depth inversion follows Swap Eyes (one setting, no conflict between the two)
            m_depth.Submit((const uint8_t*)m.pData, (int)m.RowPitch, p);
            ctx->Unmap(m_depthStaging.get(), 0);
            m_depthCopyPending = false;
        }
        else if(hr != DXGI_ERROR_WAS_STILL_DRAWING)
            m_depthCopyPending = false;
    }

    // worker idle and a new picture: reduce it and start the readback
    // (at most `depthFps` maps per second)
    const auto now      = std::chrono::steady_clock::now();
    const auto interval = std::chrono::microseconds(1000000 / std::clamp(ai.depthFps, 1, 80));
    const bool fromRing = MapSync() && BufferActive() && !m_ringQueue.empty() && m_ringSrv.size() == m_ring.size() && m_ringTime.size() == m_ring.size();
    if((newFrame || (fromRing && m_ringArrived)) && !m_depthCopyPending && m_depth.WantsFrame() && now - m_depthLastStart >= interval)
    {
        m_depthLastStart   = now;
        m_depthSubmitStamp = fromRing ? m_ringTime[m_ringQueue.back()] : now; // the map describes the picture captured at this time
        m_ringArrived      = false;
        // with the buffer the map is computed from the NEWEST waiting frame: by the time it is ready, the frame shown (the oldest one,
        // about `buffer size` frames older) is the one the map describes
        ID3D11ShaderResourceView* src = m_stereoRawView.get();
        if(fromRing)
            src = m_ringSrv[m_ringQueue.back()].get();
        m_stereo.Downscale(ctx, src, m_depthSmallRtv.get(), m_options.outputWidth, m_options.outputHeight, m_depthW, m_depthH, m_options.hardwareSrgb);
        ctx->CopyResource(m_depthStaging.get(), m_depthSmall.get());
        m_depthCopyPending = true;
    }
}

int Renderer::GetNextSlotForStereo() const
{
    return m_renderContext.inputTextures.size() == 1 ? 0 : GetNextSlot();
}

bool Renderer::NewInputRequired() const
{
    return m_shaderManager.NewInputRequired(m_renderContext);
}

void Renderer::RollInput(bool newFrame)
{
    auto numInputs = m_renderContext.inputSlots.size();
    if(numInputs == 1)
        return;

    // if we didn't get a new frame, duplicate slot of the latest frame we have
    auto latestInput = newFrame ? GetNextSlot() : m_renderContext.inputSlots.front();

    // roll input slots and add latest to front
    for(auto i = numInputs - 1; i > 0; i--)
        m_renderContext.inputSlots[i] = m_renderContext.inputSlots[i - 1];
    m_renderContext.inputSlots[0] = latestInput;
}

bool Renderer::MapSync() const
{
    return m_stereoActive && m_options.emitter.sourceFormat == StereoSource::AiDepth && m_options.emitter.ai.mapBuffer;
}

bool Renderer::BufferActive() const
{
    return (m_options.EffectiveFrameBuffer() > 0 || MapSync()) && m_options.captureWindow && !m_options.crossAdapter && !Bypassing() && !m_renderContext.inputTextures.empty();
}

void Renderer::DestroyRing()
{
    m_ringSrv.clear();
    m_ringTime.clear();
    m_ring.clear();
    m_ringQueue.clear();
    m_ringFree.clear();
    m_ringPrimed = false;
    m_ringSkip   = 0;
}

int Renderer::FrameBufferLimit() const
{
    if(!MapSync())
        return 5;
    const auto& tex = m_stereoActive ? m_stereoRaw : (m_renderContext.inputTextures.empty() ? m_stereoRaw : m_renderContext.inputTextures[0]);
    if(!tex)
        return 5;
    D3D11_TEXTURE2D_DESC d {};
    tex->GetDesc(&d);
    const size_t bytes = (size_t)d.Width * d.Height * (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
    return std::clamp((int)((512ull * 1024 * 1024) / (std::max)(bytes, (size_t)1)) - 3, 5, 30);
}

void Renderer::EnsureRing()
{
    const int n    = MapSync() ? FrameBufferLimit() : (m_options.FrameBufferIsAuto() ? 5 : std::clamp(m_options.frameBuffer, 1, 5)); // Auto: room for the largest size, no re-creation when it changes
    const int want = n + 3;
    if((int)m_ring.size() == want)
        return;
    DestroyRing();
    D3D11_TEXTURE2D_DESC d {};
    (m_stereoActive ? m_stereoRaw : m_renderContext.inputTextures[0])->GetDesc(&d);
    d.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.Usage          = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.MiscFlags      = 0;
    for(int i = 0; i < want; i++)
    {
        auto& t = m_ring.emplace_back(nullptr);
        THROW(m_renderContext.device->CreateTexture2D(&d, nullptr, t.put()), "Unable to create buffer texture");
        auto& v = m_ringSrv.emplace_back(nullptr);
        THROW(m_renderContext.device->CreateShaderResourceView(t.get(), nullptr, v.put()), "Unable to create buffer view");
        m_ringFree.push_back(i);
    }
    m_ringTime.assign(m_ring.size(), {});
}

// Adaptive size of the anti-stutter buffer (Auto, 0 to 5): the Watcher gives the gaps between the production times of the game frames.
// The buffer must bridge the worst gap (beyond the average one) of the last 4 seconds, in content frames. It grows at once (one step per
// 100 ms, immediately after a stall) and shrinks by one after 2 quiet seconds (4 seconds from 1 down to 0, where the buffer is
// switched off), so the size does not hunt up and down.
void Renderer::AdaptFrameBuffer()
{
    if(!m_options.FrameBufferIsAuto() || !m_options.captureWindow || m_options.crossAdapter)
    {
        m_fbSliceStart = {}; // not in Auto: start from scratch when it is switched on
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if(m_fbSliceStart.time_since_epoch().count() == 0)
    {
        m_fbSliceStart            = now;
        m_fbLowSince              = now;
        m_fbLastChange            = now;
        m_fbSliceMax              = 0.0;
        m_fbSlices[0] = m_fbSlices[1] = m_fbSlices[2] = m_fbSlices[3] = 0.0;
        m_options.frameBufferAuto = 1;
        double m, a;
        int    n;
        m_watcher.TakeGapStats(m, a, n); // forget what came before
    }
    {
        double maxMs, meanMs;
        int    n;
        m_watcher.TakeGapStats(maxMs, meanMs, n);
        if(n > 0)
        {
            m_fbSliceMax = (std::max)(m_fbSliceMax, maxMs);
            m_fbMeanMs   = m_fbMeanMs * 0.9 + meanMs * 0.1;
        }
    }
    if(std::chrono::duration<double>(now - m_fbSliceStart).count() >= 1.0)
    {
        for(int i = 3; i > 0; i--)
            m_fbSlices[i] = m_fbSlices[i - 1];
        m_fbSlices[0]  = m_fbSliceMax;
        m_fbSliceMax   = 0.0;
        m_fbSliceStart = now;
    }
    const double worst   = (std::max)({ m_fbSlices[0], m_fbSlices[1], m_fbSlices[2], m_fbSlices[3], m_fbSliceMax });
    const double content = 1000.0 * (std::max)(m_options.subFrames, 1) * (m_stereoActive ? 2.0 : 1.0) * (std::max)(1, m_options.frameBufferStep) / (std::max)(m_options.vsyncRate, 1.0f); // one game frame on screen
    const double extra   = (std::max)(0.0, worst - m_fbMeanMs);
    int          target  = (int)std::ceil(extra / (std::max)(content, 1.0) - 0.3); // a jitter under 30 % of a content frame needs no buffer
    const int    cap     = 5;
    target               = std::clamp(target, 0, cap);
    int          cur     = std::clamp(m_options.frameBufferAuto, 0, cap);
    const double sinceChange = std::chrono::duration<double, std::milli>(now - m_fbLastChange).count();
    if(m_fbUnderrun)
    {
        m_fbUnderrun = false;
        target       = (std::max)(target, (std::min)(cur + 1, cap));
        m_fbLowSince = now;
    }
    if(target > cur)
    {
        if(sinceChange >= 100.0)
        {
            cur++;
            m_fbLastChange = now;
        }
        m_fbLowSince = now;
    }
    else if(target < cur)
    {
        const double calm = cur == 1 ? 4.0 : 2.0;
        if(std::chrono::duration<double>(now - m_fbLowSince).count() >= calm)
        {
            cur--;
            m_fbLastChange = now;
            m_fbLowSince   = now;
        }
    }
    else
        m_fbLowSince = now;
    m_options.frameBufferAuto = cur;
}

// Anti-stutter buffer. The capture is polled every vsync into a ring; the renderer takes a frame out only when the shader needs a new
// one (content cadence = display Hz / SubFrames). A whole game frame is always taken (for 3D the left+right pair stays together, a
// repeated frame goes through the normal "no new frame" path so the eye order is kept).
//  - starts once N+1 frames are waiting (or at once when the picture is static: nothing new arrives for a while)
//  - the queue never holds more than N+1 frames: older ones are dropped (the game runs faster than the content rate)
//  - below N frames (after a freeze, or a slower game) a frame is repeated now and then (1 / (60 / missing)) until the buffer is full again
void Renderer::BufferedStep(const std::shared_ptr<CaptureBase>& capture)
{
    EnsureRing();
    const auto now = std::chrono::steady_clock::now();
    // Map buffer (2D to 3D AI): the queue is lengthened by as many game frames as the depth map takes to arrive (rounded up), so the frame
    // shown is the one the latest map was computed from. It goes through the same smooth queue as the frame buffer (one frame taken per
    // content step, a frame repeated now and then to grow, never a jump), which is added on top. Grows at once, shrinks to the needed size after 2 calm seconds.
    if(MapSync() && m_depthLatencyMs > 0.0)
    {
        const int want = std::clamp((int)std::ceil(m_depthLatencyMs / (std::max)(m_ringIntervalMs, 2.0) - 0.15), 0, FrameBufferLimit());
        if(want > m_mapFrames)
        {
            m_mapFrames   = want;
            m_mapLowSince = now;
        }
        else if(want < m_mapFrames)
        {
            if(std::chrono::duration<double>(now - m_mapLowSince).count() >= 2.0)
            {
                m_mapFrames   = want; // (the extra frames are dropped at once: one small skip instead of a long slow drift)
                m_mapLowSince = now;
            }
        }
        else
            m_mapLowSince = now;
    }
    else if(!MapSync())
        m_mapFrames = 0;
    const int n = std::clamp(m_options.EffectiveFrameBuffer() + m_mapFrames, 1, (std::max)(5, FrameBufferLimit()));

    // 1. capture into the ring
    if(m_ringFree.empty() && !m_ringQueue.empty())
    {
        m_ringFree.push_back(m_ringQueue.front());
        m_ringQueue.pop_front();
    }
    if(!m_ringFree.empty())
    {
        const int idx      = m_ringFree.back();
        m_ringFree.pop_back();
        const bool arrived = capture->Poll(m_ring[idx]);
        if(arrived)
        {
            if(m_ringLastArrival.time_since_epoch().count() != 0)
            {
                const double dt = std::chrono::duration<double, std::milli>(now - m_ringLastArrival).count();
                m_ringIntervalMs = std::clamp(m_ringIntervalMs * 0.9 + dt * 0.1, 2.0, 200.0);

            }
            m_ringLastArrival = now;
            if(idx < (int)m_ringTime.size())
                m_ringTime[idx] = now;
            m_ringArrived = true;
            m_ringQueue.push_back(idx);
            while((int)m_ringQueue.size() > n + 1)
            {
                m_ringFree.push_back(m_ringQueue.front());
                m_ringQueue.pop_front();
            }
        }
        else
            m_ringFree.push_back(idx);
    }

    // 2. the shader needs a new frame
    if(!NewInputRequired())
        return;
    if(m_stereoActive && (m_renderContext.frameNo & 1))
    {
        // 3D: one game frame feeds both eyes (the buffer runs at half the content rate); the second eye reuses the frame of the first
        ConvertStereoInput(false);
        RollInput(false);
        return;
    }
    // Divisor pacing (SubFrames Manual): when the game runs at a divisor of the content rate (content 120 fps, game 60 / 40 / 30 / 15...),
    // each game frame is shown for exactly K content frames (K eye pairs in 3D), K = content rate / game rate rounded, with some
    // hysteresis so it does not flip around a half value. The buffer is only asked for a frame every K requests, so a slower game is
    // neither seen as stutter nor shown unevenly (1 then 3 times instead of 2 and 2).
    {
        int k = 1;
        if(m_options.EffectiveSubFramesMode() == SF_MANUAL && m_options.vsyncRate > 0.0f)
        {
            const double pairRate = m_options.vsyncRate / (double)(std::max)(m_options.subFrames, 1) / (m_stereoActive ? 2.0 : 1.0);
            const double ratio    = pairRate * m_ringIntervalMs / 1000.0;
            k                     = std::clamp(m_options.frameBufferStep, 1, 16);
            if(std::abs(ratio - k) > 0.6)
                k = std::clamp((int)std::lround(ratio), 1, 16);
        }
        m_options.frameBufferStep = k;
        if(k > 1 && ++m_ringStepCount < k)
        {
            ConvertStereoInput(false); // the current game frame stays for this content frame
            RollInput(false);
            return;
        }
        m_ringStepCount = 0;
    }
    const double sinceMs = std::chrono::duration<double, std::milli>(now - m_ringLastArrival).count();
    const bool   idle    = !m_ringQueue.empty() && sinceMs > (std::max)(25.0, m_ringIntervalMs * 2.5);
    if(!m_ringPrimed && ((int)m_ringQueue.size() >= n + 1 || idle))
        m_ringPrimed = true;
    bool pop = m_ringPrimed && !m_ringQueue.empty();
    if(m_ringPrimed && m_ringQueue.empty() && !idle)
        m_fbUnderrun = true; // the shader waited for a frame that did not come: visible stutter
    if(pop && (int)m_ringQueue.size() < n && !idle)
    {
        const int missing   = n - (int)m_ringQueue.size();
        const int skipEvery = (std::max)(2, 60 / missing);
        if(++m_ringSkip >= skipEvery)
        {
            m_ringSkip = 0;
            pop        = false; // repeat the current frame once: the buffer refills slowly, without a visible jump
        }
    }
    if(pop)
    {
        const int idx = m_ringQueue.front();
        m_ringQueue.pop_front();
        m_renderContext.deviceContext->CopyResource(GetNextInput().get(), m_ring[idx].get());
        m_ringFree.push_back(idx);
        ConvertStereoInput(true);
        RollInput(true);
    }
    else
    {
        ConvertStereoInput(false);
        RollInput(false);
    }
}

void Renderer::Benchmark(const std::shared_ptr<CaptureBase>& capture)
{
    const float benchmarkDuration = 4 * TICKS_PER_SEC;

    auto  input       = GetNextInput();
    auto  start       = Helpers::GetTicks();
    float copyTime    = 0.0f;
    float renderTime  = 0.0f;
    float presentTime = 0.0f;
    float now         = start;
    float prev        = now;
    int   frames      = 0;
    do
    {
        auto pctComplete           = (now - start) / benchmarkDuration;
        m_renderContext.subFrameNo = ((int)(m_options.subFrames * pctComplete)) % m_options.subFrames;
        if(m_options.crossAdapter && (frames % m_options.subFrames == 0))
        {
            capture->BenchmarkCopy(input);
            m_renderContext.deviceContext->Flush();
            WaitTillIdle();
            auto now = Helpers::GetTicks();
            copyTime += now - prev;
            prev = now;
        }

        Render(false, false);
        m_renderContext.deviceContext->Flush();
        WaitTillIdle();
        now = Helpers::GetTicks();
        renderTime += now - prev;
        prev = now;

        Present(false);
        WaitTillIdle();
        frames++;
        now = Helpers::GetTicks();
        presentTime += now - prev;
        prev = now;
    } while(now < start + benchmarkDuration);
    auto totalTime = now - start;
    m_ui.SetBenchmark({
        .totalFPS   = totalTime != 0 ? frames / (totalTime / TICKS_PER_SEC) : 0,
        .copyFPS    = copyTime != 0 ? frames / (copyTime / TICKS_PER_SEC) : 0,
        .renderFPS  = renderTime != 0 ? frames / (renderTime / TICKS_PER_SEC) : 0,
        .presentFPS = presentTime != 0 ? frames / (presentTime / TICKS_PER_SEC) : 0,
    });
}

void Renderer::Create()
{
    auto dxgiDevice = m_renderContext.device.as<IDXGIDevice1>();
    dxgiDevice->SetMaximumFrameLatency(m_options.Tearing() ? 1 : m_options.maxQueuedFrames);
    if(m_options.gpuThreadPriority)
        dxgiDevice->SetGPUThreadPriority(m_options.gpuThreadPriority | 0x40000000);

#if _DEBUG
    m_debug = m_renderContext.device.as<ID3D11Debug>();
#endif

    winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
    THROW(dxgiDevice->GetAdapter(dxgiAdapter.put()), "Unable to get DXGI adapter");
    winrt::com_ptr<IDXGIFactory2> dxgiFactory;
    THROW(dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)dxgiFactory.put()), "Unable to get DXGI factory");

    DXGI_SWAP_CHAIN_DESC1 d3d11SwapChainDesc = {};
    d3d11SwapChainDesc.Width                 = 0;
    d3d11SwapChainDesc.Height                = 0;
    d3d11SwapChainDesc.Format                = m_options.format;
    d3d11SwapChainDesc.SampleDesc.Count      = 1;
    d3d11SwapChainDesc.SampleDesc.Quality    = 0;
    d3d11SwapChainDesc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d3d11SwapChainDesc.BufferCount           = m_options.swapChainBuffers;
    d3d11SwapChainDesc.Scaling               = DXGI_SCALING_NONE;
    d3d11SwapChainDesc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d3d11SwapChainDesc.AlphaMode             = DXGI_ALPHA_MODE_UNSPECIFIED;
    d3d11SwapChainDesc.Flags                 = 0;
    m_tearing                                = false;
    m_tearOutput                             = nullptr;
    if(m_options.Tearing())
    {
        // tearing needs a flip-model swap chain and a system that supports it; otherwise the normal vsync path is used
        BOOL                          allowed = FALSE;
        winrt::com_ptr<IDXGIFactory5> f5;
        if(SUCCEEDED(dxgiFactory->QueryInterface(__uuidof(IDXGIFactory5), f5.put_void())) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed))) && allowed)
        {
            d3d11SwapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            m_tearing                = true;
        }
    }
    // fun fact: on Intel Arc setting DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
    // makes Present(1) not wait on vsync any more and BSODs Windows

    THROW(dxgiFactory->CreateSwapChainForHwnd(m_renderContext.device.get(), m_options.outputWindow, &d3d11SwapChainDesc, 0, 0, m_swapChain.put()), "Unable to create SwapChain");

    /*
    *  works if using Output for another display, but breaks WGC capture if a window is dragged onto it for example
    */
    if(m_options.exclusive)
    {
        winrt::com_ptr<IDXGIOutput> output;
        THROW(dxgiAdapter->EnumOutputs(m_options.shaderDisplayNo, output.put()), "Unable to get output");

        DXGI_OUTPUT_DESC outputDesc;
        THROW(output->GetDesc(&outputDesc), "Unable to get output desc");

        THROW(m_swapChain->SetFullscreenState(TRUE, output.get()), "Unable to set fullscreen");

        THROW(m_swapChain->ResizeBuffers(d3d11SwapChainDesc.BufferCount, m_options.outputWidth, m_options.outputHeight, m_options.format, 0), "Unable to resize buffers");
    }

    if(m_options.useHdr)
    {
        winrt::com_ptr<IDXGISwapChain3> swapChain3;
        THROW(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swapChain3.put())), "Unable to get SwapChain3");
        THROW(swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709), "Unable to set HDR ColorSpace");
    }

    if(m_tearing)
    {
        m_swapChain->GetContainingOutput(m_tearOutput.put());
        OpenScanline();
    }
    else
        m_options.beamRacingState = 0;

    THROW(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)m_renderContext.outputTexture.put()), "Unable to get display buffer");

    D3D11_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format        = m_options.hardwareSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : m_options.format;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    THROW(m_renderContext.device->CreateRenderTargetView(m_renderContext.outputTexture.get(), &rtv, m_renderContext.outputTargetView.put()), "Unable to create render target");

    D3D11_RENDER_TARGET_VIEW_DESC uitv {};
    uitv.Format        = m_options.format;
    uitv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    THROW(m_renderContext.device->CreateRenderTargetView(m_renderContext.outputTexture.get(), &uitv, m_uiTargetView.put()), "Unable to create render target");

    D3D11_RASTERIZER_DESC rsDesc {};
    rsDesc.CullMode          = D3D11_CULL_NONE;
    rsDesc.FillMode          = D3D11_FILL_SOLID;
    rsDesc.DepthClipEnable   = FALSE;
    rsDesc.MultisampleEnable = FALSE;
    rsDesc.ScissorEnable     = TRUE;
    THROW(m_renderContext.device->CreateRasterizerState(&rsDesc, m_rasterizerState.put()), "Unable to create rasterizer state");

    D3D11_DEPTH_STENCIL_DESC dsDesc {};
    dsDesc.DepthEnable   = false;
    dsDesc.StencilEnable = false;
    THROW(m_renderContext.device->CreateDepthStencilState(&dsDesc, m_depthStencilState.put()), "Unable to create depth/stencil state");

    D3D11_RECT scissor {};
    scissor.left   = 0;
    scissor.top    = 0;
    scissor.right  = m_options.splitScreen == 1 ? m_options.outputWidth / 2 : m_options.outputWidth;
    scissor.bottom = m_options.splitScreen == 2 ? m_options.outputHeight / 2 : m_options.outputHeight;

    m_renderContext.deviceContext->RSSetState(m_rasterizerState.get());
    m_renderContext.deviceContext->OMSetDepthStencilState(m_depthStencilState.get(), 0);
    m_renderContext.deviceContext->OMSetBlendState(NULL, NULL, 1);
    m_renderContext.deviceContext->RSSetScissorRects(1, &scissor);

    m_renderContext.frameNo    = 0;
    m_renderContext.subFrameNo = 0;
}

void Renderer::CreateInputs()
{
    auto inputsRequired = m_shaderManager.NumInputsRequired();
    if(inputsRequired > MAX_INPUTS)
        THROW(E_FAIL, "Too many inputs");

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width              = m_options.outputWidth;
    desc.Height             = m_options.outputHeight;
    desc.ArraySize          = 1;
    desc.Format             = m_options.hardwareSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : m_options.format;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.MipLevels          = 1;
    desc.MiscFlags          = 0;
    desc.CPUAccessFlags     = m_options.crossAdapter ? D3D11_CPU_ACCESS_WRITE : 0;
    desc.Usage              = m_options.crossAdapter ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    if(!m_options.crossAdapter)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET; // (capture scaling when the captured screen and the output differ in resolution)

    // 3D source decoding needs render-target inputs and a raw (full 3D frame) texture; not supported with a cross-adapter setup
    m_stereoActive  = m_options.emitter.sourceFormat != StereoSource::Off && !m_options.emitter.hold && !m_options.crossAdapter && m_options.StereoAllowed();
    m_stereoHaveRaw = false;
    if(m_stereoActive)
    {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        m_stereo.Create(m_renderContext.device);
    }

    D3D11_SUBRESOURCE_DATA data {};
    auto                   formatWidth = m_options.useHdr ? 2 : 1;
    std::vector<uint32_t>  initialData;
    initialData.resize(m_options.outputWidth * m_options.outputHeight * formatWidth);
    data.pSysMem          = initialData.data();
    data.SysMemPitch      = m_options.outputWidth * sizeof(uint32_t) * formatWidth;
    data.SysMemSlicePitch = m_options.outputWidth * m_options.outputHeight * sizeof(uint32_t) * formatWidth;
    for(int i = 0; i < inputsRequired; i++)
    {
#ifdef RGB_TEST
        uint32_t color;
        switch(i)
        {
        case 2:
            color = 0xff0000ff;
            break;
        case 1:
            color = 0xff00ff00;
            break;
        case 0:
            color = 0xffff0000;
            break;
        }
        for(auto& d : initialData)
            d = color;
#endif
        m_renderContext.inputSlots.push_back(i);
        auto& inputTexture     = m_renderContext.inputTextures.emplace_back(nullptr);
        auto& inputTextureView = m_renderContext.inputTextureViews.emplace_back(nullptr);
        THROW(m_renderContext.device->CreateTexture2D(&desc, &data, inputTexture.put()), "Unable to create texture");
        THROW(m_renderContext.device->CreateShaderResourceView(inputTexture.get(), nullptr, inputTextureView.put()), "Unable to create input view");
        if(m_stereoActive)
            THROW(m_renderContext.device->CreateRenderTargetView(inputTexture.get(), nullptr, m_stereoTargets.emplace_back(nullptr).put()), "Unable to create 3D input target");
    }

    if(m_stereoActive)
    {
        auto rawDesc      = desc;
        rawDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if(!m_options.crossAdapter)
            rawDesc.BindFlags |= D3D11_BIND_RENDER_TARGET; // (capture scaling)
        THROW(m_renderContext.device->CreateTexture2D(&rawDesc, &data, m_stereoRaw.put()), "Unable to create 3D source texture");
        THROW(m_renderContext.device->CreateShaderResourceView(m_stereoRaw.get(), nullptr, m_stereoRawView.put()), "Unable to create 3D source view");
        if(m_options.emitter.sourceFormat == StereoSource::AiDepth)
            CreateDepthResources();
    }
}

void Renderer::Destroy()
{
    CloseScanline();
    m_depthStencilState              = nullptr;
    m_rasterizerState                = nullptr;
    m_swapChain                      = nullptr;
    m_uiTargetView                   = nullptr;
    m_hudSrv                         = nullptr;
    m_hudRtv                         = nullptr;
    m_hudTex                         = nullptr;
    m_hudW                           = 0;
    m_hudH                           = 0;
    // the HUD shaders and blend state belong to this device: every restart creates a new device, so they are made again with it
    // (reused across a restart they crashed Nbeam the first time the HUD was shown after a profile switch)
    m_hudVs                          = nullptr;
    m_hudPs                          = nullptr;
    m_hudBlend                       = nullptr;
    m_hudFailed                      = false;
    m_hudEmpty                       = false;
    m_hudArea                        = {};
    m_hudLast                        = {};
    m_toneCopySrv                    = nullptr;
    m_toneCopy                       = nullptr;
    m_toneCb                         = nullptr;
    m_tonePs                         = nullptr;
    m_toneW = m_toneH                = 0;
    m_toneVs                         = nullptr;
    m_renderContext.frameNo          = 0;
    m_renderContext.subFrameNo       = 0;
    m_renderContext.outputTargetView = nullptr;
    m_renderContext.outputTexture    = nullptr;
    DestroyInputs();
    if(m_renderContext.deviceContext)
    {
        m_renderContext.deviceContext->RSSetState(NULL); // fix exception in nvapi...
        m_renderContext.deviceContext->ClearState();
        m_renderContext.deviceContext->Flush();
        m_renderContext.deviceContext = nullptr;
    }
#if _DEBUG
    m_debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
    m_debug = nullptr;
#endif
    m_renderContext.device = nullptr;
}

void Renderer::DestroyInputs()
{
    DestroyRing();
    DestroyDepthResources();
    m_stereoTargets.clear();
    m_stereoRawView = nullptr;
    m_stereoRaw     = nullptr;
    m_stereo.Destroy();
    m_stereoActive  = false;
    for(int i = 0; i < m_renderContext.inputTextureViews.size(); i++)
        m_renderContext.inputTextureViews[i] = nullptr;
    m_renderContext.inputTextureViews.clear();
    for(int i = 0; i < m_renderContext.inputTextures.size(); i++)
        m_renderContext.inputTextures[i] = nullptr;
    m_renderContext.inputTextures.clear();
    m_renderContext.inputSlots.clear();
}

void Renderer::WaitTillIdle()
{
    D3D11_QUERY_DESC            queryDesc = { D3D11_QUERY_EVENT, 0 };
    winrt::com_ptr<ID3D11Query> query;
    BOOL                        done = FALSE;

    if(m_renderContext.device->CreateQuery(&queryDesc, query.put()) == S_OK)
    {
        m_renderContext.deviceContext->End(query.get());
        while(!done)
        {
            switch(m_renderContext.deviceContext->GetData(query.get(), &done, sizeof(BOOL), 0))
            {
            case S_OK:
                break;
            case S_FALSE:
                break;
            default:
                // error
                return;
            }
        }
    }
}

} // namespace ShaderBeam