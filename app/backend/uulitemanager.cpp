#include "uulitemanager.h"
#include "uuliteclient.h"
#include "uuliteabr.h"
#include "uulitecredential.h"
#include "computermanager.h"
#include "nvcomputer.h"
#include <QTimer>

#include <QCoreApplication>
#include <algorithm>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>
#include <QRandomGenerator>

static UuliteManager* s_instance = nullptr;

UuliteManager* UuliteManager::instance()
{
    return s_instance;
}

UuliteManager::UuliteManager(QObject* parent)
    : QObject(parent),
      m_client(new UuliteClient(this)),
      m_abr(new UuliteAbr(this)),
      m_enabled(false),
      m_busy(false),
      m_abrMaxMbps(28),
      m_refreshFailures(0)
{
    s_instance = this;

    loadConfig();

    connect(m_client, &UuliteClient::registered, this, &UuliteManager::onRegistered);
    connect(m_client, &UuliteClient::heartbeatFinished, this, &UuliteManager::onHeartbeatFinished);
    connect(m_client, &UuliteClient::devicesFinished, this, &UuliteManager::onDevicesFinished);
    connect(m_client, &UuliteClient::commandQueued, this, [](bool ok, const QString& id, const QString& error) {
        if (!ok) {
            qWarning() << "uu-lite: send command failed:" << error;
        } else {
            qInfo() << "uu-lite: command queued" << id;
        }
    });
    connect(m_abr, &UuliteAbr::bitrateChangeRequested, this, &UuliteManager::onBitrateChangeRequested);
    connect(m_abr, &UuliteAbr::statsUpdated, this, [](const QJsonObject&) {});

    m_heartbeatTimer.setInterval(3000);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &UuliteManager::onHeartbeatTimer);
    m_refreshTimer.setInterval(10000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &UuliteManager::onRefreshTimer);

    if (m_enabled) {
        startRuntime();
    } else {
        setStatus(QStringLiteral("未启用"));
    }
}

