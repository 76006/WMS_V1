#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QPushButton;
class QTableWidget;

class WarehousePage final : public QWidget
{
    Q_OBJECT

public:
    WarehousePage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refresh();

signals:
    void dataChanged();

private slots:
    void loadLocations();
    void addWarehouse();
    void addLocation();

private:
    bool promptCodeAndName(const QString &title, QString *code, QString *name);
    qlonglong selectedWarehouseId() const;

    QSqlDatabase m_database;
    Session m_session;
    QTableWidget *m_warehouseTable = nullptr;
    QTableWidget *m_locationTable = nullptr;
    QPushButton *m_addWarehouseButton = nullptr;
    QPushButton *m_addLocationButton = nullptr;
};

