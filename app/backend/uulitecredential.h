#pragma once

#include <QByteArray>
#include <QString>

/**
 * uu-lite: 面板设备 secret 的 DPAPI 凭据存储
 *
 * Moonlight 魔改版要把 uu-lite 面板设备的 secret 存进 QSettings / 配置文件时，
 * 不能明文落盘——配置文件可能被备份、同步、误上传，或被同机的其他程序直接读走。
 * 这个工具类统一用 Windows DPAPI（CryptProtectData / CryptUnprotectData）做
 * 用户级加密：密钥由 Windows 按当前登录用户管理，程序代码和配置文件里都
 * 不出现任何密钥。
 *
 * 典型用法（读写 QSettings）：
 *   settings.setValue("uulite_secret", UuliteCredentialStore::protectToHex(secret));
 *   secret = UuliteCredentialStore::unprotectFromHex(settings.value("uulite_secret").toString());
 *
 * 安全边界（务必了解）：
 * - DPAPI 按当前 Windows 用户加密：换一个 Windows 用户登录、重装系统、
 *   或把配置文件拷到别的电脑都无法解密，unprotect* 会返回空。
 *   上层要按“凭据已失效”处理，引导用户重新绑定设备/重新填写 secret。
 * - 它保证“配置文件里没有明文”，但防不了同一 Windows 用户下已被入侵的进程：
 *   同用户下运行的任何代码同样能调用 CryptUnprotectData 把密文解出来。
 * - 实现没有使用 CRYPTPROTECT_LOCAL_MACHINE（那会让本机所有用户都能解密）。
 *
 * 平台支持：仅 Windows（Q_OS_WIN）提供真实实现；非 Windows 平台所有方法
 * 一律返回空（明确“不支持”），绝不退化为 base64 之类的假加密。
 *
 * 链接依赖：DPAPI 实现在 Crypt32（crypt32.dll）。本类按约定不改 app.pro，
 * 链接由上层负责：qmake 写法 win32: LIBS += -lcrypt32（MSVC 下等价于在
 * app.pro 现有的 win32 LIBS 行追加 crypt32.lib）。
 *
 * 实现约定：全部为纯静态方法，无状态、不抛异常，可在任意线程调用。
 * 空输入一律按无效数据直接返回空——“空返回值 == 失败或不支持”。
 */
class UuliteCredentialStore
{
public:
    // DPAPI 加密。失败（含空输入）返回空 QByteArray。
    static QByteArray protect(const QByteArray& plaintext);

    // DPAPI 解密。失败（含空输入）返回空 QByteArray。
    static QByteArray unprotect(const QByteArray& ciphertext);

    // 加密（UTF-8 编码）后输出十六进制字符串，可直接存进 QSettings。失败返回空 QString。
    static QString protectToHex(const QString& plaintext);

    // 输入十六进制字符串，解密还原原字符串。失败（含空输入）返回空 QString。
    static QString unprotectFromHex(const QString& hex);

private:
    // 纯静态工具类，禁止实例化
    UuliteCredentialStore() = delete;
};
