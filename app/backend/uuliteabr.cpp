#include "uuliteabr.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonValue>
#include <QNetworkDatagram>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>

namespace {
constexpr int kEchoPort = 47931;
constexpr int kProbeIntervalMs = 500;
constexpr int kEvalIntervalMs = 1000;
constexpr int kWindowMs = 6000;
constexpr int kCooldownMs = 2000;
constexpr int kHoldAfterDownMs = 4000;
constexpr int kRecoverTicks = 3;
const char kMagic[4] = {'U', 'U', 'L', '1'};
std::atomic<quint32> g_videoReceived{0};
std::atomic<quint32> g_videoNetworkDropped{0};
std::atomic<quint32> g_videoPacerDropped{0};
} // namespace

UuliteAbr::UuliteAbr(QObject* parent)
    : QObject(parent), m_port(kEchoPort)
{
    m_probeTimer.setInterval(kProbeIntervalMs);
    connect(&m_probeTimer, &QTimer::timeout, this, &UuliteAbr::onProbeTick);
    m_evalTimer.setInterval(kEvalIntervalMs);
    connect(&m_evalTimer, &QTimer::timeout, this, &UuliteAbr::onEvalTick);
    connect(&m_socket, &QUdpSocket::readyRead, this, &UuliteAbr::onReadyRead);
}

void UuliteAbr::reportVideoCounters(quint32 received, quint32 networkDropped, quint32 pacerDropped)
{
    g_videoReceived.store(received, std::memory_order_relaxed);
    g_videoNetworkDropped.store(networkDropped, std::memory_order_relaxed);
    g_videoPacerDropped.store(pacerDropped, std::memory_order_relaxed);
}

UuliteAbr::~UuliteAbr()
{
    stop();
}

void UuliteAbr::start(const QString& hostIp, int maxBitrateKbps, int warmupMs, int minSamples)
{
    stop();

    m_hostIp = hostIp;
    m_host = QHostAddress(hostIp);
    if (m_host.isNull()) {
        qWarning() << "uu-lite ABR: invalid host address" << hostIp;
        return;
    }

    m_maxBitrate = std::max(2000, maxBitrateKbps);
    m_minBitrate = std::max(2000, static_cast<int>(m_maxBitrate * 0.2));
    if (m_minBitrate > m_maxBitrate) {
        m_minBitrate = m_maxBitrate;
    }
    // 起流先保守（30M 档 70%，更高 60%），ABR 探测稳定后快速爬升，避免一上来打爆上行链路。
    const int startupRatio = 35; // 首帧/关键帧保守起步；稳定后 ABR 加性爬升
    m_currentBitrate = std::max(m_minBitrate, std::min(m_maxBitrate, std::max(6000, (m_maxBitrate * startupRatio) / 100)));
    m_warmupMs = std::max(0, warmupMs);
    m_minSamples = std::max(1, minSamples);

    m_samples.clear();
    m_samples.reserve(64);
    m_nextSeq = 0;
    m_startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_lastChangeAtMs = 0;
    m_lastDownAtMs = 0;
    m_badTicks = 0;
    m_stableTicks = 0;
    m_totalChanges = 0;
    const double initialCeilingRatio = (m_maxBitrate <= 40000) ? 0.85 : 0.70;
    m_probeCeiling = std::max(m_minBitrate, std::min(m_maxBitrate, static_cast<int>(std::lround(m_maxBitrate * initialCeilingRatio))));
    m_ceilingProbeTicks = 0;
    m_holdUntilMs = 0;
    m_videoLossEwma = 0.0;
    m_lastVideoReceived = g_videoReceived.load(std::memory_order_relaxed);
    m_lastVideoNetworkDropped = g_videoNetworkDropped.load(std::memory_order_relaxed);
    m_videoCountersPrimed = true;
    m_lastReason.clear();
    m_active = true;

    if (m_socket.state() != QAbstractSocket::BoundState) {
        if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
            qWarning() << "uu-lite ABR: UDP bind failed:" << m_socket.errorString();
            m_active = false;
            return;
        }
    }

    qInfo() << "uu-lite ABR: probing" << hostIp << m_port << "max" << m_maxBitrate
            << "min" << m_minBitrate;
    m_probeTimer.start();
    m_evalTimer.start();
    if (m_currentBitrate < m_maxBitrate) {
        m_lastReason = QStringLiteral("startup");
        m_lastChangeAtMs = QDateTime::currentMSecsSinceEpoch();
        m_totalChanges = 1;
        qInfo() << "uu-lite ABR: startup at" << m_currentBitrate << "kbps (max" << m_maxBitrate << ")";
        emit bitrateChangeRequested(m_currentBitrate, m_lastReason, m_maxBitrate);
    }
}