UuliteManager::~UuliteManager()
{
    m_heartbeatTimer.stop();
    m_refreshTimer.stop();
    if (m_abr) {
        m_abr->stop();
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void UuliteManager::loadConfig()
{
    QSettings settings;
    m_panelUrl = settings.value(QStringLiteral("uulite/panel_url")).toString().trimmed();
    m_deviceName = settings.value(QStringLiteral("uulite/device_name")).toString().trimmed();
    if (m_deviceName.isEmpty()) {
        m_deviceName = QSysInfo::machineHostName();
    }
    m_deviceId = settings.value(QStringLiteral("uulite/device_id")).toString().trimmed();
    if (m_deviceId.isEmpty()) {
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
        // 32 hex, stable across restarts
        settings.setValue(QStringLiteral("uulite/device_id"), m_deviceId);
    }
    m_enabled = settings.value(QStringLiteral("uulite/enabled"), false).toBool();
    m_abrMaxMbps = settings.value(QStringLiteral("uulite/abr_max_mbps"), 28).toInt();
    if (m_abrMaxMbps < 0) m_abrMaxMbps = 0;

    const QString sealed = settings.value(QStringLiteral("uulite/secret_dpapi")).toString();
    m_secret = sealed.isEmpty() ? QString() : UuliteCredentialStore::unprotectFromHex(sealed);

    if (m_secret.isEmpty()) {
        m_enabled = false;
    }
}

void UuliteManager::persistConfig()
{
    QSettings settings;
    settings.setValue(QStringLiteral("uulite/panel_url"), m_panelUrl);
    settings.setValue(QStringLiteral("uulite/device_name"), m_deviceName);
    settings.setValue(QStringLiteral("uulite/device_id"), m_deviceId);
    settings.setValue(QStringLiteral("uulite/enabled"), m_enabled);
    if (m_secret.isEmpty()) {
        settings.remove(QStringLiteral("uulite/secret_dpapi"));
    } else {
        settings.setValue(QStringLiteral("uulite/secret_dpapi"),
                          UuliteCredentialStore::protectToHex(m_secret));
    }
    settings.sync();
    emit configChanged();
}

void UuliteManager::saveConfig(const QString& panelUrl, const QString& secret,
                               const QString& deviceName, bool enable)
{
    const QString url = panelUrl.trimmed();
    if (!url.isEmpty() && !url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        setStatus(QStringLiteral("面板地址需要以 http:// 或 https:// 开头"));
        return;
    }
    m_panelUrl = url;
    if (!secret.trimmed().isEmpty()) {
        m_secret = secret.trimmed();
    }
    const QString name = deviceName.trimmed();
    m_deviceName = name.isEmpty() ? QSysInfo::machineHostName() : name;
    m_enabled = enable;

    persistConfig();

    if (m_enabled) {
        startRuntime();
    } else {
        stopRuntime();
        setStatus(QStringLiteral("已停用"));
    }
}

void UuliteManager::refresh()
{
    if (!m_enabled || m_secret.isEmpty() || m_panelUrl.isEmpty()) {
        setStatus(QStringLiteral("请先启用并填写面板地址/secret"));
        return;
    }
    m_client->fetchDevices();
}

void UuliteManager::startRuntime()
{
    if (m_panelUrl.isEmpty() || m_secret.isEmpty()) {
        setStatus(QStringLiteral("请先填写面板地址和 secret"));
        return;
    }

    m_heartbeatTimer.stop();
    m_refreshTimer.stop();
    m_ackCommandIds.clear();
    m_refreshFailures = 0;
    m_devices = QJsonArray();
    m_hostToDeviceId.clear();
    m_deviceNames.clear();
    updateDevices(m_devices);

    m_client->setConfig(m_panelUrl, m_secret, m_deviceId, m_deviceName);
    setBusy(true);
    setStatus(QStringLiteral("正在注册到面板…"));
    m_client->registerDevice(localCandidates());

    m_heartbeatTimer.start();
    m_refreshTimer.start();
}

void UuliteManager::stopRuntime()
{
    m_heartbeatTimer.stop();
    m_refreshTimer.stop();
    stopAbr();
    m_ackCommandIds.clear();
    m_devices = QJsonArray();
    m_hostToDeviceId.clear();
    m_deviceNames.clear();
    updateDevices(m_devices);
    setBusy(false);
}

void UuliteManager::onHeartbeatTimer()
{
    if (!m_enabled) return;
    if (!m_client->hasConfig()) return;

    QJsonObject stats;
    stats.insert(QStringLiteral("streaming"), m_abr && m_abr->active());
    if (m_abr && m_abr->active()) {
        stats.insert(QStringLiteral("abr"), m_abr->status());
    }
    stats.insert(QStringLiteral("sunshine_running"), false);

    m_client->heartbeat(stats, m_ackCommandIds, localCandidates());
    m_ackCommandIds.clear();
}

void UuliteManager::onRefreshTimer()
{
    if (!m_enabled) return;
    m_client->fetchDevices();
}

void UuliteManager::onRegistered(bool ok, const QString& error)
{
    setBusy(false);
    if (!ok) {
        ++m_refreshFailures;
        setStatus(QStringLiteral("注册失败：%1").arg(error));
        return;
    }
    m_refreshFailures = 0;
    setStatus(QStringLiteral("已连接面板"));
    onHeartbeatTimer();
    m_client->fetchDevices();
}

void UuliteManager::onHeartbeatFinished(bool ok, const QJsonArray& peers, const QJsonArray& commands,
                                        const QString& error)
{
    if (!ok) {
        if (error.contains(QStringLiteral("bad_token")) ||
            error.contains(QStringLiteral("device_not_registered")) ||
            error.contains(QStringLiteral("unauthorized"))) {
            setStatus(QStringLiteral("设备未注册，正在重新注册…"));
            m_client->registerDevice(localCandidates());
            return;
        }
        setStatus(QStringLiteral("心跳失败：%1").arg(error));
        return;
    }

    updateDevices(peers);

    for (const QJsonValue& value : commands) {
        const QJsonObject command = value.toObject();
        const QString id = command.value(QStringLiteral("id")).toString();
        const QString action = command.value(QStringLiteral("action")).toString();
        if (!id.isEmpty()) {
            m_ackCommandIds.append(id);
        }
        if (action == QLatin1String("pair_pin")) {
            onPairPin(command.value(QStringLiteral("args")).toObject());
        } else {
            qInfo() << "uu-lite: received command" << action << id;
        }
    }

    if (m_enabled) {
        setStatus(QStringLiteral("已连接面板 · ABR %1")
                      .arg(m_abr && m_abr->active() ? QStringLiteral("运行中") : QStringLiteral("待机")));
    }
}

void UuliteManager::onDevicesFinished(bool ok, const QJsonArray& devices, const QString& error)
{
    if (!ok) {
        qWarning() << "uu-lite: fetch devices failed:" << error;
        return;
    }
    updateDevices(devices);
}

void UuliteManager::updateDevices(const QJsonArray& devices)
{
    m_devices = devices;
    m_hostToDeviceId.clear();
    m_deviceNames.clear();

    QStringList onlineNames;
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        const QString id = device.value(QStringLiteral("device_id")).toString();
        const QString name = device.value(QStringLiteral("name")).toString();
        const bool online = device.value(QStringLiteral("online")).toBool();
        if (id.isEmpty()) continue;
        m_deviceNames.insert(id, name);
        if (online) {
            onlineNames.append(name.isEmpty() ? id.left(8) : name);
        }
        for (const QJsonValue& addrValue : device.value(QStringLiteral("addresses")).toArray()) {
            const QJsonObject addr = addrValue.toObject();
            const QString host = addr.value(QStringLiteral("host")).toString();
            if (!host.isEmpty()) {
                m_hostToDeviceId.insert(host, id);
            }
        }
    }

    if (onlineNames.isEmpty()) {
        setDevicesText(QStringLiteral("面板上暂无在线设备"));
    } else {
        onlineNames.sort(Qt::CaseInsensitive);
        setDevicesText(QStringLiteral("在线：%1").arg(onlineNames.join(QStringLiteral("、"))));
    }
}

QString UuliteManager::findDeviceIdByHost(const QString& hostIp) const
{
    return m_hostToDeviceId.value(hostIp);
}

QString UuliteManager::findHostAddress(const QJsonArray& devices, const QString& deviceId) const
{
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        if (device.value(QStringLiteral("device_id")).toString() != deviceId) continue;
        for (const QJsonValue& addrValue : device.value(QStringLiteral("addresses")).toArray()) {
            const QJsonObject addr = addrValue.toObject();
            if (addr.value(QStringLiteral("kind")).toString() == QLatin1String("ts")) {
                return addr.value(QStringLiteral("host")).toString();
            }
        }
    }
    return {};
}

