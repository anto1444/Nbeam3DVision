// From Shadertoy https://www.shadertoy.com/view/XfKfWd
// - Improved version coming January 2025
// - See accompanying article https://blurbusters.com/crt
// - To study more about display science & physics, see Research Portal https://blurbusters.com/area51
// adapted for ShaderBeam by mausimus

cbuffer Params : register(b0)
{
    float param_gainVsBlur;
    uint param_scanDirection;
    
    // calculated on CPU side
    float param_effectiveFramesPerHz;
    float param_rasterPos;
    float param_hzCounter;
    float param_blackLevel;
};

// No Gamma setting: the standard sRGB curve is used (2.4 in its power part); the light of the pulses is added in linear light.
#define GAMMA                   2.4
#define GAIN_VS_BLUR            param_gainVsBlur
#define EFFECTIVE_FRAMES_PER_HZ param_effectiveFramesPerHz

Texture2D<float4> iChannel0 : register(t2);
Texture2D<float4> iChannel1 : register(t3);
Texture2D<float4> iChannel2 : register(t4);
SamplerState iChannel_sampler : register(s2);

/*********************************************************************************************************************/
//
//                     Blur Busters CRT Beam Simulator BFI
//                       With Seamless Gamma Correction
//
//         From Blur Busters Area 51 Display Science, Research & Engineering
//                      https://www.blurbusters.com/area51
//
//             The World's First Realtime Blur-Reducing CRT Simulator
//       Best for 60fps on 240-480Hz+ Displays, Still Works on 120Hz+ Displays
//                 Original Version 2022. Publicly Released 2024.
//
// CREDIT: Teamwork of Mark Rejhon @BlurBusters & Timothy Lottes @NOTimothyLottes
// Gamma corrected CRT simulator in a shader using clever formula-by-scanline trick
// (easily can generate LUTs, for other workflows like FPGAs or Javascript)
// - @NOTimothyLottes provided the algorithm for per-pixel BFI (Variable MPRT, higher MPRT for bright pixels)
// - @BlurBusters provided the algorithm for the CRT electron beam (2022, publicly released for first time)
//
// Contact Blur Busters for help integrating this in your product (emulator, fpga, filter, display firmware, video processor)
//
// This new algorithm has multiple breakthroughs:
//
// - Seamless; no banding*!  (*Monitor/OS configuration: SDR=on, HDR=off, ABL=off, APL=off, gamma=2.4)
// - Phosphor fadebehind simulation in rolling scan.
// - Works on LCDs and OLEDs.
// - Variable per-pixel MPRT. Spreads brighter pixels over more refresh cycles than dimmer pixels.
// - No image retention on LCDs or OLEDs.
// - No integer divisor requirement. Recommended but not necessary (e.g. 60fps 144Hz works!)
// - Gain adjustment (less motion blur at lower gain values, by trading off brightness)
// - Realtime (for retro & emulator uses) and slo-mo modes (educational)
// - Great for softer 60Hz motion blur reduction, less eyestrain than classic 60Hz BFI/strobe.
// - Algorithm can be ported to shader and/or emulator and/or FPGA and/or display firmware.
//
// For best real time CRT realism:
//
// - Reasonably fast performing GPU (many integrated GPUs are unable to keep up)
// - Fastest GtG pixel response (A settings-modified OLED looks good with this algorithm)
// - As much Hz per CRT Hz! (960Hz better than 480Hz better than 240Hz)
// - Integer divisors are still better (just not mandatory)
// - Brightest SDR display with linear response (no ABL, no APL), as HDR boost adds banding
//     (unless you can modify the firmware to make it linear brightness during a rolling scan)
//
// *** IMPORTANT ***
// *** DISPLAY REQUIREMENTS ***
//
// - Best for gaming LCD or OLED monitors with fast pixel response.
// - More Hz per simulated CRT Hz is better (240Hz, 480Hz simulates 60Hz tubes more accurately than 120Hz).
// - OLED (SDR mode) looks better than LCD, but still works on LCD
// - May have minor banding with very slow GtG, asymmetric-GtG (VA LCDs), or excessively-overdriven.
// - Designed for sample & hold displays with excess refresh rate (LCDs and OLEDs);
//     Not intended for use with strobed or impulsed displays. Please turn off your displays' BFI/strobing.
//     This is because we need 100% software control of the flicker algorithm to simulate a CRT beam.
//
// SDR MODE RECOMMENDED FOR NOW (Due to predictable gamma compensation math)
//
// - Best results occur on display configured to standard SDR gamma curve and ABL/APL disabled to go 100% bandfree
// - Please set your display gamma to 2.2 or 2.4, turn off ABL/APL in display settings, and set your OLED to SDR mode.  
// - Will NOT work well with some FALD and MiniLED due to backlight lagbehind effects.
// - Need future API access to OLED ABL/ABL algorithm to compensate for OLED ABL/APL windowing interference with algorithm.
// - This code is heavily commented because of the complexity of the algorithm.
//
/*********************************************************************************************************************/
//
// MIT License
// 
// Copyright 2024 Mark Rejhon (@BlurBusters) & Timothy Lottes (@NOTimothyLottes)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the “Software”), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
/*********************************************************************************************************************/

