#pragma once

#include "core/Session.h"

#include <QSqlDatabase>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QTableWidget;

class UserManagementPage final : public QWidget
{
    Q_OBJECT
public:
    UserManagementPage(QSqlDatabase database, Session session, QWidget *parent = nullptr);
public slots:
    void refresh();
private slots:
    void addUser();
    void editUser();
    void resetPassword();
    void updateActions();
private:
    qlonglong selectedUserId() const;
    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_keywordEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_resetButton = nullptr;
};