void UuliteManager::setAbrMaxMbps(int mbps)
{
    const int clamped = qBound(0, mbps, 200);
    if (m_abrMaxMbps == clamped) {
        return;
    }
    m_abrMaxMbps = clamped;
    QSettings settings;
    settings.setValue(QStringLiteral("uulite/abr_max_mbps"), m_abrMaxMbps);
    settings.sync();
    emit configChanged();
}

int UuliteManager::initialBitrateForHost(const QString& hostIp, int configuredKbps) const
{
    const int maxBitrate = capBitrateForHost(hostIp, configuredKbps);
    if (m_abrMaxMbps <= 0) {
        return maxBitrate;
    }
    // 起流用低码率保守试探（约上限的 35%，至少 8M），避免首帧/keyframe 就打爆上行；
    // ABR 会在探测稳定后快速加性爬升。
    return std::max(8000, std::min(maxBitrate, (maxBitrate * 35) / 100));
}
int UuliteManager::capBitrateForHost(const QString& hostIp, int configuredKbps) const
{
    Q_UNUSED(hostIp);
    if (m_abrMaxMbps <= 0) {
        return configuredKbps;
    }
    return std::min(configuredKbps, m_abrMaxMbps * 1000);
}
void UuliteManager::startAbrForHost(const QString& hostIp, int maxBitrateKbps)
{
    if (!m_enabled || m_secret.isEmpty() || hostIp.isEmpty()) {
        return;
    }
    const QString deviceId = findDeviceIdByHost(hostIp);
    if (deviceId.isEmpty()) {
        // 设备列表可能还没刷新：记住目标，等 updateDevices() 拿到映射后再启动。
        m_pendingAbrHost = hostIp;
        m_pendingAbrBitrate = maxBitrateKbps;
        qInfo() << "uu-lite: host" << hostIp << "not in panel devices yet, defer native ABR";
        return;
    }
    m_pendingAbrHost.clear();
    m_pendingAbrBitrate = 0;
    if (deviceId == m_deviceId) {
        return;
    }

    const int effectiveMax = capBitrateForHost(hostIp, maxBitrateKbps > 0 ? maxBitrateKbps : 40000);
    m_abrHostIp = hostIp;
    m_abrHostDeviceId = deviceId;
    m_abr->start(hostIp, effectiveMax);
    qInfo() << "uu-lite: native ABR started for" << hostIp << "device" << deviceId
            << "max" << maxBitrateKbps;
    setStatus(QStringLiteral("已连接面板 · ABR 运行中"));
}

