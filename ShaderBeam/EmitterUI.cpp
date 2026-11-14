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

static const char* kSourceNames[] = { "Off", "Side by Side (SBS)", "Top and Bottom (TB)", "Checkerboard", "2D to 3D (AI)", "Interlaced horizontal", "Interlaced vertical" }; // (indexed by StereoSource)
static const int   kSourceOrder[7]  = { 0, 1, 2, 3, 5, 6, 4 };                                                                                         // (order shown in the list)

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

    if(ctx.queueBlocked)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), "%s", Tr("3D is not available below 3 queued frames. Set Queued Frames to 3 or more (Advanced) to use 3D."));
        ImGui::PopTextWrapPos();
        return false;
    }

    ImGui::PushItemWidth(ctx.fontSize * 16);

    const double syncHz   = s.EffectiveSyncHz(ctx.displayHz, ctx.subFrames);
    const double periodUs = syncHz > 0 ? 1e6 / syncHz : 0.0;

    // ---- 3D rendering ----
    SectionHeader(Tr("3D Rendering"));
    int src = (int)s.sourceFormat;
    const char* sourceNames[7] = { Tr(kSourceNames[0]), Tr(kSourceNames[1]), Tr(kSourceNames[2]), Tr(kSourceNames[3]), Tr(kSourceNames[4]), Tr(kSourceNames[5]), Tr(kSourceNames[6]) };
    static std::vector<std::string> missing;
    static bool                     wasOpen = false;
    {
        // Auto source: whatever is shown; nothing changes until a different 3D source is found. Unchecked, the format stays as it is
        bool autoSrc = s.autoSource;
        if(ToggleSwitch(Tr("Auto source"), &autoSrc))
            s.autoSource = autoSrc;
        Help(Tr("Checked: the 3D Source Format is hidden and stays as it is (even Off) until Nbeam detects a different 3D source (SBS, TB, Checkerboard or Interlaced) in the captured picture; it then switches to it by itself, as if you had chosen it, and follows the picture if it changes. Unchecked: the 3D Source Format is shown again and the format stays as it is."));
    }
    if(!s.autoSource) // (hidden while the Auto source is on: it chooses the format)
    {
        const bool srcOpen = ImGui::BeginCombo(Tr("3D Source Format"), sourceNames[std::clamp(src, 0, 6)]);
        if(srcOpen)
        {
            // 2D to 3D (AI) needs files next to the exe: checked each time the list is opened, greyed out with the missing files listed
            if(!wasOpen)
                missing = MissingAiFilesNextToExe();
            wasOpen = true;
            for(int k = 0; k < 7; k++)
            {
                const int  i     = kSourceOrder[k];
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
        Help(Tr("Layout of the 3D content you capture.\nOff: 3D is disabled and the emitters are switched off.\nSBS / TB / Checkerboard / Interlaced: each captured frame holds both eyes; Nbeam extracts one eye, scales it to full screen and shows it alternately.\nThe eye changes every SubFrames output frames (eye rate = display Hz / SubFrames, e.g. 240 Hz with SubFrames 4 = 60 Hz), so each eye gets its BFI / CRT sub-frames.\nThe emitters run whenever this is not Off. Changing this rebuilds the 3D renderer."));
    }
    if(s.sourceFormat == StereoSource::SideBySide)
    {
        // experimental Full SBS: only with a game capture whose output does not cover the game
        const bool avail = ctx.gameCapture && !ctx.outputOnProcess;
        ImGui::BeginDisabled(!avail);
        bool full = s.fullSbs && avail;
        if(ToggleSwitch(Tr("Full SBS (experimental)"), &full))
        {
            s.fullSbs       = full;
            restart         = true;
            ctx.fullRestart = true;
        }
        ImGui::EndDisabled();
        Help(Tr("The game window is automatically resized (borderless) to twice the screen width, e.g. 3840x1080 on a 1080p screen, 5120x1440 on a 1440p one, and given back as it was when this is unchecked. The left half is the left eye, the right half the right eye, each shown whole and not squeezed, one after the other. The game must accept the new window size (windowed or borderless, not exclusive fullscreen) and render both eyes itself. Only for a game capture, with an output that does not cover the game (not the Process output). Changing this restarts the renderer."));
        if(!avail)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), "%s", !ctx.gameCapture ? Tr("Full SBS is not possible on the desktop: it needs a game capture.") : Tr("Full SBS is not possible when the output covers the game (Process output): the window must stay its own size."));
            ImGui::PopTextWrapPos();
        }
        else if(s.fullSbs && s.fullSbsBad)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), "%s", Tr("Full SBS is impossible with this window: it must be twice as wide as the screen (e.g. 3840x1080 on 1080p). 3D is off until it is."));
            ImGui::PopTextWrapPos();
        }
    }
    if(s.sourceFormat != StereoSource::Off)
    {
        ImGui::TextDisabled(Tr("Each eye is shown for %d output frame(s); every captured frame provides one eye."), ctx.subFrames);
        // convergence: every 3D format (and the cars of the motion test, which always come out of the screen)
        // shown -100..+100 (stored 0..100 as before: +100 = 0, 0 = 50, -100 = 100)
        float conv = (50.0f - s.ai.convergence) * 2.0f;
        if(ImGui::SliderFloat(Tr("Convergence"), &conv, -100.0f, 100.0f, "%+.0f"))
            s.ai.convergence = std::clamp(50.0f - conv * 0.5f, 0.0f, 100.0f);
        Help(Tr("Where the picture sits relative to the screen, -100 to +100. Negative = into the depth of the screen, positive = out of the screen, 0 = the default depth (2D to 3D: split around the screen; SBS / TB / Checkerboard: as captured). The cars of the motion test use it too."));
    }

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
        ImGui::SliderFloat(Tr("Depth smoothing (%)"), &s.ai.smoothing, 0.0f, 100.0f, "%.0f");
        Help(Tr("Averages the depth map over time so it does not flicker. The stronger it is, the more the map lags behind the game: the game image is held back by the same amount (one depth map at 50 %, more above) so the map stays in step. Needs Map buffer."));
        ImGui::SliderFloat(Tr("Anti-halo (%)"), &s.ai.antiHalo, -300.0f, 300.0f, "%.0f");
        Help(Tr("Reduces the halo (smeared edges) around objects: the depth of a foreground object no longer spills onto the background next to it. Too high shrinks thin objects slightly. Negative values: the same, inverted (for an inverted depth, Swap Eyes checked)."));
        ToggleSwitch(Tr("Show depth map"), &s.ai.showDepth);
        Help(Tr("Shows the estimated depth (white = near, black = far) instead of the picture, to check the AI and adjust the settings."));
        ToggleSwitch(Tr("Map buffer"), &s.ai.mapBuffer);
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
        Help(Tr("Tune Sync Offset and Open Duration for your glasses with the sliders: they are remembered for each emitter rate."));
        ImGui::TextDisabled(Tr("Eye switch rate %.2f Hz  |  period %.0f us  |  half period %.0f us"), syncHz, periodUs, periodUs / 2);
        Help(Tr("Rate at which the glasses switch eye = display Hz / SubFrames (one content frame per eye).\nOpen Duration of the glasses must stay below the period."));

        // the rates that divide the display rate (display Hz / n) inside the range the glasses detect (80 to 165 Hz), 6 decimals
        std::vector<double> rates;
        for(int n = 1; n <= 20 && ctx.displayHz > 1.0; n++)
        {
            const double hz = ctx.displayHz / n;
            if(hz > 165.5)
                continue;
            if(hz < 79.5)
                break;
            rates.push_back(hz);
        }
        if(rates.size() >= 2)
        {
            if(ToggleSwitch(Tr("Sync Rate Auto"), &s.syncAuto))
            {
                if(!s.syncAuto && syncHz >= 1.0)
                    s.syncRateHz = syncHz; // start from the rate in use
                s.autoDivisor = 0;
                restart       = true;
            }
            Help(Tr("Checked: the emitter starts on the highest rate the display offers, then it follows the game frame rate at the interval below: it picks the lowest rate whose eye pairs are a whole multiple of the game fps (480 Hz display, 40 fps: 80 Hz; 24 fps: 96 Hz, two pairs per game frame). Several rates need a very high refresh rate (240 Hz and above). The glasses timing and the Clear Frames are kept for each rate: set them on every rate you use."));
            if(s.syncAuto)
            {
                SliderFloatStep(Tr("Sync Auto interval"), &s.syncAutoSec, 0.5f, 2.0f, "%.1f s", 0.5f);
                Help(Tr("How often Sync Auto measures the game frame rate and picks the emitter rate, from 0.5 to 2 seconds."));
            }
        }
        else if(rates.size() == 1)
        {
            s.syncAuto = true; // the only rate: nothing to choose, nothing to follow
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", Tr("Sync Rate Auto is hidden: this display offers a single emitter rate (display Hz / n between 80 and 165 Hz), so there is nothing to choose between. A very high refresh rate (240 Hz and above) offers several rates: Auto then follows the game frame rate."));
            ImGui::PopTextWrapPos();
        }
        if(!s.syncAuto)
        {
            if(!rates.empty())
            {
                int cur = 0;
                for(int i = 1; i < (int)rates.size(); i++)
                    if(std::fabs(rates[i] - s.syncRateHz) < std::fabs(rates[cur] - s.syncRateHz))
                        cur = i;
                if(std::fabs(rates[cur] - s.syncRateHz) > 1e-9 && std::fabs(rates[cur] - s.syncRateHz) < 0.5)
                    s.syncRateHz = rates[cur]; // the display rate moved a little: stay on the exact divisor
                char preview[64];
                snprintf(preview, sizeof(preview), "%.3f Hz", s.syncRateHz);
                if(ImGui::BeginCombo(Tr("Sync Rate Hz"), preview))
                {
                    for(int i = 0; i < (int)rates.size(); i++)
                    {
                        char item[64];
                        snprintf(item, sizeof(item), "%.3f Hz", rates[i]);
                        if(ImGui::Selectable(item, i == cur && std::fabs(rates[cur] - s.syncRateHz) < 0.5))
                        {
                            s.syncRateHz = rates[i];
                            restart      = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            Help(Tr("Rate at which the emitter syncs each eye, picked among the rates that divide the display rate (display Hz / n) and that the glasses can detect (80 to 165 Hz). SubFrames = display Hz / this rate.\nWith BFI/CRT sim (SubFrames 2) the glasses run at half the display rate."));
        }

        const int range = periodUs > 0 ? (int)periodUs : 10000;
        if(syncHz >= 1.0)
            ImGui::TextDisabled(Tr("Sync Offset, Open Duration, Clear Frames (BFI) and Swap Eyes are saved for this rate only (%.3f Hz): set them again on each rate you use."), syncHz);
        SliderIntStep(Tr("Sync Offset (us)"), &s.syncOffsetUs, -range, 0, 1); // negative only
        Help(Tr("Time offset from the real start of the frame on screen before the emitter is signalled (the same whatever the queued frames).\nNegative only (0 to minus one eye period): the glasses switch before vsync (the other eye is signalled one period earlier)."));

        // Live slider: the emitter is reprogrammed on the fly (at most every 50 ms) while you drag; Right-click to type a value.
        const float sliderMax = periodUs > 0 ? (float)periodUs : 10000.0f;
        SliderFloatStep(Tr("Open Duration (us)"), &s.nvidia.openDurationUs, 0.0f, sliderMax, "%.0f", 1.0f);
        Help(Tr("How long an eye stays open. Applied live while you drag (Right-click to type a value).\nMust be less than the eye period shown above or there is no stereo."));
        if(s.nvidia.openDurationUs > periodUs && periodUs > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.0f, 1.0f), Tr("Open Duration exceeds the eye period"));

        ToggleSwitch(Tr("Offset correction"), &s.offsetCorrection);
        Help(Tr("At each micro-desync (the display shifts the picture by a refresh or more, the present grid jumps or Windows stops composing the picture) the Open Duration is changed by 1 us at once, which makes the emitter restart its offset (with BFI / CRT sub-frames, in the first black frame, where the glasses re-locking cannot be seen). The value alternates: 1 us less than the value you set, then the value you set again at the next one (only the value you set is saved). At most twice a second, and not while Windows composes the picture. Warning: this setting can cause slight flickering. If you are sensitive to it, switch it off and increase the Queued Frames."));

        ToggleSwitch(Tr("Swap Eyes"), &s.swapEyes);
        Help(Tr("Reverses the eye order. With 2D to 3D (AI) it also inverts the depth (near and far are swapped). Saved for each emitter rate. Hotkey: see the Hotkeys section."));

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
