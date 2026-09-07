#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

class InventoryCountPage final : public QWidget
{
    Q_OBJECT
public:
    InventoryCountPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void stockChanged();

private slots:
    void loadLocations();
    void loadSnapshot();
    void submit();

private:
    void resetSubmissionToken();

    QSqlDatabase m_database;
    Session m_session;
    QComboBox *m_warehouseCombo = nullptr;
    QComboBox *m_locationCombo = nullptr;
    QDateEdit *m_dateEdit = nullptr;
    QLineEdit *m_handlerEdit = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_submitButton = nullptr;
    QString m_submissionToken;
};
