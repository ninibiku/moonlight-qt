#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * uu-lite 面板 API 客户端（原生 C++，异步）。
 *
 * 只依赖 QtNetwork：
 *  - 设备注册 / 心跳 / 设备列表 / 命令下发
 *  - HMAC 设备 token：lowercase_hex(HMAC-SHA256(secret, "dev:" + device_id))
 *  - 所有请求带 Authorization: Bearer <token> 与 X-Device-Id
 *
 * 注意：secret 只在本对象内保存，任何日志/错误信息都不得包含 secret 或 token。
 */
class UuliteClient : public QObject
{
    Q_OBJECT

public:
    explicit UuliteClient(QObject* parent = nullptr);

    // serverUrl 形如 http://panel.example:8787（允许尾部斜杠）
    void setConfig(const QString& serverUrl, const QString& secret,
                   const QString& deviceId, const QString& deviceName);
    bool hasConfig() const;
    QString deviceId() const { return m_deviceId; }
    QString deviceName() const { return m_deviceName; }
    QString serverUrl() const { return m_serverUrl; }

    void registerDevice(const QStringList& candidateSpecs);
    void heartbeat(const QJsonObject& stats, const QStringList& commandsAck,
                   const QStringList& candidateSpecs);
    void fetchDevices();
    void sendCommand(const QString& targetDeviceId, const QString& action, const QJsonObject& args);

signals:
    void registered(bool ok, const QString& error);
    void heartbeatFinished(bool ok, const QJsonArray& peers, const QJsonArray& commands, const QString& error);
    void devicesFinished(bool ok, const QJsonArray& devices, const QString& error);
    void commandQueued(bool ok, const QString& commandId, const QString& error);

private:
    QNetworkReply* postJson(const QString& path, const QJsonObject& body);
    QString authToken() const;
    QString baseUrl() const;
    void reportError(const QString& what, QNetworkReply* reply);

    QString m_serverUrl;
    QString m_secret;
    QString m_deviceId;
    QString m_deviceName;
    QNetworkAccessManager* m_nam;
};
