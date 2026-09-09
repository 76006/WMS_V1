#include "ui/widgets/TableExcelExport.h"

#include "import/XlsxExporter.h"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableView>
#include <QTextEdit>
#include <QTimeEdit>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace {
QString safeTitle(QString value)
{
    value = value.trimmed();
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return value.isEmpty() ? QStringLiteral("清单") : value;
}

QString tableTitle(QTableView *table, const QString &pageTitle, int index, int total)
{
    const QString installed = table->property("excelExportTitle").toString().trimmed();
    if (!installed.isEmpty()) return installed;
    QString firstHeader;
    if (table->model()) {
        for (int column = 0; column < table->model()->columnCount(); ++column) {
            if (table->isColumnHidden(column)) continue;
            firstHeader = table->model()->headerData(column, Qt::Horizontal, Qt::DisplayRole)
                              .toString().trimmed();
            if (!firstHeader.isEmpty()) break;
        }
    }
    if (total == 1) return pageTitle;
    if (!firstHeader.isEmpty()) return QStringLiteral("%1-%2").arg(pageTitle, firstHeader);
    return QStringLiteral("%1-清单%2").arg(pageTitle).arg(index + 1);
}

QVariant onlineCellValue(QTableView *table, const QModelIndex &modelIndex)
{
    if (QWidget *widget = table->indexWidget(modelIndex)) {
        if (auto *combo = qobject_cast<QComboBox *>(widget)) return combo->currentText();
        if (auto *date = qobject_cast<QDateEdit *>(widget)) return date->date();
        if (auto *time = qobject_cast<QTimeEdit *>(widget)) return time->time().toString(QStringLiteral("HH:mm:ss"));
        if (auto *dateTime = qobject_cast<QDateTimeEdit *>(widget)) return dateTime->dateTime();
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) return spin->value();
        if (auto *spin = qobject_cast<QSpinBox *>(widget)) return spin->value();
        if (auto *check = qobject_cast<QCheckBox *>(widget))
            return check->isChecked() ? QStringLiteral("是") : QStringLiteral("否");
        if (auto *edit = qobject_cast<QLineEdit *>(widget)) return edit->text();
        if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) return edit->toPlainText();
        if (auto *edit = qobject_cast<QTextEdit *>(widget)) return edit->toPlainText();
    }

    QAbstractItemModel *model = table->model();
    QVariant value = model->data(modelIndex, Qt::EditRole);
    if (!value.isValid()) value = model->data(modelIndex, Qt::DisplayRole);
    const QVariant checked = model->data(modelIndex, Qt::CheckStateRole);
    if (checked.isValid() && value.toString().trimmed().isEmpty()) {
        return checked.toInt() == Qt::Checked ? QStringLiteral("是") : QStringLiteral("否");
    }
    return value;
}

QList<int> selectedRows(QTableView *table)
{
    QSet<int> uniqueRows;
    if (table->selectionModel()) {
        for (const QModelIndex &index : table->selectionModel()->selectedIndexes())
            uniqueRows.insert(index.row());
    }
    QList<int> rows = uniqueRows.values();
    std::sort(rows.begin(), rows.end());
    return rows;
}

QList<QTableView *> visibleTables(QWidget *page)
{
    QList<QTableView *> result;
    if (!page) return result;
    for (QTableView *table : page->findChildren<QTableView *>()) {
        if (table->model() && table->model()->columnCount() > 0 && table->isVisibleTo(page))
            result.append(table);
    }
    return result;
}
}

void TableExcelExport::install(QWidget *page, const QString &pageTitle)
{
    if (!page) return;
    const QList<QTableView *> tables = page->findChildren<QTableView *>();
    for (int index = 0; index < tables.size(); ++index) {
        QTableView *table = tables.at(index);
        if (table->property("excelExportInstalled").toBool()) continue;
        const QString title = tableTitle(table, pageTitle, index, tables.size());
        table->setProperty("excelExportInstalled", true);
        table->setProperty("excelExportTitle", title);
        if (table->selectionMode() != QAbstractItemView::NoSelection)
            table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QPointer<QTableView> safeTable(table);
        QObject::connect(table, &QWidget::customContextMenuRequested, table,
                         [safeTable, title](const QPoint &position) {
            if (!safeTable) return;
            QMenu menu(safeTable.data());
            QAction *selected = menu.addAction(QStringLiteral("导出选中行到Excel"));
            selected->setEnabled(!selectedRows(safeTable.data()).isEmpty());
            QAction *all = menu.addAction(QStringLiteral("导出全部到Excel"));
            QAction *chosen = menu.exec(safeTable->viewport()->mapToGlobal(position));
            if (chosen == selected) {
                TableExcelExport::exportTable(safeTable.data(), title, Scope::SelectedRows,
                                              safeTable.data());
            } else if (chosen == all) {
                TableExcelExport::exportTable(safeTable.data(), title, Scope::AllRows,
                                              safeTable.data());
            }
        });
    }
}

