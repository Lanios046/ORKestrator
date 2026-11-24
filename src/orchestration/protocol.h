#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>

namespace orchestrator
{
enum class AgentRole
{
    Pm,
    PmCritic,
    Developer,
    DeveloperCritic,
    Censor,
    Researcher,
    Diagnostician
};
inline constexpr std::array<AgentRole, 7> agentRoles{AgentRole::Pm,     AgentRole::PmCritic,   AgentRole::Developer,    AgentRole::DeveloperCritic,
                                                     AgentRole::Censor, AgentRole::Researcher, AgentRole::Diagnostician};

QString agentId(AgentRole role);
QString agentName(AgentRole role);
QString agentPrompt(AgentRole role);
QString developerDirectives();
QJsonObject agentSchema(AgentRole role);
QJsonObject agentRequestSchema(AgentRole role, const QJsonArray &history);
QJsonObject message(const QString &role, const QString &content);
QString jsonText(const QJsonObject &object);
bool parseAgentResponse(const QString &content, AgentRole role, QJsonObject &result, QString &error);
QString reviewMarkdown(const QJsonObject &review);
QJsonArray orderedStages(const QJsonArray &stages, QString &error);
} // namespace orchestrator