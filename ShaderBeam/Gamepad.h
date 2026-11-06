/*
Nbeam 3D Vision: gamepad (XInput) reading for the shortcuts. The HUD navigation itself is done by ImGui (see UI::Render).
*/
#pragma once

#include <windows.h>
#include <atomic>
#include <string>

namespace ShaderBeam
{
namespace Pad
{
// button bits (the XInput bits, plus the two triggers as buttons)
enum : unsigned
{
    UP    = 0x0001,
    DOWN  = 0x0002,
    LEFT  = 0x0004,
    RIGHT = 0x0008,
    START = 0x0010,
    BACK  = 0x0020,
    LS    = 0x0040,
    RS    = 0x0080,
    LB    = 0x0100,
    RB    = 0x0200,
    A     = 0x1000,
    B     = 0x2000,
    X     = 0x4000,
    Y     = 0x8000,
    LT    = 0x10000,
    RT    = 0x20000
};

// wheel and button of the NVIDIA 3D Vision emitter: written by the emitter reader thread (EmitterNvidia.cpp), taken by the poll thread
// (Window.cpp) and given to the 3D mini menu (UI::MenuInput). They are not pad buttons: the emitter is not used for the shortcuts.
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

struct XState
{
    DWORD packet;
    WORD  buttons;
    BYTE  lt, rt;
    SHORT lx, ly, rx, ry;
};

inline std::atomic<unsigned>& Current()
{
    static std::atomic<unsigned> s_buttons { 0 };
    return s_buttons;
}
inline std::atomic<bool>& Connected()
{
    static std::atomic<bool> s_connected { false };
    return s_connected;
}

// buttons held right now (all pads together; 0 when none is connected)
inline unsigned Buttons() { return Current().load(); }

// call every ~15 ms from one thread. Slots without a pad are probed only once a second (the call is slow on an empty slot).
inline void Poll()
{
    typedef DWORD(WINAPI * GetStateFn)(DWORD, XState*);
    static GetStateFn fn     = nullptr;
    static bool       tried  = false;
    static bool       slot[4] {};
    static ULONGLONG  probe  = 0;
    if(!tried)
    {
        tried = true;
        for(const wchar_t* name : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        {
            if(HMODULE dll = LoadLibraryW(name))
            {
                fn = (GetStateFn)GetProcAddress(dll, "XInputGetState");
                if(fn)
                    break;
            }
        }
    }
    const ULONGLONG now = GetTickCount64();
    if(!fn)
    {
        Current() = 0;
        return;
    }
    const bool probeAll = now - probe >= 1000;
    if(probeAll)
        probe = now;
    unsigned mask = 0;
    bool     any  = false;
    for(DWORD i = 0; i < 4; i++)
    {
        if(!slot[i] && !probeAll)
            continue;
        XState st {};
        if(fn(i, &st) != ERROR_SUCCESS)
        {
            slot[i] = false;
            continue;
        }
        slot[i] = true;
        any     = true;
        mask |= st.buttons & 0xF3FFu;
        if(st.lt > 30)
            mask |= LT;
        if(st.rt > 30)
            mask |= RT;
    }
    Connected() = any;
    Current()   = mask;
}

inline int Count(unsigned mask)
{
    int n = 0;
    for(; mask; mask >>= 1)
        n += mask & 1;
    return n;
}

// "Back+Start"; empty text for no button
inline std::string Text(unsigned mask)
{
    static const struct
    {
        unsigned    bit;
        const char* name;
    } names[] = { { LB, "LB" }, { RB, "RB" }, { LT, "LT" }, { RT, "RT" }, { BACK, "Back" }, { START, "Start" }, { A, "A" }, { B, "B" }, { X, "X" }, { Y, "Y" },
                  { UP, "D-pad Up" }, { DOWN, "D-pad Down" }, { LEFT, "D-pad Left" }, { RIGHT, "D-pad Right" }, { LS, "L3" }, { RS, "R3" } };
    std::string s;
    for(const auto& n : names)
        if(mask & n.bit)
        {
            if(!s.empty())
                s += "+";
            s += n.name;
        }
    return s;
}

} // namespace Pad

// NVIDIA 3D Vision emitter: reads the wheel and the button all the time, also without 3D (EmitterNvidia.cpp)
void NvidiaKeysService(bool run);   // start / stop the background reader
void NvidiaKeysPause(bool pause);   // the 3D emitter takes over the pipes while it runs (and reads the keys itself)
} // namespace ShaderBeam
