#include "pacer.h"
#include "backend/uuliteabr.h"
#include "streaming/streamutils.h"

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
#include "streaming/waylandqtsdlbridge.h"
#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>
#include <QtGlobal>
#include <QSettings>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
#define MAX_LATENCY_QUEUED_FRAMES 13
static_assert(PACER_MAX_OUTSTANDING_FRAMES >= MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES must cover pacing/render queues");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_LatencyThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_MinimumLatency(0),
    m_MaxLatencyQueuedFrames(0),
    m_VideoStats(videoStats)
{

}

Pacer::~Pacer()
{
    m_Stopping = true;

    // uu-lite: 先停止抗抖动缓冲线程
    if (m_LatencyThread != nullptr) {
        m_LatencyQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_LatencyThread, nullptr);
    }

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            QMutexLocker locker(&me->m_FrameQueueLock);
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
        }
        else {
            // Let the VSync source wait in the context of our thread
            if (!me->m_VsyncSource->waitForVsync()) {
                // Fall back to a fixed delay if the VSync source fails
                //
                // NB: We don't actually expect signalVsync() to be called, since
                // this is a synchronous VSync source, but we wait on m_VsyncSignalled
                // anyway so that the destructor can immediately wake us on shutdown.
                QMutexLocker locker(&me->m_FrameQueueLock);
                me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 1000 / me->m_DisplayFps);
            }
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        AVFrame* frame;
        {
            // Acquire the frame queue lock to protect the queue and
            // the not empty condition
            QMutexLocker locker(&me->m_FrameQueueLock);

            // Wait for a frame to be ready to render
            while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
                me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
            }

            if (me->m_Stopping) {
                // Exit this thread
                break;
            }

            frame = me->m_RenderQueue.dequeue();
        }

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue, MAX_QUEUED_FRAMES);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping || m_PacingQueue.isEmpty()) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, int minimumLatency, bool adaptiveLatency)
{
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    m_Window = window;
#endif
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    // uu-lite: 接收端抗抖动缓冲（优先级：环境变量 > Moonlight 设置滑条 > 默认 35ms；0=关闭，范围 0-120）
    // 原理：在 pacer 前加一个按帧率匀速释放的队列，把网络抖动/突发摊平，用少量额外延迟换稳定帧数。
    {
        // 优先级：环境变量（Agent/命令行覆盖） > Moonlight 设置滑条（Session 传入） > 默认 35ms
        int latencyMs = minimumLatency;
        if (qEnvironmentVariableIsSet("UULITE_MIN_LATENCY_MS")) {
            latencyMs = qEnvironmentVariableIntValue("UULITE_MIN_LATENCY_MS");
        }
        m_BaseLatency = qBound(0, latencyMs, 120);
        m_AdaptiveLatency = adaptiveLatency && m_BaseLatency > 0;
        const int fpsForMax = qMax(1, m_MaxVideoFps);
        const int frameCapMs = (MAX_LATENCY_QUEUED_FRAMES - 3) * 1000 / fpsForMax;
        // 用户可接受帧延迟 140ms；高帧率下按帧池上限折算
        m_AdaptiveMaxLatency = qMin(140, qMax(m_BaseLatency, frameCapMs));
        m_MinimumLatency = m_BaseLatency;
    }
    if (m_MinimumLatency > 0) {
        m_MaxLatencyQueuedFrames = qBound(2, 3 + m_AdaptiveMaxLatency * m_MaxVideoFps / 1000, MAX_LATENCY_QUEUED_FRAMES);
        m_LastReceivedFrames = m_VideoStats ? m_VideoStats->receivedFrames : 0;
        m_LastNetDroppedFrames = m_VideoStats ? m_VideoStats->networkDroppedFrames : 0;
        m_LastPacerDroppedFrames = m_VideoStats ? m_VideoStats->pacerDroppedFrames : 0;
        m_LastAdaptTicks = SDL_GetTicks() + 5000; // 启动 5s 宽限，避开起流抖动
        m_HoldUntilTicks = 0;
        m_AdaptiveBadTicks = 0;
        m_LatencyThread = SDL_CreateThread(Pacer::latencyThread, "PacerLatency", this);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "uu-lite jitter buffer enabled: base %d ms, adaptive %s (max %d ms, max %d frames)",
                    m_BaseLatency, m_AdaptiveLatency ? "on" : "off", m_AdaptiveMaxLatency, m_MaxLatencyQueuedFrames);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // Render it
    {
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
        auto surfaceLock = WaylandQtSdlBridge::lockSurfaceForRendering(m_Window);
#endif
        m_VsyncRenderer->renderFrame(frame);
    }
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    m_VideoStats->renderedFrames++;

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    QMutexLocker frameQueueLocker(&m_FrameQueueLock);

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        frameQueueLocker.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        frameQueueLocker.relock();
    }
}

