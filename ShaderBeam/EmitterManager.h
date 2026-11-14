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
    ~EmitterManager();

    // Creates every emitter enabled in `settings` (kept by pointer: live changes of swapEyes / syncOffset apply immediately).
    // The emitters are opened in a thread of their own (the NVIDIA emitter needs about half a second: waking it, programming it), so the
    // picture never stops while the 3D starts; Poll() (render thread) takes them once they are ready.
    bool Start(StereoEmitterSettings* settings, const EmitterEnv& env);
    void Stop();
    bool Active() const { return !m_emitters.empty(); }
    bool Poll(); // render thread: takes the emitters opened by the start thread; true when they were just taken (the status changed)

    // eye of the content frame about to be presented (call before Present, render thread)
    // applySwap=false returns the eye the *image* belongs to (Swap Eyes only reverses the emitter relative to the image)
    Eye NextEye(int frameNo, int subFrameNo, bool applySwap = true);
    // on-screen emitters
    void DrawOverlay(const EmitterFrame& f);
    // SubFrames changed without restart: refresh the sync rate everywhere
    void SetSubFrames(int subFrames);
    // call right after Present returned
    void AfterPresent(Eye eye, int subFrameNo);
    // the length of the presentation queue was changed live (render thread, before a frame): the screen delay is going to change by the same number
    // of refreshes. The change is taken at once when the statistics show it, instead of after two readings.
    void AnnounceQueue(int oldQueue, int newQueue);

    // one line per emitter for the UI ("NVIDIA: connected", "COM3: failed to open", ...)
    std::vector<std::string> Status() const;

private:
    struct Request
    {
        Eye       eye;
        long long dueQpc;
        bool      applyTimings { false }; // not a signal: the held back Open Duration is sent to the emitters (first black frame)
    };
    void ThreadProc();
    long long Qpc() const;

    StereoEmitterSettings*                 m_s { nullptr };
    EmitterEnv                             m_env;
    std::vector<std::unique_ptr<IEmitter>> m_emitters;
    std::vector<std::string>               m_status;

    double    m_periodUs { 8333.0 }; // one eye period
    long long m_qpcFreq { 1 };

    // scanout sync (DXGI frame statistics): refresh n happens at m_syncQpc + (n - m_syncCount) * m_refreshQpc, and present p is shown at
    // refresh p + m_delta. A missed refresh shows up as a larger m_delta, and the next signals follow it at once.
    bool ScanoutTime(long long& vblankQpc);
    void NudgeOffset(const char* why, int refreshes); // offset correction (see EmitterManager.cpp)
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

    // start thread (opens the emitters), its result taken by Poll()
    std::thread                            m_initThread;
    std::atomic<bool>                      m_initDone { false };
    bool                                   m_initTaken { true };
    std::vector<std::unique_ptr<IEmitter>> m_initEmitters;
    std::vector<std::string>               m_initStatus;
    void                                   JoinInit();
    std::mutex                             m_initMutex;
    unsigned                               m_initGen { 0 }; // a start whose number is no longer this one was stopped: it closes what it opened
    std::vector<std::thread>               m_oldInits;      // stopped starts, waited for by the next start

    // tracer of the glasses timing (Nbeam_ls.log): what can move the moment of the signals, summed up every 10 s when something happened
    struct SyncTrace
    {
        int       statsLost { 0 }, statsBack { 0 }; // DXGI frame statistics lost (classic timing used) / available again
        int       deltaChanges { 0 };               // a frame shown one refresh later or earlier than before (missed refresh)
        long long lastDelta { 0 };
        int       gridJumps { 0 };
        int       deltaFiltered { 0 };              // single odd readings of the screen delay, ignored
        int       modeChanges { 0 };                // Windows changed how it shows the picture (composed / direct)
        int       composedTimes { 0 }, composedCursor { 0 }, composedNbeam { 0 }; // switches to the composed display, with the pointer visible / Nbeam in front
        int       signalsMoved { 0 };               // waiting signals moved after a change of the screen delay
        int       staleStats { 0 };                 // frames with statistics far behind the presents (classic timing used)
        int       badScanout { 0 };                 // screen times that could not be right (classic timing used)                  // the regular present grid moved to a new phase
        int       presentGaps { 0 };                // present count jumped by more than one between two content frames
        int       queueFull { 0 };                  // a signal dropped because the queue was full
        double    periodDriftUs { 0.0 };            // largest difference between the measured refresh period and the nominal one
    } m_trace;
    std::atomic<int>    m_lateSignals { 0 };   // signals sent more than 0.3 ms after their time (sync thread)
    std::atomic<int>    m_lateMaxUs { 0 };
    std::atomic<int>    m_droppedSignals { 0 }; // signals more than one period late: not sent
    // duration of the writes to the hardware emitters (sync thread): summed up in the tracer, and the smoothed value is taken off the time of each
    // signal so that it reaches the emitter at its time
    std::atomic<int>    m_writeSumUs { 0 }, m_writeCount { 0 }, m_writeMaxUs { 0 };
    std::atomic<int>    m_writeCompUs { 0 };
    std::atomic<long long> m_curDue { 0 };      // time of the signal the sync thread is waiting for (0 = none)
    long long           m_traceStart { 0 };
    int                 m_lastState { 0 };
    UINT                m_lastPresentNo { 0 };
    bool                m_presentNoValid { false };
    int                 m_expectShift { 0 };      // delay change announced by AnnounceQueue (refreshes), 0 = none
    long long           m_expectUntilQpc { 0 };
    std::atomic<bool>   m_applyQueued { false }; // a request to send the held back Open Duration is waiting in the queue
    long long           m_deferSince { 0 };      // when the Open Duration was held back (given up after 100 ms)
    long long           m_lastShiftQpc { 0 };  // when the Open Duration was last nudged by the offset correction (at most twice a second)
    long long           m_deltaCand { 0 };     // screen delay read last (a new delay is taken when it is read twice in a row)
    int                 m_lastComp { -1 };     // how Windows shows the picture (DXGI_FRAME_PRESENTATION_MODE), -1 = unknown
    winrt::com_ptr<IDXGISwapChainMedia> m_media;
    bool                m_mediaTried { false };
    void                TraceTick(long long now);

    // capture of the O key: the last seconds of the glasses timing, event by event, written to Nbeam_desync.log
    struct TraceEvent
    {
        long long qpc { 0 };
        char      type { 0 };
        int       a { 0 }, b { 0 }, c { 0 };
    };
    static constexpr unsigned kEvents = 8192;
    TraceEvent                m_events[kEvents];
    std::atomic<unsigned>     m_eventN { 0 };
    void                      Rec(char type, int a = 0, int b = 0, int c = 0);
    void                      DumpRecent(long long pressQpc);

    // sync thread
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<Request>     m_queue;
    std::atomic<bool>       m_stop { false };
};

} // namespace ShaderBeam
