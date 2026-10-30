/*
ShaderBeam: shader effect overlay -- stereo emitter manager
MIT License
*/

#include "stdafx.h"
#include "EmitterManager.h"
#include "Helpers.h"
#include <cmath>

namespace ShaderBeam
{

double EmitterManager::s_latencyQpc[16] = {};

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
    m_refreshQpc    = 0.0;
    m_gridQpc       = 0.0;
    m_gridMiss      = 0;
    m_lastSyncQpc   = 0;
    m_lastSyncCount = 0;
    m_delta         = 0;
    m_haveDelta     = false;
    m_s->scanoutResyncs   = 0;
    m_s->scanoutSyncState = 0;

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

// Scanout sync: the vblank at which the frame just presented will start on screen, from the DXGI frame statistics (hardware vblank
// timestamps). The present / refresh link is re-read every frame, so after a missed refresh the glasses are back in step on the next
// content frame instead of drifting for seconds. No added latency: only the moment of the signal changes.
bool EmitterManager::ScanoutTime(long long& vblankQpc)
{
    auto* sc = m_env.swapChain;
    if(!sc)
        return false;
    UINT                  last = 0;
    DXGI_FRAME_STATISTICS st {};
    if(FAILED(sc->GetLastPresentCount(&last)) || FAILED(sc->GetFrameStatistics(&st)) || st.SyncQPCTime.QuadPart == 0 || st.PresentCount == 0)
        return false;
    const double nominal = m_env.vsyncRateHz > 1.0 ? m_qpcFreq / m_env.vsyncRateHz : m_qpcFreq / 60.0;
    if(m_refreshQpc <= 0.0)
        m_refreshQpc = nominal;
    // refresh period measured from the hardware timestamps (smoothed), kept close to the nominal one
    if(m_lastSyncCount > 0 && (long long)st.SyncRefreshCount > m_lastSyncCount)
    {
        const double p = (double)(st.SyncQPCTime.QuadPart - m_lastSyncQpc) / (double)((long long)st.SyncRefreshCount - m_lastSyncCount);
        if(p > nominal * 0.9 && p < nominal * 1.1)
            m_refreshQpc = m_refreshQpc * 0.95 + p * 0.05;
    }
    m_lastSyncQpc   = st.SyncQPCTime.QuadPart;
    m_lastSyncCount = (long long)st.SyncRefreshCount;

    const long long delta = (long long)st.PresentRefreshCount - (long long)st.PresentCount;
    if(m_haveDelta && delta != m_delta)
        m_s->scanoutResyncs++;
    m_delta     = delta;
    m_haveDelta = true;

    const long long refresh = (long long)last + m_delta; // refresh at which the frame just presented will be shown
    vblankQpc               = st.SyncQPCTime.QuadPart + (long long)((double)(refresh - (long long)st.SyncRefreshCount) * m_refreshQpc);
    return true;
}

long long EmitterManager::GridPresent(long long tp, double periodQpc)
{
    if(periodQpc <= 0.0 || m_gridQpc <= 0.0)
    {
        m_gridQpc = (double)tp;
        return tp;
    }
    const double k         = (std::max)(1.0, std::round(((double)tp - m_gridQpc) / periodQpc));
    const double predicted = m_gridQpc + k * periodQpc;
    const double err       = (double)tp - predicted;
    if(std::fabs(err) < periodQpc * 0.15)
    {
        m_gridQpc  = predicted + err * 0.1; // on the grid: the phase follows slowly
        m_gridMiss = 0;
    }
    else if(++m_gridMiss > 60)
    {
        m_gridQpc  = (double)tp; // a lasting change (new mode...): the grid moves to it
        m_gridMiss = 0;
    }
    else
        m_gridQpc = predicted; // Present late (overload): the regular time is kept, the glasses do not move
    return (long long)m_gridQpc;
}

void EmitterManager::AfterPresent(Eye eye, int subFrameNo)
{
    // reference of the Sync Offset: the real start of the frame on screen
    const long long tp      = Qpc(); // moment Present() returned
    const double    nominal = m_env.vsyncRateHz > 1.0 ? m_qpcFreq / m_env.vsyncRateHz : m_qpcFreq / 60.0;
    const long long grid    = GridPresent(tp, m_refreshQpc > 0.0 ? m_refreshQpc : nominal);
    const int       q       = std::clamp(m_env.queuedFrames, 0, 15);
    long long       presentQpc;
    long long       v = 0;
    if(ScanoutTime(v))
    {
        // statistics available (automatic): exact screen time; the Present -> screen delay is learned for the classic timing
        presentQpc = v;
        const double lat = (double)(v - grid);
        if(lat > -nominal && lat < nominal * 20.0)
            s_latencyQpc[q] = s_latencyQpc[q] > 0.0 ? s_latencyQpc[q] * 0.98 + lat * 0.02 : (std::max)(lat, 1.0);
        if(m_s)
            m_s->scanoutSyncState = 1;
    }
    else
    {
        // classic timing: the grid Present time plus the learned delay (else about one refresh per queued frame)
        const double lat = s_latencyQpc[q] > 0.0 ? s_latencyQpc[q] : (double)q * nominal;
        presentQpc       = grid + (long long)lat;
        if(m_s)
            m_s->scanoutSyncState = -1;
    }

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
    MmcssGames mmcss; // the glasses signals keep their timing while the game loads / compiles shaders
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
