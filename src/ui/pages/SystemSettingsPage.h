#pragma once
#include "core/Session.h"
#include <QSqlDatabase>
#include <QWidget>
class QLineEdit;
class SystemSettingsPage final : public QWidget
{
    Q_OBJECT
public:
    SystemSettingsPage(QSqlDatabase database, Session session,
                       const QString &databaseFilePath, QWidget *parent = nullptr);
private slots:
    void changePassword();
private:
    QSqlDatabase m_database;
    Session m_session;
    QLineEdit *m_currentPassword = nullptr;
    QLineEdit *m_newPassword = nullptr;
    QLineEdit *m_confirmPassword = nullptr;
};
