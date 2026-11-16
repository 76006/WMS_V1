#include "core/DatabaseConfig.h"
#include "core/SystemProfile.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

QString DatabaseConfig::defaultFilePath()
{
    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(dataDirectory).filePath(SystemProfile::databaseFileName());
}

DatabaseConfig DatabaseConfig::load()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("database"));

    DatabaseConfig config;
    config.filePath = settings.value(QStringLiteral("filePath"), defaultFilePath()).toString();
    return config;
}

void DatabaseConfig::save() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("database"));
    settings.setValue(QStringLiteral("filePath"), filePath);
}

