#include "MacHelper.h"
#include "ocg-helper-protocol.h"

#include "OcSettings.h"
#include "logger.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QVector>
#include <QThread>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

const char* const declinedKey = "Settings/macHelperDeclined";
const char* const importedKey = "Settings/macRootSettingsImported";

// The preferences domain OcSettings maps to on macOS.
const CFStringRef settingsDomain = CFSTR("com.openconnect-gui-team.OpenConnect-GUI");

QString helperInBundle()
{
    return QCoreApplication::applicationDirPath() + QLatin1String("/ocg-helper");
}

// Runs the bundled helper with administrator rights; returns its output,
// or an empty array when the user cancelled or it could not be started.
QByteArray runInstaller(bool exportSettings, QString* error)
{
    AuthorizationRef auth;
    if (AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, &auth) != errAuthorizationSuccess) {
        *error = QObject::tr("Failed to create authorization reference.");
        return {};
    }

    const QByteArray path = helperInBundle().toUtf8();
    char install[] = "--install";
    char exportArg[] = "--export-settings";
    char* args[] = { install, exportSettings ? exportArg : nullptr, nullptr };
    FILE* pipe = nullptr;

    // AuthorizationExecuteWithPrivileges() is deprecated, but it is what the
    // application already uses and needs no Developer ID signature.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = AuthorizationExecuteWithPrivileges(auth, path.constData(), kAuthorizationFlagDefaults, args, &pipe);
#pragma clang diagnostic pop
    AuthorizationFree(auth, kAuthorizationFlagDefaults);

    if (status == errAuthorizationCanceled) {
        return {};
    }
    if (status != errAuthorizationSuccess || !pipe) {
        *error = QObject::tr("Could not start the helper installer (error %1).").arg(status);
        return {};
    }

    QByteArray output;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
        output.append(buf, static_cast<qsizetype>(n));
    }
    fclose(pipe);
    return output;
}

// Copies the settings saved while running as root into the user's domain,
// leaving alone whatever the user already has there.
int importSettings(const QByteArray& xml)
{
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(xml.constData()), xml.size());
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!plist) {
        return 0;
    }
    int copied = 0;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = static_cast<CFDictionaryRef>(plist);
        CFIndex count = CFDictionaryGetCount(dict);
        QVector<const void*> keys(count), values(count);
        CFDictionaryGetKeysAndValues(dict, keys.data(), values.data());
        for (CFIndex i = 0; i < count; ++i) {
            CFStringRef key = static_cast<CFStringRef>(keys[i]);
            CFPropertyListRef existing = CFPreferencesCopyAppValue(key, settingsDomain);
            if (existing) {
                CFRelease(existing);
                continue;
            }
            CFPreferencesSetAppValue(key, values[i], settingsDomain);
            ++copied;
        }
        CFPreferencesAppSynchronize(settingsDomain);
    }
    CFRelease(plist);
    return copied;
}

bool install(bool update)
{
    OcSettings settings;
    const bool exportSettings = !settings.value(importedKey, false).toBool();
    QString error;
    const QByteArray output = runInstaller(exportSettings, &error);
    if (output.isEmpty()) {
        if (!error.isEmpty()) {
            QMessageBox::warning(nullptr, QCoreApplication::applicationName(), error);
        }
        return false;
    }

    const int okAt = output.lastIndexOf(OCG_INSTALL_OK);
    if (okAt < 0) {
        QString reason = QString::fromUtf8(output).trimmed();
        reason.remove(QLatin1String(OCG_INSTALL_ERR " "));
        Logger::instance().addMessage(QObject::tr("Helper installation failed: %1").arg(reason));
        QMessageBox::warning(nullptr, QCoreApplication::applicationName(),
            QObject::tr("The helper could not be installed:\n%1").arg(reason));
        return false;
    }

    const QByteArray tag = OCG_INSTALL_SETTINGS " ";
    const int at = output.indexOf(tag);
    if (at >= 0) {
        const int eol = output.indexOf('\n', at);
        bool ok = false;
        const int len = output.mid(at + tag.size(), eol - at - tag.size()).toInt(&ok);
        if (ok && eol > 0 && len > 0 && eol + 1 + len <= output.size()) {
            const int copied = importSettings(output.mid(eol + 1, len));
            Logger::instance().addMessage(QObject::tr("Imported %1 settings saved with administrator rights").arg(copied));
        }
    }
    if (exportSettings) {
        OcSettings after;
        after.setValue(importedKey, true);
    }
    Logger::instance().addMessage(update ? QObject::tr("Privileged helper updated") : QObject::tr("Privileged helper installed"));

    // launchd creates the socket right after bootstrap; give it a moment.
    for (int i = 0; i < 30 && !MacHelper::ready(); ++i) {
        QThread::msleep(100);
    }
    return MacHelper::ready();
}

}

