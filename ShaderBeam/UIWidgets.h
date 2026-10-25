/*
Nbeam 3D Vision -- small ImGui helpers: sliders with "-" / "+" buttons for fine adjustment
MIT License
*/

#pragma once

#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace ShaderBeam
{

// accent color of the section titles (set by the HUD every frame)
inline ImVec4 gSectionColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

// section title with a line, in the accent color
inline void SectionHeader(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, gSectionColor);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

namespace detail
{
// draws "label" (the part before "##") after the buttons
inline void StepLabel(const char* label)
{
    const char* hash = strstr(label, "##");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if(hash)
        ImGui::TextUnformatted(label, hash);
    else
        ImGui::TextUnformatted(label);
}
} // namespace detail

// Slider + [-] [+] buttons (held buttons repeat). The slider keeps the item width currently set (PushItemWidth).
inline bool SliderFloatStep(const char* label, float* v, float lo, float hi, const char* fmt, float step)
{
    bool         changed = false;
    const auto&  st      = ImGui::GetStyle();
    const float  btn     = ImGui::GetFrameHeight();
    const float  width   = ImGui::CalcItemWidth();
    ImGui::PushID(label);
    ImGui::SetNextItemWidth((std::max)(width - 2 * (btn + st.ItemInnerSpacing.x), btn * 2));
    changed |= ImGui::SliderFloat("##slider", v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if(ImGui::Button("-", ImVec2(btn, btn)))
    {
        *v      = (std::max)(lo, *v - step);
        changed = true;
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if(ImGui::Button("+", ImVec2(btn, btn)))
    {
        *v      = (std::min)(hi, *v + step);
        changed = true;
    }
    ImGui::PopItemFlag();
    detail::StepLabel(label);
    ImGui::PopID();
    return changed;
}

inline bool SliderIntStep(const char* label, int* v, int lo, int hi, int step = 1, const char* zeroText = nullptr, int zeroValue = 0)
{
    bool         changed = false;
    const auto&  st      = ImGui::GetStyle();
    const float  btn     = ImGui::GetFrameHeight();
    const float  width   = ImGui::CalcItemWidth();
    ImGui::PushID(label);
    ImGui::SetNextItemWidth((std::max)(width - 2 * (btn + st.ItemInnerSpacing.x), btn * 2));
    changed |= ImGui::SliderInt("##slider", v, lo, hi, (zeroText && *v == zeroValue) ? zeroText : "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if(ImGui::Button("-", ImVec2(btn, btn)))
    {
        *v      = (std::max)(lo, *v - step);
        changed = true;
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if(ImGui::Button("+", ImVec2(btn, btn)))
    {
        *v      = (std::min)(hi, *v + step);
        changed = true;
    }
    ImGui::PopItemFlag();
    detail::StepLabel(label);
    ImGui::PopID();
    return changed;
}

// step to use for a generic float parameter of the given range
inline float AutoStep(float lo, float hi)
{
    const float range = hi - lo;
    if(range >= 100.0f)
        return 1.0f;
    if(range >= 1.0f)
        return 0.01f;
    return range / 100.0f;
}

} // namespace ShaderBeam
