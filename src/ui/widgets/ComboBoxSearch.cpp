#include "ui/widgets/ComboBoxSearch.h"

#include <QComboBox>
#include <QCompleter>
#include <QLineEdit>

void ComboBoxSearch::enableContainsSearch(QComboBox *comboBox,
                                          const QString &placeholderText)
{
    if (!comboBox) return;
    comboBox->setEditable(true);
    comboBox->setInsertPolicy(QComboBox::NoInsert);
    comboBox->setMaxVisibleItems(20);
    if (comboBox->lineEdit()) {
        comboBox->lineEdit()->setPlaceholderText(placeholderText);
        comboBox->lineEdit()->setClearButtonEnabled(true);
    }
    auto *completer = new QCompleter(comboBox->model(), comboBox);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(20);
    comboBox->setCompleter(completer);
}
