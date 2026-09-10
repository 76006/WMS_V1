#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QTableWidget;

class InspectionPage final : public QWidget
{
    Q_OBJECT

public:
    explicit InspectionPage(QSqlDatabase database,
                            Session session,
                            QWidget *parent = nullptr);

public slots:
    void refreshReferenceData();

signals:
    void inspectionChanged();

private slots:
    void createNotice();
    void editPendingNotice();
    void viewHistoryNotice();
    void recordInspectionResult();
    void refreshNotices();

private:
    qlonglong selectedNoticeId(QTableWidget *table) const;
    bool editNotice(qlonglong noticeId = 0);
    void populateTable(QTableWidget *table, const QStringList &statuses);

    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_searchEdit = nullptr;
    QTableWidget *m_pendingTable = nullptr;
    QTableWidget *m_historyTable = nullptr;
    QPushButton *m_editPendingButton = nullptr;
    QPushButton *m_recordResultButton = nullptr;
    QPushButton *m_viewHistoryButton = nullptr;
};
