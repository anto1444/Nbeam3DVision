/*
ShaderBeam: shader effect overlay -- 3D source decoding
Turns a captured Side-by-Side / Top-Bottom / Checkerboard frame into a full-resolution single-eye image.
MIT License
*/

#pragma once

#include "EmitterSettings.h"

namespace ShaderBeam
{

class StereoConverter
{
public:
    // device: render device; width/height: output size (== captured frame size)
    void Create(const winrt::com_ptr<ID3D11Device>& device);
    void Destroy();

    // Renders the `eye` view of `source` (full 3D frame) into `target` (same size, bound as render target).
    // For StereoSource::AiDepth, `depth` (16-bit depth map) and `ai` give the parallax parameters.
    void Convert(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, StereoSource format, Eye eye, unsigned width, unsigned height,
                 ID3D11ShaderResourceView* depth = nullptr, const AiDepthConfig* ai = nullptr);

    // Box-filtered reduction of `source` (srcW x srcH) into `target` (dstW x dstH, BGRA8): the picture given to the depth model
    void Downscale(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, unsigned srcW, unsigned srcH, unsigned dstW, unsigned dstH, bool srcIsSrgb);

private:
    void Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* target, unsigned vpW, unsigned vpH);
    void SetParams(ID3D11DeviceContext* ctx, const unsigned (&p)[12]);

    winrt::com_ptr<ID3D11VertexShader> m_vs;
    winrt::com_ptr<ID3D11PixelShader>  m_ps;
    winrt::com_ptr<ID3D11Buffer>       m_cb;
    winrt::com_ptr<ID3D11SamplerState> m_sampler;
};

} // namespace ShaderBeam
