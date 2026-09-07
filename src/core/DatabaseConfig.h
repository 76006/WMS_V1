#pragma once

#include <QString>

struct DatabaseConfig
{
    QString filePath;

    static DatabaseConfig load();
    void save() const;
    static QString defaultFilePath();
};