//-------------------------------------------------------------------------------------------------
// Utility Macros

#define clampPixel(a) clamp(a, float3(0.0), float3(1.0))

// Selection Function: Returns 'b' if 'p' is true, else 'a'.
float SelF1(float a, float b, bool p)
{
    return p ? b : a;
}

float mod(float x, float y)
{
    return x - y * floor(x / y);
}

//-------------------------------------------------------------------------------------------------
// sRGB Encoding and Decoding Functions, to gamma correct/uncorrect

// Encode linear color to sRGB. (applies gamma curve)
float linear2srgb(float c)
{
    // exact sRGB encoding (the formula of the original shader gives wrong, even negative, values for the darkest linear values)
    return c <= 0.0031308 ? c * 12.92 : pow(c, 1.0 / GAMMA) * 1.055 - 0.055;
}
float3 linear2srgb(float3 c)
{
#if HARDWARE_SRGB == 1
    return c;
#else
    return float3(linear2srgb(c.r), linear2srgb(c.g), linear2srgb(c.b));
#endif    
}

// Decode sRGB color to linear. (undoes gamma curve)
float srgb2linear(float c)
{
    float3 j = float3(0.04045, 1.0 / 12.92, GAMMA);
    float2 k = float2(1.0 / 1.055, 0.055 / 1.055);
    return SelF1(c * j.y, pow(c * k.x + k.y, j.z), c > j.x);
}
float3 srgb2linear(float3 c)
{
#if HARDWARE_SRGB == 1
    return c;
#else
    return float3(srgb2linear(c.r), srgb2linear(c.g), srgb2linear(c.b));
#endif    
}

//------------------------------------------------------------------------------------------------
// Gets pixel from the unprocessed framebuffer.
//
// Placeholder for accessing the 3 trailing unprocessed frames (for simulating CRT on)
//   - Frame counter represents simulated CRT refresh cycle number.
//   - Always assign numbers to your refresh cycles. For reliability, keep a 3 frame trailing buffer.
//   - We index by frame counter because it is necessary for blending adjacent CRT refresh cycles, 
//      for the phosphor fade algorithm on old frame at bottom, and new frames at top.
//   - Framebuffer to retrieve from should be unscaled (e.g. original game resolution or emulator resolution).
//   - (If you do optional additional processing such as scaling+scanlines+masks, do it post-processing after this stage)
// DEMO version:
//   - We cheat by horizontally shifting shifted pixel reads from a texture.
// PRODUCTION version:
//   - Put your own code to retrieve a pixel from your series of unprocessed frame buffers.
//     IMPORTANT: For integration into firmware/software/emulators/games, this must be executed 
//     at refresh cycle granularity independently of your underlying games' framerate! 
//     There are three independent frequencies involved:
//     - Native Hz (your actual physical display)
//     - Simulated CRT Hz (Hz of simulated CRT tube)
//     - Underlying content frame rate (this shader doesn't need to know; TODO: Unless you plan to simulate VRR-CRT)
//
float3 getPixelFromOrigFrame(float2 uv, float getFromHzNumber, float currentHzCounter)
{
    float age = currentHzCounter - getFromHzNumber;
    if (age < 1)
    {
        return iChannel0.SampleLevel(iChannel_sampler, uv, 0.0).rgb;
    }
    if (age < 2)
    {
        return iChannel1.SampleLevel(iChannel_sampler, uv, 0.0).rgb;
    }
    if (age < 3)
    {
        return iChannel2.SampleLevel(iChannel_sampler, uv, 0.0).rgb;
    }
    return float3(0.0, 0.0, 0.0);
}

