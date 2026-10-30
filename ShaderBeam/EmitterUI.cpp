/*
ShaderBeam: shader effect overlay -- "Stereo Emitter" panel (ImGui)
MIT License
*/

#include "stdafx.h"
#include "EmitterUI.h"
#include "imgui.h"
#include "UIWidgets.h"
#include "Lang.h"
#include "DepthAI.h"
#include <cstdio>
#include <map>
#include <string>

namespace ShaderBeam
{

static void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(450.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Sections are always shown directly (no collapsible sub-menus).
static bool OpenNode(const char* label, bool header)
{
    if(!header)
        SectionHeader(label);
    return true;
}

static const char* kSourceNames[] = { "Off", "Side by Side (SBS)", "Top and Bottom (TB)", "Checkerboard", "2D to 3D (AI)" };

bool RenderEmitterPanel(StereoEmitterSettings& s, EmitterPanelContext& ctx)
{
    bool restart = false;
    if(!OpenNode(Tr("Stereo Emitter"), true))
        return false;

    if(ctx.stereoBlocked)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "%s", Tr("3D is only available when SubFrames Mode is Manual."));
        ImGui::TextDisabled("%s", Tr("Auto changes the frame timing, so the eye sequence of the glasses could not stay in step. Switch SubFrames Mode to Manual to use 3D."));
        ImGui::PopTextWrapPos();
        return false;
    }

    ImGui::PushItemWidth(ctx.fontSize * 16);

    const double syncHz   = s.EffectiveSyncHz(ctx.displayHz, ctx.subFrames);
    const double periodUs = syncHz > 0 ? 1e6 / syncHz : 0.0;

