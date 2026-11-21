#include "fakelmstudioserver.h"
#include "orchestration/orchestrator.h"
#include "orchestration/protocol.h"
#include "orchestration/taskexecutor.h"
#include "saving/sessionstore.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using orchestrator::AgentRole;
using orchestrator::Orchestrator;

class OrchestrationTests final : public QObject
{
    Q_OBJECT
  private:
    static orchestrator::Configuration config(const FakeLmStudioServer &server, const QString &workspace)
    {
        orchestrator::Configuration c;
        c.baseUrl = server.baseUrl();
        c.workspace = workspace;
        c.pmModel = "test-pm";
        c.criticModel = "test-pm-critic";
        c.developerModel = "test-developer";
        c.developerCriticModel = "test-developer-critic";
        c.censorModel = "test-censor";
        c.timeoutMs = 3000;
        return c;
    }
    static QList<QJsonArray> requestsFor(const FakeLmStudioServer &server, const QString &model)
    {
        QList<QJsonArray> result;
        for (const auto &request : server.requests)
        {
            const auto body = request.value("payload").toObject();
            if (body.value("model").toString() == model)
            {
                result.append(body.value("messages").toArray());
            }
        }
        return result;
    }
    static void tail(FakeLmStudioServer &server)
    {
        server.enqueue(developerReady());
        server.enqueue(approved());
        server.enqueue(censorApproved());
    }

