#pragma once
#include <QString>

namespace ServerStartup {
// Prepare process-local connection settings and DLL lookup before Qt loads QPSQL.
bool prepare(const QString &product, QString *error);
}