void UuliteAbr::stop()
{
    m_probeTimer.stop();
    m_evalTimer.stop();
    if (m_socket.state() == QAbstractSocket::BoundState) {
        m_socket.close();
    }
    m_samples.clear();
    m_active = false;
}

void UuliteAbr::onProbeTick()
{
    if (!m_active) return;
    sendProbe();
}

void UuliteAbr::sendProbe()
{
    const quint32 seq = ++m_nextSeq;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QByteArray packet(16, '\0');
    std::memcpy(packet.data(), kMagic, sizeof(kMagic));
    qToLittleEndian<quint32>(seq, reinterpret_cast<uchar*>(packet.data() + 4));
    quint64 tsBits = 0;
    std::memcpy(&tsBits, &nowMs, sizeof(tsBits));
    qToLittleEndian<quint64>(tsBits, reinterpret_cast<uchar*>(packet.data() + 8));

    m_samples.append({seq, nowMs, -1});
    pruneSamples();

    m_socket.writeDatagram(packet, m_host, m_port);
}

void UuliteAbr::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram();
        const QByteArray data = datagram.data();
        if (data.size() < 16 || std::memcmp(data.constData(), kMagic, 4) != 0) {
            continue;
        }
        const quint32 seq = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 4));
        for (Sample& sample : m_samples) {
            if (sample.seq == seq && sample.rttMs < 0) {
                sample.rttMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - sample.atMs);
                break;
            }
        }
    }
}

void UuliteAbr::pruneSamples()
{
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kWindowMs;
    while (!m_samples.isEmpty() && m_samples.first().atMs < cutoff) {
        m_samples.removeFirst();
    }
    if (m_samples.size() > 400) {
        m_samples.remove(0, m_samples.size() - 400);
    }
}

QJsonObject UuliteAbr::computeStats() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<int> rtts;
    int lost = 0;
    for (const Sample& sample : m_samples) {
        if (sample.rttMs >= 0) {
            rtts.append(sample.rttMs);
        } else if (now - sample.atMs > kProbeIntervalMs * 2) {
            ++lost;
        }
    }

    QVector<int> sorted = rtts;
    std::sort(sorted.begin(), sorted.end());
    const int median = sorted.isEmpty() ? 0 : sorted.at(sorted.size() / 2);

    QVector<int> diffs;
    for (int i = 1; i < rtts.size(); ++i) {
        diffs.append(std::abs(rtts.at(i) - rtts.at(i - 1)));
    }
    std::sort(diffs.begin(), diffs.end());
    const int jitter = diffs.isEmpty() ? 0 : diffs.at(static_cast<int>(diffs.size() * 0.75));

    const int sent = m_samples.size();
    const int recv = rtts.size();
    const double lossPct = sent > 0 ? (lost * 100.0 / sent) : 0.0;

    QJsonObject stats;
    stats.insert(QStringLiteral("ok"), recv > 0);
    stats.insert(QStringLiteral("sent"), sent);
    stats.insert(QStringLiteral("recv"), recv);
    stats.insert(QStringLiteral("lost"), lost);
    stats.insert(QStringLiteral("loss_pct"), std::round(lossPct * 10.0) / 10.0);
    stats.insert(QStringLiteral("rtt_ms"), median);
    stats.insert(QStringLiteral("jitter_ms"), jitter);
    return stats;
}

