/*
Nbeam 3D Vision: shader effect overlay -- "Stereo Emitter" panel (ImGui)
MIT License
*/

#pragma once

#include "EmitterSettings.h"
#include <string>
#include <vector>

namespace Nbeam
{

struct EmitterPanelContext
{
    double                   displayHz { 0 };   // measured output refresh
    int                      subFrames { 1 };   // output frames per content frame
    bool                     stereoBlocked { false }; // 3D is only available in Manual SubFrames mode
    bool                     outputOnProcess { false }; // the output covers the captured game (Full SBS impossible then)
    bool                     fullRestart { false };    // set by the panel: a complete restart of the renderer is needed (Full SBS toggled: the capture must start again)
    bool                     gameCapture { true };    // a game is captured (not the desktop): Full SBS exists only then
    bool                     queueBlocked { false };  // 3D needs at least 3 queued frames
    float                    fontSize { 16 };
    std::vector<std::string> status;            // EmitterManager::Status()
    std::vector<int>         comPorts;          // EnumerateComPorts()
    bool                     comScanned { false };
    std::string              aiStatus;          // "2D to 3D (AI)": state of the depth model
    float                    aiInferMs { 0 };   // time of one depth estimation
    float                    aiFps { 0 };       // depth maps per second
    float                    aiLatencyMs { 0 }; // image taken -> depth map ready
};

// Draws the collapsible "Stereo Emitter" section inside the current ImGui window.
// Returns true when a setting changed that only takes effect after a restart (emitter creation / USB programming).
// Live settings (swap eyes, sync offset, parity mode, drift threshold) are applied immediately by the renderer.
bool RenderEmitterPanel(StereoEmitterSettings& s, EmitterPanelContext& ctx);

} // namespace Nbeam
