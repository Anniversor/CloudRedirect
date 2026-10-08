#include "flatpak_update_check.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); } } while (0)
static void Write(const QString& path, const QByteArray& value) {
    QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); f.write(value);
}
static QByteArray Read(const QString& path) {
    QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString fake = dir.filePath("flatpak-fake");
    Write(fake, R"SH(#!/bin/sh
printf '%s|%s\n' "$LC_ALL" "$*" >> "$CR_TEST_DIR/commands"
if [ "$1" = '--host' ]; then
    [ "$2" = 'env' ] && [ "$3" = 'LC_ALL=C' ] && [ "$4" = 'flatpak' ] || exit 19
    shift 4
fi
case "$1" in
remote-list)
    if [ "$CR_TEST_MODE" = 'no-remote' ]; then echo cloudredirect-other; else echo cloudredirect; fi
    ;;
remote-info)
    printf '  Version: '; cat "$CR_TEST_DIR/cached-version"
    ;;
update)
    if [ "$3" = '--appstream' ]; then
        if [ "$CR_TEST_MODE" = 'refresh-fail' ]; then echo 'network unavailable' >&2; exit 3; fi
        if [ "$CR_TEST_MODE" = 'timeout' ]; then exec sleep 3; fi
        sleep 0.05
        printf '%s\n' "$CR_TEST_REMOTE" > "$CR_TEST_DIR/cached-version"
    elif [ "$CR_TEST_MODE" = 'apply-fail' ]; then
        echo 'Updating org.cloudredirect.CloudRedirect'; exit 4
    fi
    ;;
*) exit 10;;
esac
)SH");
    CHECK(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("CR_TEST_DIR", dir.path().toUtf8());
    qputenv("LANG", "zh_CN.UTF-8");

    auto check = [&](const char* mode, QString current, QString remote,
                     FlatpakUpdateCheck::Status expected, bool sandboxed = false, bool missing = false) {
        qputenv("CR_TEST_MODE", mode);
        qputenv("CR_TEST_REMOTE", remote.toUtf8());
        Write(dir.filePath("commands"), {});
        Write(dir.filePath("cached-version"), "2.6.6.3\n");
        QEventLoop loop;
        QTimer heartbeat;
        int ticks = 0, completions = 0;
        QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++ticks; });
        heartbeat.start(5);
        FlatpakUpdateCheck::Options options;
        options.program = missing ? dir.filePath("does-not-exist") : fake;
        options.sandboxed = sandboxed;
        options.timeoutMs = QByteArray(mode) == "timeout" ? 100 : 2000;
        FlatpakUpdateCheck::Result got{FlatpakUpdateCheck::Status::Failed, {}, {}};
        FlatpakUpdateCheck::Check(&loop, current, options, [&](auto result) {
            ++completions; got = result; loop.quit();
        });
        CHECK(completions == 0); // checking never blocks the UI thread
        QTimer::singleShot(5000, &loop, [&] { CHECK(false); loop.quit(); });
        loop.exec();
        CHECK(completions == 1);
        CHECK(got.status == expected);
        if (expected == FlatpakUpdateCheck::Status::Failed) CHECK(!got.error.isEmpty());
        const QByteArray commands = Read(dir.filePath("commands"));
        if (expected == FlatpakUpdateCheck::Status::Available || expected == FlatpakUpdateCheck::Status::UpToDate) {
            CHECK(ticks > 0);
            CHECK(commands.indexOf("--appstream") >= 0);
            CHECK(commands.indexOf("--appstream") < commands.indexOf("remote-info"));
            CHECK(got.remoteVersion == remote);
            CHECK(commands.startsWith("C|"));
        }
        if (QByteArray(mode) == "refresh-fail" || QByteArray(mode) == "timeout")
            CHECK(!commands.contains("remote-info")); // never consult stale data after failure
        if (expected == FlatpakUpdateCheck::Status::NoRemote) CHECK(!commands.contains("--appstream"));
    };
    using S = FlatpakUpdateCheck::Status;
    check("ok", "2.6.6.3+251adb0", "2.6.6.4", S::Available);
    check("ok", "2.6.6.3+251adb0", "2.6.6.4", S::Available, true);
    check("ok", "2.6.6.4+abcdef0", "2.6.6.4", S::UpToDate);
    check("ok", "2.6.6.5", "2.6.6.4", S::UpToDate);
    check("ok", "2.6.6.9", "2.6.6.10", S::Available);
    check("ok", "2.6.6+build", "2.6.6.0", S::UpToDate);
    check("ok", "2.6.6.0+build", "2.6.6", S::UpToDate);
    check("no-remote", "2.6.6.3", "2.6.6.4", S::NoRemote);
    check("refresh-fail", "2.6.6.3", "2.6.6.4", S::Failed);
    check("timeout", "2.6.6.3", "2.6.6.4", S::Failed);
    check("ok", "2.6.6.3", "bad-version", S::Failed);
    check("ok", "2.6.6.3", "2.6.6.4", S::Failed, false, true);
    for (const auto& mode : {QByteArray("ok"), QByteArray("apply-fail")}) {
        qputenv("CR_TEST_MODE", mode);
        QEventLoop loop;
        FlatpakUpdateCheck::Options options; options.program = fake; options.timeoutMs = 500;
        int completions = 0;
        FlatpakUpdateCheck::Apply(&loop, options, [&](bool ok, QString) {
            ++completions; CHECK(ok == (mode == "ok")); loop.quit();
        });
        QTimer::singleShot(2000, &loop, [&] { CHECK(false); loop.quit(); });
        loop.exec(); CHECK(completions == 1);
    }
    std::printf("flatpak_update_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
