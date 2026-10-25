/*
Nbeam 3D Vision: NVIDIA 3D Vision USB emitter protocol (portable, no OS calls)

The values below (USB ids, pipe names, packet bytes, timing formulas) are protocol facts of the emitter. They were established
for interoperability by analysing existing emitter software (the NVIDIA emitter plugin of WibbleWobble, GPL-3.0,
https://github.com/PHARTGAMES/WibbleWobbleCore); only these facts are used here and no code of that project is included.
Tuned on a real emitter by the author.
MIT License
*/

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ShaderBeam::Nv
{

// ---- USB identification ----------------------------------------------------------------------
// Device interface path (GUID_DEVINTERFACE_USB_DEVICE, lower-cased) must contain one of these.
inline constexpr const char* kUsbPathPatterns[] = {
    "usb#vid_0955&pid_0007", "usb#vid_0955&pid_7001", "usb#vid_0955&pid_7002",      "usb#vid_0955&pid_7003",
    "usb#vid_0955&pid_7004", "usb#vid_0955&pid_7008", "usb#vid_0955&pid_7009",      "usb#vid_0955&pid_700a",
    "usb#vid_0955&pid_700c", "usb#vid_0955&pid_700d&mi_00", "usb#vid_0955&pid_700e&mi_00"};

// Pipes are opened as  <device interface path> + "\\" + <pipe name>
// (CreateFileW, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|WRITE, OPEN_EXISTING) -- needs the NV3DVisionUSB driver.
inline constexpr const char* kWakePipe    = "PIPE02"; // opened + closed once to wake the emitter
inline constexpr const char* kControlPipe = "PIPE02"; // programming packets
inline constexpr const char* kSyncPipe    = "PIPE00"; // per-eye packets

inline constexpr int kEyePacketSize = 8;
// eye 0 (left) / eye 1 (right)
inline constexpr uint8_t kEyePacket[2][kEyePacketSize] = {{0xAA, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF}, {0xAA, 0xFE, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF}};

// ---- Programming packets (written to the control pipe, in order, one WriteFile each) ------------------
struct Timings
{
    float syncRateHz;     // eye switch rate in Hz; |value| used, 0 -> period treated as 1 us
    float openDurationUs; // config +8
};

// cvttss2si semantics: truncate toward zero, 0x80000000 on overflow / NaN
inline int32_t CvtTruncSS2SI(float v)
{
    if(!(v >= -2147483648.0f && v < 2147483648.0f))
        return INT32_MIN;
    return (int32_t)v;
}

inline void PutLE32(std::vector<uint8_t>& out, uint32_t v)
{
    for(int i = 0; i < 4; i++)
        out.push_back((uint8_t)(v >> (8 * i)));
}

inline std::vector<std::vector<uint8_t>> BuildProgrammingPackets(const Timings& t)
{
    const float rate   = std::fabs(t.syncRateHz);
    const float period = rate == 0.0f ? 1.0f : 1000000.0f / rate; // microseconds
    const float one    = 1.0f;

    const int32_t vW      = CvtTruncSS2SI(one); // fourth timing value: fixed at 0 (no effect)
    const int32_t vDelay  = CvtTruncSS2SI(one); // open delay: fixed at 0 (no longer adjustable)
    const int32_t vDur    = CvtTruncSS2SI(one - (t.openDurationUs * 4.0f));
    const int32_t vPeriod = CvtTruncSS2SI(one - period * 12.0f);
    const int32_t vRate   = CvtTruncSS2SI(rate * 4.0f);

    std::vector<std::vector<uint8_t>> p(6);
    // 1: command header
    PutLE32(p[0], 0x00031840);
    // 2: timing block (28 bytes)
    PutLE32(p[1], 0x00180001);
    PutLE32(p[1], (uint32_t)vW);
    PutLE32(p[1], (uint32_t)vDelay);
    PutLE32(p[1], (uint32_t)vDur);
    PutLE32(p[1], 0x22242830);
    PutLE32(p[1], 0x0405080A);
    PutLE32(p[1], (uint32_t)vPeriod);
    // 3: 01 1C 02 00 02 00
    p[2] = {0x01, 0x1C, 0x02, 0x00, 0x02, 0x00};
    // 4: 01 1E 02 00 <rate*4, 16 bit LE>
    p[3] = {0x01, 0x1E, 0x02, 0x00, (uint8_t)(vRate & 0xFF), (uint8_t)((vRate >> 8) & 0xFF)};
    // 5: 01 1B 01 00 07
    p[4] = {0x01, 0x1B, 0x01, 0x00, 0x07};
    // 6: 07 00 00 00
    p[5] = {0x07, 0x00, 0x00, 0x00};
    return p;
}

} // namespace ShaderBeam::Nv
