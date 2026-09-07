#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;

class StockInPage final : public QWidget
{
    Q_OBJECT

public:
    StockInPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void onMaterialChanged();
    void loadLocations();
    void updateAvailableStock();
    void generateSerialNumbers();
    void submit();

private:
    QStringList enteredSerialNumbers() const;
    void selectComboData(QComboBox *combo, const QVariant &value);

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_typeCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLabel *m_numberLabel = nullptr;
    QComboBox *m_materialCombo = nullptr;
    QLabel *m_materialDetail = nullptr;
    QDoubleSpinBox *m_quantitySpin = nullptr;
    QLineEdit *m_batchEdit = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QLabel *m_availableLabel = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_serialEdit = nullptr;
    QPushButton *m_generateSerialButton = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QPushButton *m_submitButton = nullptr;
    qlonglong m_pendingLocationId = 0;
};

