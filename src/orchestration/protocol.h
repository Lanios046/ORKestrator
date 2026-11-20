#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace orchestrator
{
QJsonObject pmSchema();
QJsonObject criticSchema();
QString pmPrompt();
QString criticPrompt();
QJsonObject message(const QString &role, const QString &content);
QString jsonText(const QJsonObject &object);
bool parseAgentResponse(const QString &content, bool critic, QJsonObject &result, QString &error);
QString reviewMarkdown(const QJsonObject &review);
} // namespace orchestrator