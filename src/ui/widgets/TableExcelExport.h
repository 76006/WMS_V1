#pragma once

#include <QString>
#include <QList>

#include <functional>

class QTableView;
class QWidget;

class TableExcelExport
{
public:
    struct FullScreenAction {
        QString text;
        std::function<void()> trigger;
    };

    static void install(QWidget *page, const QString &pageTitle);
    static void exportPage(QWidget *page, const QString &pageTitle, QWidget *dialogParent);
    static void fullScreenTable(QTableView *table, const QString &title, QWidget *dialogParent,
                                const std::function<void()> &reload = {},
                                const QList<FullScreenAction> &actions = {});

private:
    enum class Scope { AllRows, SelectedRows };

    static void exportTable(QTableView *table, const QString &title,
                            Scope scope, QWidget *dialogParent);
};