    // ---- 3D rendering ----
    SectionHeader(Tr("3D Rendering"));
    int src = (int)s.sourceFormat;
    const char* sourceNames[5] = { Tr(kSourceNames[0]), Tr(kSourceNames[1]), Tr(kSourceNames[2]), Tr(kSourceNames[3]), Tr(kSourceNames[4]) };
    static std::vector<std::string> missing;
    static bool                     wasOpen = false;
    if(ImGui::BeginCombo(Tr("3D Source Format"), sourceNames[std::clamp(src, 0, 4)]))
    {
        // 2D to 3D (AI) needs files next to the exe: checked each time the list is opened, greyed out with the missing files listed
        if(!wasOpen)
            missing = MissingAiFilesNextToExe();
        wasOpen = true;
        for(int i = 0; i < 5; i++)
        {
            const bool aiOff = i == 4 && !missing.empty();
            if(ImGui::Selectable(sourceNames[i], i == src, aiOff ? ImGuiSelectableFlags_Disabled : 0) && !aiOff)
            {
                src            = i;
                s.sourceFormat = (StereoSource)src;
                restart        = true;
            }
            if(aiOff)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
                ImGui::TextUnformatted(Tr("  Missing next to the exe:"));
                for(const auto& m : missing)
                    ImGui::Text("    - %s", m.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndCombo();
    }
    else
        wasOpen = false;
    Help(Tr("Layout of the 3D content you capture.\nOff: 3D is disabled and the emitters are switched off.\nSBS / TB / Checkerboard: each captured frame holds both eyes; ShaderBeam extracts one eye, scales it to full screen and shows it alternately.\nThe eye changes every SubFrames output frames (eye rate = display Hz / SubFrames, e.g. 240 Hz with SubFrames 4 = 60 Hz), so each eye gets its BFI / CRT sub-frames.\nThe emitters (NVIDIA 3D Vision, ComPort) run whenever this is not Off. Changing this restarts the renderer."));
    if(s.sourceFormat != StereoSource::Off)
        ImGui::TextDisabled(Tr("Each eye is shown for %d output frame(s); every captured frame provides one eye."), ctx.subFrames);

    // ---- 2D to 3D (AI): depth model parameters ----
    if(s.sourceFormat == StereoSource::AiDepth)
    {
        SectionHeader(Tr("2D to 3D (AI)"));
        const std::string& st = ctx.aiStatus;
        if(st.rfind("Ready", 0) == 0)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.3f, 1.0f), "%s", st.c_str());
        else if(st.empty() || st.rfind("Loading", 0) == 0)
            ImGui::TextDisabled("%s", st.empty() ? "..." : st.c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "%s", st.c_str());
        if(st.rfind("Ready", 0) == 0)
        {
            ImGui::TextDisabled(Tr("Depth: %.1f ms per map, %.1f maps/s"), ctx.aiInferMs, ctx.aiFps);
            if(ctx.aiLatencyMs > 0.0f)
                ImGui::TextDisabled(Tr("Depth delay: %.0f ms (with Map buffer, the image is held back by this much)"), ctx.aiLatencyMs);
        }
        Help(Tr("A plain 2D picture is turned into 3D: an AI model estimates the depth of every pixel (needs onnxruntime.dll and the model file next to the exe), then the left and right eyes are built from it."));

        ImGui::SliderInt(Tr("3D framerate (fps)"), &s.ai.depthFps, 1, 80);
        Help(Tr("Maximum number of depth maps the AI computes per second (1 to 80). The eyes are still drawn at every frame; this only limits how often the depth is refreshed. Lower it to leave GPU time to the game; the real rate is shown above and cannot exceed what the PC can do."));

        static const int   kWidths[]  = { 224, 308, 420, 518, 672, 840, 1008 };
        static const char* kLabels[]  = { "224", "308", "420", "518", "672", "840", "1008" };
        int                widthIdx   = 1;
        for(int i = 0; i < 7; i++)
            if(std::abs(kWidths[i] - s.ai.modelWidth) < std::abs(kWidths[widthIdx] - s.ai.modelWidth))
                widthIdx = i;
        if(ImGui::Combo(Tr("AI resolution (px)"), &widthIdx, kLabels, 7))
            s.ai.modelWidth = kWidths[widthIdx];
        Help(Tr("Width of the picture given to the AI (the height follows the screen ratio). Lower = faster depth updates, higher = finer depth. Applied immediately (the model reloads)."));

        ImGui::SliderFloat(Tr("Depth Strength (%)"), &s.ai.strength, 0.0f, 10.0f, "%.1f");
        Help(Tr("Maximum parallax between the two eyes, as a percentage of the screen width. 0 = flat; around 2-4 is comfortable."));
        ImGui::SliderFloat(Tr("Convergence (%)"), &s.ai.convergence, 0.0f, 100.0f, "%.0f");
        Help(Tr("Which depth stays on the screen plane. 0 = everything pops out of the screen, 100 = everything goes behind it; 50 splits the depth around the screen."));
        ImGui::SliderFloat(Tr("Depth smoothing (%)"), &s.ai.smoothing, 0.0f, 95.0f, "%.0f");
        Help(Tr("Averages the depth map over time so it does not flicker. Higher = steadier but slower to follow fast changes."));
        ImGui::SliderFloat(Tr("Anti-halo (%)"), &s.ai.antiHalo, 0.0f, 300.0f, "%.0f");
        Help(Tr("Reduces the halo (smeared edges) around objects: the depth of a foreground object no longer spills onto the background next to it. Too high shrinks thin objects slightly."));
        ImGui::Checkbox(Tr("Use GPU (DirectML)"), &s.ai.useGpu);
        Help(Tr("Runs the AI on the graphics card (much faster). If DirectML is not available the processor is used instead. Applied immediately (the model reloads)."));
        ImGui::Checkbox(Tr("Show depth map"), &s.ai.showDepth);
        Help(Tr("Shows the estimated depth (white = near, black = far) instead of the picture, to check the AI and adjust the settings."));
        ImGui::Checkbox(Tr("Map buffer"), &s.ai.mapBuffer);
        Help(Tr("Keeps the depth in step with the picture: each picture is held back until the depth map computed from it is ready (adds that delay as latency, shown above). Unchecked: no synchronisation, the latest depth map is used on the current picture (no added latency, the depth can lag behind fast motion). Works on top of the frame buffer. Needs a captured program (.exe)."));
    }

    // 3D off: no emitter / 3D adjustment settings
    if(s.sourceFormat == StereoSource::Off)
    {
        ImGui::PopItemWidth();
        return restart;
    }

