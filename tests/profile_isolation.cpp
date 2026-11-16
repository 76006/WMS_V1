#include "core/SystemProfile.h"
#include "core/DatabaseConfig.h"
#include <QCoreApplication>
#include <QSettings>
#include <QTextStream>

struct Paths {
    QString application, settings, database, archive, temporary;
};

Paths paths(bool network)
{
    SystemProfile::setNetworkClientMode(network);
    QCoreApplication::setOrganizationName(SystemProfile::organizationName());
    QCoreApplication::setApplicationName(SystemProfile::applicationName());
    QSettings settings;
    // Read path metadata only. Never change real user preferences or open a database.
    return {SystemProfile::applicationName(),settings.fileName(),
            DatabaseConfig::defaultFilePath(),SystemProfile::archiveDirectoryName(),
            SystemProfile::temporaryPath("isolation-check")};
}

int main(int argc,char **argv)
{
    QCoreApplication application(argc,argv);
    for(const auto &product:QStringList{"ICE_BEAUTY","MEI_JING_LING"}) {
        SystemProfile::setServerProfile(product);
        const auto local=paths(false),network=paths(true);
        if(local.application==network.application || local.settings==network.settings ||
           local.database==network.database || local.archive==network.archive ||
           local.temporary==network.temporary) return 1;
        const auto restored=paths(false);
        if(local.application!=restored.application || local.settings!=restored.settings ||
           local.database!=restored.database || local.archive!=restored.archive ||
           local.temporary!=restored.temporary) return 2;
        QTextStream(stdout)<<product<<": settings, database, archive and temporary paths isolated; local paths unchanged\n";
    }
    return 0;
}
