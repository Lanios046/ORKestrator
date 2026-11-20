#include "orchestrator.h"
#include "protocol.h"

#include <QDateTime>
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
} // namespace

Orchestrator::Orchestrator(QObject *parent) : QObject(parent)
{
    connect(&m_client, &LmStudioClient::completionReady, this, &Orchestrator::onCompletion);
    connect(&m_client, &LmStudioClient::failed, this, &Orchestrator::fail);
}

void Orchestrator::start(const QString &task, const Configuration &configuration)
{
    if (active())
    {
        return;
    }
    m_client.cancel();
    m_configuration = configuration;
    m_client.configure(configuration);
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_createdAt = now();
    m_task = task.trimmed();
    m_plan.clear();
    m_error.clear();
    m_pmHistory = {};
    m_pendingMessages = {};
    m_questions = {};
    m_answers = {};
    m_reviews = {};
    m_plans = {};
    m_exchanges = {};
    m_events = {};
    m_pmTurns = 0;
    m_reviewRounds = 0;
    m_formatRetries = 0;
    if (m_task.isEmpty() || configuration.pmModel.trimmed().isEmpty() || configuration.criticModel.trimmed().isEmpty() || configuration.timeoutMs <= 0 ||
        configuration.maxTokens <= 0 || configuration.maxPmTurns <= 0 || configuration.maxReviews <= 0)
    {
        fail(QStringLiteral("Нужны непустая задача, модели PM и критика, положительные лимиты и таймаут."));
        return;
    }
    m_pmHistory.append(message("system", pmPrompt()));
    m_pmHistory.append(message("user", jsonText({{"task", m_task}})));
    log(QStringLiteral("Новая сессия %1. PM: %2; критик: %3.").arg(m_id, configuration.pmModel, configuration.criticModel));
    askPm();
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
            error = QStringLiteral("Ответьте на каждый вопрос. Можно написать «не знаю». Пропущен вопрос: %1").arg(question.value("text").toString());
            return false;
        }
        batch.append(QJsonObject{{"id", id}, {"question", question.value("text")}, {"answer", answers.value(id).toString().trimmed()}});
    }
    for (const auto &value : batch)
    {
        m_answers.append(value);
    }
    m_pmHistory.append(message("user", jsonText({{"answers", batch}})));
    m_questions = {};
    log(QStringLiteral("Пользователь ответил на %1 вопросов.").arg(batch.size()));
    askPm();
    return true;
}

void Orchestrator::askPm()
{
    if (m_pmTurns >= m_configuration.maxPmTurns)
    {
        log(QStringLiteral("Достигнут лимит ходов PM. План сохранен как черновик."));
        setState(State::LimitReached);
        return;
    }
    ++m_pmTurns;
    m_critic = false;
    m_formatRetries = 0;
    m_pendingMessages = m_pmHistory;
    log(QStringLiteral("PM изучает задачу / дорабатывает план. Ход %1/%2.").arg(m_pmTurns).arg(m_configuration.maxPmTurns));
    setState(State::PmWorking);
    sendPending();
}

void Orchestrator::askCritic()
{
    if (m_reviewRounds >= m_configuration.maxReviews)
    {
        log(QStringLiteral("Достигнут лимит проверок критика. Одобрение не получено; результат — черновик."));
        setState(State::LimitReached);
        return;
    }
    ++m_reviewRounds;
    m_critic = true;
    m_formatRetries = 0;
    // Deliberately rebuild from scratch. No PM messages or previous critic reviews.
    m_pendingMessages = {message("system", criticPrompt()), message("user", jsonText({{"task", m_task}, {"answers", m_answers}, {"decomposition", m_plan}}))};
    log(QStringLiteral("Независимая проверка %1/%2. Контекст критика создан заново.").arg(m_reviewRounds).arg(m_configuration.maxReviews));
    setState(State::CriticWorking);
    sendPending();
}

void Orchestrator::sendPending()
{
    m_client.complete(m_critic ? m_configuration.criticModel : m_configuration.pmModel, m_pendingMessages, m_critic ? criticSchema() : pmSchema(),
                      m_critic ? QStringLiteral("critic_review") : QStringLiteral("pm_result"));
}

