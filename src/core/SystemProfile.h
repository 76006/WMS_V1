#pragma once

#include <QMap>
#include <QString>

class SystemProfile
{
public:
    // Server processes select one immutable product before accepting connections.
    static void setServerProfile(const QString &profile);
    static void setNetworkClientMode(bool enabled);
    static bool isNetworkClient();
    static bool isMeiJingLing();
    static QString profileId();
    static QString displayName();
    static QString windowTitle();
    static QString organizationName();
    static QString applicationName();
    static QString databaseFileName();
    static int databaseApplicationId();
    static QString templateDirectoryName();
    static QString archiveDirectoryName();
    static QString inspectionNumberStem();
    static QString batchNumberPrefix();
    static QString defaultProjectCode();
    static QString defaultProjectName();
    static QString temporaryPath(const QString &childDirectory);
    static QMap<QString, QString> defaultNumberPrefixes();
};
