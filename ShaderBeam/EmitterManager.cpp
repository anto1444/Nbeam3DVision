/*
ShaderBeam: shader effect overlay -- stereo emitter manager
MIT License
*/

#include "stdafx.h"
#include "EmitterManager.h"
#include <cmath>

namespace ShaderBeam
{

long long EmitterManager::Qpc() const
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void EmitterManager::WaitUntil(long long dueQpc) const
{
    for(;;)
    {
        auto remainingUs = (dueQpc - Qpc()) * 1000000.0 / m_qpcFreq;
        if(remainingUs <= 0)
            return;
        if(remainingUs > 2500)
            Sleep(1); // timeBeginPeriod(1) is active in ShaderBeam
        else
            YieldProcessor();
    }
}

bool EmitterManager::Start(StereoEmitterSettings* settings, const EmitterEnv& env)
{
    Stop();
    m_s   = settings;
    m_env = env;
    if(!m_s || !m_s->AnyEnabled())
        return false;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m_qpcFreq = f.QuadPart;
    m_periodUs = 1e6 / env.SyncRateHz(*m_s);

    auto add = [&](std::unique_ptr<IEmitter> e, bool enabled) {
        if(!enabled)
            return;
        std::string status;
        if(e->Init(*m_s, m_env, status))
        {
            m_status.push_back(std::string(e->Name()) + ": " + (status.empty() ? "active" : status));
            m_emitters.push_back(std::move(e));
        }
        else
        {
            m_status.push_back(std::string(e->Name()) + ": FAILED - " + status);
        }
    };
    add(CreateComPortEmitter(), m_s->comPort.comPortNo >= 0);
    add(CreateNvidiaEmitter(), true);

    bool needThread = false;
    for(auto& e : m_emitters)
        needThread |= !e->IsOnScreen();
    if(needThread)
    {
        m_stop   = false;
        m_thread = std::thread(&EmitterManager::ThreadProc, this);
        SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    return Active();
}

void EmitterManager::Stop()
{
    if(m_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    m_queue.clear();
    for(auto& e : m_emitters)
        e->Deinit();
    m_emitters.clear();
    m_status.clear();
}

void EmitterManager::SetSubFrames(int subFrames)
{
    m_env.subFrames = subFrames;
    if(!m_s)
        return;
    m_periodUs   = 1e6 / m_env.SyncRateHz(*m_s);
    for(auto& e : m_emitters)
        e->OnEnvChanged(m_env);
}

std::vector<std::string> EmitterManager::Status() const
{
    return m_status;
}

Eye EmitterManager::NextEye(int frameNo, int subFrameNo, bool applySwap)
{
    (void)subFrameNo;
    // deterministic alternation: the eye follows the content frame (one content frame = SubFrames output frames)
    int parity = frameNo & 1;
    if(applySwap && m_s->swapEyes)
        parity ^= 1;
    return parity ? Eye::Right : Eye::Left;
}

void EmitterManager::DrawOverlay(const EmitterFrame& f)
{
    for(auto& e : m_emitters)
        if(e->IsOnScreen())
            e->DrawOverlay(f);
}

void EmitterManager::AfterPresent(Eye eye, int subFrameNo)
{
    const long long presentQpc = Qpc(); // moment Present() returned: reference for Sync Offset

    // one signal per content frame (= per eye)
    if(subFrameNo != 0)
        return;
    bool hardware = false;
    for(auto& e : m_emitters)
        hardware |= !e->IsOnScreen();
    if(!hardware)
        return;

    const long long now = presentQpc;

    // positive offset: signal `offset` after vsync. negative: signal before the NEXT vsync, i.e. for the other eye one period later.
    const double offset = m_s->syncOffsetUs;
    Request      r;
    if(offset >= 0)
    {
        r.eye    = eye;
        r.dueQpc = now + (long long)(offset * m_qpcFreq / 1e6);
    }
    else
    {
        const double clamped = (std::max)(offset, -m_periodUs);
        r.eye                = eye == Eye::Left ? Eye::Right : Eye::Left;
        r.dueQpc             = now + (long long)((m_periodUs + clamped) * m_qpcFreq / 1e6);
    }
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if(m_queue.size() > 8)
            m_queue.pop_front();
        m_queue.push_back(r);
    }
    m_cv.notify_one();
}

void EmitterManager::ThreadProc()
{
    for(;;)
    {
        Request r;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            m_cv.wait(l, [&] { return m_stop || !m_queue.empty(); });
            if(m_stop)
                return;
            r = m_queue.front();
            m_queue.pop_front();
        }
        WaitUntil(r.dueQpc);
        // Safety for heavy overloads only: a signal more than one whole display period late belongs to a frame that is no
        // longer on screen; sending it would open the wrong eye, so it is dropped (never happens in normal operation).
        if(Qpc() - r.dueQpc > (long long)(m_periodUs * m_qpcFreq / 1e6))
            continue;
        for(auto& e : m_emitters)
            if(!e->IsOnScreen())
                e->Sync(r.eye);
    }
}

} // namespace ShaderBeam
