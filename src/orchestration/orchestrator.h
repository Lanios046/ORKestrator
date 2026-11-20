#pragma once

#include "network/lmstudioclient.h"

namespace orchestrator
{
class Orchestrator final : public QObject
{
    Q_OBJECT

  public:
    enum class State
    {
        Idle,
        PmWorking,
        AwaitingAnswers,
        CriticWorking,
        Completed,
        Stopped,
        Failed,
        LimitReached
    };
    Q_ENUM(State)

    explicit Orchestrator(QObject *parent = nullptr);
    void start(const QString &task, const Configuration &configuration);
    bool submitAnswers(const QJsonObject &answers, QString &error);
    void stop();
    State state() const;
    bool active() const;
    QJsonObject snapshot() const;
    QString markdown() const;
    static QString stateName(State state);

  signals:
    void stateChanged(orchestrator::Orchestrator::State state);
    void questionsReady(const QJsonArray &questions, const QString &summary);
    void planChanged(const QString &markdown);
    void reviewReady(const QJsonObject &review, int round);
    void eventOccurred(const QString &text);
    void changed();
    void failed(const QString &error);

  private:
    void askPm();
    void askCritic();
    void sendPending();
    void onCompletion(const QString &content);
    void setState(State state);
    void fail(const QString &error);
    void log(const QString &text);

    LmStudioClient m_client;
    Configuration m_configuration;
    State m_state{State::Idle};
    QString m_id;
    QString m_createdAt;
    QString m_task;
    QString m_plan;
    QString m_error;
    QJsonArray m_pmHistory;
    QJsonArray m_pendingMessages;
    QJsonArray m_questions;
    QJsonArray m_answers;
    QJsonArray m_reviews;
    QJsonArray m_plans;
    QJsonArray m_exchanges;
    QJsonArray m_events;
    int m_pmTurns{0};
    int m_reviewRounds{0};
    int m_formatRetries{0};
    bool m_critic{false};
};
} // namespace orchestrator