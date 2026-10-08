#include "flatpak_update_check.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QVersionNumber>
#include <QRegularExpression>
#include <memory>

namespace FlatpakUpdateCheck {
namespace {
struct CommandResult { bool ok; QString output; QString error; };

void Run(QObject* context, const Options& options, const QStringList& args,
         std::function<void(CommandResult)> completed) {
    auto* process = new QProcess(context);
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    struct State { bool delivered = false; bool timedOut = false; };
    auto state = std::make_shared<State>();
    auto finish = [process, timer, state, completed](bool ok) {
        if (state->delivered) return;
        state->delivered = true;
        timer->stop();
        QString error = state->timedOut ? QStringLiteral("Update check timed out")
            : QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!ok && error.isEmpty()) error = process->errorString();
        CommandResult result{ok && !state->timedOut,
            QString::fromUtf8(process->readAllStandardOutput()), error};
        process->deleteLater();
        completed(std::move(result));
    };
    QObject::connect(process, &QProcess::finished, context,
        [finish](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit && code == 0);
        });
    QObject::connect(process, &QProcess::errorOccurred, context,
        [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(false);
        });
    QObject::connect(timer, &QTimer::timeout, process, [process, state] {
        state->timedOut = true;
        process->kill();
    });
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    process->setProcessEnvironment(env);
    QString program = options.program;
    QStringList commandArgs = args;
    if (options.sandboxed) {
        if (program.isEmpty()) program = "flatpak-spawn";
        // Set the HOST process locale explicitly: translated labels must not
        // silently defeat parsing of the Version: line.
        commandArgs = QStringList{"--host", "env", "LC_ALL=C", "flatpak"} + args;
    } else if (program.isEmpty()) program = "flatpak";
    timer->start(options.timeoutMs);
    process->start(program, commandArgs);
}

QVersionNumber Version(QString text) {
    text = text.section('+', 0, 0).section('-', 0, 0).trimmed();
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+)+$"));
    return pattern.match(text).hasMatch() ? QVersionNumber::fromString(text).normalized() : QVersionNumber();
}
}

void Check(QObject* context, const QString& currentVersion, const Options& options,
           std::function<void(Result)> completed) {
    Run(context, options, {"remote-list", "--user", "--columns=name"},
        [context, currentVersion, options, completed](CommandResult remotes) {
        if (!remotes.ok) {
            completed({Status::Failed, {}, "Cannot read update sources: " + remotes.error});
            return;
        }
        bool found = false;
        for (const auto& line : remotes.output.split('\n'))
            if (line.trimmed() == "cloudredirect") found = true;
        if (!found) { completed({Status::NoRemote, {}, {}}); return; }
        Run(context, options, {"update", "--user", "--appstream", "cloudredirect"},
            [context, currentVersion, options, completed](CommandResult refresh) {
            if (!refresh.ok) {
                completed({Status::Failed, {}, "Cannot refresh update catalog: " + refresh.error});
                return; // stale metadata is not evidence that the app is up to date
            }
            Run(context, options, {"remote-info", "--user", "cloudredirect", "org.cloudredirect.CloudRedirect"},
                [currentVersion, completed](CommandResult info) {
                if (!info.ok) {
                    completed({Status::Failed, {}, "Cannot read remote version: " + info.error});
                    return;
                }
                QString remote;
                for (const auto& line : info.output.split('\n'))
                    if (line.trimmed().startsWith("Version:"))
                        remote = line.mid(line.indexOf(':') + 1).trimmed();
                auto rv = Version(remote), cv = Version(currentVersion);
                if (rv.isNull() || cv.isNull()) {
                    completed({Status::Failed, remote, "Missing or invalid update version"});
                    return;
                }
                completed({QVersionNumber::compare(rv, cv) > 0 ? Status::Available : Status::UpToDate,
                           remote, {}});
            });
        });
    });
}

void Apply(QObject* context, const Options& options,
           std::function<void(bool, QString)> completed) {
    Run(context, options, {"update", "--user", "-y", "org.cloudredirect.CloudRedirect"},
        [completed](CommandResult result) { completed(result.ok, result.error); });
}
}
