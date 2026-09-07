#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QFile>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("IceBeauty"));
    QCoreApplication::setApplicationName(QStringLiteral("IceBeautyWms"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QFile styleFile(QStringLiteral(":/resources/styles.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        application.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    DatabaseConfig config = DatabaseConfig::load();
    if (config.filePath.isEmpty()) {
        config.filePath = DatabaseConfig::defaultFilePath();
    }
    config.save();

    DatabaseManager databaseManager;
    QString errorMessage;
    if (!databaseManager.open(config, &errorMessage)) {
        QMessageBox::critical(nullptr, QStringLiteral("数据库打开失败"), errorMessage);
        return 1;
    }
    if (!SchemaMigrator::migrate(databaseManager.database(), &errorMessage)) {
        QMessageBox::critical(nullptr, QStringLiteral("数据库初始化失败"), errorMessage);
        return 2;
    }

    LoginDialog login(databaseManager.database(), databaseManager.databaseFilePath());
    if (login.exec() != QDialog::Accepted) {
        return 0;
    }

    MainWindow mainWindow(databaseManager.database(), login.session(), databaseManager.databaseFilePath());
    mainWindow.show();
    return application.exec();
}

