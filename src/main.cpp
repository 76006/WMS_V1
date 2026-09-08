#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QMessageBox>
#include <QSqlQuery>
#include <QtWebView/QtWebView>

int main(int argc, char *argv[])
{
    QtWebView::initialize();
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("IceBeauty"));
    QCoreApplication::setApplicationName(QStringLiteral("IceBeautyWms"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    QFile styleFile(QStringLiteral(":/resources/styles.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        application.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("冰美肌库存管理系统"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption databaseOption({QStringLiteral("d"), QStringLiteral("database")},
                                      QStringLiteral("使用指定的 SQLite 数据库文件。"),
                                      QStringLiteral("path"));
    QCommandLineOption smokeOption(QStringLiteral("smoke-test"),
                                   QStringLiteral("完成数据库初始化后直接退出，用于部署检查。"));
    QCommandLineOption uiSmokeOption(QStringLiteral("ui-smoke-test"),
                                     QStringLiteral("构造主界面并处理一次事件后退出。"));
    parser.addOption(databaseOption);
    parser.addOption(smokeOption);
    parser.addOption(uiSmokeOption);
    parser.process(application);

    DatabaseConfig config = DatabaseConfig::load();
    if (parser.isSet(databaseOption)) {
        config.filePath = parser.value(databaseOption);
    }
    if (config.filePath.isEmpty()) {
        config.filePath = DatabaseConfig::defaultFilePath();
    }
    if (!parser.isSet(databaseOption)) {
        config.save();
    }

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
    if (parser.isSet(smokeOption)) {
        return 0;
    }
    if (parser.isSet(uiSmokeOption)) {
        QSqlQuery user(databaseManager.database());
        if (!user.exec(QStringLiteral(
                "SELECT id,username,display_name FROM users WHERE username='admin'"))
            || !user.next()) {
            return 3;
        }
        Session session;
        session.userId = user.value(0).toLongLong();
        session.username = user.value(1).toString();
        session.displayName = user.value(2).toString();
        session.roleCode = QStringLiteral("ADMIN");
        MainWindow window(databaseManager.database(), session,
                          databaseManager.databaseFilePath());
        window.show();
        application.processEvents();
        window.close();
        return 0;
    }

    LoginDialog login(databaseManager.database(), databaseManager.databaseFilePath());
    if (login.exec() != QDialog::Accepted) {
        return 0;
    }

    MainWindow mainWindow(databaseManager.database(), login.session(), databaseManager.databaseFilePath());
    mainWindow.show();
    return application.exec();
}
