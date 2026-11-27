#include "pipeline/pipelineexecutor.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSysInfo>
#include <QTimer>

namespace pipeline
{
namespace
{
QJsonObject message(const QString &role, const QString &content)
{
    return {{"role", role}, {"content", content}};
}
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
}
PipelineExecutor::PipelineExecutor(QObject *parent) : QObject(parent)
{
    connect(&m_client, &orchestrator::LmStudioClient::completionReady, this, &PipelineExecutor::received);
    connect(&m_client, &orchestrator::LmStudioClient::failed, this, &PipelineExecutor::fail);
    connect(&m_client, &orchestrator::LmStudioClient::generationLoopDetected, this, &PipelineExecutor::fail);
    connect(&m_client, &orchestrator::LmStudioClient::reasoningBudgetExceeded, this, &PipelineExecutor::fail);
    connect(&m_client, &orchestrator::LmStudioClient::generationProgress, this, &PipelineExecutor::progress);
    connect(&m_client, &orchestrator::LmStudioClient::generationConfigured, this,
            [this](int tokens, const QString &thinking) { log(QStringLiteral("До %1 токенов ответа; мышление: %2.").arg(tokens).arg(thinking)); });
}
void PipelineExecutor::setState(State state)
{
    m_state = state;
    emit stateChanged(state);
    emit changed();
}
void PipelineExecutor::log(const QString &text)
{
    m_events.append(QJsonObject{{"time", now()}, {"agent", m_current}, {"text", text}});
    emit eventOccurred(text);
}
bool PipelineExecutor::start(const Pipeline &graph, const QString &task, const orchestrator::Configuration &connection, QString &error)
{
    if (busy())
    {
        error = QStringLiteral("Сначала остановите выполнение.");
        return false;
    }
    error = graph.validate();
    if (!error.isEmpty())
        return false;
    if (task.trimmed().isEmpty())
    {
        error = QStringLiteral("Введите задачу.");
        return false;
    }
    for (const auto &agent : graph.agents)
        if (!agent.tools.isEmpty())
        {
            orchestrator::TaskExecutor probe;
            if (!probe.configure(connection.workspace, error))
                return false;
            break;
        }
    clear();
    m_graph = graph;
    m_connection = connection;
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_task = task.trimmed();
    log(QStringLiteral("Новая задача %1. Агентов: %2.").arg(m_id).arg(graph.agents.size()));
    visit(graph.start, {}, {});
    return true;
}
bool PipelineExecutor::updateGraph(const Pipeline &graph, QString &error)
{
    if (busy())
    {
        error = QStringLiteral("Нельзя менять граф во время выполнения.");
        return false;
    }
    error = graph.validate();
    if (!error.isEmpty())
        return false;
    if (!m_current.isEmpty() && !graph.find(m_current))
    {
        error = QStringLiteral("Текущий агент удален из графа. Начните новую задачу.");
        return false;
    }
    m_graph = graph;
    return true;
}
orchestrator::TaskExecutor *PipelineExecutor::executor(const Agent &agent, QString &error)
{
    auto *worker = m_executors.value(agent.id, nullptr);
    if (!worker)
    {
        worker = new orchestrator::TaskExecutor(this);
        if (!worker->configure(m_connection.workspace, error))
        {
            delete worker;
            return nullptr;
        }
        m_executors.insert(agent.id, worker);
        connect(worker, &orchestrator::TaskExecutor::finished, this, [this, id = agent.id](const QJsonArray &results) { toolsFinished(id, results); });
        connect(worker, &orchestrator::TaskExecutor::activity, this, &PipelineExecutor::log);
    }
    return worker;
}
void PipelineExecutor::visit(const QString &id, const QString &input, const QString &note, bool automatic)
{
    ++m_epoch;
    const auto *agent = m_graph.find(id);
    if (!agent)
    {
        fail(QStringLiteral("Агент отсутствует в графе."));
        return;
    }
    m_current = id;
    m_automaticSteps = automatic ? m_automaticSteps + 1 : 0;
    m_operations = 0;
    m_changedWrites = 0;
    m_repeats.clear();
    auto &history = m_contexts[id];
    if (agent->freshContext)
    {
        history = {};
        if (auto *worker = m_executors.take(id))
            delete worker;
    }
    if (history.isEmpty())
        history.append(message("system", agent->systemPrompt()));
    else
        history[0] = message("system", agent->systemPrompt());
    history.append(message("user", jsonText({{"task", m_task},
                                             {"instruction", agent->prompt},
                                             {"input", input},
                                             {"user_note", note},
                                             {"workspace", m_connection.workspace},
                                             {"host_os", QSysInfo::productType()}})));
    m_visits.append(QJsonObject{{"agent", id}, {"name", agent->name}, {"model", agent->model}, {"started", now()}, {"input", input}, {"note", note}});
    auto config = m_connection;
    config.maxTokens = agent->tokens;
    config.maxServiceTokens = 0;
    config.timeoutMs = agent->timeout * 1000;
    config.maxReasoningCharacters = agent->reasoningBudget;
    config.reasoningMode = agent->thinking;
    config.structuredOutput = true;
    m_client.configure(config);
    m_text.clear();
    emit outputChanged({});
    log(QStringLiteral("%1 (%2): вход в узел, контекст %3 сообщений.").arg(agent->name, agent->model).arg(history.size()));
    request();
}
void PipelineExecutor::request()
{
    const auto *agent = m_graph.find(m_current);
    if (!agent)
    {
        fail(QStringLiteral("Агент отсутствует."));
        return;
    }
    setState(State::Running);
    m_client.complete(agent->model, m_contexts.value(m_current), agent->responseSchema(), "agent_result");
}
void PipelineExecutor::received(const QString &content)
{
    if (m_state != State::Running)
        return;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(content.trimmed().toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        m_text = content;
        emit outputChanged(content);
        fail(QStringLiteral("Ответ агента не является JSON-объектом. Контекст сохранен; доступен повтор с уточнением."));
        return;
    }
    const auto result = document.object();
    const auto *agent = m_graph.find(m_current);
    if (!agent)
        return;
    const auto status = result.value("status").toString();
    if (status == "actions")
    {
        const auto actions = result.value("actions").toArray();
        if (actions.isEmpty() || actions.size() > 4)
        {
            fail(QStringLiteral("Нужно от 1 до 4 операций."));
            return;
        }
        for (const auto &entry : actions)
            if (!entry.isObject() || !agent->tools.contains(entry.toObject().value("type").toString()))
            {
                fail(QStringLiteral("Агент запросил неразрешенный инструмент."));
                return;
            }
        m_contexts[m_current].append(message("assistant", content));
        const auto signature = jsonText({{"actions", actions}});
        log(QStringLiteral("%1: %2. Операций использовано %3/%4, запрошено %5.")
                .arg(agent->name, result.value("summary").toString())
                .arg(m_operations)
                .arg(agent->toolLimit)
                .arg(actions.size()));
        QString reason;
        bool routeNegative = false;
        if (m_operations + actions.size() > agent->toolLimit)
            reason = QStringLiteral("Превышен лимит операций: %1 + %2 > %3.").arg(m_operations).arg(actions.size()).arg(agent->toolLimit);
        else if (++m_repeats[signature] >= 3)
        {
            reason = QStringLiteral("Один и тот же пакет операций запрошен в третий раз за посещение.");
            routeNegative = !m_graph.next(m_current, false).isEmpty();
        }
        if (!reason.isEmpty())
        {
            QStringList rejected;
            for (const auto &action : actions)
                rejected.append(orchestrator::TaskExecutor::describeAction(action.toObject()));
            reason += QStringLiteral(" Отклонённый пакет: %1.").arg(rejected.join(", "));
            m_contexts[m_current].append(message("user", jsonText({{"event", "tool_results"}, {"error", reason}, {"executed", false}})));
            if (routeNegative)
            {
                const auto handoff = reason + "\n" + result.value("summary").toString();
                complete({{"status", "ready"}, {"summary", reason}, {"content", handoff}, {agent->resultField, agent->negative}, {"actions", QJsonArray{}}}, true);
            }
            else
                complete({{"summary", reason}, {"content", reason}, {"partial", true}}, false);
            return;
        }
        QString error;
        auto *worker = executor(*agent, error);
        if (!worker)
        {
            fail(error);
            return;
        }
        m_operations += static_cast<int>(actions.size());
        setState(State::Tools);
        worker->execute(actions, agent->tools.contains("write_file"), agent->commandTimeout * 1000);
        return;
    }
    const auto value = result.value(agent->resultField).toString();
    if (status != "ready" || !result.value("content").isString() || !result.value("actions").isArray() || !result.value("actions").toArray().isEmpty() ||
        (value != agent->positive && value != agent->negative))
    {
        m_text = content;
        emit outputChanged(content);
        fail(QStringLiteral("Ответ не содержит ожидаемый положительный или отрицательный результат. Доступен повтор с уточнением."));
        return;
    }
    m_contexts[m_current].append(message("assistant", content));
    complete(result, true);
}
void PipelineExecutor::toolsFinished(const QString &owner, const QJsonArray &results)
{
    if (owner != m_current || m_state != State::Tools)
        return;
    auto record = m_visits.last().toObject();
    auto tools = record.value("tools").toArray();
    tools.append(QJsonObject{{"time", now()}, {"results", results}});
    record.insert("tools", tools);
    m_visits[m_visits.size() - 1] = record;
    for (const auto &entry : results)
    {
        const auto result = entry.toObject();
        if (result.value("type") == "write_file" && result.value("success").toBool() && result.value("changed").toBool())
            ++m_changedWrites;
        if (result.value("type") == "run_command" && result.contains("error"))
            log(QStringLiteral("%1: %2").arg(orchestrator::TaskExecutor::describeAction(result), result.value("error").toString()));
    }
    m_contexts[m_current].append(message("user", jsonText({{"event", "tool_results"}, {"tool_results", results}})));
    request();
}
void PipelineExecutor::complete(const QJsonObject &result, bool allowAutomatic)
{
    const auto *agent = m_graph.find(m_current);
    if (!agent)
        return;
    m_results.insert(m_current, result);
    m_text = result.value("content").toString();
    auto record = m_visits.last().toObject();
    record.insert("result", result);
    record.insert("finished", now());
    m_visits[m_visits.size() - 1] = record;
    emit outputChanged(m_text);
    const bool positive = result.value(agent->resultField).toString() == agent->positive;
    const auto verdict = allowAutomatic ? result.value(agent->resultField).toString() : QStringLiteral("пауза");
    log(QStringLiteral("%1 [%2] %3").arg(agent->name, verdict, result.value("summary").toString()));
    if (!agent->tools.isEmpty())
        log(QStringLiteral("За посещение: операций %1/%2, записей write_file с изменениями: %3.").arg(m_operations).arg(agent->toolLimit).arg(m_changedWrites));
    setState(State::AwaitingChoice);
    emit nodeFinished(m_current, result);
    if (!allowAutomatic || !agent->automatic)
        return;
    const auto target = m_graph.next(m_current, positive);
    if (target.isEmpty())
    {
        log(QStringLiteral("Выход не подключен. Ветка завершена."));
        setState(State::Finished);
        return;
    }
    if (m_automaticSteps >= m_graph.automaticLimit)
    {
        log(QStringLiteral("Достигнут лимит автопереходов. Контексты сохранены; доступен ручной переход."));
        return;
    }
    const auto epoch = m_epoch;
    QTimer::singleShot(0, this, [this, epoch, target, input = jsonText(result)] {
        if (m_epoch == epoch && m_state == State::AwaitingChoice)
            this->visit(target, input, {}, true);
    });
}
bool PipelineExecutor::advance(bool positive, const QString &note, QString &error)
{
    if (busy() || m_current.isEmpty())
    {
        error = QStringLiteral("Дождитесь ответа агента.");
        return false;
    }
    const auto target = m_graph.next(m_current, positive);
    if (target.isEmpty())
    {
        error = QStringLiteral("Выбранный выход не подключен.");
        return false;
    }
    visit(target, jsonText(m_results.value(m_current).toObject()), note);
    return true;
}
bool PipelineExecutor::repeat(const QString &note, QString &error)
{
    if (busy() || m_current.isEmpty())
    {
        error = QStringLiteral("Нет агента для повтора.");
        return false;
    }
    const auto input = m_state == State::Failed ? jsonText(m_results.value(m_current).toObject()) : m_text;
    visit(m_current, input, note);
    return true;
}
void PipelineExecutor::stop()
{
    ++m_epoch;
    m_client.cancel();
    for (auto *worker : m_executors)
        worker->cancel();
    if (!m_id.isEmpty())
    {
        log(QStringLiteral("Остановлено пользователем. Контексты сохранены."));
        setState(State::Stopped);
    }
}
void PipelineExecutor::clear()
{
    ++m_epoch;
    m_client.cancel();
    qDeleteAll(m_executors);
    m_executors.clear();
    m_contexts.clear();
    m_results = {};
    m_visits = {};
    m_events = {};
    m_graph = {};
    m_current.clear();
    m_id.clear();
    m_task.clear();
    m_text.clear();
    emit outputChanged({});
    setState(State::Idle);
}
bool PipelineExecutor::updateConnection(const orchestrator::Configuration &connection, QString &error)
{
    if (busy())
    {
        error = QStringLiteral("Дождитесь окончания операции.");
        return false;
    }
    if (QDir::cleanPath(QDir::fromNativeSeparators(connection.workspace)) != QDir::cleanPath(QDir::fromNativeSeparators(m_connection.workspace)))
    {
        error = QStringLiteral("Для другой папки проекта начните новую задачу.");
        return false;
    }
    m_connection = connection;
    return true;
}
void PipelineExecutor::finish()
{
    if (!busy())
    {
        ++m_epoch;
        setState(State::Finished);
    }
}
void PipelineExecutor::resetContext(const QString &id)
{
    if (busy())
        return;
    m_contexts.remove(id);
    if (auto *worker = m_executors.take(id))
        delete worker;
    emit changed();
}
void PipelineExecutor::fail(const QString &message)
{
    ++m_epoch;
    log(message);
    setState(State::Failed);
    emit failed(message);
}
QJsonObject PipelineExecutor::snapshot() const
{
    QJsonObject contexts;
    for (auto it = m_contexts.begin(); it != m_contexts.end(); ++it)
        contexts.insert(it.key(), it.value());
    return {{"format", "orkestrator.graph-session"},
            {"version", 1},
            {"id", m_id},
            {"pipeline", m_graph.toJson()},
            {"task", m_task},
            {"workspace", m_connection.workspace},
            {"current", m_current},
            {"contexts", contexts},
            {"results", m_results},
            {"visits", m_visits},
            {"events", m_events},
            {"state", static_cast<int>(m_state)}};
}
bool PipelineExecutor::restore(const QJsonObject &session, const orchestrator::Configuration &connection, QString &error)
{
    Pipeline parsed;
    if (session.value("format").toString() != "orkestrator.graph-session" || session.value("version").toInt() != 1 ||
        !Pipeline::fromJson(session.value("pipeline").toObject(), parsed, error))
    {
        if (error.isEmpty())
            error = QStringLiteral("Неизвестный формат сессии.");
        return false;
    }
    const auto current = session.value("current").toString();
    if (!parsed.find(current))
    {
        error = QStringLiteral("В сессии отсутствует текущий агент.");
        return false;
    }
    QMap<QString, QJsonArray> contexts;
    const auto stored = session.value("contexts").toObject();
    for (auto it = stored.begin(); it != stored.end(); ++it)
    {
        if (!it.value().isArray())
        {
            error = QStringLiteral("Некорректная история агента.");
            return false;
        }
        for (const auto &entry : it.value().toArray())
            if (!entry.isObject() || !entry.toObject().value("role").isString() || !entry.toObject().value("content").isString())
            {
                error = QStringLiteral("Некорректное сообщение истории.");
                return false;
            }
        contexts.insert(it.key(), it.value().toArray());
    }
    stop();
    qDeleteAll(m_executors);
    m_executors.clear();
    m_graph = parsed;
    m_connection = connection;
    m_connection.workspace = session.value("workspace").toString();
    m_current = current;
    const QUuid storedId(session.value("id").toString());
    m_id = (storedId.isNull() ? QUuid::createUuid() : storedId).toString(QUuid::WithoutBraces);
    m_task = session.value("task").toString();
    m_contexts = contexts;
    m_results = session.value("results").toObject();
    m_visits = session.value("visits").toArray();
    m_events = session.value("events").toArray();
    m_text = m_results.value(current).toObject().value("content").toString();
    m_automaticSteps = 0;
    emit outputChanged(m_text);
    setState(State::AwaitingChoice);
    log(QStringLiteral("Сессия загружена. Автоматическое выполнение ожидает команды пользователя."));
    return true;
}
QJsonArray PipelineExecutor::context(const QString &id) const
{
    return m_contexts.value(id);
}
PipelineExecutor::State PipelineExecutor::state() const
{
    return m_state;
}
bool PipelineExecutor::busy() const
{
    return m_state == State::Running || m_state == State::Tools;
}
QString PipelineExecutor::current() const
{
    return m_current;
}
QString PipelineExecutor::resultText() const
{
    return m_text;
}
QString PipelineExecutor::sessionId() const
{
    return m_id;
}
QString PipelineExecutor::task() const
{
    return m_task;
}
Pipeline PipelineExecutor::graph() const
{
    return m_graph;
}
}
