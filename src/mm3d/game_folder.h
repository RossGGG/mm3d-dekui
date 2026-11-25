// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QtEndian>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace MM3D::GameFolder {
inline constexpr auto Hash = "9c756279e3ec00bb9009f92ca2e7b9ec8b1fcdfcd9de057658b209caa24323da";
// Folder holding the application: the .app's parent on macOS, the AppImage's
// folder on Linux, otherwise the executable's folder.
inline QString AppFolder() {
    const auto appimage = qEnvironmentVariable("APPIMAGE");
#ifdef Q_OS_MACOS
    QDir bundle(QCoreApplication::applicationDirPath());
    if (bundle.dirName() == QStringLiteral("MacOS") && bundle.cdUp() &&
        bundle.dirName() == QStringLiteral("Contents") && bundle.cdUp() &&
        bundle.dirName().endsWith(QStringLiteral(".app")) && bundle.cdUp())
        return bundle.absolutePath();
#endif
    return appimage.isEmpty() ? QCoreApplication::applicationDirPath()
                              : QFileInfo(appimage).absolutePath();
}
inline QString FindRom(const QString& root);
inline QString Root() {
#ifdef Q_OS_TVOS
    // tvOS apps cannot write Documents; Caches is the only large local
    // storage (the system may purge it when space runs out). The game and
    // prepared user folder are copied in with tools/tvos/copy-game.sh.
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/Dekui");
#elif defined(Q_OS_VISIONOS)
    // Application bundles are read-only on visionOS. Keep the imported game,
    // prepared resources, configuration, and saves in the app's Files-visible
    // Documents root. Files presents that root as the app-named Dekui folder.
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
#elif defined(Q_OS_MACOS)
    // Like the iOS and visionOS Documents container, keep the game, prepared
    // resources, configuration and saves in a per-app folder outside the
    // signed bundle: ~/Library/Application Support/<bundle name>. A portable
    // folder that already holds a game or user data beside the app still wins.
    const auto portable = AppFolder();
    if (!FindRom(portable).isEmpty() || QFileInfo(portable + QStringLiteral("/user")).isDir())
        return portable;
    auto name = QCoreApplication::applicationName();
    if (const auto value = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(),
                                                                 kCFBundleNameKey);
        value && CFGetTypeID(value) == CFStringGetTypeID())
        name = QString::fromCFString(static_cast<CFStringRef>(value));
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QLatin1Char('/') + name;
#else
    // AppImage mounts its executable read-only; use the folder holding the image.
    return AppFolder();
#endif
}
// Read-only setup tools, resources and licenses shipped with the application.
inline QString InstallRoot() {
#ifdef Q_OS_MACOS
    // An installed app may carry them inside its bundle instead of beside it.
    const auto inside = QDir(QCoreApplication::applicationDirPath())
                            .absoluteFilePath(QStringLiteral("../Resources/dekui"));
    if (QFileInfo(inside + QStringLiteral("/tools/mm3d")).isDir())
        return QDir::cleanPath(inside);
#endif
    return AppFolder();
}
inline bool EnsureRoot() {
    const auto root = Root();
    if (root.isEmpty() || !QDir().mkpath(root))
        return false;
#ifdef Q_OS_TVOS
    // tools/tvos/copy-game.sh fills these folders. Folders that devicectl
    // creates belong to root and the app could not write into them.
    QDir().mkpath(root + QStringLiteral("/user"));
#endif
#ifdef Q_OS_VISIONOS
    // Files can omit an app's empty document container. A short instruction
    // file makes the Dekui location immediately discoverable on first launch.
    QFile instructions(QDir(root).absoluteFilePath(QStringLiteral("IMPORT GAME HERE.txt")));
    if (!instructions.exists() && instructions.open(QIODevice::WriteOnly | QIODevice::Text)) {
        instructions.write("Copy exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) "
                           "and the prepared user folder into this Dekui folder, then relaunch Dekui.\n");
    }
#endif
    return true;
}
inline QString FindRom(const QString& root) {
    QStringList candidates;
    for (const auto& file : QDir(root).entryInfoList(QDir::Files, QDir::Name)) {
        const auto suffix=file.suffix().toLower();
        if(suffix==QStringLiteral("cci") || suffix==QStringLiteral("3ds"))
            candidates.push_back(file.absoluteFilePath());
    }
    if(candidates.size()==1)return candidates.front();
    if(!candidates.empty())return {}; // Never silently choose between games.
    // Preserve existing portable installations without moving their game/saves.
    const auto legacy=root+QStringLiteral("/game/mm3d.cci");
    return QFileInfo(legacy).isFile()?legacy:QString{};
}
inline QString Rom() { return FindRom(Root()); }
inline QString MissingGameMessage() {
#ifdef Q_OS_TVOS
    return QStringLiteral("Copy exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) and a prepared user folder to the Apple TV with tools/tvos/copy-game.sh, then open Dekui again.");
#elif defined(Q_OS_VISIONOS)
    return QStringLiteral("Open Files > On My Apple Vision Pro > Dekui, then import exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) there.");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("Place exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) in %1, then launch Dekui again. Use a game card with v1.1 preinstalled.").arg(Root());
#else
    return QStringLiteral("Place exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) beside the application, then launch Dekui again. Use a game card with v1.1 preinstalled.");
#endif
}
inline QString User() { return Root() + QStringLiteral("/user/"); }
inline QString Validate() {
    QFile file(Rom());
    if (!file.open(QIODevice::ReadOnly))
        return MissingGameMessage();
    const auto header = file.read(512);
    if (header.size()!=512 || header.mid(0x100,4)!="NCSD") return QStringLiteral("Unsupported game container.");
    const auto word = [&](int at) { return qFromLittleEndian<quint32>(header.constData()+at); };
    const quint64 base=quint64(word(0x120))*512, size=quint64(word(0x124))*512;
    if(base<512 || size!=654643200 || base+size>quint64(file.size()) || !file.seek(base))
        return QStringLiteral("Unsupported or incomplete game dump.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (quint64 remaining=size; remaining;) {
        const auto bytes=file.read(qint64(qMin<quint64>(remaining,1024*1024)));
        if(bytes.isEmpty()) return QStringLiteral("Could not read the game.");
        hash.addData(bytes); remaining-=bytes.size();
    }
    if(hash.result().toHex()!=Hash) return QStringLiteral("Unsupported game revision/content. The supported decrypted USA Rev 1 dump is required.");
    QFile ready(User()+QStringLiteral("setup/ready.json"));
    if(!ready.open(QIODevice::ReadOnly)) return QStringLiteral("Game setup is incomplete. Preserve your user folder and saves, then try a fresh application folder.");
    const auto setup=QJsonDocument::fromJson(ready.readAll()).object();
    if(setup[QStringLiteral("version")].toInt()!=1 || setup[QStringLiteral("rom_sha256")].toString()!=QString::fromLatin1(Hash))
        return QStringLiteral("Game setup is incompatible. Keep your saves and prepare a new application folder.");
    if(setup[QStringLiteral("resource_version")].toInt()!=2 ||
       setup[QStringLiteral("layout_version")].toInt()!=1 ||
       QFileInfo::exists(User()+QStringLiteral("setup/resource-upgrade-pending")))
        return QStringLiteral("Game resource upgrade is incomplete. Close Dekui and launch again to retry. Preserve your user folder and its upgrade backups.");
    return {};
}
}