    // ---- NVIDIA 3D Vision emitter: rate and sync first, then the open duration, then Swap Eyes ----
    if(OpenNode(Tr("NVIDIA 3D Vision emitter"), false))
    {
        ImGui::TextDisabled(Tr("Active whenever 3D Source Format is not Off."));
        Help(Tr("Needs the NV3DVisionUSB driver (3dvisioncontrollerdriver.exe from WibbleWobble, or the 425.31 NVIDIA package).\nThe monitor must already be in its 3D mode: ShaderBeam does not switch it through NvAPI.\nNo automatic timings: tune Open Duration with the slider."));
        ImGui::TextDisabled(Tr("Eye switch rate %.2f Hz  |  period %.0f us  |  half period %.0f us"), syncHz, periodUs, periodUs / 2);
        Help(Tr("Rate at which the glasses switch eye = display Hz / SubFrames (one content frame per eye).\nOpen Duration of the glasses must stay below the period."));

        if(ImGui::Checkbox(Tr("Sync Rate Auto"), &s.syncAuto))
        {
            if(!s.syncAuto && syncHz >= 1.0)
                s.syncRateHz = (float)(std::round(syncHz * 100.0) / 100.0); // start from the rate in use
            restart = true;
        }
        Help(Tr("Checked: the emitter rate is picked from the display rate (display Hz / n, between 80 and 165 Hz, closest to 120) and SubFrames follows. Out of that range nothing changes and the emitter is off.\nUnchecked: you type the emitter rate and SubFrames follows (display Hz / rate)."));
        if(s.rateBlocked)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(Tr("No emitter rate between 80 and 165 Hz fits this display: the emitter is off."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        if(!s.syncAuto)
        {
            if(ImGui::InputFloat(Tr("Sync Rate Hz"), &s.syncRateHz, 0.01f, 1.0f, "%.3f"))
            {
                s.syncRateHz = std::clamp(s.syncRateHz, 1.0f, 1000.0f);
                restart      = true;
            }
            Help(Tr("Rate at which the emitter syncs each eye. SubFrames = display Hz / this rate (rounded).\nWith BFI/CRT sim (SubFrames 2) the glasses run at half the display rate."));
        }

        const int range = periodUs > 0 ? (int)periodUs : 10000;
        SliderIntStep(Tr("Sync Offset (us)"), &s.syncOffsetUs, -range, range, 1);
        Help(Tr("Time offset from the real start of the frame on screen before the emitter is signalled (the same whatever the queued frames).\nNegative values shutter the glasses before vsync (the other eye is signalled one period earlier)."));

        // Live slider: the emitter is reprogrammed on the fly (at most every 50 ms) while you drag; Right-click to type a value.
        const float sliderMax = periodUs > 0 ? (float)periodUs : 10000.0f;
        SliderFloatStep(Tr("Open Duration (us)"), &s.nvidia.openDurationUs, 0.0f, sliderMax, "%.0f", 1.0f);
        Help(Tr("How long an eye stays open. Applied live while you drag (Right-click to type a value).\nMust be less than the eye period shown above or there is no stereo."));
        if(s.nvidia.openDurationUs > periodUs && periodUs > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.0f, 1.0f), Tr("Open Duration exceeds the eye period"));

        ImGui::Checkbox(Tr("Swap Eyes"), &s.swapEyes);
        Help(Tr("Reverses the eye order. With 2D to 3D (AI) it also inverts the depth (near and far are swapped). Hotkey: see the Hotkeys section."));

        // timing of the glasses: automatic (display scanout when Windows gives the frame statistics, classic otherwise)
        if(s.scanoutSyncState > 0)
            ImGui::TextDisabled(Tr("Sync: display scanout (DXGI), %d missed refreshes corrected"), s.scanoutResyncs);
        else if(s.scanoutSyncState < 0)
            ImGui::TextDisabled("%s", Tr("Sync: classic (frame statistics not available, picture composed by Windows)"));
        Help(Tr("Automatic. The glasses follow the real refreshes of the display (DXGI frame statistics) whenever Windows gives them; otherwise the classic timing is used, corrected by the delay measured before. The Sync Offset is always measured from the real start of the frame on screen: it does not need to be tuned again after changing the queued frames. An overload no longer shifts the glasses (late frames are put back on the regular rhythm of the display)."));
    }

    // ---- ComPort ----
    if(OpenNode(Tr("ComPort (Arduino VESA / IR sender)"), false))
    {
        if(!ctx.comScanned)
        {
            ctx.comPorts   = EnumerateComPorts();
            ctx.comScanned = true;
        }
        char current[16] = "None";
        if(s.comPort.comPortNo >= 0)
            snprintf(current, sizeof(current), "COM%d", s.comPort.comPortNo);
        if(ImGui::BeginCombo(Tr("COM Port"), s.comPort.comPortNo >= 0 ? current : Tr("None")))
        {
            if(ImGui::Selectable(Tr("None"), s.comPort.comPortNo < 0))
            {
                s.comPort.comPortNo = -1;
                restart             = true;
            }
            for(int port : ctx.comPorts)
            {
                char name[16];
                snprintf(name, sizeof(name), "COM%d", port);
                if(ImGui::Selectable(name, port == s.comPort.comPortNo))
                {
                    s.comPort.comPortNo = port;
                    restart             = true;
                }
            }
            ImGui::EndCombo();
        }
        Help(Tr("Serial port of the Arduino running WWVESASender.ino / WWIRSender.ino (115200 baud). None = disabled."));
        ImGui::SameLine();
        if(ImGui::Button(Tr("Rescan")))
            ctx.comScanned = false;
    }

    ImGui::PopItemWidth();

    if(!ctx.status.empty())
    {
        SectionHeader(Tr("Status"));
        for(const auto& line : ctx.status)
            ImGui::TextWrapped("%s", line.c_str());
    }
    return restart;
}

} // namespace ShaderBeam
