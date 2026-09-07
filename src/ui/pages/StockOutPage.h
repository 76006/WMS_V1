#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextEdit;

class StockOutPage final : public QWidget
{
    Q_OBJECT

public:
    StockOutPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void onMaterialChanged();
    void loadLocations();
    void loadBatches();
    void loadSerialNumbers();
    void updateAvailableStock();
    void submit();

private:
    QStringList selectedSerialNumbers() const;
    void selectComboData(QComboBox *combo, const QVariant &value);

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_typeCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLabel *m_numberLabel = nullptr;
    QComboBox *m_materialCombo = nullptr;
    QLabel *m_materialDetail = nullptr;
    QDoubleSpinBox *m_quantitySpin = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QComboBox *m_batchCombo = nullptr;
    QLabel *m_availableLabel = nullptr;
    QLineEdit *m_receiverEdit = nullptr;
    QLineEdit *m_purposeEdit = nullptr;
    QListWidget *m_serialList = nullptr;
    QLabel *m_serialHint = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QPushButton *m_submitButton = nullptr;
    qlonglong m_pendingLocationId = 0;
};