void UuliteManager::stopAbr()
{
    if (m_abr) {
        m_abr->stop();
    }
    if (!m_abrHostDeviceId.isEmpty()) {
        qInfo() << "uu-lite: native ABR stopped for" << m_abrHostIp;
    }
    m_abrHostIp.clear();
    m_abrHostDeviceId.clear();
    m_pendingAbrHost.clear();
    m_pendingAbrBitrate = 0;
}

void UuliteManager::onBitrateChangeRequested(int bitrate, const QString& reason, int from)
{
    if (m_abrHostDeviceId.isEmpty()) {
        return;
    }
    QJsonObject args;
    args.insert(QStringLiteral("bitrate"), bitrate);
    args.insert(QStringLiteral("reason"), reason);
    args.insert(QStringLiteral("from"), from);
    m_client->sendCommand(m_abrHostDeviceId, QStringLiteral("set_bitrate"), args);
    qInfo() << "uu-lite: ABR" << from << "->" << bitrate << "kbps (" << reason << ")";
}

QStringList UuliteManager::localCandidates() const
{
    QStringList out;
    const auto addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress& address : addresses) {
        if (address.protocol() != QAbstractSocket::IPv4Protocol) continue;
        if (address.isLoopback() || address.isLinkLocal()) continue;

        QString kind;
        if (address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)) {
            kind = QStringLiteral("ts");
        } else if (address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
                   address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
                   address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)) {
            kind = QStringLiteral("lan");
        } else {
            continue;
        }
        out.append(QStringLiteral("%1:%2:47989").arg(kind, address.toString()));
    }
    out.removeDuplicates();
    return out;
}

void UuliteManager::setStatus(const QString& text)
{
    if (m_statusText == text) return;
    m_statusText = text;
    emit statusChanged();
}

void UuliteManager::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void UuliteManager::setDevicesText(const QString& text)
{
    if (m_devicesText == text) return;
    m_devicesText = text;
    emit devicesChanged();
}
void UuliteManager::tryStartPendingAbr()
{
    if (m_pendingAbrHost.isEmpty()) {
        return;
    }
    const QString deviceId = findDeviceIdByHost(m_pendingAbrHost);
    if (deviceId.isEmpty() || deviceId == m_deviceId) {
        return;
    }
    const QString host = m_pendingAbrHost;
    const int bitrate = m_pendingAbrBitrate > 0 ? m_pendingAbrBitrate : 40000;
    m_pendingAbrHost.clear();
    m_pendingAbrBitrate = 0;
    startAbrForHost(host, bitrate);
}

void UuliteManager::attachComputerManager(QObject* computerManager)
{
    m_computerManager = qobject_cast<ComputerManager*>(computerManager);
    if (!m_computerManager) {
        qWarning() << "uu-lite: attachComputerManager got non-ComputerManager object";
        return;
    }
    connect(m_computerManager, &ComputerManager::pairingCompleted, this,
            [this](NvComputer* computer, const QString& error) {
                const QString host = computer ? computer->activeAddress.address() : QString();
                qInfo() << "uu-lite: pairing completed host=" << host << "error=" << error;
                if (error.isEmpty()) {
                    m_pairRetryAfter.remove(host);
                    setStatus(QStringLiteral("自动配对成功 · %1").arg(host));
                } else {
                    m_pairRetryAfter.insert(host, QDateTime::currentMSecsSinceEpoch() + 60000);
                    setStatus(QStringLiteral("自动配对失败：%1").arg(error));
                }
                m_pairHostIp.clear();
                m_pairHostDeviceId.clear();
                m_pairDeadlineMs = 0;
            });
    connect(m_computerManager, &ComputerManager::computerAddCompleted, this,
            [this](QVariant, QVariant) { QTimer::singleShot(2000, this, [this]() { maybeAutoPair(); }); });
    QTimer::singleShot(3000, this, [this]() { maybeAutoPair(); });
}

NvComputer* UuliteManager::findComputerByHost(const QString& host) const
{
    if (!m_computerManager || host.isEmpty()) {
        return nullptr;
    }
    for (NvComputer* computer : m_computerManager->getComputers()) {
        if (!computer) continue;
        if (computer->activeAddress.address() == host ||
            computer->localAddress.address() == host ||
            computer->manualAddress.address() == host) {
            return computer;
        }
    }
    return nullptr;
}

