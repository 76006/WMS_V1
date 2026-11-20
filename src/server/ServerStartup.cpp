#include "server/ServerStartup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QProcess>
#include <QTextStream>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
QString localRoot()
{
    QDir candidate(QCoreApplication::applicationDirPath());
    for (;;) {
        // A copied host must never fall back to another checkout on the same computer.
        if (QFileInfo::exists(candidate.filePath(QStringLiteral("WMS-HOST.txt")))
            || QFileInfo::exists(candidate.filePath(QStringLiteral("scripts/prepare-lan-server.ps1"))))
            return candidate.absolutePath();
        if (!candidate.cdUp()) return {};
    }
}

bool loadPostgresRuntime(const QString &bin, QString *error)
{
    if (bin.isEmpty()) return true; // Explicit remote configurations may supply libpq via PATH.
    const QString libraryPath = QDir(bin).filePath(QStringLiteral("libpq.dll"));
    if (!QFileInfo::exists(libraryPath)) {
        *error = QStringLiteral("PostgreSQL runtime is missing: %1").arg(libraryPath);
        return false;
    }
    qputenv("PATH", (QDir::toNativeSeparators(bin) + QDir::listSeparator()
                     + qEnvironmentVariable("PATH")).toLocal8Bit());
#ifdef Q_OS_WIN
    const QString nativeBin = QDir::toNativeSeparators(bin);
    if (!SetDllDirectoryW(reinterpret_cast<LPCWSTR>(nativeBin.utf16()))) {
        *error = QStringLiteral("Cannot set PostgreSQL DLL directory: %1 (Windows error %2)")
                     .arg(nativeBin).arg(GetLastError());
        return false;
    }
#endif
    // Keep libpq loaded for QPSQL. Loading an absolute path also exposes missing
    // transitive DLLs here instead of reporting a misleading missing database password.
    static QLibrary library;
    library.setFileName(libraryPath);
    library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
    if (!library.load()) {
        *error = QStringLiteral("Cannot load PostgreSQL runtime %1: %2")
                     .arg(libraryPath, library.errorString());
        return false;
    }
    return true;
}
}

bool ServerStartup::prepare(const QString &product, QString *error)
{
    const QString root = localRoot();
    const bool explicitSettings = qEnvironmentVariableIsSet("WMS_PG_DATABASE")
        || qEnvironmentVariableIsSet("WMS_PG_USER") || qEnvironmentVariableIsSet("WMS_PG_PASSWORD")
        || qEnvironmentVariableIsSet("WMS_PG_HOST") || qEnvironmentVariableIsSet("WMS_PG_PORT");
    QString pgBin = qEnvironmentVariable("WMS_PG_BIN");
    if (!explicitSettings) {
        const QString script = QDir(root).filePath(QStringLiteral("scripts/prepare-lan-server.ps1"));
        if (root.isEmpty() || !QFileInfo::exists(script)) {
            *error = QStringLiteral("Local startup helper is missing: scripts/prepare-lan-server.ps1. "
                                    "Keep the executable inside the build/host directory, or supply WMS_PG_DATABASE, WMS_PG_USER and WMS_PG_PASSWORD.");
            return false;
        }
        QTextStream(stdout) << "Preparing existing " << product << " PostgreSQL environment..." << Qt::endl;
        QProcess helper;
        helper.setWorkingDirectory(root);
#ifdef Q_OS_WIN
        helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif
        helper.start(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                         .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")),
                     {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                      QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                      QStringLiteral("-File"), script, QStringLiteral("-Product"), product});
        if (!helper.waitForStarted(5000)) {
            *error = QStringLiteral("Cannot start the local PostgreSQL helper: %1").arg(helper.errorString());
            return false;
        }
        if (!helper.waitForFinished(45000)) {
            helper.kill(); helper.waitForFinished(3000);
            *error = QStringLiteral("Local PostgreSQL preparation timed out. Check outputs/lan-dev/postgres.log.");
            return false;
        }
        if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0) {
            *error = QStringLiteral("Local PostgreSQL preparation failed:\n%1")
                         .arg(QString::fromUtf8(helper.readAllStandardError()));
            return false;
        }
        // stdout contains credentials: parse it in memory, never include it in diagnostics.
        QByteArray response = helper.readAllStandardOutput();
        if (response.startsWith("\xEF\xBB\xBF")) response.remove(0, 3);
        const auto document = QJsonDocument::fromJson(response);
        const auto settings = document.object();
        const QString expectedDatabase = product == QStringLiteral("ICE_BEAUTY")
            ? QStringLiteral("wms_icebeauty_business_demo") : QStringLiteral("wms_meijingling_business_demo");
        if (!document.isObject() || settings.value("product").toString() != product
            || settings.value("host").toString() != QStringLiteral("127.0.0.1")
            || settings.value("port").toInt() != 55432
            || settings.value("database").toString() != expectedDatabase
            || settings.value("user").toString() != expectedDatabase
            || settings.value("password").toString().isEmpty()
            || settings.value("pgBin").toString().isEmpty()) {
            *error = QStringLiteral("The local startup helper returned invalid or mismatched settings.");
            return false;
        }
        qputenv("WMS_PG_HOST", "127.0.0.1"); qputenv("WMS_PG_PORT", "55432");
        qputenv("WMS_PG_DATABASE", expectedDatabase.toUtf8());
        qputenv("WMS_PG_USER", expectedDatabase.toUtf8());
        qputenv("WMS_PG_PASSWORD", settings.value("password").toString().toUtf8());
        pgBin = settings.value("pgBin").toString();
        QTextStream(stdout) << "Using existing " << product << " database " << expectedDatabase
                            << "; data directory: " << settings.value("dataRoot").toString() << Qt::endl;
    } else if (qEnvironmentVariableIsEmpty("WMS_PG_DATABASE") || qEnvironmentVariableIsEmpty("WMS_PG_USER")
               || qEnvironmentVariableIsEmpty("WMS_PG_PASSWORD")) {
        *error = QStringLiteral("Explicit PostgreSQL settings are incomplete. Set WMS_PG_DATABASE, WMS_PG_USER and WMS_PG_PASSWORD together, "
                                "or remove WMS_PG_* connection overrides to use this host's existing configuration.");
        return false;
    }
    if (pgBin.isEmpty() && !root.isEmpty()) {
        const QString localBin = QDir(root).filePath(QStringLiteral("outputs/lan-dev/postgres/pgsql/bin"));
        if (QFileInfo::exists(QDir(localBin).filePath(QStringLiteral("libpq.dll")))) pgBin = localBin;
    }
    return loadPostgresRuntime(pgBin, error);
}