// uu-lite: 抗抖动缓冲线程。按目标帧率匀速从 m_LatencyQueue 取帧，
// 并根据帧的实际延迟动态校正释放节奏（目标 m_MinimumLatency）。
int Pacer::latencyThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    SDL_assert(me->m_MaxVideoFps != 0);

    Uint32 startTime = SDL_GetTicks();
    double releasePeriod = 1000.0 / me->m_MaxVideoFps;
    double scheduleShift = 0;
    int iteration = 0;

    while (!me->m_Stopping) {
        iteration++;

        if (me->m_AdaptiveLatency && me->m_MaxVideoFps > 0) {
            Uint32 now = SDL_GetTicks();
            if (now - me->m_LastAdaptTicks >= 1000) {
                const uint32_t received = me->m_VideoStats->receivedFrames;
                const uint32_t netDropped = me->m_VideoStats->networkDroppedFrames;
                const uint32_t pacerDropped = me->m_VideoStats->pacerDroppedFrames;
                const uint32_t dReceived = received >= me->m_LastReceivedFrames ? received - me->m_LastReceivedFrames : 0;
                const uint32_t dNetDropped = netDropped >= me->m_LastNetDroppedFrames ? netDropped - me->m_LastNetDroppedFrames : 0;
                const uint32_t dPacerDropped = pacerDropped >= me->m_LastPacerDroppedFrames ? pacerDropped - me->m_LastPacerDroppedFrames : 0;
                me->m_LastReceivedFrames = received;
                me->m_LastNetDroppedFrames = netDropped;
                me->m_LastPacerDroppedFrames = pacerDropped;
                const double lossPct = (dReceived + dNetDropped) > 0 ? (dNetDropped * 100.0 / (dReceived + dNetDropped)) : 0.0;
                const bool bad = lossPct > 1.0 || me->m_VideoStats->lastRttVariance > 25 || dPacerDropped > 2;
                if (bad) {
                    me->m_AdaptiveBadTicks++;
                    if (me->m_AdaptiveBadTicks >= 2 && me->m_MinimumLatency < me->m_AdaptiveMaxLatency) {
                        const int next = qMin(me->m_AdaptiveMaxLatency, me->m_MinimumLatency + 20);
                        if (next != me->m_MinimumLatency) {
                            me->m_MinimumLatency = next;
                            me->m_HoldUntilTicks = now + 5000;
                            me->m_AdaptiveBadTicks = 0;
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "uu-lite adaptive jitter buffer: %d ms (loss %.1f%%, rttVar %u, pacerDrops %u)",
                                        next, lossPct, me->m_VideoStats->lastRttVariance, dPacerDropped);
                        }
                    }
                }
                else {
                    me->m_AdaptiveBadTicks = 0;
                    if (now >= me->m_HoldUntilTicks && me->m_MinimumLatency > me->m_BaseLatency) {
                        const int next = qMax(me->m_BaseLatency, me->m_MinimumLatency - 10);
                        if (next != me->m_MinimumLatency) {
                            me->m_MinimumLatency = next;
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "uu-lite adaptive jitter buffer: lower to %d ms", next);
                        }
                    }
                }
                me->m_LastAdaptTicks = now;
            }
        }

        double nextReleaseTime = startTime + scheduleShift + releasePeriod * iteration;
        double timeToNextRelease = nextReleaseTime - SDL_GetTicks();
        if (timeToNextRelease < -releasePeriod * 0.5) {
            continue; // 落后太多，跳到下一拍
        }
        if (timeToNextRelease >= 1.0) {
            SDL_Delay((Uint32) timeToNextRelease);
        }
        if (me->m_Stopping) break;

        me->m_LatencyQueueLock.lock();

        int bufferedFrames = 1 + me->m_MinimumLatency * me->m_MaxVideoFps / 1000;
        bufferedFrames = qBound(1, bufferedFrames, MAX_LATENCY_QUEUED_FRAMES);
        int frameDropTarget = bufferedFrames;
        for (int queueHistoryEntry : std::as_const(me->m_LatencyQueueHistory)) {
            if (queueHistoryEntry <= bufferedFrames) {
                frameDropTarget = bufferedFrames + 2;
                break;
            }
        }
        if (me->m_LatencyQueueHistory.count() == me->m_DisplayFps / 2) {
            me->m_LatencyQueueHistory.dequeue();
        }
        me->m_LatencyQueueHistory.enqueue(me->m_LatencyQueue.count());

        while (me->m_LatencyQueue.count() > frameDropTarget) {
            AVFrame* frame = me->m_LatencyQueue.dequeue();
            me->m_LatencyQueueLock.unlock();
            me->m_VideoStats->pacerDroppedFrames++;
            av_frame_free(&frame);
            me->m_LatencyQueueLock.lock();
        }

        while (!me->m_Stopping && me->m_LatencyQueue.empty()) {
            me->m_LatencyQueueNotEmpty.wait(&me->m_LatencyQueueLock);
        }
        if (me->m_Stopping) {
            me->m_LatencyQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_LatencyQueue.dequeue();
        me->m_LatencyQueueLock.unlock();

        int frameLatency = (int) (SDL_GetTicks() - frame->pkt_dts);
        scheduleShift += (me->m_MinimumLatency - frameLatency) * 0.1;
        // 限制校正幅度，避免异常时间戳导致释放节奏跑飞
        scheduleShift = qBound(-200.0, scheduleShift, 200.0);

        me->m_FrameQueueLock.lock();
        if (me->m_VsyncSource == nullptr) {
            me->enqueueFrameForRenderingAndUnlock(frame);
        }
        else {
            me->dropFrameForEnqueue(me->m_PacingQueue, MAX_QUEUED_FRAMES);
            me->m_PacingQueue.enqueue(frame);
            me->m_FrameQueueLock.unlock();
            me->m_PacingQueueNotEmpty.wakeOne();
        }
    }

    return 0;
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue, int maxQueuedFrames)
{
    SDL_assert(queue.size() <= maxQueuedFrames);
    if (queue.size() == maxQueuedFrames) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // uu-lite: 每约 1s 把真实视频丢帧计数上报给 ABR（原子写，任意线程安全）
    if (m_VideoStats) {
        const Uint32 nowTicks = SDL_GetTicks();
        if (m_LastVideoStatsReport == 0 || nowTicks - m_LastVideoStatsReport >= 1000) {
            m_LastVideoStatsReport = nowTicks;
            UuliteAbr::reportVideoCounters(m_VideoStats->receivedFrames,
                                          m_VideoStats->networkDroppedFrames,
                                          m_VideoStats->pacerDroppedFrames);
        }
    }

    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // uu-lite: 有抗抖动缓冲时先进入 latency 队列，由 latencyThread 按帧节奏释放
    if (m_LatencyThread != nullptr) {
        m_LatencyQueueLock.lock();
        dropFrameForEnqueue(m_LatencyQueue, m_MaxLatencyQueuedFrames);
        m_LatencyQueue.enqueue(frame);
        m_LatencyQueueLock.unlock();
        m_LatencyQueueNotEmpty.wakeOne();
        return;
    }

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue, MAX_QUEUED_FRAMES);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
