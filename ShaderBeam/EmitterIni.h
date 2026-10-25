/*
ShaderBeam: shader effect overlay -- emitter settings persistence ([emitter] section of ShaderBeam.ini)
MIT License
*/

#pragma once

#include <filesystem> // ini.h relies on these being included first
#include <map>
#include <string>
#include "EmitterSettings.h"
#include "ini.h"

namespace ShaderBeam
{
void SaveEmitterSettings(mINI::INIStructure& ini, const StereoEmitterSettings& s);
void LoadEmitterSettings(const mINI::INIStructure& ini, StereoEmitterSettings& s);
} // namespace ShaderBeam