void UuliteAbr::onEvalTick()
{
    if (!m_active) return;

    const QJsonObject stats = computeStats();
    emit statsUpdated(stats);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_warmupMs > 0 && now - m_startedAtMs < m_warmupMs) {
        m_badTicks = 0;
        m_stableTicks = 0;
        return;
    }
    if (stats.value(QStringLiteral("sent")).toInt() < m_minSamples) {
        m_badTicks = 0;
        m_stableTicks = 0;
        return;
    }
    if (!stats.value(QStringLiteral("ok")).toBool()) {
        return;
    }

    evaluate(stats);
}

void UuliteAbr::evaluate(const QJsonObject& stats)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double probeLoss = stats.value(QStringLiteral("loss_pct")).toDouble();
    // Pacer 每约 1s 上报的是 Moonlight 当前 1s 区间的视频统计（解码器会每秒清零），
    // 因此这里直接按“本区间”计算丢帧率，不做跨区间差值。
    const quint32 videoReceived = g_videoReceived.load(std::memory_order_relaxed);
    const quint32 videoNetworkDropped = g_videoNetworkDropped.load(std::memory_order_relaxed);
    (void) g_videoPacerDropped.load(std::memory_order_relaxed);
    double videoLossPct = 0.0;
    if (videoReceived + videoNetworkDropped > 0) {
        videoLossPct = (videoNetworkDropped * 100.0) / (videoReceived + videoNetworkDropped);
    }
    m_videoLossEwma = (m_videoLossEwma * 0.5) + (videoLossPct * 0.5);
    const double loss = std::max(probeLoss, m_videoLossEwma);
    if (videoNetworkDropped > 0 && videoLossPct > 0.5) {
        qInfo() << "uu-lite ABR video stats:" << videoReceived << "recv" << videoNetworkDropped << "lost, loss" << QString::number(videoLossPct, 'f', 2) << "%, ewma" << QString::number(m_videoLossEwma, 'f', 2) << "%";
    }
    const int rtt = stats.value(QStringLiteral("rtt_ms")).toInt();
    const int jitter = stats.value(QStringLiteral("jitter_ms")).toInt();

    const bool emergency = loss >= 8.0;
    const bool severe = loss >= 3.0 || rtt > 250 || jitter > 60;
    const bool mild = loss >= 1.0 || rtt > 150 || jitter > 35;
    const bool healthy = loss < 0.8 && jitter < 25 && rtt < 100;

    const auto clampBitrate = [this](int value) {
        return std::max(m_minBitrate, std::min(m_maxBitrate, value));
    };
    const auto emitChange = [this, now](int from, int to, const QString& reason, bool down) {
        if (to == from) {
            return;
        }
        m_currentBitrate = to;
        m_lastChangeAtMs = now;
        if (down) {
            m_lastDownAtMs = now;
        }
        m_lastReason = reason;
        ++m_totalChanges;
        qInfo() << "uu-lite ABR:" << from << "->" << to << "kbps (" << reason << ")";
        emit bitrateChangeRequested(to, reason, from);
    };

    if (emergency || severe || mild) {
        m_stableTicks = 0;
        m_ceilingProbeTicks = 0;
        ++m_badTicks;
        const int required = emergency ? 1 : 2;
        if (m_badTicks < required) {
            return;
        }
        m_badTicks = 0;

        // 一次丢包事件只降一档，并进入 hold；避免旧版连续 52→10 的雪崩。
        if (m_holdUntilMs > 0 && now < m_holdUntilMs) {
            return;
        }

        const int from = m_currentBitrate;
        const double factor = emergency ? 0.65 : (severe ? 0.8 : 0.9);
        QString reason;
        if (loss >= 1.0) {
            reason = QStringLiteral("loss %1%").arg(loss, 0, 'f', 1);
        } else if (rtt > 150) {
            reason = QStringLiteral("rtt %1ms").arg(rtt);
        } else {
            reason = QStringLiteral("jitter %1ms").arg(jitter);
        }

        // 只有“真实丢包/视频丢帧”才下调安全上限；单纯 jitter/rtt 抖动只降当前码率，不永久压低上限。
        const bool lossDriven = (probeLoss >= 1.0) || (m_videoLossEwma >= 0.8);
        if (lossDriven) {
            const int learnedCeiling = clampBitrate(static_cast<int>(std::lround(from * 0.85)));
            if (m_probeCeiling <= 0) {
                m_probeCeiling = learnedCeiling;
            } else {
                m_probeCeiling = std::min(m_probeCeiling, learnedCeiling);
            }
            m_probeCeiling = std::max(m_probeCeiling, m_minBitrate);
        }

        int target = static_cast<int>(std::lround(from * factor));
        target = std::min(target, m_probeCeiling);
        if (target >= from) {
            target = std::max(m_minBitrate, from - 1000);
        }
        target = clampBitrate(target);
        if (target == from) {
            return;
        }

        m_holdUntilMs = now + (emergency ? 8000 : 5000);
        qInfo() << "uu-lite ABR: ceiling" << m_probeCeiling << "hold" << (emergency ? 8000 : 5000) << "ms";
        emitChange(from, target, reason, true);
        return;
    }

    if (!healthy) {
        m_badTicks = 0;
        m_stableTicks = 0;
        m_ceilingProbeTicks = 0;
        return;
    }

    m_badTicks = 0;
    if (m_holdUntilMs > 0 && now < m_holdUntilMs) {
        return;
    }

    if (m_currentBitrate < m_probeCeiling) {
        m_ceilingProbeTicks = 0;
        ++m_stableTicks;
        if (m_stableTicks >= kRecoverTicks) {
            const int step = std::max(1000, std::min(3000, static_cast<int>(std::lround(m_currentBitrate * 0.10))));
            const int target = clampBitrate(std::min(m_probeCeiling, m_currentBitrate + step));
            m_stableTicks = 0;
            emitChange(m_currentBitrate, target, QStringLiteral("probe_up"), false);
        }
        return;
    }

    // 已经到本次学到的安全上限：稳定 30s 才缓慢上探 5%，避免长期停在 85% 上限。
    m_stableTicks = 0;
    ++m_ceilingProbeTicks;
    if (m_ceilingProbeTicks >= 30 && m_probeCeiling < m_maxBitrate) {
        m_ceilingProbeTicks = 0;
        const int nextCeiling = clampBitrate(std::max(m_probeCeiling + 1000, static_cast<int>(std::lround(m_probeCeiling * 1.05))));
        const int step = std::max(1000, std::min(3000, static_cast<int>(std::lround(m_currentBitrate * 0.10))));
        const int target = clampBitrate(std::min(nextCeiling, m_currentBitrate + step));
        if (target > m_currentBitrate) {
            m_probeCeiling = nextCeiling;
            emitChange(m_currentBitrate, target, QStringLiteral("ceiling_up"), false);
        }
    }
}
QJsonObject UuliteAbr::status() const
{
    const QJsonObject stats = computeStats();
    QJsonObject out;
    out.insert(QStringLiteral("active"), m_active);
    out.insert(QStringLiteral("host"), m_hostIp);
    out.insert(QStringLiteral("current_bitrate"), m_currentBitrate);
    out.insert(QStringLiteral("max_bitrate"), m_maxBitrate);
    out.insert(QStringLiteral("min_bitrate"), m_minBitrate);
    out.insert(QStringLiteral("total_changes"), m_totalChanges);
    out.insert(QStringLiteral("last_reason"), m_lastReason);
    out.insert(QStringLiteral("video_loss_pct"), std::round(m_videoLossEwma * 10.0) / 10.0);
    out.insert(QStringLiteral("probe_loss_pct"), stats.value(QStringLiteral("loss_pct")));
    out.insert(QStringLiteral("video_received"), static_cast<qint64>(g_videoReceived.load(std::memory_order_relaxed)));
    out.insert(QStringLiteral("video_network_dropped"), static_cast<qint64>(g_videoNetworkDropped.load(std::memory_order_relaxed)));
    out.insert(QStringLiteral("metrics"), stats);
    return out;
}
