#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;

class SerialTracePage final : public QWidget
{
    Q_OBJECT
public:
    explicit SerialTracePage(QSqlDatabase database, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void loadSerials();
    void loadHistory();

private:
    QSqlDatabase m_database;
    QLineEdit *m_keywordEdit = nullptr;
    QComboBox *m_statusCombo = nullptr;
    QComboBox *m_warehouseCombo = nullptr;
    QTableWidget *m_serialTable = nullptr;
    QLabel *m_relationLabel = nullptr;
    QTableWidget *m_attachmentTable = nullptr;
    QTableWidget *m_historyTable = nullptr;
};
