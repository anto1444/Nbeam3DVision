/*
ShaderBeam: shader effect overlay -- stereo emitter manager
MIT License
*/

#include "stdafx.h"
#include "EmitterManager.h"
#include "Helpers.h"
#include "Common.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ShaderBeam
{

double EmitterManager::s_latencyQpc[16] = {};

long long EmitterManager::Qpc() const
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
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
    m_trace          = SyncTrace {};
    m_traceStart     = 0;
    m_lastState      = 0;
    m_presentNoValid = false;
    m_lastComp       = -1;
    m_media          = nullptr;
    m_mediaTried     = false;

    // opened in a thread of their own: the render thread (live 3D changes) and the window thread (start) never wait for the USB emitter
    m_status.push_back("3D emitter: connecting...");
    m_initDone  = false;
    m_initTaken = false;
    const bool comPort = m_s->comPort.comPortNo >= 0;
    StereoEmitterSettings* s = m_s;
    const EmitterEnv       e0 = m_env;
    unsigned               gen;
    {
        std::lock_guard<std::mutex> l(m_initMutex);
        gen = ++m_initGen;
    }
    // starts that were stopped before they ended are waited for by this thread (not by the render thread), so that two starts never
    // open the emitter at the same time
    auto previous = std::make_shared<std::vector<std::thread>>(std::move(m_oldInits));
    m_oldInits.clear();
    m_initThread = std::thread([this, s, e0, comPort, gen, previous]() {
        for(auto& t : *previous)
            if(t.joinable())
                t.join();
        std::vector<std::unique_ptr<IEmitter>> ready;
        std::vector<std::string>               status;
        auto add = [&](std::unique_ptr<IEmitter> e, bool enabled) {
            if(!enabled)
                return;
            std::string st;
            if(e->Init(*s, e0, st))
            {
                status.push_back(std::string(e->Name()) + ": " + (st.empty() ? "active" : st));
                ready.push_back(std::move(e));
            }
            else
                status.push_back(std::string(e->Name()) + ": FAILED - " + st);
        };
        add(CreateComPortEmitter(), comPort);
        add(CreateNvidiaEmitter(), true);
        std::lock_guard<std::mutex> l(m_initMutex);
        if(gen != m_initGen)
        {
            // stopped meanwhile: what was opened is closed here
            for(auto& e : ready)
                e->Deinit();
            return;
        }
        m_initEmitters = std::move(ready);
        m_initStatus   = std::move(status);
        m_initDone     = true;
    });
    return true;
}

