#pragma once

#include <QString>

class QTableView;
class QWidget;

class TableExcelExport
{
public:
    static void install(QWidget *page, const QString &pageTitle);
    static void exportPage(QWidget *page, const QString &pageTitle, QWidget *dialogParent);
    static void fullScreenTable(QTableView *table, const QString &title, QWidget *dialogParent);

private:
    enum class Scope { AllRows, SelectedRows };

    static void exportTable(QTableView *table, const QString &title,
                            Scope scope, QWidget *dialogParent);
};
