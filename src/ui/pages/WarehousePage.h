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
    void editWarehouse();
    void editLocation();
    void toggleWarehouse();
    void toggleLocation();
    void updateActions();

private:
    bool promptCodeAndName(const QString &title, QString *code, QString *name,
                           const QString &initialCode = {}, const QString &initialName = {});
    bool writeAudit(const QString &action, const QString &entityType,
                    qlonglong entityId, const QString &detail);
    qlonglong selectedWarehouseId() const;
    qlonglong selectedLocationId() const;

    QSqlDatabase m_database;
    Session m_session;
    QTableWidget *m_warehouseTable = nullptr;
    QTableWidget *m_locationTable = nullptr;
    QPushButton *m_addWarehouseButton = nullptr;
    QPushButton *m_addLocationButton = nullptr;
    QPushButton *m_editWarehouseButton = nullptr;
    QPushButton *m_editLocationButton = nullptr;
    QPushButton *m_toggleWarehouseButton = nullptr;
    QPushButton *m_toggleLocationButton = nullptr;
};
