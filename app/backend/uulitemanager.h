#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>

class UuliteClient;
class ComputerManager;
class NvComputer;
class UuliteAbr;

/**
 * uu-lite 原生控制器（控制端）。
 *
 * 职责：
 *  - 保存面板地址 / 设备名 / DPAPI 加密的 secret（QSettings）；
 *  - 注册、3s 心跳、拉设备列表，维护 TS 地址 -> host device_id 映射；
 *  - 为当前串流主机启动原生 ABR，码率变化时通过面板命令通道下发 set_bitrate；
 *  - 给设置页暴露状态/QML 属性。
 *
 * 性能：全部异步（QNetworkAccessManager + QTimer + QUdpSocket），无代理、无阻塞；
 * 安全：secret 只经 DPAPI 落盘，日志不输出 secret/token。
 */
class UuliteManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString panelUrl READ panelUrl NOTIFY configChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY configChanged)
    Q_PROPERTY(bool enabled READ enabled NOTIFY configChanged)
    Q_PROPERTY(bool secretSaved READ secretSaved NOTIFY configChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString devicesText READ devicesText NOTIFY devicesChanged)
    Q_PROPERTY(int abrMaxMbps READ abrMaxMbps WRITE setAbrMaxMbps NOTIFY configChanged)

public:
    static UuliteManager* instance();

    explicit UuliteManager(QObject* parent = nullptr);
    ~UuliteManager() override;

    QString statusText() const { return m_statusText; }
    QString panelUrl() const { return m_panelUrl; }
    QString deviceName() const { return m_deviceName; }
    bool enabled() const { return m_enabled; }
    bool secretSaved() const { return !m_secret.isEmpty(); }
    bool busy() const { return m_busy; }
    QString devicesText() const { return m_devicesText; }
    int abrMaxMbps() const { return m_abrMaxMbps; }
    void setAbrMaxMbps(int mbps);

    Q_INVOKABLE void saveConfig(const QString& panelUrl, const QString& secret,
                                const QString& deviceName, bool enabled);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void attachComputerManager(QObject* computerManager);

    // 由 Session 在串流开始时调用；hostIp 为 Moonlight 实际连接的主机地址。
    void startAbrForHost(const QString& hostIp, int maxBitrateKbps);
    int capBitrateForHost(const QString& hostIp, int configuredKbps) const;
    int initialBitrateForHost(const QString& hostIp, int configuredKbps) const;
    void stopAbr();

signals:
    void statusChanged();
    void configChanged();
    void busyChanged();
    void devicesChanged();

private slots:
    void onHeartbeatTimer();
    void onRefreshTimer();
    void onRegistered(bool ok, const QString& error);
    void onHeartbeatFinished(bool ok, const QJsonArray& peers, const QJsonArray& commands, const QString& error);
    void onDevicesFinished(bool ok, const QJsonArray& devices, const QString& error);
    void onBitrateChangeRequested(int bitrate, const QString& reason, int from);

private:
    void loadConfig();
    void persistConfig();
    void startRuntime();
    void stopRuntime();
    void setStatus(const QString& text);
    void setBusy(bool busy);
    void setDevicesText(const QString& text);
    void maybeAutoPair();
    void onPairPin(const QJsonObject& args);
    NvComputer* findComputerByHost(const QString& host) const;
    void updateDevices(const QJsonArray& devices);
    QStringList localCandidates() const;
    QString findDeviceIdByHost(const QString& hostIp) const;
    QString findHostAddress(const QJsonArray& devices, const QString& deviceId) const;

    UuliteClient* m_client;
    UuliteAbr* m_abr;

    QString m_panelUrl;
    QString m_secret;
    QString m_deviceName;
    QString m_deviceId;
    bool m_enabled;
    bool m_busy;
    int m_abrMaxMbps;

    QTimer m_heartbeatTimer;
    QTimer m_refreshTimer;
    QStringList m_ackCommandIds;
    QJsonArray m_devices;
    QHash<QString, QString> m_hostToDeviceId; // TS/LAN IP -> device_id
    QHash<QString, QString> m_deviceNames;
    QString m_abrHostIp;
    QString m_abrHostDeviceId;
    ComputerManager* m_computerManager = nullptr;
    QString m_pairHostIp;
    QString m_pairHostDeviceId;
    qint64 m_pairDeadlineMs = 0;
    QHash<QString, qint64> m_pairRetryAfter;

    QString m_pendingAbrHost;
    int m_pendingAbrBitrate = 0;
    void tryStartPendingAbr();

    QString m_statusText;
    QString m_devicesText;
    int m_refreshFailures;
};
