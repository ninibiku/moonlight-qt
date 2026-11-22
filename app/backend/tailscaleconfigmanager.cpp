#include "tailscaleconfigmanager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUrl>

#include <SDL_log.h>

TailscaleManager::TailscaleManager(QObject* parent)
    : QObject(parent),
      m_statusText(QStringLiteral("Not detected")),
      m_installed(false),
      m_busy(false)
{
    QSettings settings;
    m_loginServer = settings.value(QStringLiteral("tailscale_loginserver")).toString();
    refresh();
}

void TailscaleManager::setLoginServer(const QString& server)
{
    const QString trimmed = server.trimmed();
    if (m_loginServer == trimmed) return;
    m_loginServer = trimmed;
    QSettings settings;
    settings.setValue(QStringLiteral("tailscale_loginserver"), m_loginServer);
    emit loginServerChanged();
}

QString TailscaleManager::findExe() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("tailscale/tailscale.exe")),
        QStringLiteral("C:/Program Files/Tailscale/tailscale.exe"),
        QStringLiteral("C:/Program Files (x86)/Tailscale/tailscale.exe"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return QStandardPaths::findExecutable(QStringLiteral("tailscale"));
}

QString TailscaleManager::findInstaller() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("tailscale/tailscale-setup.exe")),
        QDir(appDir).filePath(QStringLiteral("tailscale/tailscale-setup.msi")),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

void TailscaleManager::setStatus(const QString& text, const QString& backend, const QString& ip)
{
    m_statusText = text;
    m_backendState = backend;
    m_tailscaleIp = ip;
    emit statusChanged();
}

void TailscaleManager::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void TailscaleManager::refresh()
{
    runStatus();
}

void TailscaleManager::runStatus()
{
    const QString exe = findExe();
    m_installed = !exe.isEmpty();
    if (!m_installed) {
        setStatus(QStringLiteral("Not installed - click 'Install Tailscale' or download it first"));
        return;
    }

    QProcess process;
    process.start(exe, {QStringLiteral("status"), QStringLiteral("--json")});
    if (!process.waitForFinished(6000)) {
        process.kill();
        setStatus(QStringLiteral("Tailscale CLI timeout"));
        return;
    }
    if (process.exitCode() != 0) {
        setStatus(QStringLiteral("Tailscale CLI error: %1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
        return;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(process.readAllStandardOutput(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus(QStringLiteral("Tailscale status parse error"));
        return;
    }

    const QJsonObject root = doc.object();
    const QString backend = root.value(QStringLiteral("BackendState")).toString();
    QString ip;
    const QJsonObject self = root.value(QStringLiteral("Self")).toObject();
    for (const QJsonValue& value : self.value(QStringLiteral("TailscaleIPs")).toArray()) {
        const QString candidate = value.toString();
        if (candidate.contains(QLatin1Char('.'))) {
            ip = candidate;
            break;
        }
    }

    QString text;
    if (backend == QLatin1String("Running")) {
        text = QStringLiteral("Connected%1").arg(ip.isEmpty() ? QString() : QStringLiteral(" · ") + ip);
    } else if (backend == QLatin1String("NeedsLogin")) {
        text = QStringLiteral("Needs login - enter an auth key below and click Login");
    } else if (backend == QLatin1String("Stopped")) {
        text = QStringLiteral("Stopped - click Login to bring it up");
    } else {
        text = QStringLiteral("State: %1").arg(backend.isEmpty() ? QStringLiteral("unknown") : backend);
    }
    setStatus(text, backend, ip);
}

void TailscaleManager::runElevated(const QString& program, const QStringList& args)
{
    QStringList quoted;
    for (const QString& arg : args) {
        QString escaped = arg;
        escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
        quoted << QStringLiteral("'%1'").arg(escaped);
    }
    QString escapedProgram = program;
    escapedProgram.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString command = QStringLiteral("Start-Process -FilePath '%1' -ArgumentList %2 -Verb RunAs -Wait")
                                .arg(escapedProgram, quoted.join(QLatin1Char(',')));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tailscale: running elevated command (UAC prompt)");
    QProcess ps;
    ps.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), command});
    ps.waitForFinished(60000);
}

void TailscaleManager::login(const QString& authKey)
{
    if (m_busy) return;
    const QString key = authKey.trimmed();
    if (key.isEmpty()) {
        setStatus(QStringLiteral("Auth key is empty - generate one in Tailscale admin (tskey-...)"));
        return;
    }
    const QString exe = findExe();
    if (exe.isEmpty()) {
        setStatus(QStringLiteral("Tailscale is not installed"));
        return;
    }

    QStringList args = {QStringLiteral("up"), QStringLiteral("--auth-key=%1").arg(key)};
    if (!m_loginServer.isEmpty()) {
        args << QStringLiteral("--login-server=%1").arg(m_loginServer);
    }

    setBusy(true);
    setStatus(QStringLiteral("Logging in... (approve the UAC prompt)"));
    runElevated(exe, args);
    setBusy(false);
    runStatus();
}

void TailscaleManager::logout()
{
    if (m_busy) return;
    const QString exe = findExe();
    if (exe.isEmpty()) return;
    setBusy(true);
    runElevated(exe, {QStringLiteral("logout")});
    setBusy(false);
    runStatus();
}

void TailscaleManager::installTailscale()
{
    if (m_busy) return;
    const QString installer = findInstaller();
    if (installer.isEmpty()) {
        openDownloadPage();
        return;
    }
    setBusy(true);
    setStatus(QStringLiteral("Installing Tailscale... (approve the UAC prompt)"));
    QStringList args;
    if (installer.endsWith(QLatin1String(".msi"), Qt::CaseInsensitive)) {
        args = {QStringLiteral("/i"), installer, QStringLiteral("/quiet"), QStringLiteral("/norestart")};
        runElevated(QStringLiteral("msiexec.exe"), args);
    } else {
        // Tailscale 的 NSIS 安装包支持 /S 静默安装
        args = {QStringLiteral("/S")};
        // Start-Process 直接以 installer 为 program，参数 /S
        runElevated(installer, args);
    }
    setBusy(false);
    runStatus();
}

void TailscaleManager::openDownloadPage()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://tailscale.com/download/windows")));
}

void TailscaleManager::importCaCertificate(const QString& fileUrl)
{
    if (m_busy) return;

    QString path = fileUrl;
    if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        path = QUrl(path).toLocalFile();
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        setStatus(QStringLiteral("CA 证书文件不存在"));
        return;
    }

    QTemporaryFile script(QDir::tempPath() + QStringLiteral("/uu-lite-import-ca-XXXXXX.ps1"));
    if (!script.open()) {
        setStatus(QStringLiteral("无法创建临时脚本"));
        return;
    }
    QString escaped = path;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString body = QStringLiteral(
        "$ErrorActionPreference='Stop'\r\n"
        "Import-Certificate -FilePath '%1' -CertStoreLocation Cert:\\LocalMachine\\Root | Out-Null\r\n"
        "Restart-Service Tailscale -ErrorAction SilentlyContinue\r\n")
        .arg(escaped);
    script.write(body.toUtf8());
    script.close();

    setBusy(true);
    setStatus(QStringLiteral("正在导入 CA 证书（请确认 UAC）..."));
    runElevated(QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                 QStringLiteral("-File"), script.fileName()});
    script.remove();
    setBusy(false);
    runStatus();
    setStatus(QStringLiteral("CA 证书已导入（LocalMachine\\Root），Tailscale 已重启"));
}
