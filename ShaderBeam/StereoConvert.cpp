/*
ShaderBeam: shader effect overlay -- 3D source decoding
MIT License
*/

#include "stdafx.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include "StereoConvert.h"
#include "Helpers.h"

namespace ShaderBeam
{

static const char kStereoHlsl[] = R"HLSL(
cbuffer Params : register(b0)
{
    uint  mode;     // 1 = side by side, 2 = top and bottom, 3 = checkerboard, 4 = AI depth, 5 = downscale
    uint  eye;      // 0 = left, 1 = right
    uint  width;
    uint  height;
    float strength; // AI depth: maximum parallax, fraction of the width
    float conv;     // AI depth: depth (0..1) that stays on the screen plane
    uint  flags;    // bit 0: show the depth map, bit 1: source is sRGB-decoded (re-encode)
    uint  outW;
    uint  outH;
    float halo;     // AI depth: anti-halo search radius, fraction of the width
    uint  pad1;
    uint  pad2;
};

Texture2D<float4> src : register(t0);
Texture2D<float>  depthTex : register(t1);
SamplerState      lin : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSmain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

float4 PSmain(VSOut i) : SV_Target
{
    if(mode == 5)
    {
        // average of 4x4 taps spread over the footprint of the destination pixel
        float2 fp = float2(width, height) / float2(outW, outH);
        float3 acc = float3(0.0, 0.0, 0.0);
        [unroll] for(int y = 0; y < 4; y++)
        [unroll] for(int x = 0; x < 4; x++)
        {
            float2 o = ((float2(x, y) + 0.5) / 4.0 - 0.5) * fp / float2(width, height);
            acc += src.SampleLevel(lin, i.uv + o, 0.0).rgb;
        }
        acc = saturate(acc / 16.0);
        if(flags & 2)
            acc = pow(acc, 1.0 / 2.2);
        return float4(acc, 1.0);
    }
    if(mode == 4)
    {
        // parallax from the depth map: near objects move right in the left eye, left in the right eye.
        // Backward warp: the depth is read where the shifted pixel comes from (3 refinement steps).
        float sgn = eye == 0 ? 1.0 : -1.0;
        float2 duv = i.uv;
        float d = 0.5;
        [unroll] for(int k = 0; k < 3; k++)
        {
            d = depthTex.SampleLevel(lin, duv, 0.0);
            // anti-halo: the farthest depth around the pixel (horizontally, the direction of the shift), so the depth of a
            // foreground object does not spill onto the background next to it and drag the object's colours along
            [branch] if(halo > 0.0)
            {
                [unroll] for(int t = -4; t <= 4; t++)
                    d = min(d, depthTex.SampleLevel(lin, duv + float2(t * 0.25 * halo, 0.0), 0.0));
            }
            duv = float2(i.uv.x - (d - conv) * strength * 0.5 * sgn, i.uv.y);
        }
        if(flags & 1)
            return float4(d, d, d, 1.0);
        // left and right edges: straight black bands as wide as the largest shift, identical in both eyes (on the screen plane),
        // instead of stretched / repeated edge pixels that make ragged borders
        float band = max(conv, 1.0 - conv) * strength * 0.5;
        if(i.uv.x < band || i.uv.x > 1.0 - band || duv.x < 0.0 || duv.x > 1.0)
            return float4(0.0, 0.0, 0.0, 1.0);
        return src.SampleLevel(lin, duv, 0.0);
    }
    if(mode == 1)
    {
        float2 uv = float2(i.uv.x * 0.5 + 0.5 * eye, i.uv.y);
        return src.SampleLevel(lin, uv, 0.0);
    }
    if(mode == 2)
    {
        float2 uv = float2(i.uv.x, i.uv.y * 0.5 + 0.5 * eye);
        return src.SampleLevel(lin, uv, 0.0);
    }
    // checkerboard: left eye on pixels with (x + y) even, right eye on odd; the missing pixels of the
    // requested eye are the average of the four neighbours (which all belong to that eye)
    int2 p = int2(i.pos.xy);
    if((uint)((p.x + p.y) & 1) == eye)
        return src.Load(int3(p, 0));
    int2 m = int2(width - 1, height - 1);
    float4 a = src.Load(int3(clamp(p + int2(1, 0), int2(0, 0), m), 0));
    float4 b = src.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), m), 0));
    float4 c = src.Load(int3(clamp(p + int2(0, 1), int2(0, 0), m), 0));
    float4 d = src.Load(int3(clamp(p + int2(0, -1), int2(0, 0), m), 0));
    return (a + b + c + d) * 0.25;
}
)HLSL";

