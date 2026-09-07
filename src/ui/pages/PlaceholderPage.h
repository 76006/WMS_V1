#pragma once

#include <QWidget>

class PlaceholderPage final : public QWidget
{
    Q_OBJECT

public:
    explicit PlaceholderPage(const QString &title,
                             const QString &description,
                             QWidget *parent = nullptr);
};