  private slots:
    void fiveRolesKeepTheirHistoriesAcrossStagesAndCensorReturns()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        FakeLmStudioServer server;
        server.enqueue(questions());
        server.enqueue(ready("# Plan", mockStages(2)));
        server.enqueue(revise());
        server.enqueue(ready("# Fixed plan", mockStages(2)));
        server.enqueue(approved());
        server.enqueue(developerActions({tool("write_file", "result.txt", "implemented")}));
        server.enqueue(developerActions({tool("run_command", "", "", "cmake", {"-E", "echo", "checked"})}));
        server.enqueue(developerReady("Stage 1"));
        server.enqueue(revise());
        server.enqueue(developerReady("Stage 1 fixed"));
        server.enqueue(approved());
        server.enqueue(developerReady("Stage 2"));
        server.enqueue(approved());
        server.enqueue(censorRevise("developer", "S1"));
        server.enqueue(developerReady("Stage 1 after censor"));
        server.enqueue(approved());
        server.enqueue(developerReady("Stage 2 rechecked"));
        server.enqueue(approved());
        server.enqueue(censorApproved());

        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.apiKey = "private-key";
        workflow.start("ORIGINAL_TASK", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QString error;
        QVERIFY(!workflow.submitAnswers({}, error));
        QVERIFY(workflow.submitAnswers({{"scope", "MVP"}}, error));
        QTRY_COMPARE_WITH_TIMEOUT(workflow.state(), Orchestrator::State::Completed, 15000);
        QCOMPARE(server.requests.size(), 19);
        QFile file(workspace.filePath("result.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("implemented"));
        const auto snapshot = workflow.snapshot();
        QCOMPARE(snapshot.value("agents").toObject().size(), 5);
        QCOMPARE(snapshot.value("accepted_stages").toArray().size(), 2);
        QCOMPARE(snapshot.value("stage_history").toArray().size(), 4);
        QVERIFY(snapshot.value("approved").toBool());
        QVERIFY(!QJsonDocument(snapshot).toJson().contains("private-key"));

        const auto pmCritic = requestsFor(server, "test-pm-critic");
        QCOMPARE(pmCritic.size(), 2);
        QCOMPARE(pmCritic[0].size(), 2);
        QCOMPARE(pmCritic[1].size(), 4); // The first review remains in this role's history.
        const auto developer = requestsFor(server, "test-developer");
        for (qsizetype i = 1; i < developer.size(); ++i)
        {
            QVERIFY(developer[i].size() > developer[i - 1].size());
        }
        const auto developerCritic = requestsFor(server, "test-developer-critic");
        for (qsizetype i = 1; i < developerCritic.size(); ++i)
        {
            QVERIFY(developerCritic[i].size() > developerCritic[i - 1].size());
        }
        const auto censor = requestsFor(server, "test-censor");
        QCOMPARE(censor.size(), 2);
        QCOMPARE(censor[0].size(), 2);
        QCOMPARE(censor[1].size(), 4);
        for (auto role : orchestrator::agentRoles)
        {
            const auto messages = snapshot.value("agents").toObject().value(orchestrator::agentId(role)).toObject().value("messages").toArray();
            int systemMessages = 0;
            for (const auto &m : messages)
            {
                systemMessages += m.toObject().value("role").toString() == "system";
            }
            QCOMPARE(systemMessages, 1);
            QCOMPARE(messages.first().toObject().value("content").toString(), orchestrator::agentPrompt(role));
        }
        QCOMPARE(snapshot.value("tools").toArray().size(), 2);
        const auto command = snapshot.value("tools").toArray().last().toObject().value("results").toArray().first().toObject();
        QVERIFY(command.value("success").toBool());
        QCOMPARE(command.value("exit_code").toInt(), 0);
        QVERIFY(command.value("output").toString().contains("checked"));
    }

    void censorCanReturnToPmWithoutResettingAnyRole()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerReady());
        server.enqueue(approved());
        server.enqueue(censorRevise("pm", ""));
        server.enqueue(ready("# Revised decomposition"));
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("plan_revision").toInt(), 2);
        for (const auto &model : {"test-pm", "test-pm-critic", "test-developer", "test-developer-critic", "test-censor"})
        {
            const auto requests = requestsFor(server, model);
            QCOMPARE(requests.size(), 2);
            QCOMPARE(requests[0].size(), 2);
            QCOMPARE(requests[1].size(), 4);
        }
    }

    void tokenLimitRetryKeepsAnswersSessionAndAllHistories()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(questions());
        server.replies.enqueue({orchestrator::jsonText(ready()), 200, "length"});
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QString error;
        QVERIFY(workflow.submitAnswers({{"scope", "MVP"}}, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        const auto before = workflow.snapshot();
        QVERIFY(workflow.canRetry());
        c.maxTokens = 131072;
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), before.value("id"));
        QCOMPARE(workflow.snapshot().value("answers"), before.value("answers"));
        const auto pm = requestsFor(server, "test-pm");
        QCOMPARE(pm[1], pm[2]); // The truncated response was never accepted into history.
        QCOMPARE(server.requests[2].value("payload").toObject().value("max_tokens").toInt(), 131072);
    }

    void newTaskClearsAllFiveHistoriesAndCancellationIgnoresLateReplies()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.hold = true;
        Orchestrator workflow;
        workflow.start("OLD_TASK", config(server, workspace.path()));
        QTRY_COMPARE(server.requests.size(), 1);
        workflow.stop();
        QCOMPARE(workflow.state(), Orchestrator::State::Stopped);
        server.hold = false;
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        workflow.start("NEW_TASK", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QVERIFY(!QJsonDocument(workflow.snapshot().value("agents").toObject()).toJson().contains("OLD_TASK"));
    }

    void timeoutsHttpErrorsAndMalformedJsonDoNotApprove()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.hold = true;
        auto c = config(server, workspace.path());
        c.timeoutMs = 80;
        Orchestrator workflow;
        workflow.start("Timeout", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains("время ожидания"));
        server.hold = false;
        server.replies.enqueue({"Unavailable", 503});
        workflow.start("HTTP", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains("503"));
        for (int i = 0; i < 3; ++i)
        {
            server.replies.enqueue({"not JSON"});
        }
        workflow.start("Invalid", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        QCOMPARE(workflow.snapshot().value("exchanges").toArray().size(), 3);
    }

    void malformedJsonRepairIsRetainedInThePmContext()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.replies.enqueue({"not JSON"});
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto pm = requestsFor(server, "test-pm");
        QCOMPARE(pm.size(), 2);
        QCOMPARE(pm[1].size(), 4);
        QVERIFY(QJsonDocument(pm[1]).toJson().contains("not JSON"));
    }

    void reviewLimitAndQuestionLimitPreservePartialResults()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(revise());
        server.enqueue(ready());
        auto c = config(server, workspace.path());
        c.maxReviews = 1;
        Orchestrator workflow;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QCOMPARE(server.requests.size(), 3);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        server.enqueue(questions());
        c.maxPmTurns = 1;
        workflow.start("Question limit", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QString error;
        QVERIFY(workflow.submitAnswers({{"scope", "не знаю"}}, error));
        QCOMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QCOMPARE(workflow.snapshot().value("answers").toArray().size(), 1);
    }

    void cyclesUnknownDependenciesAndContradictoryVerdictsAreRejected()
    {
        auto stages = mockStages(2);
        auto first = stages[0].toObject();
        first.insert("depends_on", QJsonArray{"S2"});
        stages[0] = first;
        QString error;
        QVERIFY(orchestrator::orderedStages(stages, error).isEmpty());
        QVERIFY(!error.isEmpty());
        first.insert("depends_on", QJsonArray{"missing"});
        stages[0] = first;
        error.clear();
        QVERIFY(orchestrator::orderedStages(stages, error).isEmpty());
        auto response = revise();
        response.insert("verdict", "approved");
        QJsonObject result;
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::DeveloperCritic, result, error));
        QVERIFY(orchestrator::parseAgentResponse(orchestrator::jsonText(censorApproved()), AgentRole::Censor, result, error));
    }

    void fileToolsStayInWorkspaceAndRequireReadBeforeOverwrite()
    {
        QTemporaryDir workspace;
        orchestrator::TaskExecutor executor;
        QString error;
        QVERIFY(executor.configure(workspace.path(), error));
        QSignalSpy finished(&executor, &orchestrator::TaskExecutor::finished);
        executor.execute({tool("write_file", "src/test.txt", "first"), tool("read_file", "src/test.txt"), tool("write_file", "../escape.txt", "bad"),
                          tool("write_file", ".git/config", "bad")},
                         true, 3000);
        QTRY_COMPARE(finished.count(), 1);
        const auto results = qvariant_cast<QJsonArray>(finished.takeFirst().first());
        QVERIFY(results[0].toObject().value("success").toBool());
        QVERIFY(results[1].toObject().value("success").toBool());
        QVERIFY(!results[2].toObject().value("success").toBool());
        QVERIFY(!results[3].toObject().value("success").toBool());
        QFile current(workspace.filePath("src/test.txt"));
        QVERIFY(current.open(QIODevice::WriteOnly));
        current.write("user edit");
        current.close();
        executor.execute({tool("write_file", "src/test.txt", "clobbered")}, true, 3000);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!qvariant_cast<QJsonArray>(finished.takeFirst().first()).first().toObject().value("success").toBool());
        executor.execute({tool("write_file", "reviewer.txt", "bad")}, false, 3000);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!qvariant_cast<QJsonArray>(finished.takeFirst().first()).first().toObject().value("success").toBool());
        QVERIFY(!QFile::exists(workspace.filePath("reviewer.txt")));
    }

    void commandsHaveRealExitCodesTimeoutsAndCancellation()
    {
        QTemporaryDir workspace;
        orchestrator::TaskExecutor executor;
        QString error;
        QVERIFY(executor.configure(workspace.path(), error));
        QSignalSpy finished(&executor, &orchestrator::TaskExecutor::finished);
        executor.execute({tool("run_command", "", "", ORKESTRATOR_PROCESS_FIXTURE, {"success"})}, false, 3000);
        QTRY_COMPARE(finished.count(), 1);
        auto result = qvariant_cast<QJsonArray>(finished.takeFirst().first()).first().toObject();
        QVERIFY(result.value("success").toBool());
        QVERIFY(result.value("output").toString().contains("fixture-ok"));
        executor.execute({tool("run_command", "", "", ORKESTRATOR_PROCESS_FIXTURE, {"sleep"})}, false, 80);
        QTRY_COMPARE(finished.count(), 1);
        result = qvariant_cast<QJsonArray>(finished.takeFirst().first()).first().toObject();
        QVERIFY(result.value("timed_out").toBool());
        QVERIFY(!result.value("success").toBool());
        executor.execute({tool("run_command", "", "", ORKESTRATOR_PROCESS_FIXTURE, {"sleep"})}, false, 3000);
        executor.cancel();
        QTest::qWait(120);
        QCOMPARE(finished.count(), 0);
    }

    void persistenceIncludesAllFiveContextsAndUtf8()
    {
        QTemporaryDir directory;
        QString error;
        QJsonObject agents;
        for (auto role : orchestrator::agentRoles)
        {
            agents.insert(orchestrator::agentId(role), QJsonArray{orchestrator::message("system", "Контекст")});
        }
        QVERIFY(orchestrator::SessionStore::save({{"id", "session"}, {"agents", agents}, {"approved", false}}, "# Результат", directory.path(), error));
        QFile file(directory.filePath("session.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("agents").toObject().size(), 5);
        QVERIFY(!orchestrator::SessionStore::save({{"id", "../escape"}}, "", directory.path(), error));
    }
};
QTEST_GUILESS_MAIN(OrchestrationTests)
#include "orchestration_tests.moc"