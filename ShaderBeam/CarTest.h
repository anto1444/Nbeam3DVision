/*
Nbeam 3D Vision - motion test
MIT License
*/

#pragma once

namespace ShaderBeam
{

// Motion test: a process of its own ("Nbeam3DVision.exe /cartest x y w h nbeamPid") shows detailed cars scrolling in a borderless DirectX
// window over one screen, one new position every refresh of the screen; Nbeam captures that window like a game, so the cars go through
// every effect (BFI, CRT, 3D...) exactly like a real game picture. The settings of the test come from Nbeam with WM_COPYDATA
// (CarTestParams): the window stays black until they arrive (Nbeam sends them once its effects run with the right settings). The window
// closes with Escape, and it ends with Nbeam (job object, and it watches the Nbeam process).
constexpr const wchar_t* CARTEST_CLASS    = L"Nbeam3DVisionCarTest";
constexpr const wchar_t* CARTEST_TITLE    = L"Nbeam 3D Vision - Test";
constexpr const char*    CARTEST_TITLE_A  = "Nbeam 3D Vision - Test";
constexpr ULONG_PTR      CARTEST_COPYDATA = 0x4E42C001;

struct CarTestParams
{
    int   layout { 0 };      // picture layout: 0 = 2D, 1 = side by side, 2 = top / bottom, 3 = checkerboard (the 3D source format of Nbeam)
    int   speed { 960 };     // pixels per second
    int   carSize { 100 };   // %, 100 = 3 rows of cars
    float bg[3] { 0.16f, 0.17f, 0.19f };
    float accent[3] { 0.463f, 0.725f, 0.0f };
    int   french { 1 };      // eye marks G / D (French) or L / R
};

int RunCarTest(HINSTANCE instance, const wchar_t* cmdLine);

} // namespace ShaderBeam
