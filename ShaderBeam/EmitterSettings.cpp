/*
ShaderBeam: shader effect overlay -- emitter settings <-> ShaderBeam.ini ([emitter] section)
MIT License
*/

#include "stdafx.h"
#include "EmitterIni.h"

namespace ShaderBeam
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
    try { v = std::clamp(std::stof(m.get(k)), lo, hi); } catch(...) { }
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
    Put(e, "sourceFormat", (int)s.sourceFormat);
    Put(e, "syncAuto", s.syncAuto);
    Put(e, "syncRateHz", s.syncRateHz);
    Put(e, "syncOffsetUs", s.syncOffsetUs);
    Put(e, "swapEyes", s.swapEyes);
    Put(e, "comPortNo", s.comPort.comPortNo);
    Put(e, "nvOpenDurationUs", s.nvidia.openDurationUs);
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
    s.sourceFormat = (StereoSource)(src <= 4 ? src : 0); // (the removed Auto / Full formats of earlier builds fall back to Off)
    Get(e, "syncRateHz", s.syncRateHz, 0.0f, 10000.0f);
    if(e.has("syncAuto"))
        Get(e, "syncAuto", s.syncAuto);
    else
        s.syncAuto = s.syncRateHz <= 0.0f; // old profile: 0 meant automatic
    if(s.syncRateHz < 1.0f)
        s.syncRateHz = 120.0f;
    Get(e, "syncOffsetUs", s.syncOffsetUs, -100000, 100000);
    Get(e, "swapEyes", s.swapEyes);
    Get(e, "comPortNo", s.comPort.comPortNo, -1, 256);
    Get(e, "nvOpenDurationUs", s.nvidia.openDurationUs, -100000.0f, 100000.0f);
    Get(e, "aiDepthFps", s.ai.depthFps, 1, 80);
    Get(e, "aiModelWidth", s.ai.modelWidth, 126, 1008);
    Get(e, "aiStrength", s.ai.strength, 0.0f, 10.0f);
    Get(e, "aiConvergence", s.ai.convergence, 0.0f, 100.0f);
    Get(e, "aiSmoothing", s.ai.smoothing, 0.0f, 95.0f);
    Get(e, "aiAntiHalo", s.ai.antiHalo, 0.0f, 300.0f);
    Get(e, "aiUseGpu", s.ai.useGpu);
    Get(e, "aiShowDepth", s.ai.showDepth);
    Get(e, "aiMapBuffer", s.ai.mapBuffer);
}

} // namespace ShaderBeam