int MacHelper::installedVersion()
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, OCG_HELPER_SOCKET, sizeof(addr.sun_path));
    struct timeval tv = { 3, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int version = -1;
    struct ocg_msg_hdr hdr = { OCG_MAGIC, OCG_HELPER_VERSION, OCG_OP_VERSION, 0 };
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0
        && write(sock, &hdr, sizeof(hdr)) == static_cast<ssize_t>(sizeof(hdr))
        && recv(sock, &hdr, sizeof(hdr), MSG_WAITALL) == static_cast<ssize_t>(sizeof(hdr))
        && hdr.magic == OCG_MAGIC && hdr.code == 0) {
        version = static_cast<int>(hdr.version);
    }
    close(sock);
    return version;
}

bool MacHelper::ready()
{
    return installedVersion() == OCG_HELPER_VERSION;
}

bool MacHelper::ensure()
{
    const int version = installedVersion();
    if (version == OCG_HELPER_VERSION) {
        return true;
    }

    OcSettings settings;
    const bool update = version >= 0;
    if (!update && settings.value(declinedKey, false).toBool()) {
        return false;
    }

    QMessageBox box;
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QCoreApplication::applicationName());
    if (update) {
        box.setText(QObject::tr("The privileged helper needs to be updated."));
        box.setInformativeText(QObject::tr("This asks for an administrator password once."));
    } else {
        box.setText(QObject::tr("Setting up a VPN connection needs administrator rights."));
        box.setInformativeText(QObject::tr("Install a small system helper to do this, and the password "
                                           "will be asked only now, not at every start. Otherwise the "
                                           "whole application runs as administrator, as before."));
        box.setCheckBox(new QCheckBox(QObject::tr("Don't ask again")));
    }
    QPushButton* installButton = box.addButton(update ? QObject::tr("Update") : QObject::tr("Install"), QMessageBox::AcceptRole);
    box.addButton(QObject::tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(installButton);
    box.exec();

    if (box.clickedButton() != installButton) {
        if (box.checkBox() && box.checkBox()->isChecked()) {
            settings.setValue(declinedKey, true);
        }
        return false;
    }
    return install(update);
}

QByteArray MacHelper::tunScriptCommand(const QString& interfaceName)
{
    // Run by /bin/sh -c: quote the path, it may contain spaces.
    QString path = QCoreApplication::applicationDirPath() + QLatin1String("/ocg-tun-client");
    path.replace(QLatin1String("'"), QLatin1String("'\\''"));
    QString cmd = QLatin1Char('\'') + path + QLatin1Char('\'');
    if (!interfaceName.isEmpty()) {
        QString name = interfaceName;
        name.replace(QLatin1String("'"), QLatin1String("'\\''"));
        cmd += QLatin1String(" '") + name + QLatin1Char('\'');
    }
    return cmd.toUtf8();
}

void MacHelper::enlargeTunSocketBuffers()
{
    // The socket pair is unnamed, unlike e.g. a connection to syslog.
    const int maxFd = getdtablesize();
    for (int fd = 0; fd < maxFd; ++fd) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            continue;
        }
        int type = 0;
        socklen_t len = sizeof(type);
        struct sockaddr_un peer = {};
        socklen_t peerLen = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_DGRAM
            || getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer), &peerLen) != 0
            || peer.sun_family != AF_UNIX || peer.sun_path[0] != '\0') {
            continue;
        }
        int size = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        size = 65536 + 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
}
