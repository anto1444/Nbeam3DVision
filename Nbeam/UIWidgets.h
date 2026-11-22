/*
Nbeam 3D Vision -- small ImGui helpers: sliders with "-" / "+" buttons for fine adjustment
MIT License
*/

#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace Nbeam
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


// Windows 11 style switch (a pill with a round knob; accent color when on) instead of a check box. The label is on the right, like the check box.
// mixed = some of the things it stands for are on and some are off: the knob stays in the middle, and a click turns it on
inline bool ToggleSwitch(const char* label, bool* v, bool mixed = false)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if(window->SkipItems)
        return false;
    ImGuiContext&     g     = *GImGui;
    const ImGuiStyle& st    = g.Style;
    const ImGuiID     id    = window->GetID(label);
    const ImVec2      lsize = ImGui::CalcTextSize(label, nullptr, true);
    const float       h     = ImGui::GetFrameHeight();
    const float       w     = h * 1.9f;
    const ImVec2      pos   = window->DC.CursorPos;
    const ImRect      total(pos, ImVec2(pos.x + w + (lsize.x > 0.0f ? st.ItemInnerSpacing.x + lsize.x : 0.0f), pos.y + h));
    ImGui::ItemSize(total, st.FramePadding.y);
    if(!ImGui::ItemAdd(total, id))
        return false;
    bool       hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(total, id, &hovered, &held);
    if(pressed)
    {
        *v = mixed ? true : !*v;
        ImGui::MarkItemEdited(id);
    }
    const bool  disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const float alpha    = disabled ? 0.4f : 1.0f;
    // progressive slide (0.5 s): t goes from 0 (off) to 1 (on), kept per switch
    float&      t        = *window->DC.StateStorage->GetFloatRef(id, mixed ? 0.5f : *v ? 1.0f : 0.0f);
    const float target   = mixed ? 0.5f : *v ? 1.0f : 0.0f;
    if(t != target)
    {
        const float step = g.IO.DeltaTime / 0.5f;
        t                = t < target ? (std::min)(t + step, target) : (std::max)(t - step, target);
    }
    const float e = t * t * (3.0f - 2.0f * t); // smooth start and end
    ImDrawList* dl = window->DrawList;
    const ImVec2 p0(pos.x, pos.y + h * 0.1f), p1(pos.x + w, pos.y + h * 0.9f);
    const float  rr     = (p1.y - p0.y) * 0.5f;
    const float  cy     = (p0.y + p1.y) * 0.5f;
    ImVec4       accent = gSectionColor;
    if(hovered)
        accent = ImVec4(accent.x * 0.9f, accent.y * 0.9f, accent.z * 0.9f, accent.w);
    ImVec4 line = ImGui::GetStyle().Colors[ImGuiCol_Text];
    line.w      = (hovered ? 0.95f : 0.65f) * alpha;
    // fill fades in, outline fades out
    ImVec4 fill = accent;
    fill.w      = e * alpha;
    if(e < 1.0f)
    {
        ImVec4 ol = line;
        ol.w *= 1.0f - e;
        dl->AddRect(p0, p1, ImGui::GetColorU32(ol), rr, 0, 1.5f);
    }
    if(e > 0.0f)
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(fill), rr);
    // the knob slides, grows a little and goes from the outline color to dark
    const float  kr = rr * ((held ? 0.55f : 0.45f) + (0.62f - 0.45f) * e);
    const float  kx = (p0.x + rr) + ((p1.x - rr) - (p0.x + rr)) * e;
    const ImVec4 dark(0.04f, 0.04f, 0.04f, alpha);
    const ImVec4 kc(line.x + (dark.x - line.x) * e, line.y + (dark.y - line.y) * e, line.z + (dark.z - line.z) * e, line.w + (alpha - line.w) * e);
    dl->AddCircleFilled(ImVec2(kx, cy), kr, ImGui::GetColorU32(kc), 24);
    if(lsize.x > 0.0f)
    {
        const ImVec2 tp(pos.x + w + st.ItemInnerSpacing.x, pos.y + (h - lsize.y) * 0.5f);
        ImGui::RenderText(tp, label);
    }
    return pressed;
}