//-------------------------------------------------------------------------------------------------
// Rolling scan with phosphor fade + brightness redistributor algorithm
//
// New variable 'per-pixel MPRT' algorithm that mimics CRT phosphor decay too.
// - We emit as many photons as possible as early as possible, and if we can't emit it all (e.g. RGB 255)
//   then we continue emitting in the next refresh cycle until we've hit our target (gamma-compensated).
// - This is a clever trick to keep CRT simulation brighter but still benefit motion clarity of most colors.
//   Besides, real CRT tubes behave roughly similar too! (overexcited phosphor take longer to decay)
// - This also concurrently produces a phosphor-fade style behavior.
// - Win-win!
//
// How the light of a pixel is shared over time: the three colour channels of a pixel share ONE pulse, as long as the brightest channel.
// The colour ratio (hue and saturation) is the same in every refresh, only the brightness varies, so a saturated moving edge has no
// coloured fringe. Each channel still receives exactly its own total light; a pulse longer than a refresh cycle is cut and made brighter.
//
// The code between the TEST-EXTRACT markers is also compiled by the test bench (Tests/test_bfi_sweep.cpp): keep it plain
// C-like HLSL (float, float3, max, min, arithmetic only).

// TEST-EXTRACT-BEGIN
#define INTERVAL_OVERLAP(Astart, Aend, Bstart, Bend) max(0.0, min(Aend, Bend) - max(Astart, Bstart))

// One cycle of one pixel: the pulse is as long as the brightest channel and every channel is lit at the same
// share of its value, so the colour ratio never changes.
// A pulse longer than one refresh cycle would reach into cycles that are not summed (only the 3 trailing ones are), and its light would be
// lost (bright HDR pixels: SDR white sits above 1.0 in scRGB). Such a pulse is cut at one cycle and made brighter by the same factor for the
// three channels (same energy, same hue): this is the "boost of the light" of a bright pixel.
float3 mprtPulse(float3 photons, float start, float fStart, float fEnd, float framesPerHz)
{
    float len = max(photons.x, max(photons.y, photons.z));
    if (len <= 0.0)
        return float3(0.0, 0.0, 0.0);
    float shown = min(len, framesPerHz);
    return photons * (INTERVAL_OVERLAP(start, start + shown, fStart, fEnd) / shown);
}

// Light of one pixel in the output frame window [fStart, fStart + 1] (in frames), from the three trailing refresh cycles:
// colorPrev2 / colorPrev1 / colorCurr: the photon budget of each cycle (= how long the pulse lasts), tubeFrame: where the beam of this pixel starts.
float3 mprtLightHueSafe(float3 colorPrev2, float3 colorPrev1, float3 colorCurr, float tubeFrame, float fStart, float framesPerHz)
{
    float fEnd = fStart + 1.0;
    float3 result = mprtPulse(colorPrev2, tubeFrame - framesPerHz, fStart, fEnd, framesPerHz);
    result = result + mprtPulse(colorPrev1, tubeFrame, fStart, fEnd, framesPerHz);
    result = result + mprtPulse(colorCurr, tubeFrame + framesPerHz, fStart, fEnd, framesPerHz);
    return result;
}
// TEST-EXTRACT-END

