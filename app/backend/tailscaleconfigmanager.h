#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

/**
 * uu-lite: 内置 Tailscale 管理
 *
 * 在 Moonlight 设置页里直接查看 Tailscale 状态、配置自定义控制服务器（Headscale）、
 * 用 Auth Key 登录/重登、退出登录、安装 Tailscale。
 * 登录成功后，tailnet 内运行 Sunshine 的主机会由 TailscaleDiscovery 自动加入主机列表，
 * 迁移到新电脑时只需：解压 Moonlight → 安装 Tailscale → 粘贴 Auth Key 登录。
 */
class TailscaleManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString backendState READ backendState NOTIFY statusChanged)
    Q_PROPERTY(QString tailscaleIp READ tailscaleIp NOTIFY statusChanged)
    Q_PROPERTY(bool installed READ installed NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString loginServer READ loginServer WRITE setLoginServer NOTIFY loginServerChanged)

public:
    explicit TailscaleManager(QObject* parent = nullptr);

    QString statusText() const { return m_statusText; }
    QString backendState() const { return m_backendState; }
    QString tailscaleIp() const { return m_tailscaleIp; }
    bool installed() const { return m_installed; }
    bool busy() const { return m_busy; }
    QString loginServer() const { return m_loginServer; }
    void setLoginServer(const QString& server);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void login(const QString& authKey);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void installTailscale();
    Q_INVOKABLE void importCaCertificate(const QString& fileUrl);
    Q_INVOKABLE void openDownloadPage();

signals:
    void statusChanged();
    void busyChanged();
    void loginServerChanged();

private:
    QString findExe() const;
    QString findInstaller() const;
    void setBusy(bool busy);
    void runStatus();
    void runElevated(const QString& program, const QStringList& args);
    void setStatus(const QString& text, const QString& backend = QString(), const QString& ip = QString());

    QString m_statusText;
    QString m_backendState;
    QString m_tailscaleIp;
    QString m_loginServer;
    bool m_installed;
    bool m_busy;
};
