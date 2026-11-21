#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QProcess;

namespace orchestrator
{
class TaskExecutor final : public QObject
{
    Q_OBJECT

  public:
    explicit TaskExecutor(QObject *parent = nullptr);
    ~TaskExecutor() override;
    bool configure(const QString &workspace, QString &error);
    void execute(const QJsonArray &actions, bool canWrite, int timeoutMs);
    void cancel();
    QString workspace() const;

  signals:
    void finished(const QJsonArray &results);
    void activity(const QString &text);

  private:
    void next();
    void scheduleNext();
    QJsonObject fileAction(const QJsonObject &action);
    QString resolvePath(const QString &relative, QString &error) const;
    void runCommand(const QJsonObject &action);

    QString m_workspace;
    QHash<QString, QByteArray> m_observed;
    QJsonArray m_actions;
    QJsonArray m_results;
    QPointer<QProcess> m_process;
    int m_index{0};
    int m_timeoutMs{600000};
    quint64 m_generation{0};
    bool m_canWrite{false};
    bool m_active{false};
};
} // namespace orchestrator