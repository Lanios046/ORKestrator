#pragma once
#include "network/lmstudioclient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QStringList>
#include <QUuid>

namespace pipeline
{
struct Agent
{
    QString id{QUuid::createUuid().toString(QUuid::WithoutBraces)};
    QString name{QStringLiteral("Агент")};
    QString model;
    QString rules{QStringLiteral("Выполни поручение пользователя. Изучи нужные данные. Отделяй факты от предположений. "
                                 "Не добавляй комментарии в код без прямого разрешения пользователя. "
                                 "Не запускай сборку и проверки, если пользователь их запретил.")};
    QString prompt{QStringLiteral("Обработай задачу и входные данные согласно своим правилам.")};
    QString resultField{"result"};
    QString positive{"positive"};
    QString negative{"negative"};
    QStringList tools;
    orchestrator::ReasoningMode thinking{orchestrator::ReasoningMode::Auto};
    int tokens{65536}, timeout{1800}, commandTimeout{300}, toolLimit{12}, reasoningBudget{24000};
    bool automatic{true}, freshContext{false};
    QPointF position;
    QJsonObject toJson() const;
    static Agent fromJson(const QJsonObject &json);
    QString validate(bool requireModel = true) const;
    QJsonObject responseSchema() const;
    QString systemPrompt() const;
};
struct Edge
{
    QString from, to;
    bool positive{true};
    QJsonObject toJson() const;
};
struct Pipeline
{
    QList<Agent> agents;
    QList<Edge> edges;
    QString start;
    int automaticLimit{100};
    const Agent *find(const QString &id) const;
    QString next(const QString &from, bool positive) const;
    QString validate(bool requireModels = true) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, Pipeline &result, QString &error);
};
QStringList toolNames();
QString jsonText(const QJsonObject &value);
bool writeJson(const QString &path, const QJsonObject &json, QString &error);
bool readJson(const QString &path, QJsonObject &json, QString &error);
}