bool EmitterManager::Poll()
{
    if(m_initTaken || !m_initDone)
        return false;
    JoinInit(); // (it has ended: m_initDone is set at its very end)
    std::lock_guard<std::mutex> l(m_initMutex);
    m_initTaken = true;
    m_emitters  = std::move(m_initEmitters);
    m_status    = std::move(m_initStatus);
    m_initEmitters.clear();
    m_initStatus.clear();
    for(auto& e : m_emitters)
        e->OnEnvChanged(m_env); // (SubFrames may have changed meanwhile)
    bool needThread = false;
    for(auto& e : m_emitters)
        needThread |= !e->IsOnScreen();
    if(needThread)
    {
        m_stop   = false;
        m_thread = std::thread(&EmitterManager::ThreadProc, this);
        SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    return true;
}

void EmitterManager::JoinInit()
{
    if(m_initThread.joinable())
        m_initThread.join();
}

EmitterManager::~EmitterManager()
{
    Stop();
    for(auto& t : m_oldInits)
        if(t.joinable())
            t.join();
}

void EmitterManager::Stop()
{
    // a start still running is not waited for (the render thread would stop for up to a few seconds): it is told it was stopped, closes
    // what it opened by itself, and the next start waits for it in its own thread
    {
        std::lock_guard<std::mutex> l(m_initMutex);
        ++m_initGen;
        for(auto& e : m_initEmitters)
            e->Deinit();
        m_initEmitters.clear();
        m_initStatus.clear();
        m_initTaken = true;
        m_initDone  = false;
    }
    if(m_initThread.joinable())
        m_oldInits.push_back(std::move(m_initThread));
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
    // the statistics interface holds a reference to the swap chain of the renderer: released here, or the old swap chain would stay alive
    // on the window and the next one could not be made (swap chain error, the last picture of the previous game left on screen)
    m_media      = nullptr;
    m_mediaTried = false;
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
// Offset correction: every micro-desync (screen delay moved, present grid jumped, Windows composition over) changes the Open Duration by 1 us,
// alternately one below the value set by the user and back to it. The emitter takes the new duration at its next signal and restarts its
// offset. Not more than twice a second, and not while Windows composes the picture (the delay then keeps going back and forth).
void EmitterManager::NudgeOffset(const char* why, int refreshes)
{
    if(!m_s || !m_s->offsetCorrection || m_lastComp == 0)
        return;
    const long long now = Qpc();
    if(m_lastShiftQpc != 0 && (double)(now - m_lastShiftQpc) < 0.5 * (double)m_qpcFreq)
        return;
    m_lastShiftQpc = now;
    // the value of the user (profile, slider, wheel) is the reference: the Open Duration goes one below it, then back to it, and so on, so that
    // reloading a profile or many corrections never move the glasses away from the value that was set
    NvidiaConfig& n = m_s->nvidia;
    if(!(n.openNudged > 0.0f && n.openDurationUs == n.openNudged))
        n.openBase = n.openDurationUs; // (not ours: the user changed it, or a profile was loaded)
    const int dir = n.openDurationUs == n.openBase && n.openBase >= 2.0f ? -1 : (n.openDurationUs == n.openBase ? 1 : 0);
    n.openDurationUs = n.openBase + (float)dir; // (dir 0: was one below, back to the reference)
    n.openNudged     = n.openDurationUs;
    // with black frames (BFI / CRT) the emitter takes the new value in the first of them, where the glasses restarting cannot be seen; else at the next signal
    n.openApplyDeferred = m_env.subFrames >= 2;
    m_deferSince        = now;
    Rec('N', dir, refreshes);
    LsLog("offset correction (%s, %d refresh(es)): Open Duration %.0f us (set value %.0f us)", why, refreshes, n.openDurationUs, n.openBase);
}

bool EmitterManager::ScanoutTime(long long& vblankQpc)
{
    auto* sc = m_env.swapChain;
    if(!sc)
        return false;
    UINT                  last = 0;
    DXGI_FRAME_STATISTICS st {};
    // the "media" statistics also tell how Windows shows the picture (composed by DWM, or directly: independent flip / overlay); a change
    // of that mode changes the delay to the screen (the HUD shown, a return to Windows...)
    if(!m_mediaTried)
    {
        m_mediaTried = true;
        sc->QueryInterface(__uuidof(IDXGISwapChainMedia), (void**)m_media.put());
    }
    int comp = -1;
    if(m_media)
    {
        DXGI_FRAME_STATISTICS_MEDIA sm {};
        if(FAILED(sc->GetLastPresentCount(&last)) || FAILED(m_media->GetFrameStatisticsMedia(&sm)))
            return false;
        st.PresentCount        = sm.PresentCount;
        st.PresentRefreshCount = sm.PresentRefreshCount;
        st.SyncRefreshCount    = sm.SyncRefreshCount;
        st.SyncQPCTime         = sm.SyncQPCTime;
        st.SyncGPUTime         = sm.SyncGPUTime;
        comp                   = (int)sm.CompositionMode;
    }
    else if(FAILED(sc->GetLastPresentCount(&last)) || FAILED(sc->GetFrameStatistics(&st)))
        return false;
    if(st.SyncQPCTime.QuadPart == 0 || st.PresentCount == 0)
        return false;
    // statistics that no longer follow the presents (Windows composes the picture and drops or holds frames: the last measured present is
    // far behind): the screen time would be extrapolated from them and drift a little more at every frame, then jump back when they are
    // updated (the glasses timing swept back and forth every second until the renderer was restarted). The classic timing is used meanwhile.
    if((long long)last - (long long)st.PresentCount > 12)
    {
        m_trace.staleStats++; Rec('T');
        m_lastPresentNo  = last;
        m_presentNoValid = true;
        return false;
    }
    if(comp >= 0 && comp != m_lastComp)
    {
        if(m_lastComp >= 0)
        {
            m_trace.modeChanges++;
            Rec('C', m_lastComp, comp);
        }
        const bool backFromWindows = m_lastComp == 0 && comp != 0;
        m_lastComp = comp;
        if(backFromWindows)
            NudgeOffset("back from Windows composition", 0);
        if(comp == 0)
        {
            // Windows starts composing the picture: is the mouse pointer visible, is Nbeam (its HUD) in front?
            CURSORINFO ci { sizeof(ci) };
            if(GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
                m_trace.composedCursor++;
            DWORD pid = 0;
            if(HWND fg = GetForegroundWindow())
                GetWindowThreadProcessId(fg, &pid);
            if(pid == GetCurrentProcessId())
                m_trace.composedNbeam++;
            m_trace.composedTimes++;
        }
    }
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

    // delay (in refreshes) between a present and its refresh. A new value is taken once it is read twice in a row: a single odd reading
    // (statistics read while Windows changes how it shows the picture) no longer moves the glasses back and forth.
    const long long delta = (long long)st.PresentRefreshCount - (long long)st.PresentCount;
    if(m_haveDelta && delta != m_delta)
    {
        const bool announced = m_expectShift != 0 && delta - m_delta == m_expectShift && Qpc() < m_expectUntilQpc; // (the queue was changed on purpose)
        if(!announced && delta != m_deltaCand)
        {
            m_deltaCand = delta;
            m_trace.deltaFiltered++; Rec('f');
            vblankQpc = 0;
            const long long refresh0 = (long long)last + m_delta; // (the value in use stays for this frame)
            m_lastSyncQpc   = st.SyncQPCTime.QuadPart;
            m_lastSyncCount = (long long)st.SyncRefreshCount;
            vblankQpc       = st.SyncQPCTime.QuadPart + (long long)((double)(refresh0 - (long long)st.SyncRefreshCount) * m_refreshQpc);
            m_lastPresentNo  = last;
            m_presentNoValid = true;
            return true;
        }
        if(announced)
            m_expectShift = 0;
        else
            m_trace.deltaFiltered--; // (the first reading was the start of a real change)
        m_s->scanoutResyncs++;
        m_trace.deltaChanges++;
        m_trace.lastDelta = delta - m_delta;
        Rec('M', (int)(delta - m_delta), m_lastComp);
        if(!announced)
            NudgeOffset("screen delay", (int)(delta - m_delta));
        // The frames now reach the screen (delta - m_delta) refreshes later (a refresh missed: a sub-frame shown twice) or earlier. The signals
        // already waiting were timed on the old screen times: with a negative Sync Offset the signal of the next eye was planned one eye period
        // after the start of this eye, and this eye now lasts one refresh more, so the glasses opened too early until the next eye. They are moved
        // by the same number of refreshes.
        // (not more than one eye period either way: a bigger jump is a reading error, and not while Windows composes the picture, where the
        // delay keeps going back and forth)
        const double    maxShift = m_periodUs * m_qpcFreq / 1e6;
        const long long shift    = (long long)std::clamp((double)(delta - m_delta) * m_refreshQpc, -maxShift, maxShift);
        if(m_lastComp != 0)
        {
            std::lock_guard<std::mutex> l(m_mutex);
            for(auto& q : m_queue)
            {
                q.dueQpc += shift;
                m_trace.signalsMoved++;
            }
            long long cur = m_curDue.load();
            if(cur != 0 && m_curDue.compare_exchange_strong(cur, cur + shift))
                m_trace.signalsMoved++;
            m_cv.notify_one();
        }
    }
    m_deltaCand = delta;
    if(m_refreshQpc > 0.0)
        m_trace.periodDriftUs = (std::max)(m_trace.periodDriftUs, std::fabs(m_refreshQpc - nominal) * 1e6 / m_qpcFreq);
    if(m_presentNoValid && last > m_lastPresentNo + 1 && last - m_lastPresentNo < 1000)
        {
            m_trace.presentGaps++;
            Rec('P', (int)(last - m_lastPresentNo));
        }
    m_lastPresentNo  = last;
    m_presentNoValid = true;
    m_delta     = delta;
    m_haveDelta = true;

    const long long refresh = (long long)last + m_delta; // refresh at which the frame just presented will be shown
    vblankQpc               = st.SyncQPCTime.QuadPart + (long long)((double)(refresh - (long long)st.SyncRefreshCount) * m_refreshQpc);
    // a screen time that cannot be right (in the past, or more than 12 refreshes ahead: a wrong delay after a long pause of the
    // statistics): the classic timing is used for this frame
    const long long ahead = vblankQpc - Qpc();
    if(ahead < -(long long)nominal || ahead > (long long)(nominal * 12.0))
    {
        m_trace.badScanout++; Rec('X');
        return false;
    }
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
        m_trace.gridJumps++; Rec('J');
        NudgeOffset("grid jump", 0);
    }
    else
        m_gridQpc = predicted; // Present late (overload): the regular time is kept, the glasses do not move
    return (long long)m_gridQpc;
}

void EmitterManager::AnnounceQueue(int oldQueue, int newQueue)
{
    m_env.queuedFrames = newQueue; // (classic timing: delay learned per queue length)
    m_expectShift      = newQueue - oldQueue;
    m_expectUntilQpc   = Qpc() + m_qpcFreq; // (1 s to show in the statistics)
}

void EmitterManager::AfterPresent(Eye eye, int subFrameNo)
{
    if(const long long req = g_desyncRequestQpc.exchange(0))
        DumpRecent(req);
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
        if(m_lastState == -1)
        {
            m_trace.statsBack++;
            Rec('B');
        }
        m_lastState = 1;
    }
    else
    {
        // classic timing: the grid Present time plus the learned delay (else about one refresh per queued frame)
        const double lat = s_latencyQpc[q] > 0.0 ? s_latencyQpc[q] : (double)q * nominal;
        presentQpc       = grid + (long long)lat;
        if(m_s)
            m_s->scanoutSyncState = -1;
        if(m_lastState == 1)
        {
            m_trace.statsLost++;
            Rec('L');
        }
        m_lastState = -1;
    }
    TraceTick(tp);

    bool hardware = false;
    for(auto& e : m_emitters)
        hardware |= !e->IsOnScreen();
    if(!hardware)
        return;
    // held back Open Duration (offset correction): sent a third of a refresh into the first black frame, or at the next signal after 100 ms
    if(m_s->nvidia.openApplyDeferred)
    {
        if((double)(tp - m_deferSince) > 0.1 * (double)m_qpcFreq)
        {
            m_s->nvidia.openApplyDeferred = false;
            m_applyQueued                 = false;
        }
        else if(subFrameNo == 1 && !m_applyQueued.exchange(true))
        {
            Request a;
            a.eye          = eye;
            a.dueQpc       = presentQpc + (long long)(m_refreshQpc > 0.0 ? m_refreshQpc / 3.0 : nominal / 3.0);
            a.applyTimings = true;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_queue.push_back(a);
            }
            m_cv.notify_one();
        }
    }

    // one signal per content frame (= per eye)
    if(subFrameNo != 0)
        return;

    const long long now = presentQpc;

    // positive offset: signal `offset` after vsync. negative: signal before the NEXT vsync, i.e. for the other eye one period later.
    const double offset = m_s->syncOffsetUs;
    Request r;
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
        {
            m_queue.pop_front();
            m_trace.queueFull++; Rec('Q');
        }
        m_queue.push_back(r);
        Rec('F', (int)r.eye, (int)((r.dueQpc - now) * 1000000 / m_qpcFreq), (int)m_queue.size());
    }
    m_cv.notify_one();
}

