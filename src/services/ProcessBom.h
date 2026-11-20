#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

struct ProcessBomItem {
    QString code;
    QString name;
    double quantity = 1;
    qlonglong materialId = 0;
    QString issueError;
    QString specification, category, unit, processing, stock;
    QList<ProcessBomItem> items;
    QString label() const { return code.isEmpty() ? name : code + " - " + name; }
    bool issueMaterials(double count, QList<QPair<qlonglong, double>> &lines, QString &error) const;
};

struct ProcessBom {
    QString productCode;
    QString bomType;
    QList<ProcessBomItem> groups;
    QString originalPayload;
    static bool parse(const QByteArray &json, ProcessBom &bom, QString &error);
    static bool load(QSqlDatabase db, qlonglong productId, ProcessBom &bom, QString &error);
    bool resolveMaterials(QSqlDatabase db, QString &error);
    bool store(QSqlDatabase db, qlonglong productId, const QString *expected, QString &error) const;
};
