#pragma once

#include <QString>

class QComboBox;

namespace ComboBoxSearch {
void enableContainsSearch(QComboBox *comboBox, const QString &placeholderText);
}
