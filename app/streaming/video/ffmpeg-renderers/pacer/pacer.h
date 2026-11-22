#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

// The maximum number of frames pacer will ever hold is:
// - up to 13 frames in the optional latency (jitter buffer) queue
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
// uu-lite: 增大上式以支持「接收端抗抖动缓冲」（UULITE_MIN_LATENCY_MS，默认 35ms，上限 120ms）
#define PACER_MAX_OUTSTANDING_FRAMES (13 + 3 + 1 + 1)

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual bool waitForVsync()
    {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
        return false;
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, int minimumLatency, bool adaptiveLatency);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    // uu-lite: 接收端抗抖动缓冲线程（可配置最小延迟）
    static int latencyThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue, int maxQueuedFrames);

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    SDL_Window* m_Window = nullptr;
#endif
    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    // uu-lite: 抗抖动缓冲队列
    QQueue<AVFrame*> m_LatencyQueue;
    QQueue<int> m_LatencyQueueHistory;
    QMutex m_LatencyQueueLock;
    QWaitCondition m_LatencyQueueNotEmpty;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    SDL_Thread* m_LatencyThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    int m_MinimumLatency;
    int m_BaseLatency;
    int m_AdaptiveMaxLatency;
    bool m_AdaptiveLatency;
    uint32_t m_LastReceivedFrames;
    uint32_t m_LastNetDroppedFrames;
    uint32_t m_LastPacerDroppedFrames;
    Uint32 m_LastAdaptTicks;
    Uint32 m_HoldUntilTicks;
    Uint32 m_LastVideoStatsReport;
    int m_AdaptiveBadTicks;
    int m_MaxLatencyQueuedFrames;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
};
