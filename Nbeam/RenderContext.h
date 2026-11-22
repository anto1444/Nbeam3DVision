/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "Common.h"

namespace Nbeam
{

struct RenderContext
{
    RenderContext(const Options& options) : options(options) { }

    int                                                   frameNo { 0 };
    int                                                   subFrameNo { 0 };
    // BFI phase lock: the cycle being shown is one refresh shorter (its last subframe dropped): its length in refreshes (0 = a normal cycle)
    // and its light (linear, 1 = unchanged) so the brightness stays even. The shaders use them in their own pass.
    int                                                   cycleSubFrames { 0 };
    float                                                 cycleGain { 1.0f };
    winrt::com_ptr<ID3D11Device>                          device;
    winrt::com_ptr<ID3D11DeviceContext>                   deviceContext;
    std::vector<winrt::com_ptr<ID3D11Texture2D>>          inputTextures;
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> inputTextureViews;
    std::vector<int>                                      inputSlots;
    winrt::com_ptr<ID3D11Texture2D>                       outputTexture;
    winrt::com_ptr<ID3D11RenderTargetView>                outputTargetView;

    const Options& options;
};
} // namespace Nbeam