void Orchestrator::onCompletion(const QString &content)
{
    if (m_state != State::PmWorking && m_state != State::CriticWorking)
    {
        return;
    }
    m_exchanges.append(QJsonObject{{"agent", m_critic ? "critic" : "pm"}, {"time", now()}, {"messages", m_pendingMessages}, {"response", content}});
    m_pendingMessages.append(message("assistant", content));
    QJsonObject result;
    QString error;
    if (!parseAgentResponse(content, m_critic, result, error))
    {
        if (m_formatRetries >= 2)
        {
            fail(QStringLiteral("Модель трижды вернула ответ с нарушением протокола: %1").arg(error));
            return;
        }
        ++m_formatRetries;
        log(QStringLiteral("Повтор формата %1/2: %2").arg(m_formatRetries).arg(error));
        m_pendingMessages.append(message("user", QStringLiteral("Исправь формат JSON предыдущего ответа: %1. Верни полный объект по схеме.").arg(error)));
        emit changed();
        sendPending();
        return;
    }
    if (m_critic)
    {
        m_reviews.append(QJsonObject{{"round", m_reviewRounds}, {"decomposition", m_plan}, {"review", result}});
        emit reviewReady(result, m_reviewRounds);
        if (result.value("verdict").toString() == "approved")
        {
            log(QStringLiteral("Критик одобрил декомпозицию."));
            setState(State::Completed);
        }
        else
        {
            log(QStringLiteral("Критик вернул план PM на доработку: %1").arg(result.value("summary").toString()));
            m_pmHistory.append(
                message("user", jsonText({{"critic_feedback", result},
                                          {"instruction", QStringLiteral("Исправь существенные замечания. При необходимости задай вопросы пользователю.")}})));
            askPm();
        }
        return;
    }

    m_pmHistory = m_pendingMessages;
    const auto plan = result.value("decomposition").toString().trimmed();
    if (!plan.isEmpty())
    {
        m_plan = plan;
        m_plans.append(QJsonObject{{"pm_turn", m_pmTurns}, {"decomposition", m_plan}});
        emit planChanged(m_plan);
    }
    if (result.value("status").toString() == "questions")
    {
        m_questions = result.value("questions").toArray();
        log(QStringLiteral("PM подготовил опросник: %1 вопросов.").arg(m_questions.size()));
        setState(State::AwaitingAnswers);
        emit questionsReady(m_questions, result.value("summary").toString());
    }
    else
    {
        askCritic();
    }
}

void Orchestrator::stop()
{
    if (!active())
    {
        return;
    }
    m_client.cancel();
    log(QStringLiteral("Сессия остановлена пользователем. Сохранен черновик."));
    setState(State::Stopped);
}

Orchestrator::State Orchestrator::state() const
{
    return m_state;
}

bool Orchestrator::active() const
{
    return m_state == State::PmWorking || m_state == State::CriticWorking || m_state == State::AwaitingAnswers;
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
        return QStringLiteral("Критик проверяет декомпозицию");
    case State::Completed:
        return QStringLiteral("Декомпозиция одобрена");
    case State::Stopped:
        return QStringLiteral("Остановлено — черновик");
    case State::Failed:
        return QStringLiteral("Ошибка — одобрение не получено");
    case State::LimitReached:
        return QStringLiteral("Достигнут лимит — черновик");
    }
    return {};
}

QJsonObject Orchestrator::snapshot() const
{
    return {{"schema_version", 1},
            {"id", m_id},
            {"created_at", m_createdAt},
            {"updated_at", now()},
            {"status", QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(static_cast<int>(m_state)))},
            {"approved", m_state == State::Completed},
            {"task", m_task},
            {"decomposition", m_plan},
            {"pending_questions", m_questions},
            {"answers", m_answers},
            {"reviews", m_reviews},
            {"plans", m_plans},
            {"pm_messages", m_pmHistory},
            {"exchanges", m_exchanges},
            {"events", m_events},
            {"error", m_error},
            {"configuration", QJsonObject{{"base_url", LmStudioClient::normalizedBaseUrl(m_configuration.baseUrl).toString()},
                                          {"pm_model", m_configuration.pmModel},
                                          {"critic_model", m_configuration.criticModel},
                                          {"timeout_ms", m_configuration.timeoutMs},
                                          {"max_tokens", m_configuration.maxTokens},
                                          {"max_pm_turns", m_configuration.maxPmTurns},
                                          {"max_reviews", m_configuration.maxReviews},
                                          {"structured_output", m_configuration.structuredOutput}}}};
}

QString Orchestrator::markdown() const
{
    QString text = QStringLiteral("# ORKestrator — %1\n\nСессия: %2\n\nСтатус: %3\n\n## Исходная задача\n\n%4\n\n")
                       .arg(m_state == State::Completed ? QStringLiteral("декомпозиция") : QStringLiteral("черновик"), m_id, stateName(m_state), m_task);
    if (!m_answers.isEmpty())
    {
        text += QStringLiteral("## Уточнения пользователя\n\n");
        for (const auto &value : m_answers)
        {
            const auto answer = value.toObject();
            text += QStringLiteral("**%1**\n\n%2\n\n").arg(answer.value("question").toString(), answer.value("answer").toString());
        }
    }
    text += QStringLiteral("## План\n\n%1\n").arg(m_plan);
    if (!m_reviews.isEmpty())
    {
        text += QStringLiteral("\n## Последняя проверка критика\n\n") + reviewMarkdown(m_reviews.last().toObject().value("review").toObject());
    }
    return text;
}
} // namespace orchestrator