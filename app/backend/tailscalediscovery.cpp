#include "tailscalediscovery.h"
#include "computermanager.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QDebug>

namespace {
    constexpr int kProbeTimeoutMs = 900;
    constexpr int kRetryCooldownMs = 10 * 60 * 1000;
    constexpr quint16 kSunshineHttpPort = 47989;
    const char* const kTailscaleCgnatPrefix = "100.64.0.0";
    constexpr int kTailscaleCgnatBits = 10;
}

TailscaleDiscovery::TailscaleDiscovery(ComputerManager* computerManager)
    : QObject(computerManager),
      m_ComputerManager(computerManager),
      m_ScanTimer(this),
      m_Process(nullptr),
      m_Scanning(false)
{
    m_ScanTimer.setInterval(60 * 1000);
    connect(&m_ScanTimer, &QTimer::timeout, this, &TailscaleDiscovery::scan);
}

void TailscaleDiscovery::start()
{
    // uu-lite: 由 Agent 在走本机代理时设置该环境变量，避免启动后再次改写主机地址
    if (qEnvironmentVariableIsSet("UULITE_DISABLE_TS_DISCOVERY") &&
        qEnvironmentVariableIntValue("UULITE_DISABLE_TS_DISCOVERY") != 0) {
        qInfo() << "Tailscale discovery disabled by UULITE_DISABLE_TS_DISCOVERY";
        return;
    }

    // 启动后稍等应用初始化完成，再开始扫描
    QTimer::singleShot(5000, this, &TailscaleDiscovery::scan);
    m_ScanTimer.start();
    qInfo() << "Tailscale discovery started";
}

QString TailscaleDiscovery::findTailscaleExe() const
{
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/Tailscale/tailscale.exe"),
        QStringLiteral("C:/Program Files (x86)/Tailscale/tailscale.exe"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("tailscale"));
}

void TailscaleDiscovery::scan()
{
    if (m_Scanning || m_Process != nullptr) {
        return;
    }

    const QString exe = findTailscaleExe();
    if (exe.isEmpty()) {
        return; // 未安装 Tailscale
    }

    m_Scanning = true;
    m_Process = new QProcess(this);
    m_Process->setProgram(exe);
    m_Process->setArguments({QStringLiteral("status"), QStringLiteral("--json")});

    connect(m_Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &TailscaleDiscovery::handleStatusFinished);
    connect(m_Process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_Process) {
            m_Process->deleteLater();
            m_Process = nullptr;
        }
        m_Scanning = false;
    });

    // 防止 tailscale CLI 卡住
    QTimer::singleShot(6000, this, [this]() {
        if (m_Process && m_Process->state() != QProcess::NotRunning) {
            m_Process->kill();
        }
    });

    m_Process->start();
}

void TailscaleDiscovery::handleStatusFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess* process = m_Process;
    m_Process = nullptr;
    m_Scanning = false;

    if (!process) {
        return;
    }

    const QByteArray output = process->readAllStandardOutput();
    process->deleteLater();

    if (exitCode != 0 || status != QProcess::NormalExit || output.isEmpty()) {
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Tailscale discovery: invalid status JSON:" << parseError.errorString();
        return;
    }

    // 跳过本机自己的 Tailscale IP
    QHash<QString, bool> selfIps;
    const QJsonObject self = doc.object().value(QStringLiteral("Self")).toObject();
    for (const QJsonValue& value : self.value(QStringLiteral("TailscaleIPs")).toArray()) {
        selfIps.insert(value.toString(), true);
    }

    const QJsonObject peers = doc.object().value(QStringLiteral("Peer")).toObject();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int candidates = 0;

    for (auto it = peers.begin(); it != peers.end(); ++it) {
        const QJsonObject peer = it.value().toObject();
        if (!peer.value(QStringLiteral("Online")).toBool(false)) {
            continue;
        }

        QString ipv4;
        for (const QJsonValue& value : peer.value(QStringLiteral("TailscaleIPs")).toArray()) {
            const QString text = value.toString();
            QHostAddress address;
            if (address.setAddress(text) &&
                address.protocol() == QAbstractSocket::IPv4Protocol &&
                address.isInSubnet(QHostAddress(QString::fromLatin1(kTailscaleCgnatPrefix)), kTailscaleCgnatBits)) {
                ipv4 = text;
                break;
            }
        }
        if (ipv4.isEmpty() || selfIps.contains(ipv4)) {
            continue;
        }

        // 冷却窗口内不重复探测/添加
        const qint64 lastAttempt = m_AttemptedMs.value(ipv4, 0);
        if (lastAttempt != 0 && now - lastAttempt < kRetryCooldownMs) {
            continue;
        }
        m_AttemptedMs.insert(ipv4, now);
        candidates++;
        probeAndAdd(ipv4);
    }

    if (candidates > 0) {
        qInfo() << "Tailscale discovery: probing" << candidates << "peer(s)";
    }
}

void TailscaleDiscovery::probeAndAdd(const QString& ip)
{
    QTcpSocket* socket = new QTcpSocket(this);
    socket->setProperty("uu_ip", ip);

    auto finish = [this, socket]() {
        if (socket) {
            socket->deleteLater();
        }
    };

    // 端口探测：Sunshine 的 HTTP 端口开放即视为候选主机，交给正常流程验证 serverinfo/配对
    connect(socket, &QTcpSocket::connected, this, [this, socket, ip, finish]() {
        qInfo() << "Tailscale discovery: Sunshine found at" << ip;
        m_ComputerManager->addNewHostManually(ip + QStringLiteral(":") + QString::number(kSunshineHttpPort));
        finish();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [socket, finish](QAbstractSocket::SocketError) {
        // 连接失败/超时：冷却期内不再尝试（m_AttemptedMs 已在 scan() 中写入）
        finish();
    });
    QTimer::singleShot(kProbeTimeoutMs, socket, [socket]() {
        if (socket && socket->state() != QAbstractSocket::ConnectedState) {
            socket->abort();
        }
    });

    socket->connectToHost(ip, kSunshineHttpPort);
}
