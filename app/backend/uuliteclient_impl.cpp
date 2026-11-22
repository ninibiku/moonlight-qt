#include "uuliteclient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMessageAuthenticationCode>
#include <QUrl>
#include <QDebug>

namespace {
QString truncated(const QString& text, int maxLen = 200)
{
    return text.size() <= maxLen ? text : text.left(maxLen) + QStringLiteral("...");
}

QString jsonErrorFromReply(QNetworkReply* reply)
{
    const QByteArray body = reply->readAll();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        QString error = obj.value(QStringLiteral("error")).toString();
        const QString message = obj.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) {
            error += QStringLiteral(": ") + message;
        }
        if (!error.isEmpty()) {
            return truncated(error);
        }
    }
    return truncated(QString::fromUtf8(body).trimmed());
}
} // namespace

UuliteClient::UuliteClient(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
}

void UuliteClient::setConfig(const QString& serverUrl, const QString& secret,
                             const QString& deviceId, const QString& deviceName)
{
    m_serverUrl = serverUrl.trimmed();
    while (m_serverUrl.endsWith(QLatin1Char('/'))) {
        m_serverUrl.chop(1);
    }
    m_secret = secret;
    m_deviceId = deviceId;
    m_deviceName = deviceName;
}

bool UuliteClient::hasConfig() const
{
    return !m_serverUrl.isEmpty() && !m_secret.isEmpty() && !m_deviceId.isEmpty();
}

QString UuliteClient::baseUrl() const
{
    return m_serverUrl;
}

QString UuliteClient::authToken() const
{
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, m_secret.toUtf8());
    mac.addData(QStringLiteral("dev:").toUtf8());
    mac.addData(m_deviceId.toUtf8());
    return QString::fromLatin1(mac.result().toHex());
}

QNetworkReply* UuliteClient::postJson(const QString& path, const QJsonObject& body)
{
    QNetworkRequest request(QUrl(baseUrl() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + authToken().toUtf8());
    request.setRawHeader("X-Device-Id", m_deviceId.toUtf8());
    request.setRawHeader("User-Agent", QByteArrayLiteral("uu-lite-moonlight/0.1.0"));
    request.setTransferTimeout(5000);
    return m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void UuliteClient::registerDevice(const QStringList& candidateSpecs)
{
    QJsonArray candidates;
    for (const QString& spec : candidateSpecs) {
        const QStringList parts = spec.split(QLatin1Char(':'));
        if (parts.size() != 3) continue;
        QJsonObject candidate;
        candidate.insert(QStringLiteral("kind"), parts.at(0));
        candidate.insert(QStringLiteral("host"), parts.at(1));
        candidate.insert(QStringLiteral("port"), parts.at(2).toInt());
        candidates.append(candidate);
    }

    QJsonObject body;
    body.insert(QStringLiteral("device_id"), m_deviceId);
    body.insert(QStringLiteral("name"), m_deviceName);
    body.insert(QStringLiteral("os"), QStringLiteral("windows"));
    body.insert(QStringLiteral("agent_version"), QStringLiteral("native-moonlight"));
    body.insert(QStringLiteral("port"), 47989);
    body.insert(QStringLiteral("candidates"), candidates);
    body.insert(QStringLiteral("caps"), QJsonArray{QStringLiteral("control")});

    QNetworkReply* reply = postJson(QStringLiteral("/api/v1/device/register"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = jsonErrorFromReply(reply);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit registered(false, QStringLiteral("HTTP %1 %2").arg(status).arg(detail));
            return;
        }
        const QByteArray payload = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            emit registered(false, QStringLiteral("invalid json from register"));
            return;
        }
        emit registered(true, QString());
    });
}

void UuliteClient::heartbeat(const QJsonObject& stats, const QStringList& commandsAck,
                             const QStringList& candidateSpecs)
{
    QJsonArray ack;
    for (const QString& id : commandsAck) {
        ack.append(id);
    }

    QJsonArray candidates;
    for (const QString& spec : candidateSpecs) {
        const QStringList parts = spec.split(QLatin1Char(':'));
        if (parts.size() != 3) continue;
        QJsonObject candidate;
        candidate.insert(QStringLiteral("kind"), parts.at(0));
        candidate.insert(QStringLiteral("host"), parts.at(1));
        candidate.insert(QStringLiteral("port"), parts.at(2).toInt());
        candidates.append(candidate);
    }

    QJsonObject body;
    body.insert(QStringLiteral("device_id"), m_deviceId);
    body.insert(QStringLiteral("commands_ack"), ack);
    body.insert(QStringLiteral("stats"), stats);
    body.insert(QStringLiteral("candidates"), candidates);

    QNetworkReply* reply = postJson(QStringLiteral("/api/v1/device/heartbeat"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = jsonErrorFromReply(reply);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit heartbeatFinished(false, QJsonArray(), QJsonArray(),
                                  QStringLiteral("HTTP %1 %2").arg(status).arg(detail));
            return;
        }
        const QByteArray payload = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit heartbeatFinished(false, QJsonArray(), QJsonArray(),
                                  QStringLiteral("invalid json from heartbeat"));
            return;
        }
        const QJsonObject obj = doc.object();
        emit heartbeatFinished(true, obj.value(QStringLiteral("peers")).toArray(),
                              obj.value(QStringLiteral("commands")).toArray(), QString());
    });
}

void UuliteClient::fetchDevices()
{
    QNetworkRequest request(QUrl(baseUrl() + QStringLiteral("/api/v1/devices")));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + authToken().toUtf8());
    request.setRawHeader("X-Device-Id", m_deviceId.toUtf8());
    request.setRawHeader("User-Agent", QByteArrayLiteral("uu-lite-moonlight/0.1.0"));
    request.setTransferTimeout(5000);

    QNetworkReply* reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = jsonErrorFromReply(reply);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit devicesFinished(false, QJsonArray(),
                                 QStringLiteral("HTTP %1 %2").arg(status).arg(detail));
            return;
        }
        const QByteArray payload = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit devicesFinished(false, QJsonArray(), QStringLiteral("invalid json from devices"));
            return;
        }
        emit devicesFinished(true, doc.object().value(QStringLiteral("devices")).toArray(), QString());
    });
}

void UuliteClient::sendCommand(const QString& targetDeviceId, const QString& action,
                               const QJsonObject& args)
{
    QJsonObject body;
    body.insert(QStringLiteral("device_id"), targetDeviceId);
    body.insert(QStringLiteral("action"), action);
    body.insert(QStringLiteral("args"), args);

    QNetworkReply* reply = postJson(QStringLiteral("/api/v1/command"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = jsonErrorFromReply(reply);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit commandQueued(false, QString(),
                               QStringLiteral("HTTP %1 %2").arg(status).arg(detail));
            return;
        }
        const QByteArray payload = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit commandQueued(false, QString(), QStringLiteral("invalid json from command"));
            return;
        }
        emit commandQueued(true, doc.object().value(QStringLiteral("command_id")).toString(), QString());
    });
}

void UuliteClient::reportError(const QString& what, QNetworkReply* reply)
{
    qWarning() << "uu-lite:" << what << reply->errorString();
}