//
// Parameters of getPixelSweep:
// - uv: position in the picture; tubePos: where the simulated beam reaches this pixel (0 at the top for a vertical scan).
// - rasterPos: normalized raster position [0..1] representing the current position of the simulated electron beam.
// - hzCounter: number of the simulated CRT refresh cycle; framesPerHz: output frames per CRT refresh (not necessarily an integer).
//
float3 getPixelSweep(float2 uv, float rasterPos, float hzCounter, float framesPerHz, float tubePos)
{
    // Get pixels from three consecutive refresh cycles
    float3 pixelPrev2 = srgb2linear(getPixelFromOrigFrame(uv, hzCounter - 2.0, hzCounter));
    float3 pixelPrev1 = srgb2linear(getPixelFromOrigFrame(uv, hzCounter - 1.0, hzCounter));
    float3 pixelCurr = srgb2linear(getPixelFromOrigFrame(uv, hzCounter, hzCounter));

    // Compute "photon budgets" for all three cycles
    float brightnessScale = framesPerHz * GAIN_VS_BLUR;
    float3 colorPrev2 = pixelPrev2 * brightnessScale;
    float3 colorPrev1 = pixelPrev1 * brightnessScale;
    float3 colorCurr = pixelCurr * brightnessScale;

    // Convert normalized values to frame space
    float tubeFrame = tubePos * framesPerHz;
    float fStart = rasterPos * framesPerHz;

    float3 result = mprtLightHueSafe(colorPrev2, colorPrev1, colorCurr, tubeFrame, fStart, framesPerHz);

    return linear2srgb(result);
}

struct VSIn
{
    uint vertexId : SV_VertexID;
};
                                      
struct VSOut
{
    float2 vTexCoord : TEXCOORD0;
    float tubePos : TEXCOORD1;
    float4 pos : SV_Position;
};

struct PSIn
{
    float2 vTexCoord : TEXCOORD0;
    float tubePos : TEXCOORD1;
};

struct PSOut
{
    float4 FragColor : SV_Target0;
};

//-------------------------------------------------------------------------------------------------
// Main Image Function
//
PSOut PSmain(PSIn input)
{
    PSOut output;
    
    // uv: Normalized coordinates ranging from (0,0) at the bottom-left to (1,1) at the top-right.
    float2 uv = input.vTexCoord;

    //-----------------------------------------------------------------------------------------
    // Get the swept version of the pixel
    float3 col = getPixelSweep(uv, param_rasterPos, param_hzCounter, EFFECTIVE_FRAMES_PER_HZ, input.tubePos);
    // black level: what is darker than the level becomes a pure black (true black between the lines, OLED / HDR)
    // (only when set: the output is left untouched otherwise, values above 1 are kept for HDR)
    if (param_blackLevel > 0.0)
        col = saturate((col - param_blackLevel) / max(1.0 - param_blackLevel, 0.0001));
    output.FragColor = float4(col, 1.0);
    return output;
}

//-------------------------------------------------------------------------------------------------
// Vertex Shader
//
VSOut VSmain(VSIn input)
{
    VSOut output;
    
    float tubePos;
    float2 vTexCoord;

    // hardcoded triangle
    if (input.vertexId == 0)
    {
        output.pos = float4(0.0, 2.0, 0.0, 1.0);
        vTexCoord = float2(0.5, -0.5);
    }
    else if (input.vertexId == 2)
    {
        output.pos = float4(-3.0, -1.0, 0.0, 1.0);
        vTexCoord = float2(-1.0, 1.0);
    }
    else if (input.vertexId == 1)
    {
        output.pos = float4(3.0, -1.0, 0.0, 1.0);
        vTexCoord = float2(2.0, 1.0);
    }
    
    // precalculate tubePos
    if (int(param_scanDirection) == 1)
    {
        tubePos = vTexCoord.y;
    }
    else
    {
        if (int(param_scanDirection) == 2)
        {
            tubePos = 1.0f - vTexCoord.y;
        }
        else
        {
            if (int(param_scanDirection) == 3)
            {
                tubePos = vTexCoord.x;
            }
            else
            {
                if (int(param_scanDirection) == 4)
                {
                    tubePos = 1.0f - vTexCoord.x;
                }
                else
                {
                    tubePos = 0.0f;
                }
            }
        }
    }
    
    output.tubePos = tubePos;
    output.vTexCoord = vTexCoord;
    return output;
}

//-------------------------------------------------------------------------------------------------
// Credits Reminder:
// Please credit BLUR BUSTERS & TIMOTHY LOTTES if this algorithm is used in your project/product.
// Hundreds of hours of research was done on related work that led to this algorithm.
//-------------------------------------------------------------------------------------------------
