#include "ui/pages/PlaceholderPage.h"

#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

PlaceholderPage::PlaceholderPage(const QString &title,
                                 const QString &description,
                                 QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(32, 30, 32, 30);
    auto *heading = new QLabel(title, panel);
    heading->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    auto *body = new QLabel(description, panel);
    body->setObjectName(QStringLiteral("mutedText"));
    body->setWordWrap(true);
    layout->addWidget(heading);
    layout->addSpacing(8);
    layout->addWidget(body);
    layout->addStretch();
    root->addWidget(panel);
}

