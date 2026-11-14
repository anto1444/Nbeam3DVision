/*
Nbeam 3D Vision: wheel and button of the NVIDIA 3D Vision emitter (the 3D mini menu). There is no gamepad support.
*/
#pragma once

#include <windows.h>
#include <atomic>

namespace ShaderBeam
{
namespace Pad
{
// wheel and button of the NVIDIA 3D Vision emitter: written by the emitter reader thread (EmitterNvidia.cpp), taken by the poll thread
// (Window.cpp) and given to the 3D mini menu (UI::MenuInput).
struct EmitterInput
{
    std::atomic<int> menuWheel { 0 };  // ticks turned since the last take (positive / negative = the two directions)
    std::atomic<int> menuHeld { 0 };   // the same, turned while the button is held down
    std::atomic<int> menuClicks { 0 }; // reports of the button (the emitter reports it when it is released)
};
inline EmitterInput& Emitter()
{
    static EmitterInput s_emitter;
    return s_emitter;
}

} // namespace Pad

// NVIDIA 3D Vision emitter: reads the wheel and the button all the time, also without 3D (EmitterNvidia.cpp)
void NvidiaKeysService(bool run);   // start / stop the background reader
void NvidiaKeysPause(bool pause);   // the 3D emitter takes over the pipes while it runs (and reads the keys itself)
} // namespace ShaderBeam
