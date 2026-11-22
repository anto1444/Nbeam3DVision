/*
Nbeam 3D Vision: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "SimpleBFIShader.h"
#include "BfiSweepShader.h"

namespace Nbeam
{

// The number of a shader is the number saved in the profiles (shaderKind): 0 = BFI, 1 = Sweep BFI via MPRT
static ShaderProfile* s_shaders[] = {
    new SimpleBFIShader(),
    new BfiSweepShader(),
};
}