static void Compile(const char* entry, const char* profile, winrt::com_ptr<ID3DBlob>& blob)
{
    std::string errors;
    HRESULT     hr = Helpers::CompileCached(kStereoHlsl, sizeof(kStereoHlsl) - 1, "StereoConvert", nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, blob, errors);
    if(FAILED(hr))
        throw std::runtime_error(std::string("Unable to compile 3D source shader: ") + errors);
}

void StereoConverter::Create(const winrt::com_ptr<ID3D11Device>& device)
{
    winrt::com_ptr<ID3DBlob> vs, ps;
    Compile("VSmain", "vs_5_0", vs);
    Compile("PSmain", "ps_5_0", ps);
    THROW(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_vs.put()), "Unable to create 3D vertex shader");
    THROW(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_ps.put()), "Unable to create 3D pixel shader");

    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth      = 48;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    THROW(device->CreateBuffer(&bd, nullptr, m_cb.put()), "Unable to create 3D constant buffer");

    D3D11_SAMPLER_DESC sd {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    THROW(device->CreateSamplerState(&sd, m_sampler.put()), "Unable to create 3D sampler");
}

void StereoConverter::Destroy()
{
    m_sampler = nullptr;
    m_cb      = nullptr;
    m_ps      = nullptr;
    m_vs      = nullptr;
}

void StereoConverter::SetParams(ID3D11DeviceContext* ctx, const unsigned (&p)[12])
{
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if(SUCCEEDED(ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, p, sizeof(p));
        ctx->Unmap(m_cb.get(), 0);
    }
}

void StereoConverter::Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* target, unsigned vpW, unsigned vpH)
{
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)vpW, (float)vpH, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ctx->PSSetShader(m_ps.get(), nullptr, 0);
    ID3D11Buffer* cb = m_cb.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[2] = { source, depth };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* ss = m_sampler.get();
    ctx->PSSetSamplers(0, 1, &ss);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11Buffer* nullCb = nullptr;
    ctx->PSSetConstantBuffers(0, 1, &nullCb);
    ID3D11SamplerState* nullSs = nullptr;
    ctx->PSSetSamplers(0, 1, &nullSs);
}

void StereoConverter::Convert(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, StereoSource format, Eye eye, unsigned width, unsigned height,
                              ID3D11ShaderResourceView* depth, const AiDepthConfig* ai)
{
    if(!m_vs || format == StereoSource::Off)
        return;
    if(format == StereoSource::AiDepth && (!depth || !ai))
        return;

    unsigned p[12] = { (unsigned)format, (unsigned)eye, width, height, 0, 0, 0, 0, 0, 0, 0, 0 };
    if(format == StereoSource::AiDepth)
    {
        const float strength = ai->strength * 0.01f;
        const float conv     = ai->convergence * 0.01f;
        memcpy(&p[4], &strength, sizeof(float));
        memcpy(&p[5], &conv, sizeof(float));
        p[6] = ai->showDepth ? 1u : 0u;
        const float halo = ai->antiHalo * 0.01f * 0.012f; // 100 % = 1.2% of the width (slider up to 300 %)
        memcpy(&p[9], &halo, sizeof(float));
    }
    SetParams(ctx, p);
    Draw(ctx, source, depth, target, width, height);
}

void StereoConverter::Downscale(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, unsigned srcW, unsigned srcH, unsigned dstW, unsigned dstH, bool srcIsSrgb)
{
    if(!m_vs)
        return;
    unsigned p[12] = { 5, 0, srcW, srcH, 0, 0, srcIsSrgb ? 2u : 0u, dstW, dstH, 0, 0, 0 };
    SetParams(ctx, p);
    Draw(ctx, source, nullptr, target, dstW, dstH);
}

} // namespace ShaderBeam
