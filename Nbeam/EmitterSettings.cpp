/*
Nbeam 3D Vision: shader effect overlay -- emitter settings <-> Nbeam ini ([emitter] section)
MIT License
*/

#include "stdafx.h"
#include "EmitterIni.h"
#include <cmath>

namespace Nbeam
{

namespace
{
template <typename T> void Put(mINI::INIMap<std::string>& m, const char* k, T v) { m[k] = std::to_string(v); }
void                       Put(mINI::INIMap<std::string>& m, const char* k, bool v) { m[k] = v ? "1" : "0"; }

void Get(const mINI::INIMap<std::string>& m, const char* k, int& v, int lo, int hi)
{
    if(!m.has(k))
        return;
    try { v = std::clamp(std::stoi(m.get(k)), lo, hi); } catch(...) { }
}
void Get(const mINI::INIMap<std::string>& m, const char* k, float& v, float lo, float hi)
{
    if(!m.has(k))
        return;
    try { const float x = std::stof(m.get(k)); if(std::isfinite(x)) v = std::clamp(x, lo, hi); } catch(...) { }
}
void Get(const mINI::INIMap<std::string>& m, const char* k, double& v, double lo, double hi)
{
    if(!m.has(k))
        return;
    try { const double x = std::stod(m.get(k)); if(std::isfinite(x)) v = std::clamp(x, lo, hi); } catch(...) { }
}
void Get(const mINI::INIMap<std::string>& m, const char* k, bool& v)
{
    if(m.has(k))
        v = m.get(k) == "1";
}
} // namespace

void SaveEmitterSettings(mINI::INIStructure& ini, const StereoEmitterSettings& s)
{
    auto& e = ini["emitter"];
    Put(e, "sourceFormat", s.autoSource ? 0 : (int)s.sourceFormat); // (Auto source: the detected format is not kept, 3D starts again when a source is found)
    Put(e, "autoSource", s.autoSource);
    Put(e, "syncAuto", s.syncAuto);
    Put(e, "syncRateHz", s.syncRateHz);
    Put(e, "syncOffsetUs", s.syncOffsetUs);
    Put(e, "syncAutoSec", s.syncAutoSec);
    Put(e, "scanoutSync", s.scanoutSync);
    Put(e, "fullSbs", s.fullSbs);
    Put(e, "offsetCorrection", s.offsetCorrection);
    Put(e, "desyncMethod", s.desyncMethod);
    if(s.expPeriodMarginPct > 0.0f)
        Put(e, "expPeriodMarginPct", s.expPeriodMarginPct);
    Put(e, "swapEyes", s.swapEyes);
    Put(e, "comPortNo", s.comPort.comPortNo);
    Put(e, "nvOpenDurationUs", s.nvidia.Saved());
    {
        // timing of the glasses for each emitter rate: hz,offset,open,clear frames,swap eyes|hz,...
        std::string tm;
        for(int i = 0; i < s.timingCount; i++)
        {
            const bool live = i == s.timingActive;
            if(!tm.empty())
                tm += "|";
            tm += std::to_string(s.timings[i].hz) + "," + std::to_string(live ? s.syncOffsetUs : s.timings[i].offsetUs) + "," + std::to_string((int)(live ? s.nvidia.Saved() : s.timings[i].openUs)) + "," + std::to_string(s.timings[i].clearFrames) + "," + std::to_string(live ? (s.swapEyes ? 1 : 0) : s.timings[i].swap);
        }
        e["rateTimings"] = tm;
    }
    Put(e, "aiDepthFps", s.ai.depthFps);
    Put(e, "aiModelWidth", s.ai.modelWidth);
    Put(e, "aiStrength", s.ai.strength);
    Put(e, "aiConvergence", s.ai.convergence);
    Put(e, "aiSmoothing", s.ai.smoothing);
    Put(e, "aiAntiHalo", s.ai.antiHalo);
    Put(e, "aiUseGpu", s.ai.useGpu);
    Put(e, "aiShowDepth", s.ai.showDepth);
    Put(e, "aiMapBuffer", s.ai.mapBuffer);
}

void LoadEmitterSettings(const mINI::INIStructure& ini, StereoEmitterSettings& s)
{
    if(!ini.has("emitter"))
        return;
    const auto e = ini.get("emitter");
    int src = (int)s.sourceFormat;
    Get(e, "sourceFormat", src, 0, 6);
    const bool hasAuto = e.has("autoSource"); // (old files without it: 5 was the old "Auto" format)
    s.sourceFormat = (StereoSource)(src <= 4 || (hasAuto && src <= 6) ? src : 0);
    if(e.has("autoSource"))
        Get(e, "autoSource", s.autoSource);
    if(src == 5 && !hasAuto)
        s.autoSource = true; // old "Auto" format: now the Auto source switch
    if(s.autoSource)
        s.sourceFormat = StereoSource::Off;
    Get(e, "syncRateHz", s.syncRateHz, 0.0, 10000.0);
    if(e.has("syncAuto"))
        Get(e, "syncAuto", s.syncAuto);
    else
        s.syncAuto = s.syncRateHz <= 0.0; // old profile: 0 meant automatic
    if(s.syncRateHz < 1.0)
        s.syncRateHz = 120.0;
    Get(e, "syncOffsetUs", s.syncOffsetUs, -100000, 0); // negative only: a positive value of an older profile becomes 0
    Get(e, "syncAutoSec", s.syncAutoSec, 0.5f, 2.0f);
    s.syncAutoSec = std::round(s.syncAutoSec * 2.0f) / 2.0f;
    Get(e, "scanoutSync", s.scanoutSync);
    Get(e, "fullSbs", s.fullSbs);
    Get(e, "offsetCorrection", s.offsetCorrection);
    s.desyncMethod = 0;
    s.expPeriodMarginPct = 0.0f;
    Get(e, "desyncMethod", s.desyncMethod, 0, 1);
    if(e.has("expPeriodMarginPct"))
        Get(e, "expPeriodMarginPct", s.expPeriodMarginPct, 0.0f, 100.0f);
    Get(e, "swapEyes", s.swapEyes);
    Get(e, "comPortNo", s.comPort.comPortNo, -1, 256);
    Get(e, "nvOpenDurationUs", s.nvidia.openDurationUs, -100000.0f, 100000.0f);
    s.timingCount  = 0;
    s.timingActive = -1;
    if(e.has("rateTimings"))
    {
        const std::string tm = e.get("rateTimings");
        size_t            pos = 0;
        while(pos < tm.size() && s.timingCount < StereoEmitterSettings::kTimingSlots)
        {
            size_t end = tm.find('|', pos);
            if(end == std::string::npos)
                end = tm.size();
            int hz = 0, off = 0, open = 0, clear = -1, swap = -1;
            if(sscanf_s(tm.substr(pos, end - pos).c_str(), "%d,%d,%d,%d,%d", &hz, &off, &open, &clear, &swap) >= 3 && hz > 0)
            {
                auto& r    = s.timings[s.timingCount++];
                r.hz       = hz;
                r.offsetUs = std::clamp(off, -100000, 0);
                r.openUs   = std::clamp((float)open, -100000.0f, 100000.0f);
                r.clearFrames = std::clamp(clear, -1, 20);
                r.swap        = std::clamp(swap, -1, 1);
            }
            pos = end + 1;
        }
    }
    Get(e, "aiDepthFps", s.ai.depthFps, 1, 80);
    Get(e, "aiModelWidth", s.ai.modelWidth, 126, 1008);
    Get(e, "aiStrength", s.ai.strength, 0.0f, 10.0f);
    Get(e, "aiConvergence", s.ai.convergence, 0.0f, 100.0f);
    Get(e, "aiSmoothing", s.ai.smoothing, 0.0f, 100.0f);
    Get(e, "aiAntiHalo", s.ai.antiHalo, -300.0f, 300.0f);
    s.ai.useGpu = true; // the GPU (DirectML) is always used; the CPU is only the fallback when it is not available
    Get(e, "aiShowDepth", s.ai.showDepth);
    Get(e, "aiMapBuffer", s.ai.mapBuffer);
}

} // namespace Nbeam
