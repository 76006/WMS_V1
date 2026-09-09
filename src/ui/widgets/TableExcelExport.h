#pragma once

#include <QString>

class QTableView;
class QWidget;

class TableExcelExport
{
public:
    static void install(QWidget *page, const QString &pageTitle);
    static void exportPage(QWidget *page, const QString &pageTitle, QWidget *dialogParent);

private:
    enum class Scope { AllRows, SelectedRows };

    static void exportTable(QTableView *table, const QString &title,
                            Scope scope, QWidget *dialogParent);
};