void UuliteManager::maybeAutoPair()
{
    if (!m_enabled || !m_computerManager || !m_pairHostIp.isEmpty()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonValue& value : m_devices) {
        const QJsonObject device = value.toObject();
        if (!device.value(QStringLiteral("online")).toBool()) continue;
        const QString id = device.value(QStringLiteral("device_id")).toString();
        if (id.isEmpty() || id == m_deviceId) continue;

        QString tsHost;
        for (const QJsonValue& addrValue : device.value(QStringLiteral("addresses")).toArray()) {
            const QJsonObject addr = addrValue.toObject();
            if (addr.value(QStringLiteral("kind")).toString() == QLatin1String("ts")) {
                tsHost = addr.value(QStringLiteral("host")).toString();
                if (!tsHost.isEmpty()) break;
            }
        }
        if (tsHost.isEmpty()) continue;
        if (m_pairRetryAfter.value(tsHost, 0) > now) continue;

        NvComputer* computer = findComputerByHost(tsHost);
        if (!computer || computer->pairState == NvComputer::PS_PAIRED) continue;

        // 先请主机 Agent 生成 PIN，并等待它通过 pair_pin 命令回传。
        m_pairHostIp = tsHost;
        m_pairHostDeviceId = id;
        m_pairDeadlineMs = now + 12000;
        const int fallbackPin = 1000 + (QRandomGenerator::global()->bounded(9000));
        QJsonObject args;
        args.insert(QStringLiteral("mode"), QStringLiteral("qr_preferred"));
        args.insert(QStringLiteral("controller_id"), m_deviceId);
        args.insert(QStringLiteral("name"), m_deviceName);
        args.insert(QStringLiteral("pin"), QString::number(fallbackPin));
        m_client->sendCommand(id, QStringLiteral("pair_accept"), args);
        setStatus(QStringLiteral("正在自动配对 %1…").arg(tsHost));
        qInfo() << "uu-lite: requested auto pairing for" << tsHost << "device" << id;

        QTimer::singleShot(12000, this, [this, tsHost]() {
            if (m_pairHostIp == tsHost) {
                qWarning() << "uu-lite: auto pairing timed out for" << tsHost;
                m_pairRetryAfter.insert(tsHost, QDateTime::currentMSecsSinceEpoch() + 60000);
                m_pairHostIp.clear();
                m_pairHostDeviceId.clear();
                m_pairDeadlineMs = 0;
            }
        });
        return;
    }
}

void UuliteManager::onPairPin(const QJsonObject& args)
{
    const QString pin = args.value(QStringLiteral("pin")).toString();
    if (pin.isEmpty()) {
        qWarning() << "uu-lite: pair_pin without pin";
        return;
    }

    // 主机回传的 host 可能是它的 LAN 地址，而 Moonlight 里登记的是 Tailscale 地址；
    // 依次尝试：回传 host、本端发起时记录的 TS host、provider_id 对应设备的 TS 地址。
    QString host = args.value(QStringLiteral("host")).toString();
    NvComputer* computer = findComputerByHost(host);
    if (!computer) {
        host = m_pairHostIp;
        computer = findComputerByHost(host);
    }
    if (!computer) {
        const QString provider = args.value(QStringLiteral("provider_id")).toString();
        for (const QJsonValue& value : m_devices) {
            const QJsonObject device = value.toObject();
            if (device.value(QStringLiteral("device_id")).toString() != provider) continue;
            for (const QJsonValue& addrValue : device.value(QStringLiteral("addresses")).toArray()) {
                const QJsonObject addr = addrValue.toObject();
                if (addr.value(QStringLiteral("kind")).toString() != QLatin1String("ts")) continue;
                host = addr.value(QStringLiteral("host")).toString();
                computer = findComputerByHost(host);
                if (computer) break;
            }
            if (computer) break;
        }
    }
    if (!computer || host.isEmpty()) {
        qWarning() << "uu-lite: pair_pin host not found in ComputerManager:" << host;
        return;
    }

    qInfo() << "uu-lite: received pairing PIN for" << host << "starting Moonlight pairing";
    m_pairHostIp = host;
    setStatus(QStringLiteral("正在配对 %1…").arg(host));
    m_computerManager->pairHost(computer, pin);
}
