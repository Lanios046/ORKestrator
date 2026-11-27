#pragma once
#include "network/lmstudioclient.h"
#include "orchestration/taskexecutor.h"
#include "pipeline/pipeline.h"

#include <QMap>

namespace pipeline
{
class PipelineExecutor final : public QObject
{
    Q_OBJECT
  public:
    enum class State
    {
        Idle,
        Running,
        Tools,
        AwaitingChoice,
        Stopped,
        Failed,
        Finished
    };
    Q_ENUM(State)
    explicit PipelineExecutor(QObject *parent = nullptr);
    bool start(const Pipeline &graph, const QString &task, const orchestrator::Configuration &connection, QString &error);
    bool updateGraph(const Pipeline &graph, QString &error);
    bool advance(bool positive, const QString &note, QString &error);
    bool repeat(const QString &note, QString &error);
    void stop();
    void clear();
    bool updateConnection(const orchestrator::Configuration &connection, QString &error);
    void finish();
    void resetContext(const QString &id);
    bool restore(const QJsonObject &session, const orchestrator::Configuration &connection, QString &error);
    QJsonObject snapshot() const;
    QJsonArray context(const QString &id) const;
    State state() const;
    bool busy() const;
    QString current() const;
    QString resultText() const;
    QString sessionId() const;
    QString task() const;
    Pipeline graph() const;

  signals:
    void stateChanged(pipeline::PipelineExecutor::State state);
    void outputChanged(const QString &text);
    void nodeFinished(const QString &id, const QJsonObject &result);
    void eventOccurred(const QString &text);
    void changed();
    void failed(const QString &error);
    void progress(qint64 content, qint64 reasoning);

  private:
    void visit(const QString &id, const QString &input, const QString &note, bool automatic = false);
    void request();
    void received(const QString &content);
    void toolsFinished(const QString &owner, const QJsonArray &results);
    void fail(const QString &message);
    void setState(State state);
    void log(const QString &text);
    void complete(const QJsonObject &result, bool allowAutomatic);
    orchestrator::TaskExecutor *executor(const Agent &agent, QString &error);
    Pipeline m_graph;
    orchestrator::LmStudioClient m_client;
    orchestrator::Configuration m_connection;
    QMap<QString, orchestrator::TaskExecutor *> m_executors;
    QMap<QString, QJsonArray> m_contexts;
    QMap<QString, int> m_repeats;
    QJsonArray m_events, m_visits;
    QJsonObject m_results;
    QString m_id, m_task, m_current, m_text;
    State m_state{State::Idle};
    int m_operations{0}, m_changedWrites{0}, m_automaticSteps{0};
    quint64 m_epoch{0};
};
}
