#include "orchestrator.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QSysInfo>
#include <QUuid>
#include <spdlog/spdlog.h>

namespace orchestrator
{
namespace
{
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString digest(const QJsonObject &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString actionKey(const QJsonObject &action)
{
    const auto type = action.value("type").toString();
    auto path = QDir::cleanPath(QDir::fromNativeSeparators(action.value("path").toString()));
    if (path == ".")
    {
        path.clear();
    }
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    QJsonObject key{{"type", type}, {"path", path}};
    if (type == "run_command")
    {
        key.insert("program", action.value("program"));
        key.insert("arguments", action.value("arguments"));
    }
    else if (type == "write_file")
    {
        key.insert("content", action.value("content"));
    }
    return digest(key);
}
size_t indexOf(AgentRole role)
{
    return static_cast<size_t>(role);
}
Orchestrator::State workingState(AgentRole role)
{
    switch (role)
    {
    case AgentRole::Pm:
        return Orchestrator::State::PmWorking;
    case AgentRole::PmCritic:
        return Orchestrator::State::CriticWorking;
    case AgentRole::Developer:
        return Orchestrator::State::DeveloperWorking;
    case AgentRole::DeveloperCritic:
        return Orchestrator::State::DeveloperCriticWorking;
    case AgentRole::Censor:
        return Orchestrator::State::CensorWorking;
    case AgentRole::Researcher:
        return Orchestrator::State::ResearcherWorking;
    case AgentRole::Diagnostician:
        return Orchestrator::State::DiagnosticianWorking;
    }
    return Orchestrator::State::Failed;
}
bool validConfiguration(const Configuration &c)
{
    return !c.pmModel.trimmed().isEmpty() && !c.criticModel.trimmed().isEmpty() && !c.developerModel.trimmed().isEmpty() &&
           !c.developerCriticModel.trimmed().isEmpty() && !c.censorModel.trimmed().isEmpty() && c.timeoutMs > 0 && c.commandTimeoutMs > 0 && c.maxTokens > 0 &&
           c.maxPmTurns > 0 && c.maxAgentTurns > 0 && c.maxReviews > 0 && c.maxResearchTurns > 0 && c.maxDiagnosticTurns > 0 && c.maxDiagnosticsPerStage > 0 &&
           c.maxServiceTokens >= 0 && c.maxReasoningCharacters >= 0;
}
} // namespace
Orchestrator::Orchestrator(QObject *parent) : QObject(parent), m_executor(this)
{
    connect(&m_client, &LmStudioClient::completionReady, this, &Orchestrator::onCompletion);
    connect(&m_client, &LmStudioClient::failed, this, &Orchestrator::fail);
    connect(&m_client, &LmStudioClient::generationLoopDetected, this, [this](const QString &error) { handleGenerationFailure(error, false); });
    connect(&m_client, &LmStudioClient::reasoningBudgetExceeded, this, [this](const QString &error) { handleGenerationFailure(error, true); });
    connect(&m_client, &LmStudioClient::generationConfigured, this, [this](int tokens, const QString &reasoning) {
        log(QStringLiteral("%1: до %2 токенов ответа; мышление: %3.").arg(agentName(m_role)).arg(tokens).arg(reasoning));
    });
    connect(&m_client, &LmStudioClient::generationProgress, this,
            [this](qint64 content, qint64 reasoning) { emit generationProgress(agentName(m_role), content, reasoning); });
    connect(&m_executor, &TaskExecutor::finished, this, &Orchestrator::onToolsFinished);
    connect(&m_executor, &TaskExecutor::activity, this, [this](const QString &s) { log(agentName(m_role) + ": " + s); });
}
void Orchestrator::handleGenerationFailure(const QString &error, bool reasoningBudget)
{
    if (!active() || m_state == State::AwaitingAnswers || m_state == State::ExecutingTools)
    {
        return;
    }
    m_generationFailures.append(QJsonObject{
        {"agent", agentId(m_role)}, {"context_id", m_contextKeys[indexOf(m_role)]}, {"time", now()}, {"error", error}, {"reasoning_budget", reasoningBudget}});
    const auto currentModel = model(m_role);
    const bool canDisable = m_configuration.reasoningMode == ReasoningMode::Auto && m_client.reasoningCanBeDisabled(currentModel);
    if (m_generationRetries >= 1 || (reasoningBudget && !canDisable))
    {
        fail(QStringLiteral("%1 Измените настройки мышления или его бюджет и повторите запрос.").arg(error));
        return;
    }
    ++m_generationRetries;
    m_disableReasoningOnRetry = canDisable;
    log(agentName(m_role) + (canDisable ? QStringLiteral(": одна повторная попытка без мышления и без поврежденного ответа в контексте.")
                                        : QStringLiteral(": одна повторная попытка без поврежденного ответа в контексте.")));
    sendCurrent();
}
QString Orchestrator::model(AgentRole role) const
{
    switch (role)
    {
    case AgentRole::Pm:
        return m_configuration.pmModel;
    case AgentRole::PmCritic:
        return m_configuration.criticModel;
    case AgentRole::Developer:
        return m_configuration.developerModel;
    case AgentRole::DeveloperCritic:
        return m_configuration.developerCriticModel;
    case AgentRole::Censor:
        return m_configuration.censorModel;
    case AgentRole::Researcher:
        return m_configuration.researcherModel.trimmed().isEmpty() ? m_configuration.pmModel : m_configuration.researcherModel;
    case AgentRole::Diagnostician:
        return m_configuration.diagnosticianModel.trimmed().isEmpty() ? m_configuration.developerModel : m_configuration.diagnosticianModel;
    }
    return {};
}
void Orchestrator::start(const QString &task, const Configuration &configuration)
{
    if (active())
    {
        return;
    }
    m_client.cancel();
    m_executor.cancel();
    m_configuration = configuration;
    if (m_configuration.developerModel.isEmpty())
    {
        m_configuration.developerModel = configuration.pmModel;
    }
    if (m_configuration.developerCriticModel.isEmpty())
    {
        m_configuration.developerCriticModel = configuration.criticModel;
    }
    if (m_configuration.censorModel.isEmpty())
    {
        m_configuration.censorModel = configuration.criticModel;
    }
    m_client.configure(m_configuration);
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_createdAt = now();
    m_task = task.trimmed();
    m_plan.clear();
    m_implementation.clear();
    m_error.clear();
    m_questions = {};
    m_answers = {};
    m_stages = {};
    m_accepted = {};
    m_stageHistory = {};
    m_reviews = {};
    m_plans = {};
    m_exchanges = {};
    m_events = {};
    m_tools = {};
    m_toolObservations = {};
    m_pendingActions = {};
    m_toolLoopEvents = {};
    m_stagnantToolBatches.clear();
    m_workspaceRevision = 0;
    m_calls.fill(0);
    m_contextCalls.fill(0);
    m_contextKeys.fill(QString{});
    m_histories = {};
    m_contexts = {};
    m_research = {};
    m_diagnostics = {};
    m_generationFailures = {};
    m_generationRetries = 0;
    m_disableReasoningOnRetry = false;
    m_diagnosticFeedback = {};
    m_diagnosticCounts.clear();
    m_diagnosticSequence = 0;
    m_failedActions.fill(0);
    m_diagnosticReturnRole = AgentRole::Developer;
    m_stageIndex = 0;
    m_planRevision = 0;
    m_pmReviews = 0;
    m_developerReviews = 0;
    m_censorReviews = 0;
    m_formatRetries = 0;
    m_planApproved = false;
    m_requestStarted = false;
    m_resumePoint = ResumePoint::Request;
    m_role = AgentRole::Pm;
    for (auto role : agentRoles)
    {
        activateContext(role, agentId(role) + ":task");
    }
    QString error;
    if (m_task.isEmpty() || !validConfiguration(m_configuration))
    {
        fail(QStringLiteral("Нужны задача, модели основных ролей, положительные лимиты и таймауты."));
        return;
    }
    if (!m_executor.configure(configuration.workspace, error))
    {
        fail(error);
        return;
    }
    m_configuration.workspace = m_executor.workspace();
    log(QStringLiteral("Новая задача %1. Семь ролей, контексты по этапам. Проект: %2").arg(m_id, m_configuration.workspace));
    sendTo(AgentRole::Researcher, context());
}
QJsonObject Orchestrator::currentStage() const
{
    return m_stageIndex >= 0 && m_stageIndex < m_stages.size() ? m_stages.at(m_stageIndex).toObject() : QJsonObject{};
}
QJsonArray Orchestrator::checks() const
{
    QJsonArray result;
    for (const auto &entry : m_tools)
    {
        const auto batch = entry.toObject();
        for (const auto &value : batch.value("results").toArray())
        {
            auto tool = value.toObject();
            if (tool.value("type").toString() != "run_command")
            {
                continue;
            }
            tool.insert("stage_id", batch.value("stage_id"));
            tool.insert("plan_revision", batch.value("plan_revision"));
            tool.insert("agent", batch.value("agent"));
            tool.insert("output", tool.value("output").toString().right(4096));
            result.append(tool);
        }
    }
    return result;
}
QJsonObject Orchestrator::context(bool allStages) const
{
    QJsonArray relevantChecks;
    for (const auto &value : checks())
    {
        const auto check = value.toObject();
        if (check.value("plan_revision").toInt() == m_planRevision &&
            (allStages || check.value("stage_id").toString() == currentStage().value("id").toString()))
        {
            relevantChecks.append(check);
        }
    }
    return {{"developer_directives", developerDirectives()},
            {"task", m_task},
            {"answers", m_answers},
            {"decomposition", m_plan},
            {"stages", m_stages},
            {"plan_revision", m_planRevision},
            {"current_stage", currentStage()},
            {"accepted_stages", m_accepted},
            {"workspace", m_configuration.workspace},
            {"host_os", QSysInfo::productType()},
            {"implementation", m_implementation},
            {"executed_checks", relevantChecks},
            {"project_research", m_research},
            {"max_research_turns", m_configuration.maxResearchTurns},
            {"max_diagnostic_turns", m_configuration.maxDiagnosticTurns}};
}
QJsonObject Orchestrator::contextRecord(AgentRole role) const
{
    const auto index = indexOf(role);
    auto record = m_contexts.value(m_contextKeys[index]).toObject();
    record.insert("context_id", m_contextKeys[index]);
    record.insert("agent", agentId(role));
    record.insert("messages", m_histories[index]);
    record.insert("calls", m_contextCalls[index]);
    return record;
}
void Orchestrator::activateContext(AgentRole role, const QString &key)
{
    const auto index = indexOf(role);
    if (m_contextKeys[index] == key)
    {
        return;
    }
    if (!m_contextKeys[index].isEmpty())
    {
        m_contexts.insert(m_contextKeys[index], contextRecord(role));
    }
    m_contextKeys[index] = key;
    if (m_contexts.contains(key))
    {
        const auto saved = m_contexts.value(key).toObject();
        m_histories[index] = saved.value("messages").toArray();
        m_contextCalls[index] = saved.value("calls").toInt();
    }
    else
    {
        m_histories[index] = {message("system", agentPrompt(role))};
        m_contextCalls[index] = 0;
        m_contexts.insert(key, QJsonObject{{"agent", agentId(role)}, {"stage_id", currentStage().value("id").toString()}, {"plan_revision", m_planRevision}});
    }
}
bool Orchestrator::beginDiagnosis(AgentRole returnRole, const QJsonObject &feedback, const QString &reason)
{
    const auto key = QString::number(m_planRevision) + ":" + currentStage().value("id").toString();
    auto &count = m_diagnosticCounts[key];
    if (count >= m_configuration.maxDiagnosticsPerStage || m_calls[indexOf(AgentRole::Diagnostician)] >= m_configuration.maxAgentTurns)
    {
        log(QStringLiteral("Лимит диагностики этапа исчерпан; исходная роль получает результаты без нового диагноста."));
        return false;
    }
    ++count;
    ++m_diagnosticSequence;
    m_diagnosticReturnRole = returnRole;
    m_diagnosticFeedback = feedback;
    activateContext(AgentRole::Diagnostician, "diagnostician:" + QString::number(m_diagnosticSequence));
    auto payload = context();
    payload.insert("event", "diagnose_failure");
    payload.insert("source_agent", agentId(returnRole));
    payload.insert("trigger", reason);
    payload.insert("source_feedback", feedback);
    log(QStringLiteral("Подключен диагност: %1. Обращение %2/%3 для этапа.").arg(reason).arg(count).arg(m_configuration.maxDiagnosticsPerStage));
    sendTo(AgentRole::Diagnostician, payload);
    return true;
}
void Orchestrator::finishDiagnosis(const QJsonObject &result)
{
    auto report = result;
    report.insert("context_id", m_contextKeys[indexOf(AgentRole::Diagnostician)]);
    report.insert("source_agent", agentId(m_diagnosticReturnRole));
    report.insert("stage_id", currentStage().value("id"));
    report.insert("plan_revision", m_planRevision);
    m_diagnostics.append(report);
    emit specialistReportReady(agentName(AgentRole::Diagnostician), result.value("result").toString());
    log(QStringLiteral("Диагност: %1").arg(result.value("summary").toString()));
    auto payload = m_diagnosticFeedback;
    payload.insert("diagnosis", report);
    m_diagnosticFeedback = {};
    sendTo(m_diagnosticReturnRole, payload);
}
void Orchestrator::sendTo(AgentRole role, const QJsonObject &payload)
{
    m_role = role;
    m_resumePoint = ResumePoint::Request;
    m_formatRetries = 0;
    m_generationRetries = 0;
    m_disableReasoningOnRetry = false;
    m_histories[indexOf(role)].append(message("user", jsonText(payload)));
    sendCurrent();
}
void Orchestrator::sendCurrent()
{
    auto &count = m_calls[indexOf(m_role)];
    const int limit = m_role == AgentRole::Pm ? m_configuration.maxPmTurns : m_configuration.maxAgentTurns;
    if (m_role == AgentRole::Diagnostician && (m_contextCalls[indexOf(m_role)] >= m_configuration.maxDiagnosticTurns || count >= limit))
    {
        finishDiagnosis(
            {{"status", "incomplete"},
             {"summary", QStringLiteral("Достигнут лимит диагностики.")},
             {"result", QStringLiteral("Диагностика не завершена. Не считайте причину установленной; используйте исходную ошибку и доступные факты.")}});
        return;
    }
    if (count >= limit || (m_role == AgentRole::Researcher && m_contextCalls[indexOf(m_role)] >= m_configuration.maxResearchTurns))
    {
        log(QStringLiteral("Достигнут лимит запросов роли %1. Контексты сохранены.").arg(agentName(m_role)));
        setState(State::LimitReached);
        return;
    }
    ++count;
    ++m_contextCalls[indexOf(m_role)];
    m_requestStarted = true;
    log(QStringLiteral("%1: запрос %2/%3, контекст %4 сообщений.").arg(agentName(m_role)).arg(count).arg(limit).arg(m_histories[indexOf(m_role)].size()));
    setState(workingState(m_role));
    m_client.complete(model(m_role), m_histories[indexOf(m_role)], agentRequestSchema(m_role, m_histories[indexOf(m_role)]), agentId(m_role) + "_result",
                      m_disableReasoningOnRetry);
}
bool Orchestrator::submitAnswers(const QJsonObject &answers, QString &error)
{
    if (m_state != State::AwaitingAnswers)
    {
        error = QStringLiteral("Сейчас опросник не ожидает ответов.");
        return false;
    }
    QJsonArray batch;
    for (const auto &value : m_questions)
    {
        const auto question = value.toObject();
        const auto id = question.value("id").toString();
        if (!answers.value(id).isString() || answers.value(id).toString().trimmed().isEmpty())
        {
            error = QStringLiteral("Ответьте на каждый вопрос. Можно написать «не знаю»: %1").arg(question.value("text").toString());
            return false;
        }
        batch.append(
            QJsonObject{{"id", id}, {"agent", agentId(m_role)}, {"question", question.value("text")}, {"answer", answers.value(id).toString().trimmed()}});
    }
    for (const auto &answer : batch)
    {
        m_answers.append(answer);
    }
    m_questions = {};
    m_stagnantToolBatches.remove(toolGuardScope());
    log(QStringLiteral("Получено ответов: %1.").arg(batch.size()));
    sendTo(m_role, {{"event", "user_answers"}, {"answers", batch}});
    return true;
}
bool Orchestrator::canRetry() const
{
    return m_requestStarted && (m_state == State::Failed || m_state == State::LimitReached);
}
bool Orchestrator::retry(const Configuration &configuration, QString &error)
{
    if (!canRetry())
    {
        error = QStringLiteral("Нет прерванного запроса для повтора.");
        return false;
    }
    if (!validConfiguration(configuration) || QDir::cleanPath(QDir::fromNativeSeparators(configuration.workspace)) != m_configuration.workspace)
    {
        error = QStringLiteral("Для повтора сохраните рабочую папку, укажите модели основных ролей и положительные лимиты.");
        return false;
    }
    const bool resumeLimit = m_state == State::LimitReached;
    if (resumeLimit)
    {
        bool sufficient = true;
        if (m_resumePoint == ResumePoint::Request)
        {
            const int limit = m_role == AgentRole::Pm ? configuration.maxPmTurns : configuration.maxAgentTurns;
            sufficient =
                m_calls[indexOf(m_role)] < limit && (m_role != AgentRole::Researcher || m_contextCalls[indexOf(m_role)] < configuration.maxResearchTurns);
        }
        else
        {
            const int reviews =
                m_resumePoint == ResumePoint::PmReview ? m_pmReviews : (m_resumePoint == ResumePoint::DeveloperReview ? m_developerReviews : m_censorReviews);
            sufficient = reviews < configuration.maxReviews;
        }
        if (!sufficient)
        {
            error = QStringLiteral("Увеличьте исчерпанный лимит перед продолжением задачи.");
            return false;
        }
    }
    m_client.cancel();
    m_configuration = configuration;
    m_configuration.workspace = m_executor.workspace();
    m_stagnantToolBatches.remove(toolGuardScope());
    m_client.configure(configuration);
    m_error.clear();
    m_formatRetries = 0;
    m_generationRetries = 0;
    m_disableReasoningOnRetry = false;
    if (resumeLimit)
    {
        log(QStringLiteral("Продолжение задачи после увеличения лимита, контексты сохранены."));
        switch (m_resumePoint)
        {
        case ResumePoint::Request:
            sendCurrent();
            break;
        case ResumePoint::PmReview:
            askPmCritic();
            break;
        case ResumePoint::DeveloperReview:
            askDeveloperCritic();
            break;
        case ResumePoint::CensorReview:
            askCensor();
            break;
        }
        return true;
    }
    auto &history = m_histories[indexOf(m_role)];
    if (history.last().toObject().value("role").toString() == "assistant")
    {
        history.append(message("user", QStringLiteral("Повтори полный ответ по схеме, исправив предыдущую ошибку формата.")));
    }
    log(QStringLiteral("Повтор запроса %1 в текущем контексте.").arg(agentName(m_role)));
    sendCurrent();
    return true;
}
void Orchestrator::formatError(const QString &error)
{
    if (m_formatRetries >= 2)
    {
        fail(QStringLiteral("Модель трижды нарушила протокол: %1").arg(error));
        return;
    }
    ++m_formatRetries;
    auto &history = m_histories[indexOf(m_role)];
    auto payload = QJsonDocument::fromJson(history.last().toObject().value("content").toString().toUtf8()).object();
    payload.insert("protocol_repair", QJsonObject{{"error", error},
                                                  {"attempt", m_formatRetries},
                                                  {"instruction", QStringLiteral("Верни новый ответ по схеме текущей роли. Некорректный ответ отклонен, "
                                                                                 "операции из него не выполнены. Не повторяй прежнюю ошибку.")}});
    history[history.size() - 1] = message("user", jsonText(payload));
    log(QStringLiteral("%1: исправление формата %2/2. %3").arg(agentName(m_role)).arg(m_formatRetries).arg(error));
    sendCurrent();
}
void Orchestrator::onCompletion(const QString &content)
{
    if (!active() || m_state == State::AwaitingAnswers || m_state == State::ExecutingTools)
    {
        return;
    }
    auto &history = m_histories[indexOf(m_role)];
    m_exchanges.append(QJsonObject{{"agent", agentId(m_role)},
                                   {"context_id", m_contextKeys[indexOf(m_role)]},
                                   {"time", now()},
                                   {"request_message_count", history.size()},
                                   {"response", content}});
    QJsonObject result;
    QString error;
    if (!parseAgentResponse(content, m_role, result, error))
    {
        formatError(error);
        return;
    }
    if (m_role == AgentRole::Censor && result.value("status").toString() == "ready" && result.value("return_to").toString() == "developer")
    {
        bool known = false;
        for (const auto &stage : m_stages)
        {
            known |= stage.toObject().value("id") == result.value("stage_id");
        }
        if (!known)
        {
            formatError(QStringLiteral("Цензор указал неизвестный stage_id."));
            return;
        }
    }
    auto contextual = result;
    auto operations = contextual.value("actions").toArray();
    for (qsizetype i = 0; i < operations.size(); ++i)
    {
        auto operation = operations[i].toObject();
        const auto type = operation.value("type").toString();
        if (type != "write_file")
        {
            operation.remove("content");
        }
        if (type != "run_command")
        {
            operation.remove("program");
            operation.remove("arguments");
        }
        operations[i] = operation;
    }
    if (contextual.contains("actions"))
    {
        contextual.insert("actions", operations);
    }
    history.append(message("assistant", jsonText(contextual)));
    m_formatRetries = 0;
    const auto status = result.value("status").toString();
    if (status == "actions")
    {
        const auto actions = result.value("actions").toArray();
        if (blockRepeatedActions(actions))
        {
            return;
        }
        m_pendingActions = actions;
        setState(State::ExecutingTools);
        m_executor.execute(actions, m_role == AgentRole::Developer, m_configuration.commandTimeoutMs);
        return;
    }
    if (status == "questions")
    {
        if (m_role == AgentRole::Pm && !result.value("decomposition").toString().trimmed().isEmpty())
        {
            m_plan = result.value("decomposition").toString();
            emit planChanged(m_plan);
        }
        m_questions = result.value("questions").toArray();
        setState(State::AwaitingAnswers);
        emit questionsReady(m_questions, agentName(m_role) + ": " + result.value("summary").toString());
        return;
    }
    if (m_role == AgentRole::Researcher)
    {
        m_research = result;
        emit specialistReportReady(agentName(m_role), result.value("result").toString());
        log(QStringLiteral("Исследование проекта завершено."));
        sendTo(AgentRole::Pm, context());
    }
    else if (m_role == AgentRole::Diagnostician)
    {
        finishDiagnosis(result);
    }
    else if (m_role == AgentRole::Pm)
    {
        m_plan = result.value("decomposition").toString();
        m_stages = result.value("stages").toArray();
        ++m_planRevision;
        m_planApproved = false;
        m_accepted = {};
        m_implementation.clear();
        m_stageIndex = 0;
        m_plans.append(QJsonObject{{"revision", m_planRevision}, {"decomposition", m_plan}, {"stages", m_stages}});
        emit planChanged(m_plan);
        emit implementationChanged(implementationMarkdown());
        askPmCritic();
    }
    else if (m_role == AgentRole::Developer)
    {
        m_implementation = result.value("result").toString();
        emit implementationChanged(implementationMarkdown());
        askDeveloperCritic();
    }
    else
    {
        handleReview(result);
    }
}
QString Orchestrator::toolGuardScope() const
{
    return m_contextKeys[indexOf(m_role)] + ":" + QString::number(m_workspaceRevision);
}
bool Orchestrator::blockRepeatedActions(const QJsonArray &actions)
{
    const auto scope = toolGuardScope();
    if (m_stagnantToolBatches.value(scope) < 2)
    {
        return false;
    }
    for (const auto &value : actions)
    {
        const auto action = value.toObject();
        const auto previous = m_toolObservations.value(scope + ":" + actionKey(action)).toObject();
        if (previous.isEmpty())
        {
            return false;
        }
        const auto type = action.value("type").toString();
        if (type != "run_command")
        {
            const auto fingerprint = m_executor.observationFingerprint(action);
            if (fingerprint.isEmpty() || fingerprint != previous.value("observation").toString())
            {
                return false;
            }
        }
    }
    const QJsonObject feedback{{"event", "tool_loop_blocked"},
                               {"actions", actions},
                               {"reason", QStringLiteral("Повтор уже выполненных операций после предупреждения, без новых данных.")},
                               {"instruction", QStringLiteral("Используй полученные результаты. Выбери другое необходимое действие либо верни status=ready; "
                                                              "не объявляй задачу выполненной без доказательств.")}};
    m_histories[indexOf(m_role)].append(message("user", jsonText(feedback)));
    m_toolLoopEvents.append(
        QJsonObject{{"agent", agentId(m_role)}, {"context_id", m_contextKeys[indexOf(m_role)]}, {"time", now()}, {"blocked", true}, {"feedback", feedback}});
    fail(QStringLiteral("%1: остановлен цикл инструментов. Повторные операции не выполнены. "
                        "Контекст сохранен; после проверки причины доступен ручной повтор.")
             .arg(agentName(m_role)));
    return true;
}
QJsonObject Orchestrator::toolFeedback(const QJsonArray &results)
{
    for (const auto &value : results)
    {
        const auto result = value.toObject();
        if (result.value("type").toString() == "write_file" && result.value("success").toBool() && result.value("changed").toBool())
        {
            ++m_workspaceRevision;
        }
    }
    const auto scope = toolGuardScope();
    const auto messageIndex = m_histories[indexOf(m_role)].size();
    bool newInformation = false;
    QJsonArray compact;
    for (qsizetype i = 0; i < results.size(); ++i)
    {
        const auto result = results[i].toObject();
        const auto key = scope + ":" + actionKey(m_pendingActions.at(i).toObject());
        auto stable = result;
        stable.remove("content");
        stable.remove("changed");
        stable.remove("path");
        const auto fingerprint = digest(stable);
        const auto previous = m_toolObservations.value(key).toObject();
        const bool unchanged = !previous.isEmpty() && previous.value("fingerprint").toString() == fingerprint;
        newInformation |= !unchanged;
        auto delivered = result;
        if (unchanged && result.value("success").toBool() &&
            (result.value("type").toString() == "read_file" || result.value("type").toString() == "list_files"))
        {
            delivered.remove("content");
            delivered.remove("entries");
            delivered.insert("unchanged", true);
            delivered.insert("content_omitted", true);
            delivered.insert("source_context", m_contextKeys[indexOf(m_role)]);
            delivered.insert("source_message", previous.value("source_message"));
            delivered.insert("note", QStringLiteral("Данные не изменились. Полный результат уже находится в указанном сообщении этого контекста."));
        }
        compact.append(delivered);
        QString observation;
        const auto type = result.value("type").toString();
        if (result.value("success").toBool())
        {
            if (type == "read_file" || type == "write_file")
            {
                observation = result.value("sha256").toString();
            }
            else if (type == "list_files")
            {
                observation = digest({{"entries", result.value("entries")}, {"truncated", result.value("truncated")}});
            }
        }
        m_toolObservations.insert(key, QJsonObject{{"fingerprint", fingerprint},
                                                   {"observation", observation},
                                                   {"source_message", unchanged ? previous.value("source_message") : QJsonValue(messageIndex)}});
    }
    m_pendingActions = {};
    auto &stagnant = m_stagnantToolBatches[scope];
    stagnant = newInformation ? 0 : stagnant + 1;
    QJsonObject feedback{{"event", "tool_results"}, {"tool_results", compact}};
    if (stagnant >= 2)
    {
        const QJsonObject control{
            {"event", stagnant >= 3 ? "tool_loop_blocked" : "tool_loop_warning"},
            {"blocked", stagnant >= 3},
            {"stagnant_batches", stagnant},
            {"instruction", QStringLiteral("Два последовательных пакета операций не дали новых данных. Не повторяй их. "
                                           "Используй уже прочитанное, выбери необходимое новое действие либо заверши отчет status=ready.")}};
        feedback.insert("loop_control", control);
        m_toolLoopEvents.append(QJsonObject{
            {"agent", agentId(m_role)}, {"context_id", m_contextKeys[indexOf(m_role)]}, {"time", now()}, {"blocked", stagnant >= 3}, {"feedback", control}});
        log(agentName(m_role) + QStringLiteral(": повторные операции без новых данных; отправлено предупреждение перед блокировкой цикла."));
    }
    return feedback;
}
void Orchestrator::onToolsFinished(const QJsonArray &results)
{
    if (m_state != State::ExecutingTools)
    {
        return;
    }
    m_tools.append(QJsonObject{{"agent", agentId(m_role)},
                               {"context_id", m_contextKeys[indexOf(m_role)]},
                               {"stage_id", currentStage().value("id")},
                               {"plan_revision", m_planRevision},
                               {"time", now()},
                               {"results", results}});
    emit toolResultsReady(results);
    const auto feedback = toolFeedback(results);
    if (feedback.value("loop_control").toObject().value("blocked").toBool())
    {
        m_histories[indexOf(m_role)].append(message("user", jsonText(feedback)));
        fail(QStringLiteral("%1: остановлен цикл инструментов после повторных результатов без новых данных. "
                            "Контекст сохранен; доступен ручной повтор.")
                 .arg(agentName(m_role)));
        return;
    }
    bool failed = false;
    bool commandFailed = false;
    for (const auto &value : results)
    {
        const auto result = value.toObject();
        if (!result.value("success").toBool())
        {
            failed = true;
            commandFailed |= result.value("type").toString() == "run_command";
        }
    }
    if (m_role == AgentRole::Developer || m_role == AgentRole::DeveloperCritic || m_role == AgentRole::Censor)
    {
        auto &failures = m_failedActions[indexOf(m_role)];
        failures = failed ? failures + 1 : 0;
        if (!feedback.contains("loop_control") && (commandFailed || failures >= 2) &&
            beginDiagnosis(m_role, feedback, commandFailed ? QStringLiteral("Ошибка команды") : QStringLiteral("Повторный сбой инструмента")))
        {
            failures = 0;
            return;
        }
    }
    sendTo(m_role, feedback);
}
void Orchestrator::askPmCritic()
{
    if (m_pmReviews >= m_configuration.maxReviews)
    {
        m_resumePoint = ResumePoint::PmReview;
        log(QStringLiteral("Достигнут лимит проверок плана. Увеличьте лимит и нажмите «Продолжить после лимита»."));
        setState(State::LimitReached);
        return;
    }
    ++m_pmReviews;
    sendTo(AgentRole::PmCritic, context());
}
void Orchestrator::beginStage(int index, const QJsonObject &feedback)
{
    m_stageIndex = index;
    m_developerReviews = 0;
    m_failedActions.fill(0);
    const auto suffix = ":" + QString::number(m_planRevision) + ":" + currentStage().value("id").toString();
    activateContext(AgentRole::Developer, agentId(AgentRole::Developer) + suffix);
    activateContext(AgentRole::DeveloperCritic, agentId(AgentRole::DeveloperCritic) + suffix);
    m_implementation.clear();
    emit stageChanged(index + 1, m_stages.size(), currentStage().value("title").toString());
    log(QStringLiteral("Реализация этапа %1/%2: %3").arg(index + 1).arg(m_stages.size()).arg(currentStage().value("title").toString()));
    auto payload = context();
    payload.insert("event", "implement_stage");
    if (!feedback.isEmpty())
    {
        payload.insert("feedback", feedback);
    }
    sendTo(AgentRole::Developer, payload);
}
void Orchestrator::askDeveloperCritic()
{
    if (m_developerReviews >= m_configuration.maxReviews)
    {
        m_resumePoint = ResumePoint::DeveloperReview;
        log(QStringLiteral("Достигнут лимит проверок этапа. Увеличьте лимит и нажмите «Продолжить после лимита»."));
        setState(State::LimitReached);
        return;
    }
    ++m_developerReviews;
    sendTo(AgentRole::DeveloperCritic, context());
}
void Orchestrator::askCensor()
{
    if (m_censorReviews >= m_configuration.maxReviews)
    {
        m_resumePoint = ResumePoint::CensorReview;
        log(QStringLiteral("Достигнут лимит финального ревью. Увеличьте лимит и нажмите «Продолжить после лимита»."));
        setState(State::LimitReached);
        return;
    }
    ++m_censorReviews;
    sendTo(AgentRole::Censor, context(true));
}
void Orchestrator::handleReview(const QJsonObject &review)
{
    const bool approved = review.value("verdict").toString() == "approved";
    const int round = m_role == AgentRole::PmCritic ? m_pmReviews : (m_role == AgentRole::DeveloperCritic ? m_developerReviews : m_censorReviews);
    m_reviews.append(QJsonObject{
        {"agent", agentId(m_role)}, {"round", round}, {"plan_revision", m_planRevision}, {"stage_id", currentStage().value("id")}, {"review", review}});
    emit roleReviewReady(agentName(m_role), review, round);
    log(agentName(m_role) + ": " + review.value("summary").toString());
    if (m_role == AgentRole::PmCritic)
    {
        emit reviewReady(review, round);
        if (approved)
        {
            m_planApproved = true;
            beginStage(0);
        }
        else
        {
            auto payload = context();
            payload.insert("pm_critic_feedback", review);
            sendTo(AgentRole::Pm, payload);
        }
        return;
    }
    if (m_role == AgentRole::DeveloperCritic)
    {
        if (!approved)
        {
            auto payload = context();
            payload.insert("developer_critic_feedback", review);
            if (m_developerReviews >= 2 && beginDiagnosis(AgentRole::Developer, payload, QStringLiteral("Повторные блокирующие замечания к этапу")))
            {
                return;
            }
            sendTo(AgentRole::Developer, payload);
            return;
        }
        const QJsonObject accepted{{"stage", currentStage()}, {"plan_revision", m_planRevision}, {"implementation", m_implementation}, {"review", review}};
        m_accepted.append(accepted);
        m_stageHistory.append(accepted);
        emit implementationChanged(implementationMarkdown());
        if (m_stageIndex + 1 < m_stages.size())
        {
            beginStage(m_stageIndex + 1);
        }
        else
        {
            askCensor();
        }
        return;
    }
    if (approved)
    {
        log(QStringLiteral("Цензор одобрил итог. Задача завершена."));
        setState(State::Completed);
    }
    else if (review.value("return_to").toString() == "pm")
    {
        m_planApproved = false;
        auto payload = context();
        payload.insert("censor_feedback", review);
        sendTo(AgentRole::Pm, payload);
    }
    else
    {
        int target = 0;
        while (m_stages.at(target).toObject().value("id") != review.value("stage_id"))
        {
            ++target;
        }
        while (m_accepted.size() > target)
        {
            m_accepted.removeLast();
        }
        beginStage(target, review);
    }
}
void Orchestrator::stop()
{
    if (!active())
    {
        return;
    }
    m_client.cancel();
    m_executor.cancel();
    log(QStringLiteral("Задача остановлена. Контексты и промежуточные результаты сохранены."));
    setState(State::Stopped);
}
Orchestrator::State Orchestrator::state() const
{
    return m_state;
}
bool Orchestrator::active() const
{
    return m_state == State::ResearcherWorking || m_state == State::DiagnosticianWorking || m_state == State::PmWorking || m_state == State::CriticWorking ||
           m_state == State::DeveloperWorking || m_state == State::DeveloperCriticWorking || m_state == State::CensorWorking ||
           m_state == State::ExecutingTools || m_state == State::AwaitingAnswers;
}
void Orchestrator::setState(State state)
{
    m_state = state;
    emit stateChanged(state);
    emit changed();
}
void Orchestrator::fail(const QString &error)
{
    m_client.cancel();
    m_executor.cancel();
    m_error = error;
    log(error);
    setState(State::Failed);
    emit failed(error);
}
void Orchestrator::log(const QString &text)
{
    m_events.append(QJsonObject{{"time", now()}, {"text", text}});
    spdlog::info("{}", text.toUtf8().constData());
    emit eventOccurred(text);
}
QString Orchestrator::stateName(State state)
{
    switch (state)
    {
    case State::Idle:
        return QStringLiteral("Готов к работе");
    case State::PmWorking:
        return QStringLiteral("PM анализирует задачу");
    case State::ResearcherWorking:
        return QStringLiteral("Исследователь изучает проект");
    case State::DiagnosticianWorking:
        return QStringLiteral("Диагност разбирает сбой");
    case State::AwaitingAnswers:
        return QStringLiteral("Ожидаются ответы пользователя");
    case State::CriticWorking:
        return QStringLiteral("PM-критик проверяет план");
    case State::DeveloperWorking:
        return QStringLiteral("Разработчик реализует этап");
    case State::DeveloperCriticWorking:
        return QStringLiteral("Критик проверяет реализацию");
    case State::CensorWorking:
        return QStringLiteral("Цензор выполняет финальное ревью");
    case State::ExecutingTools:
        return QStringLiteral("Выполняются операции с проектом");
    case State::Completed:
        return QStringLiteral("Задача завершена — цензор одобрил");
    case State::Stopped:
        return QStringLiteral("Остановлено — промежуточный результат");
    case State::Failed:
        return QStringLiteral("Ошибка — результат не одобрен");
    case State::LimitReached:
        return QStringLiteral("Достигнут лимит — результат не одобрен");
    }
    return {};
}
QJsonObject Orchestrator::snapshot() const
{
    QJsonObject agents;
    auto contexts = m_contexts;
    for (auto role : agentRoles)
    {
        auto record = contextRecord(role);
        if (!m_contextKeys[indexOf(role)].isEmpty())
        {
            contexts.insert(m_contextKeys[indexOf(role)], record);
        }
        record.insert("model", model(role));
        record.insert("total_calls", m_calls[indexOf(role)]);
        agents.insert(agentId(role), record);
    }
    return {{"schema_version", 3},
            {"id", m_id},
            {"created_at", m_createdAt},
            {"updated_at", now()},
            {"status", QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(static_cast<int>(m_state)))},
            {"active_agent", agentId(m_role)},
            {"resume_point", static_cast<int>(m_resumePoint)},
            {"approved", m_state == State::Completed},
            {"plan_approved", m_planApproved},
            {"developer_directives", developerDirectives()},
            {"task", m_task},
            {"decomposition", m_plan},
            {"implementation", m_implementation},
            {"stage_index", m_stageIndex},
            {"plan_revision", m_planRevision},
            {"stages", m_stages},
            {"accepted_stages", m_accepted},
            {"stage_history", m_stageHistory},
            {"pending_questions", m_questions},
            {"answers", m_answers},
            {"agents", agents},
            {"contexts", contexts},
            {"project_research", m_research},
            {"diagnostics", m_diagnostics},
            {"generation_failures", m_generationFailures},
            {"tool_loop_events", m_toolLoopEvents},
            {"workspace_revision", static_cast<qint64>(m_workspaceRevision)},
            {"reviews", m_reviews},
            {"plans", m_plans},
            {"exchanges", m_exchanges},
            {"events", m_events},
            {"tools", m_tools},
            {"error", m_error},
            {"configuration", QJsonObject{{"base_url", LmStudioClient::normalizedBaseUrl(m_configuration.baseUrl).toString()},
                                          {"workspace", m_configuration.workspace},
                                          {"host_os", QSysInfo::productType()},
                                          {"pm_model", m_configuration.pmModel},
                                          {"critic_model", m_configuration.criticModel},
                                          {"developer_model", m_configuration.developerModel},
                                          {"developer_critic_model", m_configuration.developerCriticModel},
                                          {"censor_model", m_configuration.censorModel},
                                          {"researcher_model", model(AgentRole::Researcher)},
                                          {"diagnostician_model", model(AgentRole::Diagnostician)},
                                          {"max_research_turns", m_configuration.maxResearchTurns},
                                          {"max_diagnostic_turns", m_configuration.maxDiagnosticTurns},
                                          {"max_diagnostics_per_stage", m_configuration.maxDiagnosticsPerStage},
                                          {"timeout_ms", m_configuration.timeoutMs},
                                          {"command_timeout_ms", m_configuration.commandTimeoutMs},
                                          {"max_tokens", m_configuration.maxTokens},
                                          {"max_service_tokens", m_configuration.maxServiceTokens},
                                          {"max_reasoning_characters", m_configuration.maxReasoningCharacters},
                                          {"max_pm_turns", m_configuration.maxPmTurns},
                                          {"max_agent_turns", m_configuration.maxAgentTurns},
                                          {"max_reviews", m_configuration.maxReviews},
                                          {"structured_output", m_configuration.structuredOutput},
                                          {"sampling_profile", static_cast<int>(m_configuration.samplingProfile)},
                                          {"reasoning_mode", static_cast<int>(m_configuration.reasoningMode)}}}};
}
QString Orchestrator::implementationMarkdown() const
{
    QString result;
    for (const auto &value : m_accepted)
    {
        const auto stage = value.toObject();
        result += QStringLiteral("## %1 — принято\n\n%2\n\n")
                      .arg(stage.value("stage").toObject().value("title").toString(), stage.value("implementation").toString());
    }
    if (!m_implementation.isEmpty() && m_accepted.size() <= m_stageIndex)
    {
        result += QStringLiteral("## Текущий этап: %1\n\n%2\n").arg(currentStage().value("title").toString(), m_implementation);
    }
    return result;
}
QString Orchestrator::markdown() const
{
    QString result = QStringLiteral("# ORKestrator — %1\n\nСессия: %2\n\nСтатус: %3\n\nПроект: %4\n\n## Задача\n\n%5\n\n")
                         .arg(m_state == State::Completed ? QStringLiteral("результат") : QStringLiteral("промежуточный результат"), m_id, stateName(m_state),
                              m_configuration.workspace, m_task);
    for (const auto &value : m_answers)
    {
        const auto a = value.toObject();
        result += QStringLiteral("**%1**\n\n%2\n\n").arg(a.value("question").toString(), a.value("answer").toString());
    }
    result += QStringLiteral("## Исследование проекта\n\n%1\n\n").arg(m_research.value("result").toString());
    for (const auto &value : m_diagnostics)
    {
        const auto diagnostic = value.toObject();
        result += QStringLiteral("## Диагностика этапа %1\n\n%2\n\n").arg(diagnostic.value("stage_id").toString(), diagnostic.value("result").toString());
    }
    result += QStringLiteral("## План\n\n%1\n\n## Реализация\n\n%2\n\n## Проверки и ревью\n\n").arg(m_plan, implementationMarkdown());
    for (const auto &value : m_reviews)
    {
        const auto r = value.toObject();
        result += QStringLiteral("### %1 / %2 / редакция %3\n\n%4\n")
                      .arg(r.value("agent").toString(), r.value("stage_id").toString())
                      .arg(r.value("plan_revision").toInt())
                      .arg(reviewMarkdown(r.value("review").toObject()));
    }
    for (const auto &value : checks())
    {
        const auto c = value.toObject();
        result += QStringLiteral("\n### Команда: %1 (этап %2)\n\nКод: %3\n\n~~~text\n%4\n~~~\n")
                      .arg(c.value("program").toString(), c.value("stage_id").toString())
                      .arg(c.value("exit_code").toInt(-1))
                      .arg(c.value("output").toString());
    }
    return result;
}
} // namespace orchestrator