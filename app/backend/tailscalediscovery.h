#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QHash>
#include <QString>

class ComputerManager;

/**
 * uu-lite: Tailscale 原生发现
 *
 * Moonlight 本身只会通过 mDNS 发现局域网主机；Tailscale 组网下的主机（100.64.0.0/10）
 * 不会被自动发现。本类定期执行 `tailscale status --json`，对在线节点的 Tailscale IP
 * 探测 Sunshine 端口（47989），命中后交给 ComputerManager 走正常的 serverinfo/配对流程，
 * 从而在 Moonlight 主机列表里自动出现“Tailscale 主机”，可直接连接/串流。
 */
class TailscaleDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit TailscaleDiscovery(ComputerManager* computerManager);

    void start();

private:
    void scan();
    void handleStatusFinished(int exitCode, QProcess::ExitStatus status);
    void probeAndAdd(const QString& ip);
    QString findTailscaleExe() const;

    ComputerManager* m_ComputerManager;
    QTimer m_ScanTimer;
    QProcess* m_Process;
    QHash<QString, qint64> m_AttemptedMs;
    bool m_Scanning;
};
