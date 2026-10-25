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

    // sync thread
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<Request>     m_queue;
    std::atomic<bool>       m_stop { false };
};

} // namespace ShaderBeam
