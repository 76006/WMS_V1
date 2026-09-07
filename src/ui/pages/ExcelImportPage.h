#pragma once

#include "core/Session.h"
#include "import/LegacyInventoryImporter.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class ExcelImportPage final : public QWidget
{
    Q_OBJECT
public:
    ExcelImportPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void browseFile();
    void loadLocations();
    void preview();
    void importInventory();

private:
    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_fileEdit = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_importButton = nullptr;
    QList<LegacyImportRow> m_rows;
};
