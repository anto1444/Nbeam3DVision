/*
ShaderBeam: shader effect overlay -- "Stereo Emitter" panel (ImGui)
MIT License
*/

#pragma once

#include "EmitterSettings.h"
#include <string>
#include <vector>

namespace ShaderBeam
{

struct EmitterPanelContext
{
    double                   displayHz { 0 };   // measured output refresh
    int                      subFrames { 1 };   // output frames per content frame
    bool                     stereoBlocked { false }; // 3D is only available in Manual SubFrames mode
    float                    fontSize { 16 };
    std::vector<std::string> status;            // EmitterManager::Status()
    std::vector<int>         comPorts;          // EnumerateComPorts()
    bool                     comScanned { false };
    std::string              aiStatus;          // "2D to 3D (AI)": state of the depth model
    float                    aiInferMs { 0 };   // time of one depth estimation
    float                    aiFps { 0 };       // depth maps per second
};

// Draws the collapsible "Stereo Emitter" section inside the current ImGui window.
// Returns true when a setting changed that only takes effect after a restart (emitter creation / USB programming).
// Live settings (swap eyes, sync offset, parity mode, drift threshold) are applied immediately by the renderer.
bool RenderEmitterPanel(StereoEmitterSettings& s, EmitterPanelContext& ctx);

} // namespace ShaderBeam
