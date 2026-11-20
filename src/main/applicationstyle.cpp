#include "applicationstyle.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace client
{
void setupApplicationStyle(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#202630"));
    palette.setColor(QPalette::WindowText, QColor("#e2e7ee"));
    palette.setColor(QPalette::Base, QColor("#171c24"));
    palette.setColor(QPalette::AlternateBase, QColor("#262e3b"));
    palette.setColor(QPalette::Text, QColor("#e2e7ee"));
    palette.setColor(QPalette::Button, QColor("#303b4b"));
    palette.setColor(QPalette::ButtonText, QColor("#e2e7ee"));
    palette.setColor(QPalette::Highlight, QColor("#3975b8"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor("#303b4b"));
    palette.setColor(QPalette::ToolTipText, QColor("#e2e7ee"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#7a8595"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#7a8595"));
    palette.setColor(QPalette::PlaceholderText, QColor("#8994a5"));
    app.setPalette(palette);
    app.setFont(QFont(QStringLiteral("Segoe UI"), 10));
}
} // namespace client
