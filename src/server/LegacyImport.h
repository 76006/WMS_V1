#pragma once

#include <QSqlDatabase>
#include <QString>

namespace LegacyImport {
// Source is a validated SQLite snapshot owned by the importer. Target is the
// product's raw PostgreSQL connection; importing and marking completion are atomic.
bool run(QSqlDatabase target, QSqlDatabase source, const QString &product,
         QString *report, QString *error);
}
