#pragma once
#include <QObject>
#include <QString>
#include <functional>

namespace FlatpakUpdateCheck {
enum class Status { NoRemote, UpToDate, Available, Failed };
struct Result {
    Status status;
    QString remoteVersion;
    QString error;
};
struct Options {
    bool sandboxed = false;
    QString program; // default: flatpak-spawn inside Flatpak, otherwise flatpak
    int timeoutMs = 30000;
};
// Refresh AppStream before reading Version: remote-info's commit can be fresh
// while its version label is still cached. All subprocesses are asynchronous.
void Check(QObject* context, const QString& currentVersion, const Options& options,
           std::function<void(Result)> completed);
void Apply(QObject* context, const Options& options,
           std::function<void(bool, QString)> completed);
}