// Vertical selector between two choices: the two labels one above the other, and to their right a vertical switch. Off (false) = the knob is up,
// towards the top choice; on (true) = the knob is down, towards the bottom choice. The chosen label is brighter. Returns true when it changes.
inline bool VerticalSelector(const char* id, const char* top, const char* bottom, bool* v)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if(window->SkipItems)
        return false;
    ImGuiContext&     g    = *GImGui;
    const ImGuiStyle& st   = g.Style;
    const ImGuiID     wid  = window->GetID(id);
    const float       h    = ImGui::GetFrameHeight();
    const float       tw   = (std::max)(ImGui::CalcTextSize(top).x, ImGui::CalcTextSize(bottom).x);
    const float       sw   = h * 0.95f;                    // width of the switch
    const ImVec2      pos  = window->DC.CursorPos;
    const ImRect      total(pos, ImVec2(pos.x + tw + st.ItemInnerSpacing.x * 2.0f + sw, pos.y + h * 2.0f));
    ImGui::ItemSize(total, st.FramePadding.y);
    if(!ImGui::ItemAdd(total, wid))
        return false;
    bool       hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(total, wid, &hovered, &held);
    bool       changed = false;
    if(pressed)
    {
        // a click on the upper half chooses the top choice, on the lower half the bottom one; on the switch it flips
        const float mouseY = g.IO.MousePos.y;
        const float mouseX = g.IO.MousePos.x;
        const bool  onSwitch = mouseX >= pos.x + tw + st.ItemInnerSpacing.x;
        const bool  want     = onSwitch ? !*v : mouseY >= pos.y + h;
        if(want != *v)
        {
            *v      = want;
            changed = true;
            ImGui::MarkItemEdited(wid);
        }
    }
    const bool  disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const float alpha    = disabled ? 0.4f : 1.0f;
    float&      t        = *window->DC.StateStorage->GetFloatRef(wid, *v ? 1.0f : 0.0f);
    const float target   = *v ? 1.0f : 0.0f;
    if(t != target)
    {
        const float step = g.IO.DeltaTime / 0.35f;
        t                = t < target ? (std::min)(t + step, target) : (std::max)(t - step, target);
    }
    const float e = t * t * (3.0f - 2.0f * t);
    ImDrawList* dl = window->DrawList;
    ImVec4      text = st.Colors[ImGuiCol_Text];
    ImVec4      dim  = text;
    dim.w *= 0.45f * alpha;
    text.w *= alpha;
    // the labels: the chosen one in full colour, the other dimmed
    dl->AddText(ImVec2(pos.x, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(*v ? dim : text), top);
    dl->AddText(ImVec2(pos.x, pos.y + h + (h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(*v ? text : dim), bottom);
    // the switch: a vertical pill, the knob slides from the top (off) to the bottom (on)
    const float  sx = pos.x + tw + st.ItemInnerSpacing.x * 2.0f;
    const ImVec2 p0(sx, pos.y + h * 0.1f), p1(sx + sw, pos.y + h * 1.9f);
    const float  rr = sw * 0.5f;
    ImVec4       accent = gSectionColor;
    if(hovered)
        accent = ImVec4(accent.x * 0.9f, accent.y * 0.9f, accent.z * 0.9f, accent.w);
    ImVec4 line = st.Colors[ImGuiCol_Text];
    line.w      = (hovered ? 0.95f : 0.65f) * alpha;
    ImVec4 fill = accent;
    fill.w      = e * alpha;
    if(e < 1.0f)
    {
        ImVec4 ol = line;
        ol.w *= 1.0f - e;
        dl->AddRect(p0, p1, ImGui::GetColorU32(ol), rr, 0, 1.5f);
    }
    if(e > 0.0f)
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(fill), rr);
    const float  kr = rr * ((held ? 0.55f : 0.45f) + (0.62f - 0.45f) * e) * 1.6f;
    const float  ky = (p0.y + rr) + ((p1.y - rr) - (p0.y + rr)) * e;
    const ImVec4 dark(0.04f, 0.04f, 0.04f, alpha);
    const ImVec4 kc(line.x + (dark.x - line.x) * e, line.y + (dark.y - line.y) * e, line.z + (dark.z - line.z) * e, line.w + (alpha - line.w) * e);
    dl->AddCircleFilled(ImVec2(sx + rr, ky), (std::min)(kr, rr * 0.92f), ImGui::GetColorU32(kc), 24);
    return changed;
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
// snap > 0: the [-] / [+] buttons also round the value to a multiple of snap (2.25 -> 2.2 / 2.3 with a 0.1 step)
inline bool SliderFloatStep(const char* label, float* v, float lo, float hi, const char* fmt, float step, float snap = 0.0f)
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
        if(snap > 0.0f)
            *v = (std::clamp)(std::round(*v / snap) * snap, lo, hi);
        changed = true;
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if(ImGui::Button("+", ImVec2(btn, btn)))
    {
        *v      = (std::min)(hi, *v + step);
        if(snap > 0.0f)
            *v = (std::clamp)(std::round(*v / snap) * snap, lo, hi);
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

} // namespace Nbeam