void EmitterManager::Rec(char type, int a, int b, int c)
{
    if(!g_logEnabled.load(std::memory_order_relaxed))
        return;
    TraceEvent& e = m_events[m_eventN.fetch_add(1) % kEvents];
    e.qpc  = Qpc();
    e.type = type;
    e.a    = a;
    e.b    = b;
    e.c    = c;
}

// O key: the events of the last 3 seconds before the press (frame by frame, signal by signal) and the lines of the log of the same moment, written
// to Nbeam_desync.log next to the program. Each line has the clock time, and the time before the press.
void EmitterManager::DumpRecent(long long pressQpc)
{
    if(!g_logEnabled)
        return;
    const long long from = pressQpc - 3 * m_qpcFreq;
    const long long nowQ = Qpc();
    SYSTEMTIME      st {};
    GetLocalTime(&st);
    const unsigned n     = m_eventN.load();
    const unsigned count = (std::min)(n, kEvents);
    std::vector<TraceEvent> ev;
    for(unsigned i = 0; i < count; i++)
        if(m_events[i].type && m_events[i].qpc >= from && m_events[i].qpc <= nowQ)
            ev.push_back(m_events[i]);
    std::sort(ev.begin(), ev.end(), [](const TraceEvent& x, const TraceEvent& y) { return x.qpc < y.qpc; });
    std::vector<std::string> lines = LsLogRecent(3500);
    std::string              out;
    char                     buf[256];
    snprintf(buf, sizeof(buf), "=== O pressed at %02d:%02d:%02d.%03d: last 3 s, %d events (F = frame queued: eye, signal planned after the screen time (us), queue; S = signal sent: eye, late (us), write (us); "
                               "D = signal dropped, late (us); Q = queue full; M = screen delay moved (refreshes), display mode; L/B = statistics lost / back; T = stale statistics; X = bad screen time; "
                               "J = present grid moved; P = present count gap; C = display mode change; f = odd screen delay ignored) | sync offset %.0f us, eye period %.0f us ===\r\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (int)ev.size(), m_s ? (double)m_s->syncOffsetUs : 0.0, m_periodUs);
    out += buf;
    for(const auto& l : lines)
        out += "  log   " + l + "\r\n";
    for(const auto& e : ev)
    {
        const double msBefore = (double)(nowQ - e.qpc) * 1000.0 / m_qpcFreq;
        // clock time of the event: now minus how long ago it happened
        long long ms = (long long)st.wHour * 3600000 + st.wMinute * 60000 + st.wSecond * 1000 + st.wMilliseconds - (long long)msBefore;
        ms           = (ms % 86400000 + 86400000) % 86400000;
        snprintf(buf, sizeof(buf), "  %02d:%02d:%02d.%03d  %c  %d %d %d\r\n", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60), (int)(ms % 1000), e.type, e.a, e.b, e.c);
        out += buf;
    }
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path);
    const auto   slash = p.find_last_of(L"\\/");
    p                  = (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"Nbeam_desync.log";
    FILE* f            = nullptr;
    if(_wfopen_s(&f, p.c_str(), L"ab") == 0 && f)
    {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        LsLog("O key: the last 3 s of the glasses timing (%d events) are in Nbeam_desync.log", (int)ev.size());
    }
}

