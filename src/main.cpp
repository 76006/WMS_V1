#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QMessageBox>
#include <QScrollBar>
#include <QSqlQuery>
#include <QWheelEvent>
#include <QtWebView/QtWebView>

namespace {
class TableWheelFilter final : public QObject
{
public:
    explicit TableWheelFilter(QObject *parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Wheel) return false;

        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;

        bool insideValueEditor = false;
        QAbstractItemView *table = nullptr;
        for (QWidget *current = widget; current; current = current->parentWidget()) {
            if (qobject_cast<QComboBox *>(current)
                || qobject_cast<QAbstractSpinBox *>(current)) {
                insideValueEditor = true;
            }
            if (auto *view = qobject_cast<QAbstractItemView *>(current)) {
                table = view;
                break;
            }
        }
        if (!table) return false;

        if (!table->property("wheelScrollConfigured").toBool()) {
            table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            table->setProperty("wheelScrollConfigured", true);
        }

        auto *wheel = static_cast<QWheelEvent *>(event);
        const QPoint pixelDelta = wheel->pixelDelta();
        const QPoint angleDelta = wheel->angleDelta();
        const bool horizontal = wheel->modifiers().testFlag(Qt::ShiftModifier)
            || qAbs(pixelDelta.x()) > qAbs(pixelDelta.y())
            || (pixelDelta.isNull() && qAbs(angleDelta.x()) > qAbs(angleDelta.y()));

        QScrollBar *scrollBar = horizontal ? table->horizontalScrollBar()
                                           : table->verticalScrollBar();
        int delta = 0;
        bool pixelBased = false;
        if (horizontal) {
            if (pixelDelta.x() != 0) {
                delta = pixelDelta.x();
                pixelBased = true;
            } else if (wheel->modifiers().testFlag(Qt::ShiftModifier)
                       && pixelDelta.y() != 0) {
                delta = pixelDelta.y();
                pixelBased = true;
            } else {
                delta = angleDelta.x() != 0 ? angleDelta.x() : angleDelta.y();
            }
        } else if (pixelDelta.y() != 0) {
            delta = pixelDelta.y();
            pixelBased = true;
        } else {
            delta = angleDelta.y();
        }

        // 内层明细表到达上下边界后，把滚轮继续交给外层页面滚动区，
        // 避免低分辨率或高缩放下表格底部被页面裁切而无法看到最后一行。
        const bool towardMaximum = delta < 0;
        const bool atInnerBoundary = delta != 0
            && ((towardMaximum && scrollBar->value() >= scrollBar->maximum())
                || (!towardMaximum && scrollBar->value() <= scrollBar->minimum()));
        if (!horizontal && atInnerBoundary) {
            for (QWidget *current = table->parentWidget(); current;
                 current = current->parentWidget()) {
                auto *outerArea = qobject_cast<QAbstractScrollArea *>(current);
                if (!outerArea) continue;
                QScrollBar *outerBar = outerArea->verticalScrollBar();
                const bool outerCanScroll = towardMaximum
                    ? outerBar->value() < outerBar->maximum()
                    : outerBar->value() > outerBar->minimum();
                if (!outerCanScroll) continue;
                const int step = qMax(24, outerBar->singleStep() * 3);
                outerBar->setValue(outerBar->value() + (towardMaximum ? step : -step));
                wheel->accept();
                return true;
            }
        }

        // 非编辑区域保留 Qt 原生的纵向滚动，只补充横向滚动支持。
        if (!insideValueEditor && !horizontal) return false;

        if (delta != 0) {
            int distance = pixelBased
                ? delta
                : (delta * qMax(1, scrollBar->singleStep()) * 3) / 120;
            if (distance == 0) distance = delta > 0 ? 1 : -1;
            scrollBar->setValue(scrollBar->value() - distance);
        }
        wheel->accept();
        return true;
    }
};
}

int main(int argc, char *argv[])
{
    QtWebView::initialize();
    QApplication application(argc, argv);
    TableWheelFilter tableWheelFilter(&application);
    application.installEventFilter(&tableWheelFilter);
    QCoreApplication::setOrganizationName(QStringLiteral("IceBeauty"));
    QCoreApplication::setApplicationName(QStringLiteral("IceBeautyWms"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.1.0"));

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
