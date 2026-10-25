/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License

Simple BFI with adjustable ratio: of every SubFrames output frames, the first "Clear Frames" show the image
and the others show it darkened by "Gain vs Blur" (1.0 = fully black).
*/

#pragma once

#include <d3dcompiler.h>
#include "ShaderProfile.h"
#include "Helpers.h"

namespace ShaderBeam
{

class SimpleBFIShader : public ShaderProfile
{
public:
    float m_gainVsBlur { 1.0f };
    int   m_clearFrames { 1 };

    SimpleBFIShader()
    {
        m_name = "Simple BFI";
        AddParameter("Gain vs Blur",
                     "How dark the inserted frames are.\n"
                     "- 1.0 is fully black (classic BFI).\n"
                     "- Lower values let part of the image show through (softer, brighter, less flicker, more blur).",
                     &m_gainVsBlur,
                     0.0f,
                     1.0f);
        AddParameter("Clear Frames",
                     "Number of clear (image) frames out of every SubFrames output frames, 1 to 20.\n"
                     "- The others are the BFI (dark) frames: with SubFrames 4, 1 clear = 3 black, 2 clear = 2 black, 3 clear = 1 black.\n"
                     "- A value of SubFrames or more means no BFI at all.",
                     &m_clearFrames,
                     1,
                     20);
    }

    void Create(const RenderContext& renderContext)
    {
        static const char hlsl[] = R"HLSL(
cbuffer Params : register(b0) { float4 scale; };
Texture2D<float4> src : register(t0);
float4 VSmain(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position
{
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = src.Load(int3(int2(pos.xy), 0));
    return float4(c.rgb * scale.x, c.a);
}
)HLSL";
        winrt::com_ptr<ID3DBlob> vs, ps;
        auto compile = [&](const char* entry, const char* profile, winrt::com_ptr<ID3DBlob>& blob) {
            std::string errors;
            if(FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "SimpleBFI", nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, blob, errors)))
                throw std::runtime_error(std::string("Unable to compile Simple BFI shader: ") + errors);
        };
        compile("VSmain", "vs_5_0", vs);
        compile("PSmain", "ps_5_0", ps);
        THROW(renderContext.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_vs.put()), "Unable to create BFI vertex shader");
        THROW(renderContext.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_ps.put()), "Unable to create BFI pixel shader");
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth      = 16;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        THROW(renderContext.device->CreateBuffer(&bd, nullptr, m_cb.put()), "Unable to create BFI constant buffer");
    }

    void Destroy()
    {
        m_cb = nullptr;
        m_ps = nullptr;
        m_vs = nullptr;
    }

    void Render(const RenderContext& renderContext)
    {
        const bool  clear = renderContext.subFrameNo < m_clearFrames;
        const float scale = clear ? 1.0f : 1.0f - m_gainVsBlur;

        if(scale >= 0.999f)
            Passthrough(renderContext);
        else if(scale <= 0.001f)
        {
            static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            renderContext.deviceContext->ClearRenderTargetView(renderContext.outputTargetView.get(), black);
        }
        else
            Scale(renderContext, scale);
    }

private:
    winrt::com_ptr<ID3D11VertexShader> m_vs;
    winrt::com_ptr<ID3D11PixelShader>  m_ps;
    winrt::com_ptr<ID3D11Buffer>       m_cb;

    void Scale(const RenderContext& rc, float scale)
    {
        auto* ctx = rc.deviceContext.get();
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if(SUCCEEDED(ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            float v[4] = { scale, 0, 0, 0 };
            memcpy(mapped.pData, v, sizeof(v));
            ctx->Unmap(m_cb.get(), 0);
        }
        D3D11_TEXTURE2D_DESC td {};
        rc.outputTexture->GetDesc(&td);
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)td.Width, (float)td.Height, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv = rc.outputTargetView.get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(m_vs.get(), nullptr, 0);
        ctx->PSSetShader(m_ps.get(), nullptr, 0);
        ID3D11Buffer* cb = m_cb.get();
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srv = rc.inputTextureViews[rc.inputSlots[0]].get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    }
};
} // namespace ShaderBeam