// every 10 s (render thread): one line in Nbeam_ls.log when something could have moved the glasses timing (written by a short thread of
// its own: the render thread never waits for the file)
void EmitterManager::TraceTick(long long now)
{
    if(m_traceStart == 0)
        m_traceStart = now;
    if(now - m_traceStart < m_qpcFreq * 10)
        return;
    m_traceStart     = now;
    const auto&  t   = m_trace;
    const int    late = m_lateSignals.exchange(0), lateMax = m_lateMaxUs.exchange(0), dropped = m_droppedSignals.exchange(0);
    const int    wSum = m_writeSumUs.exchange(0), wCount = m_writeCount.exchange(0), wMax = m_writeMaxUs.exchange(0);
    const bool   any = wMax > 1000 || t.staleStats || t.badScanout || t.statsLost || t.statsBack || t.deltaChanges || t.deltaFiltered || t.modeChanges || t.gridJumps || t.presentGaps || t.queueFull || late || dropped || t.periodDriftUs > 20.0;
    if(any && m_s)
    {
        char buf[1100];
        static const char* kComp[] = { "composed by Windows (DWM)", "direct (overlay plane)", "direct (independent flip)", "composition failure" };
        snprintf(buf, sizeof(buf),
                 "glasses timing (10 s): timing %s, display %s, display mode changes %d (composed %d times: pointer visible %d, Nbeam in front %d), frame statistics lost %d / back %d, screen delay changes %d (last %+lld), "
                 "odd readings ignored %d, present grid moved %d, presents skipped %d, signals late >0.3 ms %d (max %.2f ms), dropped %d, queue full %d, "
                 "waiting signals moved %d, stale statistics %d, impossible screen times %d, refresh period drift %.1f us, sync offset %.0f us, "
                 "emitter write average %d us (max %.2f ms, taken off the signal time: %d us)",
                 m_lastState > 0 ? "scanout (DXGI)" : "classic", m_lastComp >= 0 && m_lastComp <= 3 ? kComp[m_lastComp] : "?", t.modeChanges, t.composedTimes, t.composedCursor, t.composedNbeam, t.statsLost, t.statsBack,
                 t.deltaChanges, t.lastDelta, t.deltaFiltered, t.gridJumps, t.presentGaps, late, lateMax / 1000.0, dropped, t.queueFull, t.signalsMoved, t.staleStats, t.badScanout, t.periodDriftUs,
                 (double)m_s->syncOffsetUs, wCount > 0 ? wSum / wCount : 0, wMax / 1000.0, m_writeCompUs.load());
        std::string line(buf);
        std::thread([line]() { LsLog("%s", line.c_str()); }).detach();
    }
    m_trace = SyncTrace {};
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void EmitterManager::ThreadProc()
{
    MmcssGames mmcss; // the glasses signals keep their timing while the game loads / compiles shaders
    // high resolution timer for the long part of the wait (Windows 10 1803 and later; else Sleep(1) as before), the last moments are waited for actively
    struct TimerGuard
    {
        HANDLE h { CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS) };
        ~TimerGuard()
        {
            if(h)
                CloseHandle(h);
        }
    } timer;
    double writeAvgUs = 0.0; // smoothed duration of the write to the emitters (the signal is sent that much earlier so that it arrives at its time)
    int    writeN     = 0;
    m_writeCompUs     = 0;
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
            m_curDue = r.dueQpc; // (the signal being waited for can still be moved, see ScanoutTime)
        }
        // wait until its time; the time is read again while waiting (moved when a frame reached the screen later than planned)
        for(;;)
        {
            if(m_stop)
                return;
            const long long compQpc     = (long long)(m_writeCompUs.load() * m_qpcFreq / 1000000);
            const long long due         = m_curDue.load() - compQpc;
            const double    remainingUs = (due - Qpc()) * 1000000.0 / m_qpcFreq;
            if(remainingUs <= 0)
            {
                r.dueQpc = due;
                break;
            }
            if(timer.h && remainingUs > 1500.0)
            {
                // at most 1 ms at a time: a time moved meanwhile (see ScanoutTime) and the stop are seen at once
                LARGE_INTEGER wait;
                wait.QuadPart = -(LONGLONG)((std::min)(remainingUs - 1200.0, 1000.0) * 10.0);
                if(SetWaitableTimer(timer.h, &wait, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer.h, 5);
                else
                    Sleep(1);
            }
            else if(!timer.h && remainingUs > 2500)
                Sleep(1); // timeBeginPeriod(1) is active in ShaderBeam
            else
                YieldProcessor();
        }
        m_curDue = 0;
        if(r.applyTimings)
        {
            for(auto& e : m_emitters)
                if(!e->IsOnScreen())
                    e->ApplyTimings();
            m_applyQueued = false;
            Rec('A');
            continue;
        }
        // Safety for heavy overloads only: a signal more than one whole display period late belongs to a frame that is no
        // longer on screen; sending it would open the wrong eye, so it is dropped (never happens in normal operation).
        const long long lateQpc = Qpc() - r.dueQpc;
        if(lateQpc > (long long)(m_periodUs * m_qpcFreq / 1e6))
        {
            m_droppedSignals++;
            Rec('D', (int)(lateQpc * 1000000 / m_qpcFreq));
            continue;
        }
        const int lateUs = (int)(lateQpc * 1000000 / m_qpcFreq);
        if(lateUs > 300)
        {
            m_lateSignals++;
            int cur = m_lateMaxUs.load();
            while(lateUs > cur && !m_lateMaxUs.compare_exchange_weak(cur, lateUs))
            { }
        }
        const long long w0 = Qpc();
        for(auto& e : m_emitters)
            if(!e->IsOnScreen())
                e->Sync(r.eye);
        const int writeUs = (int)((Qpc() - w0) * 1000000 / m_qpcFreq);
        Rec('S', (int)r.eye, lateUs, writeUs);
        m_writeSumUs += writeUs;
        m_writeCount++;
        int curMax = m_writeMaxUs.load();
        while(writeUs > curMax && !m_writeMaxUs.compare_exchange_weak(curMax, writeUs))
        { }
        if(writeUs < 2000) // (a write held up by the emitter service does not count: it is not the usual duration)
        {
            writeAvgUs = writeN > 0 ? writeAvgUs * 0.95 + writeUs * 0.05 : (double)writeUs;
            writeN++;
            m_writeCompUs = writeN >= 20 ? (int)(std::min)(writeAvgUs, 1000.0) : 0;
        }
    }
}

} // namespace ShaderBeam
