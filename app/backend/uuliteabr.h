#pragma once

#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

/**
 * uu-lite 原生 ABR（控制端）。
 *
 * 与 agent/src/abr.js 的 UdpEchoProbe + AbrController 等价：
 *  - 每 500ms 向 host:47931 发 16B 包：'UUL1' + seq(u32 LE) + sentMs(double LE)，主机原样回显；
 *  - 滚动窗口 6s 统计 RTT 中位数 / P75 jitter / loss；
 *  - 每 2s 评估一次，码率变化时发出 bitrateChangeRequested()；
 *  - 启动 5s 预热 + 样本数 < 6 不评估，避免起流瞬间小样本误降；
 *  - 探测完全不通时保持不动。
 *
 * 全部在主线程非阻塞运行；单个 UDP socket + 两个 QTimer，开销可忽略。
 */
class UuliteAbr : public QObject
{
    Q_OBJECT

public:
    explicit UuliteAbr(QObject* parent = nullptr);
    ~UuliteAbr() override;

    // maxBitrateKbps 一般取当前 Moonlight 预设码率；min 默认 max*0.2（不低于 2000）
    void start(const QString& hostIp, int maxBitrateKbps, int warmupMs = 8000, int minSamples = 6);

    // 由 Pacer 每约 1s 从任意线程上报 Moonlight 的真实视频计数（原子存储）。
    static void reportVideoCounters(quint32 received, quint32 networkDropped, quint32 pacerDropped);
    void stop();
    bool active() const { return m_active; }
    int currentBitrate() const { return m_currentBitrate; }
    QJsonObject status() const;

signals:
    // 请求控制端通过面板命令通道下发 set_bitrate（reason/from 仅用于日志与诊断）
    void bitrateChangeRequested(int bitrate, const QString& reason, int from);
    void statsUpdated(const QJsonObject& stats);

private slots:
    void onProbeTick();
    void onEvalTick();
    void onReadyRead();

private:
    struct Sample {
        quint32 seq = 0;
        qint64 atMs = 0;
        int rttMs = -1;
    };

    void sendProbe();
    void pruneSamples();
    QJsonObject computeStats() const;
    void evaluate(const QJsonObject& stats);

    QUdpSocket m_socket;
    QTimer m_probeTimer;
    QTimer m_evalTimer;
    QHostAddress m_host;
    QString m_hostIp;
    quint16 m_port = 47931;

    QVector<Sample> m_samples;
    quint32 m_nextSeq = 0;
    qint64 m_startedAtMs = 0;

    bool m_active = false;
    int m_maxBitrate = 40000;
    int m_minBitrate = 8000;
    int m_currentBitrate = 40000;
    int m_warmupMs = 5000;
    int m_minSamples = 6;

    qint64 m_lastChangeAtMs = 0;
    qint64 m_lastDownAtMs = 0;
    int m_badTicks = 0;
    int m_stableTicks = 0;
    int m_totalChanges = 0;
    int m_probeCeiling = 0;
    int m_ceilingProbeTicks = 0;
    qint64 m_holdUntilMs = 0;
    QString m_lastReason;

    double m_videoLossEwma = 0.0;
    quint32 m_lastVideoReceived = 0;
    quint32 m_lastVideoNetworkDropped = 0;
    bool m_videoCountersPrimed = false;
};
