/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shareplaytls.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>

#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
template<typename T, auto Free>
using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

bool generate(QByteArray &pem)
{
    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *rawKey = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 3072) <= 0
        || EVP_PKEY_keygen(context.get(), &rawKey) <= 0) {
        return false;
    }
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(rawKey, EVP_PKEY_free);
    OpenSslPtr<X509, X509_free> cert(X509_new(), X509_free);
    OpenSslPtr<BIO, BIO_free> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!cert || !bio || X509_set_version(cert.get(), 2) != 1 || ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1
        || !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) || !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 60 * 60)
        || X509_set_pubkey(cert.get(), key.get()) != 1) {
        return false;
    }
    X509_NAME *name = X509_get_subject_name(cert.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Haruna SharePlay"), -1, -1, 0) != 1
        || X509_set_issuer_name(cert.get(), name) != 1 || X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0
        || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 || PEM_write_bio_X509(bio.get(), cert.get()) != 1) {
        return false;
    }
    char *data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length <= 0) {
        return false;
    }
    pem = QByteArray(data, length);
    return true;
}

bool securePath(const QString &path, bool directory)
{
#ifdef Q_OS_UNIX
    struct stat st;
    const QByteArray encoded = QFile::encodeName(path);
    return lstat(encoded.constData(), &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0077) == 0 && (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))
        && (directory || st.st_nlink == 1);
#else
    Q_UNUSED(path);
    Q_UNUSED(directory);
    // Qt file permissions do not establish or verify a private Windows ACL.
    return false;
#endif
}

bool writeAtomic(const QString &path, const QByteArray &data, QString &error)
{
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot create TLS file %1: %2").arg(path, output.errorString());
        return false;
    }
    if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        error = QStringLiteral("Cannot secure TLS file %1: %2").arg(path, output.errorString());
        return false;
    }
    if (output.write(data) != data.size() || !output.flush()) {
        error = QStringLiteral("Cannot write TLS file %1: %2").arg(path, output.errorString());
        return false;
    }
#ifdef Q_OS_UNIX
    if (::fsync(output.handle()) != 0) {
        error = QStringLiteral("Cannot synchronize TLS file: %1").arg(path);
        return false;
    }
#endif
    if (!output.commit()) {
        error = QStringLiteral("Cannot commit TLS file %1: %2").arg(path, output.errorString());
        return false;
    }
    return true;
}
}

bool SharePlayTls::loadOrCreate(QSslConfiguration &configuration, QString &error)
{
    if (!QSslSocket::supportsSsl()) {
        error = QStringLiteral("TLS backend is unavailable");
        return false;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty() || !QDir().mkpath(base)) {
        error = QStringLiteral("Cannot create application data directory");
        return false;
    }
    const QString dir = base + QStringLiteral("/shareplay-tls");
    if (!QFileInfo::exists(dir)) {
#ifdef Q_OS_UNIX
        const bool created = ::mkdir(QFile::encodeName(dir).constData(), 0700) == 0;
#else
        const bool created = QDir().mkdir(dir);
#endif
        if (!created && !QFileInfo::exists(dir)) {
            error = QStringLiteral("Cannot create TLS directory: %1").arg(dir);
            return false;
        }
    }
    if (!securePath(dir, true)) {
        error = QStringLiteral("TLS directory is not private or is unsafe: %1").arg(dir);
        return false;
    }
    QLockFile lock(dir + QStringLiteral("/identity.lock"));
    if (!lock.tryLock(10000)) {
        error = QStringLiteral("Cannot lock SharePlay TLS identity");
        return false;
    }
    const QString path = dir + QStringLiteral("/identity.pem");
    const QString fingerprintPath = dir + QStringLiteral("/identity.sha256");
    const QFileInfo fingerprintInfo(fingerprintPath);
    const bool hasFingerprint = fingerprintInfo.exists() || fingerprintInfo.isSymLink();
    if (hasFingerprint && !securePath(fingerprintPath, false)) {
        error = QStringLiteral("TLS fingerprint is not private or is unsafe: %1").arg(fingerprintPath);
        return false;
    }
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
        if (hasFingerprint) {
            error = QStringLiteral(
                        "TLS identity is missing but its fingerprint exists; restore identity.pem or explicitly remove identity.sha256 to create a new "
                        "identity: %1")
                        .arg(dir);
            return false;
        }
        QByteArray pem;
        if (!generate(pem)) {
            error = QStringLiteral("Cannot generate TLS identity");
            return false;
        }
        if (!writeAtomic(path, pem, error)) {
            return false;
        }
    }
    if (!securePath(path, false)) {
        error = QStringLiteral("TLS identity is not private or is unsafe: %1").arg(path);
        return false;
    }
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read TLS identity: %1").arg(input.errorString());
        return false;
    }
    const QByteArray pem = input.readAll();
    OpenSslPtr<BIO, BIO_free> keyBio(BIO_new_mem_buf(pem.constData(), pem.size()), BIO_free);
    OpenSslPtr<BIO, BIO_free> certBio(BIO_new_mem_buf(pem.constData(), pem.size()), BIO_free);
    const auto noPassword = [](char *, int, int, void *) {
        return 0;
    };
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(keyBio ? PEM_read_bio_PrivateKey(keyBio.get(), nullptr, noPassword, nullptr) : nullptr, EVP_PKEY_free);
    OpenSslPtr<X509, X509_free> cert(certBio ? PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr) : nullptr, X509_free);
    const QSslKey qtKey(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    const auto certificates = QSslCertificate::fromData(pem, QSsl::Pem);
    if (!key || !cert || X509_check_private_key(cert.get(), key.get()) != 1 || X509_verify(cert.get(), key.get()) != 1 || qtKey.isNull()
        || certificates.size() != 1 || certificates.first().effectiveDate() > QDateTime::currentDateTimeUtc()
        || certificates.first().expiryDate() <= QDateTime::currentDateTimeUtc()) {
        error = QStringLiteral("Stored TLS identity is invalid or expired: %1").arg(path);
        return false;
    }
    const QByteArray fingerprint = certificates.first().digest(QCryptographicHash::Sha256).toHex() + '\n';
    if (hasFingerprint) {
        QFile fingerprintFile(fingerprintPath);
        if (!fingerprintFile.open(QIODevice::ReadOnly) || fingerprintFile.readAll() != fingerprint) {
            error = QStringLiteral("TLS identity fingerprint is unreadable or changed; restore the original identity and fingerprint: %1").arg(dir);
            return false;
        }
    } else if (!writeAtomic(fingerprintPath, fingerprint, error)) {
        return false;
    }
    configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(certificates.first());
    configuration.setPrivateKey(qtKey);
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    return true;
}