void TableExcelExport::exportPage(QWidget *page, const QString &pageTitle, QWidget *dialogParent)
{
    install(page, pageTitle);
    const QList<QTableView *> tables = visibleTables(page);
    if (tables.isEmpty()) {
        QMessageBox::information(dialogParent, QStringLiteral("没有可导出清单"),
                                 QStringLiteral("当前页面没有可导出的表格清单。"));
        return;
    }

    QTableView *table = tables.first();
    QString title = tableTitle(table, pageTitle, 0, tables.size());
    if (tables.size() > 1) {
        QStringList names;
        for (int index = 0; index < tables.size(); ++index) {
            QString name = tableTitle(tables.at(index), pageTitle, index, tables.size());
            if (names.contains(name)) name += QStringLiteral("（%1）").arg(index + 1);
            names.append(name);
        }
        bool accepted = false;
        const QString selectedName = QInputDialog::getItem(
            dialogParent, QStringLiteral("选择导出清单"), QStringLiteral("清单"),
            names, 0, false, &accepted);
        if (!accepted) return;
        const int selectedIndex = names.indexOf(selectedName);
        if (selectedIndex < 0) return;
        table = tables.at(selectedIndex);
        title = selectedName;
    }

    Scope scope = Scope::AllRows;
    if (!selectedRows(table).isEmpty()) {
        QMessageBox choice(dialogParent);
        choice.setWindowTitle(QStringLiteral("选择导出范围"));
        choice.setText(QStringLiteral("当前清单已有选中记录，请选择导出范围。"));
        QPushButton *selectedButton = choice.addButton(QStringLiteral("仅导出选中行"),
                                                       QMessageBox::AcceptRole);
        QPushButton *allButton = choice.addButton(QStringLiteral("导出全部"),
                                                  QMessageBox::ActionRole);
        choice.addButton(QMessageBox::Cancel);
        choice.setDefaultButton(selectedButton);
        choice.exec();
        if (choice.clickedButton() == selectedButton) scope = Scope::SelectedRows;
        else if (choice.clickedButton() != allButton) return;
    }
    exportTable(table, title, scope, dialogParent);
}

void TableExcelExport::exportTable(QTableView *table, const QString &title,
                                   Scope scope, QWidget *dialogParent)
{
    if (!table || !table->model()) return;
    QAbstractItemModel *model = table->model();
    while (model->canFetchMore(QModelIndex())) model->fetchMore(QModelIndex());

    QList<int> columns;
    QStringList headers;
    for (int column = 0; column < model->columnCount(); ++column) {
        if (table->isColumnHidden(column)) continue;
        columns.append(column);
        QString header = model->headerData(column, Qt::Horizontal, Qt::DisplayRole)
                             .toString().trimmed();
        headers.append(header.isEmpty() ? QStringLiteral("列%1").arg(column + 1) : header);
    }
    if (headers.isEmpty()) {
        QMessageBox::information(dialogParent, QStringLiteral("无法导出"),
                                 QStringLiteral("当前清单没有可见列。"));
        return;
    }

    QList<int> rows;
    if (scope == Scope::SelectedRows) {
        rows = selectedRows(table);
        if (rows.isEmpty()) {
            QMessageBox::information(dialogParent, QStringLiteral("请选择记录"),
                                     QStringLiteral("请先在清单中选择需要导出的行。"));
            return;
        }
    } else {
        for (int row = 0; row < model->rowCount(); ++row) rows.append(row);
    }

    QList<QList<QVariant>> data;
    for (int row : rows) {
        QList<QVariant> values;
        for (int column : columns)
            values.append(onlineCellValue(table, model->index(row, column)));
        data.append(values);
    }

    const QString cleanTitle = safeTitle(title);
    const QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defaultPath = QDir(baseDirectory).filePath(
        QStringLiteral("%1-%2.xlsx").arg(cleanTitle,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    QString path = QFileDialog::getSaveFileName(
        dialogParent, QStringLiteral("导出Excel"), defaultPath,
        QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".xlsx"), Qt::CaseInsensitive))
        path += QStringLiteral(".xlsx");

    QString error;
    if (!XlsxExporter::writeSingleSheet(path, cleanTitle.left(31), headers, data, &error)) {
        QMessageBox::warning(dialogParent, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(dialogParent, QStringLiteral("导出完成"),
                             QStringLiteral("已导出 %1 条记录。\n%2")
                                 .arg(data.size()).arg(QDir::toNativeSeparators(path)));
}
