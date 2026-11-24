#pragma once
#include "network/lmstudioclient.h"
#include "protocol.h"
#include "taskexecutor.h"

#include <QMap>

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
        ResearcherWorking,
        DiagnosticianWorking,
        AwaitingAnswers,
        CriticWorking,
        DeveloperWorking,
        DeveloperCriticWorking,
        CensorWorking,
        ExecutingTools,
        Completed,
        Stopped,
        Failed,
        LimitReached
    };
    Q_ENUM(State)
    explicit Orchestrator(QObject *parent = nullptr);
    void start(const QString &task, const Configuration &configuration);
    bool submitAnswers(const QJsonObject &answers, QString &error);
    bool retry(const Configuration &configuration, QString &error);
    bool canRetry() const;
    void stop();
    State state() const;
    bool active() const;
    QJsonObject snapshot() const;
    QString markdown() const;
    static QString stateName(State state);
  signals:
    void stateChanged(orchestrator::Orchestrator::State state);
    void specialistReportReady(const QString &role, const QString &report);
    void generationProgress(const QString &role, qint64 contentCharacters, qint64 reasoningCharacters);
    void questionsReady(const QJsonArray &questions, const QString &summary);
    void planChanged(const QString &markdown);
    void implementationChanged(const QString &markdown);
    void reviewReady(const QJsonObject &review, int round);
    void roleReviewReady(const QString &role, const QJsonObject &review, int round);
    void toolResultsReady(const QJsonArray &results);
    void stageChanged(int index, int count, const QString &title);
    void eventOccurred(const QString &text);
    void changed();
    void failed(const QString &error);

  private:
    enum class ResumePoint
    {
        Request,
        PmReview,
        DeveloperReview,
        CensorReview
    };
    ResumePoint m_resumePoint{ResumePoint::Request};
    void sendTo(AgentRole role, const QJsonObject &payload);
    void sendCurrent();
    void activateContext(AgentRole role, const QString &key);
    QJsonObject contextRecord(AgentRole role) const;
    void handleGenerationFailure(const QString &error, bool reasoningBudget);
    bool beginDiagnosis(AgentRole returnRole, const QJsonObject &feedback, const QString &reason);
    void finishDiagnosis(const QJsonObject &result);
    void onCompletion(const QString &content);
    void onToolsFinished(const QJsonArray &results);
    QString toolGuardScope() const;
    bool blockRepeatedActions(const QJsonArray &actions);
    QJsonObject toolFeedback(const QJsonArray &results);
    void formatError(const QString &error);
    void askPmCritic();
    void beginStage(int index, const QJsonObject &feedback = {});
    void askDeveloperCritic();
    void askCensor();
    void handleReview(const QJsonObject &review);
    void setState(State state);
    void fail(const QString &error);
    void log(const QString &text);
    QString model(AgentRole role) const;
    QJsonObject context(bool allStages = false) const;
    QJsonObject currentStage() const;
    QJsonArray checks() const;
    QString implementationMarkdown() const;
    LmStudioClient m_client;
    TaskExecutor m_executor;
    Configuration m_configuration;
    State m_state{State::Idle};
    AgentRole m_role{AgentRole::Pm};
    std::array<QJsonArray, agentRoles.size()> m_histories;
    std::array<QString, agentRoles.size()> m_contextKeys;
    std::array<int, agentRoles.size()> m_calls{}, m_contextCalls{}, m_failedActions{};
    QJsonObject m_contexts, m_research, m_diagnosticFeedback, m_toolObservations;
    QJsonArray m_pendingActions, m_toolLoopEvents;
    QMap<QString, int> m_stagnantToolBatches;
    quint64 m_workspaceRevision{0};
    QJsonArray m_diagnostics, m_generationFailures;
    QMap<QString, int> m_diagnosticCounts;
    AgentRole m_diagnosticReturnRole{AgentRole::Developer};
    int m_diagnosticSequence{0}, m_generationRetries{0};
    QString m_id, m_createdAt, m_task, m_plan, m_implementation, m_error;
    QJsonArray m_questions, m_answers, m_stages, m_accepted, m_stageHistory, m_reviews, m_plans, m_exchanges, m_events, m_tools;
    int m_stageIndex{0}, m_planRevision{0}, m_pmReviews{0}, m_developerReviews{0}, m_censorReviews{0}, m_formatRetries{0};
    bool m_planApproved{false}, m_requestStarted{false}, m_disableReasoningOnRetry{false};
};
} // namespace orchestrator