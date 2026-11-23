#include "orchestrator.h"
#include <QDateTime>
#include <QDir>
#include <QMetaEnum>
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
    }
    return Orchestrator::State::Failed;
}
bool validConfiguration(const Configuration &c)
{
    return !c.pmModel.trimmed().isEmpty() && !c.criticModel.trimmed().isEmpty() && !c.developerModel.trimmed().isEmpty() &&
           !c.developerCriticModel.trimmed().isEmpty() && !c.censorModel.trimmed().isEmpty() && c.timeoutMs > 0 && c.commandTimeoutMs > 0 && c.maxTokens > 0 &&
           c.maxPmTurns > 0 && c.maxAgentTurns > 0 && c.maxReviews > 0;
}
} // namespace
Orchestrator::Orchestrator(QObject *parent) : QObject(parent), m_executor(this)
{
    connect(&m_client, &LmStudioClient::completionReady, this, &Orchestrator::onCompletion);
    connect(&m_client, &LmStudioClient::failed, this, &Orchestrator::fail);
    connect(&m_executor, &TaskExecutor::finished, this, &Orchestrator::onToolsFinished);
    connect(&m_executor, &TaskExecutor::activity, this, [this](const QString &s) { log(agentName(m_role) + ": " + s); });
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
    m_calls.fill(0);
    m_stageIndex = 0;
    m_planRevision = 0;
    m_pmReviews = 0;
    m_developerReviews = 0;
    m_censorReviews = 0;
    m_formatRetries = 0;
    m_planApproved = false;
    m_requestStarted = false;
    m_role = AgentRole::Pm;
    // Initialized once per task. Stage changes and revisions never reset these histories.
    for (auto role : agentRoles)
    {
        m_histories[indexOf(role)] = {message("system", agentPrompt(role))};
    }
    QString error;
    if (m_task.isEmpty() || !validConfiguration(m_configuration))
    {
        fail(QStringLiteral("Нужны задача, модели пяти ролей, положительные лимиты и таймауты."));
        return;
    }
    if (!m_executor.configure(configuration.workspace, error))
    {
        fail(error);
        return;
    }
    m_configuration.workspace = m_executor.workspace();
    log(QStringLiteral("Новая задача %1. Пять постоянных контекстов. Проект: %2").arg(m_id, m_configuration.workspace));
    sendTo(AgentRole::Pm, context());
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
QJsonObject Orchestrator::context() const
{
    return {{"developer_directives", developerDirectives()},
            {"task", m_task},
            {"answers", m_answers},
            {"decomposition", m_plan},
            {"stages", m_stages},
            {"plan_revision", m_planRevision},
            {"current_stage", currentStage()},
            {"accepted_stages", m_accepted},
            {"workspace", m_configuration.workspace},
            {"implementation", m_implementation},
            {"executed_checks", checks()}};
}
void Orchestrator::sendTo(AgentRole role, const QJsonObject &payload)
{
    m_role = role;
    m_formatRetries = 0;
    m_histories[indexOf(role)].append(message("user", jsonText(payload)));
    sendCurrent();
}
void Orchestrator::sendCurrent()
{
    auto &count = m_calls[indexOf(m_role)];
    const int limit = m_role == AgentRole::Pm ? m_configuration.maxPmTurns : m_configuration.maxAgentTurns;
    if (count >= limit)
    {
        log(QStringLiteral("Достигнут лимит запросов роли %1. Контексты сохранены.").arg(agentName(m_role)));
        setState(State::LimitReached);
        return;
    }
    ++count;
    m_requestStarted = true;
    log(QStringLiteral("%1: запрос %2/%3, контекст %4 сообщений.").arg(agentName(m_role)).arg(count).arg(limit).arg(m_histories[indexOf(m_role)].size()));
    setState(workingState(m_role));
    m_client.complete(model(m_role), m_histories[indexOf(m_role)], agentSchema(m_role), agentId(m_role) + "_result");
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
    log(QStringLiteral("Получено ответов: %1.").arg(batch.size()));
    sendTo(m_role, {{"event", "user_answers"}, {"answers", batch}});
    return true;
}
bool Orchestrator::canRetry() const
{
    return m_requestStarted && m_state == State::Failed;
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
        error = QStringLiteral("Для повтора сохраните рабочую папку, укажите пять моделей и положительные лимиты.");
        return false;
    }
    m_client.cancel();
    m_configuration = configuration;
    m_configuration.workspace = m_executor.workspace();
    m_client.configure(configuration);
    m_error.clear();
    m_formatRetries = 0;
    auto &history = m_histories[indexOf(m_role)];
    if (history.last().toObject().value("role").toString() == "assistant")
    {
        history.append(message("user", QStringLiteral("Повтори полный ответ по схеме, исправив предыдущую ошибку формата.")));
    }
    log(QStringLiteral("Повтор запроса %1 без сброса пяти контекстов.").arg(agentName(m_role)));
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
    m_histories[indexOf(m_role)].append(message("user", QStringLiteral("Исправь JSON: %1. Верни полный объект по схеме.").arg(error)));
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
    m_exchanges.append(QJsonObject{{"agent", agentId(m_role)}, {"time", now()}, {"request_message_count", history.size()}, {"response", content}});
    history.append(message("assistant", content));
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
    m_formatRetries = 0;
    const auto status = result.value("status").toString();
    if (status == "actions")
    {
        setState(State::ExecutingTools);
        m_executor.execute(result.value("actions").toArray(), m_role == AgentRole::Developer, m_configuration.commandTimeoutMs);
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
    if (m_role == AgentRole::Pm)
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
void Orchestrator::onToolsFinished(const QJsonArray &results)
{
    if (m_state != State::ExecutingTools)
    {
        return;
    }
    m_tools.append(QJsonObject{
        {"agent", agentId(m_role)}, {"stage_id", currentStage().value("id")}, {"plan_revision", m_planRevision}, {"time", now()}, {"results", results}});
    emit toolResultsReady(results);
    sendTo(m_role, {{"event", "tool_results"}, {"tool_results", results}});
}
void Orchestrator::askPmCritic()
{
    if (m_pmReviews >= m_configuration.maxReviews)
    {
        log(QStringLiteral("Достигнут лимит проверок плана."));
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
        log(QStringLiteral("Достигнут лимит проверок этапа."));
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
        log(QStringLiteral("Достигнут лимит финального ревью."));
        setState(State::LimitReached);
        return;
    }
    ++m_censorReviews;
    sendTo(AgentRole::Censor, context());
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
    return m_state == State::PmWorking || m_state == State::CriticWorking || m_state == State::DeveloperWorking || m_state == State::DeveloperCriticWorking ||
           m_state == State::CensorWorking || m_state == State::ExecutingTools || m_state == State::AwaitingAnswers;
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
    for (auto role : agentRoles)
    {
        agents.insert(agentId(role), QJsonObject{{"model", model(role)}, {"calls", m_calls[indexOf(role)]}, {"messages", m_histories[indexOf(role)]}});
    }
    return {{"schema_version", 2},
            {"id", m_id},
            {"created_at", m_createdAt},
            {"updated_at", now()},
            {"status", QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(static_cast<int>(m_state)))},
            {"active_agent", agentId(m_role)},
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
            {"reviews", m_reviews},
            {"plans", m_plans},
            {"exchanges", m_exchanges},
            {"events", m_events},
            {"tools", m_tools},
            {"error", m_error},
            {"configuration", QJsonObject{{"base_url", LmStudioClient::normalizedBaseUrl(m_configuration.baseUrl).toString()},
                                          {"workspace", m_configuration.workspace},
                                          {"pm_model", m_configuration.pmModel},
                                          {"critic_model", m_configuration.criticModel},
                                          {"developer_model", m_configuration.developerModel},
                                          {"developer_critic_model", m_configuration.developerCriticModel},
                                          {"censor_model", m_configuration.censorModel},
                                          {"timeout_ms", m_configuration.timeoutMs},
                                          {"command_timeout_ms", m_configuration.commandTimeoutMs},
                                          {"max_tokens", m_configuration.maxTokens},
                                          {"max_pm_turns", m_configuration.maxPmTurns},
                                          {"max_agent_turns", m_configuration.maxAgentTurns},
                                          {"max_reviews", m_configuration.maxReviews},
                                          {"structured_output", m_configuration.structuredOutput}}}};
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