/*
ShaderBeam: shader effect overlay -- stereo emitter manager
MIT License
*/

#pragma once

#include "Emitter.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ShaderBeam
{

class EmitterManager
{
public:
    ~EmitterManager() { Stop(); }

    // Creates every emitter enabled in `settings` (kept by pointer: live changes of swapEyes / syncOffset apply immediately).
    bool Start(StereoEmitterSettings* settings, const EmitterEnv& env);
    void Stop();
    bool Active() const { return !m_emitters.empty(); }

    // eye of the content frame about to be presented (call before Present, render thread)
    // applySwap=false returns the eye the *image* belongs to (Swap Eyes only reverses the emitter relative to the image)
    Eye NextEye(int frameNo, int subFrameNo, bool applySwap = true);
    // on-screen emitters
    void DrawOverlay(const EmitterFrame& f);
    // SubFrames changed without restart: refresh the sync rate everywhere
    void SetSubFrames(int subFrames);
    // call right after Present returned
    void AfterPresent(Eye eye, int subFrameNo);

    // one line per emitter for the UI ("NVIDIA: connected", "COM3: failed to open", ...)
    std::vector<std::string> Status() const;

private:
    struct Request
    {
        Eye       eye;
        long long dueQpc;
    };
    void ThreadProc();
    long long Qpc() const;
    void      WaitUntil(long long dueQpc) const;

    StereoEmitterSettings*                 m_s { nullptr };
    EmitterEnv                             m_env;
    std::vector<std::unique_ptr<IEmitter>> m_emitters;
    std::vector<std::string>               m_status;

    double    m_periodUs { 8333.0 }; // one eye period
    long long m_qpcFreq { 1 };

    // scanout sync (DXGI frame statistics): refresh n happens at m_syncQpc + (n - m_syncCount) * m_refreshQpc, and present p is shown at
    // refresh p + m_delta. A missed refresh shows up as a larger m_delta, and the next signals follow it at once.
    bool ScanoutTime(long long& vblankQpc);
    double    m_refreshQpc { 0.0 };
    long long m_lastSyncQpc { 0 };
    long long m_lastSyncCount { 0 };
    long long m_delta { 0 };
    bool      m_haveDelta { false };

    // Every Sync Offset is measured from the real start of the frame on screen (whatever the queue and the timing used).
    // 1. Present times snapped on the regular grid of the display: a Present returned late because of an overload no longer moves
    //    the glasses (the predicted time is kept); a lasting change of phase is followed after a while.
    // 2. Without frame statistics, the screen time is the Present time plus the Present -> screen delay learned while the statistics
    //    were available (one value per queue length, kept for the whole session), or an estimate from the queue length.
    long long GridPresent(long long tp, double periodQpc);
    double    m_gridQpc { 0.0 };
    int       m_gridMiss { 0 };
    static double s_latencyQpc[16];   // learned Present -> screen delay per queue length (0 = not learned yet)

    // sync thread
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<Request>     m_queue;
    std::atomic<bool>       m_stop { false };
};

} // namespace ShaderBeam
