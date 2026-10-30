# Third-party notices

Nbeam 3D Vision is distributed under the MIT License (see `LICENSE`). It includes or uses the following third-party works.

## ShaderBeam

Base of this project (capture, rendering, BFI, structure), including its logo (`Misc/icon.png`, shown in the HUD).

Copyright (c) 2025-2026 mausimus - https://github.com/mausimus/ShaderBeam

License: MIT

## CRT Beam Simulator

CRT simulation shader (`Shaders/CRTBeamSimulator.hlsl`). Please credit Blur Busters and Timothy Lottes if this algorithm is used.

Copyright (c) 2024 Mark Rejhon (@BlurBusters) & Timothy Lottes (@NOTimothyLottes) - https://github.com/blurbusters/crt-beam-simulator

License: MIT

## Dear ImGui

User interface (`ImGui/`, `imgui_impl_dx11.cpp`). The bundled `imstb_*.h` files are by Sean Barrett and others (stb, public domain / MIT).

Copyright (c) 2014-2025 Omar Cornut - https://github.com/ocornut/imgui

License: MIT

## mINI

INI reading and writing (`ShaderBeam/ini.h`).

Copyright (c) 2018 Danijel Durakovic - https://github.com/metayeti/mINI

License: MIT

## ONNX Runtime (C API header)

Header `ShaderBeam/ort/onnxruntime_c_api.h`. `onnxruntime.dll` is NOT distributed with Nbeam: users provide their own copy.

Copyright (c) Microsoft Corporation - https://github.com/microsoft/onnxruntime

License: MIT

### MIT License text (applies to every MIT-licensed work above, with its own copyright line)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Inter (HUD font)

Subset of the Inter font embedded in `ShaderBeam/HudFont.h`.

Copyright (c) The Inter Project Authors (https://github.com/rsms/inter)

License: SIL Open Font License 1.1

```
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide development of collaborative font projects, to support the font creation efforts of academic and linguistic communities, and to provide a free and open framework in which fonts may be shared and improved in partnership with others.

The OFL allows the licensed fonts to be used, studied, modified and redistributed freely as long as they are not sold by themselves. The fonts, including any derivative works, can be bundled, embedded, redistributed and/or sold with any software provided that any reserved names are not used by derivative works. The fonts and derivatives, however, cannot be released under any other type of license. The requirement for fonts to remain under this license does not apply to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright Holder(s) under this license and clearly marked as such. This may include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the copyright statement(s).

"Original Version" refers to the collection of Font Software components as distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting, or substituting -- in part or in whole -- any of the components of the Original Version, by changing formats or by porting the Font Software to a new environment.

"Author" refers to any designer, engineer, programmer, technical writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining a copy of the Font Software, to use, study, copy, merge, embed, modify, redistribute, and sell modified and unmodified copies of the Font Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components, in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled, redistributed and/or sold with any software, provided that each copy contains the above copyright notice and this license. These can be included either as stand-alone text files, human-readable headers or in the appropriate machine-readable metadata fields within text or binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font Name(s) unless explicit written permission is granted by the corresponding Copyright Holder. This restriction only applies to the primary font name as presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font Software shall not be used to promote, endorse or advertise any Modified Version, except to acknowledge the contribution(s) of the Copyright Holder(s) and the Author(s) or with their explicit written permission.

5) The Font Software, modified or unmodified, in part or in whole, must be distributed entirely under this license, and must not be distributed under any other license. The requirement for fonts to remain under this license does not apply to any document created using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Not distributed

- AI depth models (`.onnx`, e.g. Depth Anything V2): provided by the user; each model has its own license (the Small model is Apache-2.0, the larger ones are non-commercial).
- `onnxruntime.dll`, `DirectML.dll`: provided by the user, each under its own license.
- NVIDIA 3D Vision emitter driver (NV3DVisionUSB) and WibbleWobble tools: obtained by the user from their own sources.

## Trademarks

NVIDIA and 3D Vision are trademarks of NVIDIA Corporation, used only to describe compatibility. Nbeam 3D Vision is not affiliated with or endorsed by NVIDIA, Blur Busters, WibbleWobble or ShaderBeam.
