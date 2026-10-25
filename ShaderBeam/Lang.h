/*
Nbeam 3D Vision -- French / English interface text (MIT License)
*/

#pragma once

namespace ShaderBeam
{

#define LANG_FR 0
#define LANG_EN 1

int         GetLanguage();
void        SetLanguage(int lang);
const char* Tr(const char* en); // English text -> text in the selected language (returns the same text if there is no translation)

} // namespace ShaderBeam
