#include "uulitecredential.h"

#ifdef Q_OS_WIN

#include <windows.h>
#include <wincrypt.h> // CryptProtectData / CryptUnprotectData、CRYPTPROTECT_UI_FORBIDDEN 与 DATA_BLOB（dpapi.h 由 wincrypt.h 一并带入）

// 链接提示（本文件按约定不改动 app.pro）：DPAPI 实现在 Crypt32。
// 上层需在构建脚本里补：win32: LIBS += -lcrypt32
// （MSVC 下等价于在 app.pro 现有的 win32 LIBS 行追加 crypt32.lib）。

QByteArray UuliteCredentialStore::protect(const QByteArray& plaintext)
{
    // 约定：空输入直接按失败处理。否则 protect("") 成功后 unprotect 只能
    // 还原出空明文，与“解密失败”无法区分，上层就没法判断凭据是否有效。
    if (plaintext.isEmpty()) {
        return {};
    }

    // DPAPI 的输入/输出都通过 DATA_BLOB 传递。输入 blob 直接指向调用者的
    // 缓冲区（CryptProtectData 只读不写）。设备 secret 远小于 4GB，
    // 长度收窄到 DWORD 是安全的。
    DATA_BLOB inBlob;
    inBlob.cbData = static_cast<DWORD>(plaintext.size());
    inBlob.pbData = reinterpret_cast<LPBYTE>(const_cast<char*>(plaintext.constData()));

    DATA_BLOB outBlob = {};
    if (!CryptProtectData(&inBlob,
                          nullptr,                   // szDataDescr：不往密文里嵌描述串，避免多一段明文标记
                          nullptr,                   // pOptionalEntropy：无附加熵，凭据只按当前 Windows 用户绑定
                          nullptr,                   // pvReserved：必须为 nullptr
                          nullptr,                   // pPromptStruct：不弹提示
                          CRYPTPROTECT_UI_FORBIDDEN, // 后台/服务场景禁止弹 UI
                          &outBlob)) {
        return {};
    }

    // outBlob 由 DPAPI 用 LocalAlloc 分配，必须用 LocalFree 释放。
    // 先拷贝进 QByteArray 再释放；拷贝不抛异常，没有泄漏路径。
    const QByteArray result(reinterpret_cast<const char*>(outBlob.pbData),
                            static_cast<int>(outBlob.cbData));
    LocalFree(reinterpret_cast<HLOCAL>(outBlob.pbData));
    return result;
}

QByteArray UuliteCredentialStore::unprotect(const QByteArray& ciphertext)
{
    if (ciphertext.isEmpty()) {
        return {};
    }

    DATA_BLOB inBlob;
    inBlob.cbData = static_cast<DWORD>(ciphertext.size());
    inBlob.pbData = reinterpret_cast<LPBYTE>(const_cast<char*>(ciphertext.constData()));

    DATA_BLOB outBlob = {};
    // 第二个参数 ppszDataDescr 传 nullptr，DPAPI 就不会返回描述串，无需额外释放。
    if (!CryptUnprotectData(&inBlob,
                            nullptr,
                            nullptr,                   // pOptionalEntropy：与 protect() 对称，无附加熵
                            nullptr,
                            nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN,
                            &outBlob)) {
        // 常见失败原因：换用户/换机器后留下的历史密文、被篡改或截断的数据。
        // 按约定返回空，由上层按“凭据已失效”处理。
        return {};
    }

    const QByteArray result(reinterpret_cast<const char*>(outBlob.pbData),
                            static_cast<int>(outBlob.cbData));
    LocalFree(reinterpret_cast<HLOCAL>(outBlob.pbData));
    return result;
}

QString UuliteCredentialStore::protectToHex(const QString& plaintext)
{
    if (plaintext.isEmpty()) {
        return {};
    }

    // 先转 UTF-8 再加密，unprotectFromHex 用 fromUtf8 对称还原，
    // 非 ASCII 的 secret 也能无损往返。
    const QByteArray cipher = protect(plaintext.toUtf8());
    if (cipher.isEmpty()) {
        return {};
    }

    // 密文转十六进制（纯 ASCII 小写），可直接写进 QSettings；fromLatin1 零损耗。
    return QString::fromLatin1(cipher.toHex());
}

QString UuliteCredentialStore::unprotectFromHex(const QString& hex)
{
    // QByteArray::fromHex 会跳过/丢弃非法字符，所以乱写的 hex 要么解成空、
    // 要么解成打不开的乱码——两种情况 CryptUnprotectData 都会失败并返回空。
    const QByteArray cipherBytes = QByteArray::fromHex(hex.toLatin1());
    if (cipherBytes.isEmpty()) {
        return {};
    }

    const QByteArray plainBytes = unprotect(cipherBytes);
    if (plainBytes.isEmpty()) {
        return {};
    }

    return QString::fromUtf8(plainBytes);
}

#else // !Q_OS_WIN

/*
 * 非 Windows 平台：不支持 DPAPI，这里刻意返回空（=失败），安全降级。
 * 不要退化为明文 / base64“假加密”——那只会制造“已经加密”的错觉，
 * 密钥根本不存在。上层拿到空返回值要按“凭据不可用”处理，
 * 引导用户重新输入/绑定；如将来要在 Linux/macOS 支持，请在此接入
 * 等效的系统凭据机制（如 libsecret / macOS Keychain），
 * 并保持“失败返回空”的约定。
 */

QByteArray UuliteCredentialStore::protect(const QByteArray& plaintext)
{
    Q_UNUSED(plaintext);
    return {};
}

QByteArray UuliteCredentialStore::unprotect(const QByteArray& ciphertext)
{
    Q_UNUSED(ciphertext);
    return {};
}

QString UuliteCredentialStore::protectToHex(const QString& plaintext)
{
    Q_UNUSED(plaintext);
    return {};
}

QString UuliteCredentialStore::unprotectFromHex(const QString& hex)
{
    Q_UNUSED(hex);
    return {};
}

#endif // Q_OS_WIN
