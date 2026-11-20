#pragma once

#include <QJsonObject>
#include <QString>

namespace JsonFormRenderer {
bool write(const QString &templatePath, const QJsonObject &document,
           const QString &outputPath, QString *errorMessage);
}
