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
        c.researcherModel = "test-researcher";
        c.diagnosticianModel = "test-diagnostician";
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
    void stageContextsAreIsolatedAndRestoredOnCensorReturns()
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
        QCOMPARE(server.requests.size(), 20);
        QFile file(workspace.filePath("result.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("implemented"));
        const auto snapshot = workflow.snapshot();
        QCOMPARE(snapshot.value("agents").toObject().size(), 7);
        QCOMPARE(snapshot.value("accepted_stages").toArray().size(), 2);
        QCOMPARE(snapshot.value("stage_history").toArray().size(), 4);
        QVERIFY(snapshot.value("approved").toBool());
        QVERIFY(!QJsonDocument(snapshot).toJson().contains("private-key"));

        const auto pmCritic = requestsFor(server, "test-pm-critic");
        QCOMPARE(pmCritic.size(), 2);
        QCOMPARE(pmCritic[0].size(), 2);
        QCOMPARE(pmCritic[1].size(), 4); // The first review remains in this role's history.
        const auto developer = requestsFor(server, "test-developer");
        QCOMPARE(developer.size(), 7);
        QCOMPARE(developer[0].size(), 2);
        QCOMPARE(developer[3].size(), 8);
        QCOMPARE(developer[4].size(), 2);
        QCOMPARE(developer[5].size(), 10);
        QCOMPARE(developer[6].size(), 4);
        const auto contexts = snapshot.value("contexts").toObject();
        QVERIFY(contexts.contains("developer:2:S1"));
        QVERIFY(contexts.contains("developer:2:S2"));
        QCOMPARE(contexts.value("developer:2:S1").toObject().value("calls").toInt(), 5);
        QCOMPARE(contexts.value("developer:2:S2").toObject().value("calls").toInt(), 2);
        const auto developerCritic = requestsFor(server, "test-developer-critic");
        QCOMPARE(developerCritic.size(), 5);
        QCOMPARE(developerCritic[0].size(), 2);
        QCOMPARE(developerCritic[1].size(), 4);
        QCOMPARE(developerCritic[2].size(), 2);
        QCOMPARE(developerCritic[3].size(), 6);
        QCOMPARE(developerCritic[4].size(), 4);
        QVERIFY(requestsFor(server, "test-diagnostician").isEmpty());
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

    void planRevisionCreatesNewStageContextsAndRetainsTaskContexts()
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
            QCOMPARE(requests[1].size(), QString(model).startsWith("test-developer") ? 2 : 4);
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
        c.maxServiceTokens = 0;
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
        QCOMPARE(server.requests[3].value("payload").toObject().value("max_tokens").toInt(), 131072);
    }

    void newTaskClearsAllContextsAndCancellationIgnoresLateReplies()
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
        QCOMPARE(workflow.snapshot().value("exchanges").toArray().size(), 4);
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
        QCOMPARE(pm[1].size(), 2);
        QVERIFY(!QJsonDocument(pm[1]).toJson().contains("not JSON"));
        QVERIFY(QJsonDocument(workflow.snapshot().value("exchanges").toArray()).toJson().contains("not JSON"));
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
        QCOMPARE(server.requests.size(), 4);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        QVERIFY(workflow.canRetry());
        const auto reviewSession = workflow.snapshot().value("id");
        QString error;
        QVERIFY(!workflow.retry(c, error));
        c.maxReviews = 3;
        server.enqueue(approved());
        tail(server);
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), reviewSession);
        QCOMPARE(requestsFor(server, "test-pm").size(), 2);
        QCOMPARE(requestsFor(server, "test-pm-critic").size(), 2);
        server.enqueue(questions());
        c.maxPmTurns = 1;
        workflow.start("Question limit", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QVERIFY(workflow.submitAnswers({{"scope", "не знаю"}}, error));
        QCOMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QCOMPARE(workflow.snapshot().value("answers").toArray().size(), 1);
        const auto answers = workflow.snapshot().value("answers");
        const auto questionSession = workflow.snapshot().value("id");
        c.maxPmTurns = 4;
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), questionSession);
        QCOMPARE(workflow.snapshot().value("answers"), answers);
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

    void researcherReadsProjectAndHandsFindingsToPm()
    {
        QTemporaryDir workspace;
        QFile readme(workspace.filePath("README.md"));
        QVERIFY(readme.open(QIODevice::WriteOnly));
        readme.write("Existing project instructions");
        readme.close();
        FakeLmStudioServer server;
        server.automaticResearch = false;
        server.enqueue(specialistActions({tool("read_file", "README.md")}));
        server.enqueue(specialistReady("README.md: Existing project instructions"));
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto pm = requestsFor(server, "test-pm");
        const auto payload = QJsonDocument::fromJson(pm.first()[1].toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(payload.value("project_research").toObject().value("result").toString(), QStringLiteral("README.md: Existing project instructions"));
        const auto research = requestsFor(server, "test-researcher");
        QCOMPARE(research.size(), 2);
        QVERIFY(QJsonDocument(research.last()).toJson().contains("Existing project instructions"));
        const auto result = workflow.snapshot().value("tools").toArray().first().toObject().value("results").toArray().first().toObject();
        QVERIFY(result.value("success").toBool());
        QCOMPARE(workflow.snapshot().value("schema_version").toInt(), 3);
        QJsonObject parsed;
        QString error;
        for (auto role : {AgentRole::Researcher, AgentRole::Diagnostician})
        {
            QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(specialistActions({tool("write_file", "forbidden.txt", "bad")})), role, parsed,
                                                      error));
            QVERIFY(orchestrator::parseAgentResponse(orchestrator::jsonText(specialistReady()), role, parsed, error));
        }
        QVERIFY(!QFile::exists(workspace.filePath("forbidden.txt")));
    }

    void diagnosesHaveSeparateContextsAndDoNotRecurseOnTheirOwnErrors()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        const auto failure = developerActions({tool("run_command", "", "", "orkestrator-missing-test-compiler")});
        server.enqueue(failure);
        server.enqueue(specialistActions({tool("read_file", "missing-diagnostic-input.txt")}));
        server.enqueue(specialistReady("First diagnosis: compiler unavailable"));
        server.enqueue(failure);
        server.enqueue(specialistReady("Second diagnosis: verify compiler environment"));
        server.enqueue(failure);
        tail(server);
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.maxDiagnosticsPerStage = 2;
        workflow.start("Task", c);
        QTRY_COMPARE_WITH_TIMEOUT(workflow.state(), Orchestrator::State::Completed, 15000);
        const auto diagnostics = workflow.snapshot().value("diagnostics").toArray();
        QCOMPARE(diagnostics.size(), 2);
        const auto calls = requestsFor(server, "test-diagnostician");
        QCOMPARE(calls.size(), 3);
        QCOMPARE(calls[0].size(), 2);
        QCOMPARE(calls[1].size(), 4);
        QCOMPARE(calls[2].size(), 2);
        QVERIFY(!QJsonDocument(calls[2]).toJson().contains("First diagnosis: compiler unavailable"));
        const auto developer = requestsFor(server, "test-developer");
        QCOMPARE(developer.size(), 4);
        const auto feedback = QJsonDocument::fromJson(developer[1].last().toObject().value("content").toString().toUtf8()).object();
        QVERIFY(feedback.contains("tool_results"));
        QCOMPARE(feedback.value("diagnosis").toObject().value("source_agent").toString(), QStringLiteral("developer"));
        const auto contexts = workflow.snapshot().value("contexts").toObject();
        QVERIFY(contexts.contains("diagnostician:1"));
        QVERIFY(contexts.contains("diagnostician:2"));
        QVERIFY(QJsonDocument(contexts.value("diagnostician:1").toObject()).toJson().contains("First diagnosis"));
    }

    void exhaustedDiagnosticBudgetReturnsIncompleteReportToSource()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerActions({tool("run_command", "", "", "orkestrator-missing-test-compiler")}));
        server.enqueue(specialistActions({tool("list_files")}));
        tail(server);
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.maxDiagnosticTurns = 1;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(requestsFor(server, "test-diagnostician").size(), 1);
        const auto report = workflow.snapshot().value("diagnostics").toArray().first().toObject();
        QCOMPARE(report.value("status").toString(), QStringLiteral("incomplete"));
        const auto developer = requestsFor(server, "test-developer");
        const auto feedback = QJsonDocument::fromJson(developer.last().last().toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(feedback.value("diagnosis").toObject().value("status").toString(), QStringLiteral("incomplete"));
        QVERIFY(feedback.contains("tool_results"));
    }

    void repeatedReviewRejectionsRequestDiagnosisAndReturnToDeveloper()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerReady());
        server.enqueue(revise());
        server.enqueue(developerReady());
        server.enqueue(revise());
        server.enqueue(specialistReady("Investigate the missing acceptance check"));
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(requestsFor(server, "test-diagnostician").size(), 1);
        const auto developer = requestsFor(server, "test-developer");
        QCOMPARE(developer.size(), 3);
        const auto feedback = QJsonDocument::fromJson(developer.last().last().toObject().value("content").toString().toUtf8()).object();
        QVERIFY(feedback.contains("developer_critic_feedback"));
        QVERIFY(feedback.contains("diagnosis"));
    }

    void diagnosticCommandFailureResumesTheOriginalReviewer_data()
    {
        QTest::addColumn<int>("source");
        QTest::newRow("developer-critic") << static_cast<int>(AgentRole::DeveloperCritic);
        QTest::newRow("censor") << static_cast<int>(AgentRole::Censor);
    }

    void diagnosticCommandFailureResumesTheOriginalReviewer()
    {
        QFETCH(int, source);
        const auto role = static_cast<AgentRole>(source);
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerReady());
        if (role == AgentRole::Censor)
        {
            server.enqueue(approved());
        }
        auto action = role == AgentRole::Censor ? censorApproved() : approved();
        action.insert("status", "actions");
        action.insert("actions", QJsonArray{tool("run_command", "", "", "orkestrator-missing-test-compiler")});
        server.enqueue(action);
        server.enqueue(specialistReady("Check the compiler environment"));
        if (role == AgentRole::DeveloperCritic)
        {
            server.enqueue(approved());
        }
        server.enqueue(censorApproved());
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto calls = requestsFor(server, role == AgentRole::Censor ? "test-censor" : "test-developer-critic");
        QCOMPARE(calls.size(), 2);
        const auto feedback = QJsonDocument::fromJson(calls.last().last().toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(feedback.value("diagnosis").toObject().value("source_agent").toString(), orchestrator::agentId(role));
        QVERIFY(feedback.contains("tool_results"));
    }

    void diagnosticApiRetryKeepsThePendingSourceAndContext()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerActions({tool("run_command", "", "", "orkestrator-missing-test-compiler")}));
        server.replies.enqueue({"Unavailable", 503});
        Orchestrator workflow;
        const auto c = config(server, workspace.path());
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        const auto before = workflow.snapshot();
        QCOMPARE(before.value("active_agent").toString(), QStringLiteral("diagnostician"));
        QVERIFY(workflow.canRetry());
        server.enqueue(specialistReady("Compiler unavailable"));
        tail(server);
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), before.value("id"));
        const auto calls = requestsFor(server, "test-diagnostician");
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[0], calls[1]);
        QCOMPARE(requestsFor(server, "test-developer").size(), 2);
        QCOMPARE(workflow.snapshot().value("diagnostics").toArray().size(), 1);
    }

    void researchLimitPreservesThePartialContextWithoutStartingPm()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.automaticResearch = false;
        server.enqueue(specialistActions({tool("list_files")}));
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.maxResearchTurns = 1;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QVERIFY(requestsFor(server, "test-pm").isEmpty());
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 1);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        QCOMPARE(workflow.snapshot().value("agents").toObject().value("researcher").toObject().value("messages").toArray().size(), 4);
        const auto session = workflow.snapshot().value("id");
        c.maxResearchTurns = 2;
        server.enqueue(specialistReady());
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), session);
        QCOMPARE(requestsFor(server, "test-researcher").size(), 2);
    }

    void reviewLimitsResumeAtThePendingReviewer_data()
    {
        QTest::addColumn<int>("source");
        QTest::newRow("developer-review") << static_cast<int>(AgentRole::DeveloperCritic);
        QTest::newRow("final-review") << static_cast<int>(AgentRole::Censor);
    }

    void reviewLimitsResumeAtThePendingReviewer()
    {
        QFETCH(int, source);
        const auto role = static_cast<AgentRole>(source);
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerReady());
        if (role == AgentRole::DeveloperCritic)
        {
            server.enqueue(revise());
        }
        else
        {
            server.enqueue(approved());
            server.enqueue(censorRevise("developer", "S1"));
        }
        server.enqueue(developerReady());
        if (role == AgentRole::Censor)
        {
            server.enqueue(approved());
        }
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.maxReviews = 1;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QVERIFY(workflow.canRetry());
        const auto session = workflow.snapshot().value("id");
        const auto developerCalls = requestsFor(server, "test-developer").size();
        if (role == AgentRole::DeveloperCritic)
        {
            server.enqueue(approved());
        }
        server.enqueue(censorApproved());
        c.maxReviews = 2;
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), session);
        QCOMPARE(requestsFor(server, "test-developer").size(), developerCalls);
    }

    void roleRequestLimitResumesThePendingToolFeedback()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerActions({tool("list_files")}));
        Orchestrator workflow;
        auto c = config(server, workspace.path());
        c.maxAgentTurns = 1;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::LimitReached);
        const auto before = workflow.snapshot();
        QCOMPARE(before.value("active_agent").toString(), QStringLiteral("developer"));
        QVERIFY(workflow.canRetry());
        c.maxAgentTurns = 2;
        tail(server);
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), before.value("id"));
        const auto calls = requestsFor(server, "test-developer");
        QCOMPARE(calls.size(), 2);
        const auto payload = QJsonDocument::fromJson(calls.last().last().toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(payload.value("event").toString(), QStringLiteral("tool_results"));
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 1);
    }

    void streamingPreservesUtf8AndUsesTheSelectedSamplingProfile()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        auto c = config(server, workspace.path());
        c.maxTokens = 256000;
        c.maxServiceTokens = 0;
        c.reasoningMode = orchestrator::ReasoningMode::ServerDefaults;
        orchestrator::LmStudioClient client;
        client.configure(c);
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        QSignalSpy failed(&client, &orchestrator::LmStudioClient::failed);
        FakeLmStudioServer::Reply reply;
        reply.content = orchestrator::jsonText(ready(QStringLiteral("# Русский план")));
        reply.stream = true;
        reply.splitTransport = true;
        server.replies.enqueue(reply);
        client.complete("qwen/qwen3.8-27b", {}, orchestrator::agentSchema(AgentRole::Pm), "pm_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(completed.first().first().toString(), reply.content);
        QCOMPARE(failed.count(), 0);
        auto payload = server.requests.last().value("payload").toObject();
        QVERIFY(payload.value("stream").toBool());
        QCOMPARE(payload.value("temperature").toDouble(), 1.0);
        QCOMPARE(payload.value("top_p").toDouble(), 0.95);
        QCOMPARE(payload.value("top_k").toInt(), 20);
        QCOMPARE(payload.value("max_tokens").toInt(), 256000);
        c.samplingProfile = orchestrator::SamplingProfile::QwenInstruct;
        client.configure(c);
        server.enqueue(ready());
        client.complete("test-model", {}, orchestrator::agentSchema(AgentRole::Pm), "pm_result");
        QTRY_COMPARE(completed.count(), 2);
        payload = server.requests.last().value("payload").toObject();
        QCOMPARE(payload.value("temperature").toDouble(), 0.7);
        QCOMPARE(payload.value("presence_penalty").toDouble(), 1.5);
        c.samplingProfile = orchestrator::SamplingProfile::ServerDefaults;
        client.configure(c);
        server.enqueue(ready());
        client.complete("test-model", {}, orchestrator::agentSchema(AgentRole::Pm), "pm_result");
        QTRY_COMPARE(completed.count(), 3);
        payload = server.requests.last().value("payload").toObject();
        QVERIFY(!payload.contains("temperature"));
        QVERIFY(!payload.contains("presence_penalty"));
    }

    void repetitionStopsAnUnfinishedStream_data()
    {
        QTest::addColumn<bool>("reasoning");
        QTest::addColumn<QString>("text");
        QTest::newRow("repeated-word-content") << false << QString("Global ").repeated(80);
        QTest::newRow("repeated-word-reasoning") << true << QString("Global ").repeated(80);
        QTest::newRow("repeated-phrase-reasoning") << true << QString("The plan must specify every requirement ").repeated(30);
    }

    void repetitionStopsAnUnfinishedStream()
    {
        QFETCH(bool, reasoning);
        QFETCH(QString, text);
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        FakeLmStudioServer::Reply reply;
        reply.stream = true;
        reply.keepOpen = true;
        if (reasoning)
        {
            reply.reasoning = text;
        }
        else
        {
            reply.content = text;
        }
        server.replies.enqueue(reply);
        orchestrator::LmStudioClient client;
        client.configure(config(server, workspace.path()));
        QSignalSpy loops(&client, &orchestrator::LmStudioClient::generationLoopDetected);
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        QSignalSpy failed(&client, &orchestrator::LmStudioClient::failed);
        client.complete("test-model", {}, orchestrator::agentSchema(AgentRole::PmCritic), "pm_critic_result");
        QTRY_COMPARE_WITH_TIMEOUT(loops.count(), 1, 1500);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(failed.count(), 0);
        server.enqueue(approved());
        client.complete("test-model", {}, orchestrator::agentSchema(AgentRole::PmCritic), "pm_critic_result");
        QTRY_COMPARE(completed.count(), 1);
    }

    void repeatedFixtureContentsArePreservedInDeveloperActions()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        const auto content = QString("Global ").repeated(100);
        const auto action = developerActions({tool("write_file", "fixture.txt", content)});
        FakeLmStudioServer::Reply reply;
        reply.content = orchestrator::jsonText(action);
        reply.stream = true;
        server.replies.enqueue(reply);
        orchestrator::LmStudioClient client;
        client.configure(config(server, workspace.path()));
        QSignalSpy loops(&client, &orchestrator::LmStudioClient::generationLoopDetected);
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        client.complete("test-model", {}, orchestrator::agentSchema(AgentRole::Developer), "developer_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(completed.first().first().toString(), reply.content);
        QCOMPARE(loops.count(), 0);
    }

    void cyclicReviewGetsOneCleanRetryAndCanBeRetriedManually()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        FakeLmStudioServer::Reply loop;
        loop.reasoning = QString("Global ").repeated(80);
        loop.stream = true;
        loop.keepOpen = true;
        server.replies.enqueue(loop);
        server.replies.enqueue(loop);
        Orchestrator workflow;
        const auto c = config(server, workspace.path());
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QCOMPARE(workflow.snapshot().value("generation_failures").toArray().size(), 2);
        const auto before = requestsFor(server, "test-pm-critic");
        QCOMPARE(before.size(), 2);
        QCOMPARE(before[0], before[1]);
        QVERIFY(workflow.canRetry());
        const auto session = workflow.snapshot().value("id");
        server.enqueue(approved());
        tail(server);
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), session);
        const auto calls = requestsFor(server, "test-pm-critic");
        QCOMPARE(calls.size(), 3);
        QCOMPARE(calls[0], calls[2]);
        QVERIFY(!QJsonDocument(workflow.snapshot().value("agents").toObject()).toJson().contains("Global Global"));
    }

    void roleReasoningAndServiceLimitsAreIndependentOfTheCodeLimit()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        auto c = config(server, workspace.path());
        c.maxTokens = 256000;
        orchestrator::LmStudioClient client;
        client.configure(c);
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        server.enqueue(specialistReady());
        client.complete("qwen3.6-35b-a3b", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 1);
        auto payload = server.requests.last().value("payload").toObject();
        QCOMPARE(payload.value("reasoning_effort").toString(), QStringLiteral("none"));
        QCOMPARE(payload.value("max_tokens").toInt(), 8192);
        QCOMPARE(payload.value("temperature").toDouble(), 0.7);
        QCOMPARE(payload.value("presence_penalty").toDouble(), 1.5);
        server.enqueue(developerReady());
        client.complete("qwen3.6-35b-a3b", {}, orchestrator::agentSchema(AgentRole::Developer), "developer_result");
        QTRY_COMPARE(completed.count(), 2);
        payload = server.requests.last().value("payload").toObject();
        QVERIFY(!payload.contains("reasoning_effort"));
        QCOMPARE(payload.value("max_tokens").toInt(), 256000);
        QCOMPARE(payload.value("temperature").toDouble(), 0.6);
        server.enqueue(developerReady());
        client.complete("qwen3.6-35b-a3b", {}, orchestrator::agentSchema(AgentRole::Developer), "developer_result", true);
        QTRY_COMPARE(completed.count(), 3);
        payload = server.requests.last().value("payload").toObject();
        QCOMPARE(payload.value("reasoning_effort").toString(), QStringLiteral("none"));
        QCOMPARE(payload.value("max_tokens").toInt(), 256000);
        QCOMPARE(payload.value("temperature").toDouble(), 0.7);
        c.reasoningMode = orchestrator::ReasoningMode::Off;
        c.samplingProfile = orchestrator::SamplingProfile::ServerDefaults;
        client.configure(c);
        server.enqueue(specialistReady());
        client.complete("other-model", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 4);
        payload = server.requests.last().value("payload").toObject();
        QCOMPARE(payload.value("reasoning_effort").toString(), QStringLiteral("none"));
        QVERIFY(!payload.contains("temperature"));
        c.reasoningMode = orchestrator::ReasoningMode::ServerDefaults;
        c.maxServiceTokens = 0;
        client.configure(c);
        server.enqueue(specialistReady());
        client.complete("qwen3.6-35b-a3b", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 5);
        payload = server.requests.last().value("payload").toObject();
        QVERIFY(!payload.contains("reasoning_effort"));
        QCOMPARE(payload.value("max_tokens").toInt(), 256000);
    }

    void nonrepeatingReasoningIsBoundedAndTheGuardCanBeDisabled()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        auto c = config(server, workspace.path());
        c.maxReasoningCharacters = 128;
        FakeLmStudioServer::Reply reply;
        reply.stream = true;
        reply.keepOpen = true;
        for (int i = 0; i < 200; ++i)
        {
            reply.reasoning += QStringLiteral("Observation_%1 ").arg(i);
        }
        server.replies.enqueue(reply);
        orchestrator::LmStudioClient client;
        client.configure(c);
        QSignalSpy budgets(&client, &orchestrator::LmStudioClient::reasoningBudgetExceeded);
        QSignalSpy loops(&client, &orchestrator::LmStudioClient::generationLoopDetected);
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        client.complete("other-model", {}, orchestrator::agentSchema(AgentRole::Pm), "pm_result");
        QTRY_COMPARE_WITH_TIMEOUT(budgets.count(), 1, 1500);
        QCOMPARE(loops.count(), 0);
        QCOMPARE(completed.count(), 0);
        c.maxReasoningCharacters = 0;
        client.configure(c);
        reply.keepOpen = false;
        reply.content = orchestrator::jsonText(ready());
        server.replies.enqueue(reply);
        client.complete("other-model", {}, orchestrator::agentSchema(AgentRole::Pm), "pm_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(budgets.count(), 1);
    }

    void reasoningBudgetRetriesTheSameDeveloperRequestWithoutThinking_data()
    {
        QTest::addColumn<QString>("model");
        QTest::newRow("qwen") << QStringLiteral("qwen3.6-35b-a3b");
        QTest::newRow("glm") << QStringLiteral("glm-4.7-flash-30b-a3b-hellfire-turbo-brilliance-50x-agi-neo@q6_k");
        QTest::newRow("other-hybrid") << QStringLiteral("custom-hybrid");
    }

    void reasoningBudgetRetriesTheSameDeveloperRequestWithoutThinking()
    {
        QFETCH(QString, model);
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        FakeLmStudioServer::Reply reply;
        reply.stream = true;
        reply.keepOpen = true;
        for (int i = 0; i < 200; ++i)
        {
            reply.reasoning += QStringLiteral("Observation_%1 ").arg(i);
        }
        server.replies.enqueue(reply);
        tail(server);
        auto c = config(server, workspace.path());
        c.developerModel = model;
        c.maxReasoningCharacters = 128;
        Orchestrator workflow;
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto calls = requestsFor(server, c.developerModel);
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[0], calls[1]);
        QList<QJsonObject> bodies;
        for (const auto &request : server.requests)
        {
            const auto body = request.value("payload").toObject();
            if (body.value("model").toString() == c.developerModel)
            {
                bodies.append(body);
            }
        }
        QVERIFY(!bodies[0].contains("reasoning_effort"));
        QCOMPARE(bodies[1].value("reasoning_effort").toString(), QStringLiteral("none"));
        const auto failure = workflow.snapshot().value("generation_failures").toArray().first().toObject();
        QVERIFY(failure.value("reasoning_budget").toBool());
        QVERIFY(!QJsonDocument(workflow.snapshot().value("agents").toObject()).toJson().contains("Observation_"));
    }

    void repeatedReadBatchesAreBlockedDespiteChangedSummariesAndUnusedFields()
    {
        QTemporaryDir workspace;
        for (const auto &name : {"a.txt", "b.txt"})
        {
            QFile file(workspace.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name);
        }
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 4; ++i)
        {
            const auto prefix = i % 2 == 0 ? QString{} : QStringLiteral("./");
            auto response = specialistActions({tool("read_file", prefix + "a.txt", "", i % 2 == 0 ? "cat" : "ignored", {QString::number(i)}),
                                               tool("read_file", prefix + "b.txt", "", "unused", {""})});
            response.insert("summary", QStringLiteral("Different wording %1").arg(i));
            server.enqueue(response);
        }
        Orchestrator workflow;
        const auto c = config(server, workspace.path());
        workflow.start("Task", c);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains(QStringLiteral("цикл инструментов")));
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 3);
        QCOMPARE(workflow.snapshot().value("tool_loop_events").toArray().size(), 2);
        QVERIFY(workflow.snapshot().value("tool_loop_events").toArray().last().toObject().value("blocked").toBool());
        const auto calls = requestsFor(server, "test-researcher");
        QCOMPARE(calls.size(), 4);
        const auto feedback = QJsonDocument::fromJson(calls[2].last().toObject().value("content").toString().toUtf8()).object();
        const auto compact = feedback.value("tool_results").toArray().first().toObject();
        QVERIFY(compact.value("unchanged").toBool());
        QVERIFY(compact.value("content_omitted").toBool());
        QVERIFY(!compact.contains("content"));
        const auto source = compact.value("source_message").toInt();
        const auto original = QJsonDocument::fromJson(calls[2][source].toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(original.value("tool_results").toArray().first().toObject().value("content").toString(), QStringLiteral("a.txt"));
        QVERIFY(requestsFor(server, "test-pm").isEmpty());
        const auto session = workflow.snapshot().value("id");
        server.enqueue(specialistReady());
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        QString error;
        QVERIFY(workflow.retry(c, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("id"), session);
    }

    void alternatingReadsWithoutNewInformationAreAlsoBlocked()
    {
        QTemporaryDir workspace;
        for (const auto &name : {"a.txt", "b.txt"})
        {
            QFile file(workspace.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name);
        }
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 5; ++i)
        {
            server.enqueue(specialistActions({tool("read_file", i % 2 == 0 ? "a.txt" : "b.txt")}));
        }
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 4);
        QCOMPARE(requestsFor(server, "test-researcher").size(), 5);
        QVERIFY(workflow.snapshot().value("error").toString().contains(QStringLiteral("цикл инструментов")));
    }

    void agentCanFinishResearchAfterTheStagnationWarning()
    {
        QTemporaryDir workspace;
        QFile file(workspace.filePath("a.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Facts for the report");
        file.close();
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 3; ++i)
        {
            server.enqueue(specialistActions({tool("read_file", "a.txt")}));
        }
        server.enqueue(specialistReady("Report based on a.txt"));
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("tool_loop_events").toArray().size(), 1);
        QVERIFY(!workflow.snapshot().value("tool_loop_events").toArray().first().toObject().value("blocked").toBool());
        const auto calls = requestsFor(server, "test-researcher");
        const auto feedback = QJsonDocument::fromJson(calls.last().last().toObject().value("content").toString().toUtf8()).object();
        QVERIFY(feedback.contains("loop_control"));
    }

    void externalFileChangesPermitAnotherReadAfterAWarning()
    {
        QTemporaryDir workspace;
        QFile file(workspace.filePath("a.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("before");
        file.close();
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 4; ++i)
        {
            server.enqueue(specialistActions({tool("read_file", "a.txt")}));
        }
        server.enqueue(specialistReady());
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        bool changed = false;
        connect(&workflow, &Orchestrator::eventOccurred, this, [&](const QString &event) {
            if (!changed && event.contains(QStringLiteral("повторные операции без новых данных")))
            {
                changed = true;
                QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
                file.write("after");
                file.close();
            }
        });
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QVERIFY(changed);
        const auto tools = workflow.snapshot().value("tools").toArray();
        QCOMPARE(tools.size(), 4);
        const auto first = tools.first().toObject().value("results").toArray().first().toObject();
        const auto last = tools.last().toObject().value("results").toArray().first().toObject();
        QVERIFY(first.value("sha256") != last.value("sha256"));
        QCOMPARE(last.value("content").toString(), QStringLiteral("after"));
    }

    void changingCodeAllowsTheSameCommandToBeCheckedAgain()
    {
        QTemporaryDir workspace;
        QFile file(workspace.filePath("a.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("before");
        file.close();
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerActions({tool("read_file", "a.txt")}));
        const auto check = developerActions({tool("run_command", "", "", "cmake", {"-E", "echo", "checked"})});
        for (int i = 0; i < 3; ++i)
        {
            server.enqueue(check);
        }
        server.enqueue(developerActions({tool("write_file", "a.txt", "after")}));
        server.enqueue(check);
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto snapshot = workflow.snapshot();
        QCOMPARE(snapshot.value("workspace_revision").toInt(), 1);
        QCOMPARE(snapshot.value("tools").toArray().size(), 6);
        QCOMPARE(snapshot.value("tool_loop_events").toArray().size(), 1);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("after"));
    }

    void rewritingIdenticalContentsDoesNotCountAsProgress()
    {
        QTemporaryDir workspace;
        QFile file(workspace.filePath("a.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("same");
        file.close();
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(approved());
        server.enqueue(developerActions({tool("read_file", "a.txt")}));
        for (int i = 0; i < 4; ++i)
        {
            server.enqueue(developerActions({tool("write_file", "a.txt", "same")}));
        }
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QCOMPARE(workflow.snapshot().value("workspace_revision").toInt(), 0);
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 4);
        QVERIFY(workflow.snapshot().value("error").toString().contains(QStringLiteral("цикл инструментов")));
    }

    void repeatedReadErrorsStopEvenWithoutAFileFingerprint()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 4; ++i)
        {
            server.enqueue(specialistActions({tool("read_file", "missing.txt")}));
        }
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QCOMPARE(requestsFor(server, "test-researcher").size(), 4);
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 4);
        QVERIFY(workflow.snapshot().value("tool_loop_events").toArray().last().toObject().value("blocked").toBool());
        QVERIFY(workflow.snapshot().value("error").toString().contains(QStringLiteral("цикл инструментов")));
    }

    void changingDirectoryContentsPermitANewListing()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.automaticResearch = false;
        for (int i = 0; i < 4; ++i)
        {
            server.enqueue(specialistActions({tool("list_files")}));
        }
        server.enqueue(specialistReady());
        server.enqueue(ready());
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        bool changed = false;
        connect(&workflow, &Orchestrator::eventOccurred, this, [&](const QString &event) {
            if (!changed && event.contains(QStringLiteral("повторные операции без новых данных")))
            {
                changed = true;
                QFile file(workspace.filePath("external.txt"));
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write("new entry");
            }
        });
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(workflow.snapshot().value("tools").toArray().size(), 4);
        QVERIFY(changed);
    }

    void pmCriticSchemaAndRepairDoNotOfferImplementationTools()
    {
        const auto branches = orchestrator::agentSchema(AgentRole::PmCritic).value("anyOf").toArray();
        QCOMPARE(branches.size(), 2);
        for (const auto &branch : branches)
        {
            const auto properties = branch.toObject().value("properties").toObject();
            QCOMPARE(properties.value("status").toObject().value("enum").toArray(), QJsonArray{"ready"});
            QCOMPARE(properties.value("actions").toObject().value("maxItems").toInt(), 0);
        }
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue(ready());
        auto invalid = approved();
        invalid.insert("status", "actions");
        invalid.insert("actions", QJsonArray{QJsonObject{{"type", "read_file"}, {"path", "C:/invalid/repeated-plan-read"}}});
        server.enqueue(invalid);
        server.enqueue(approved());
        tail(server);
        Orchestrator workflow;
        workflow.start("Task", config(server, workspace.path()));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        const auto calls = requestsFor(server, "test-pm-critic");
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[0].size(), 2);
        QCOMPARE(calls[1].size(), 2);
        const auto repair =
            QJsonDocument::fromJson(calls[1].last().toObject().value("content").toString().toUtf8()).object().value("protocol_repair").toObject();
        QVERIFY(repair.value("error").toString().contains(QStringLiteral("инструменты недоступны")));
        QVERIFY(!QJsonDocument(calls[1]).toJson().contains("C:/invalid/repeated-plan-read"));
        QVERIFY(QJsonDocument(workflow.snapshot()).toJson().contains("C:/invalid/repeated-plan-read"));
        QVERIFY(workflow.snapshot().value("tools").toArray().isEmpty());
    }

    void firstResearchRequestRequiresAFileObservation()
    {
        QJsonArray history{orchestrator::message("system", "Instructions"), orchestrator::message("user", "Task")};
        const auto schema = orchestrator::agentRequestSchema(AgentRole::Researcher, history);
        const auto properties = schema.value("properties").toObject();
        QCOMPARE(properties.value("status").toObject().value("enum").toArray(), QJsonArray{"actions"});
        const auto actions = properties.value("actions").toObject();
        QCOMPARE(actions.value("minItems").toInt(), 1);
        QCOMPARE(actions.value("maxItems").toInt(), 4);
        const auto operations = actions.value("items").toObject().value("anyOf").toArray();
        QCOMPARE(operations.size(), 2);
        for (const auto &operation : operations)
        {
            const auto type = operation.toObject().value("properties").toObject().value("type").toObject().value("enum").toArray();
            QVERIFY(type == QJsonArray{"read_file"} || type == QJsonArray{"list_files"});
        }
        history.append(orchestrator::message("assistant", orchestrator::jsonText(specialistActions({tool("list_files")}))));
        QCOMPARE(orchestrator::agentRequestSchema(AgentRole::Researcher, history), orchestrator::agentSchema(AgentRole::Researcher));
    }

    void compactActionsAreAdaptedWithoutGrantingMorePermissions()
    {
        QJsonObject response{
            {"status", "actions"}, {"summary", "Read"}, {"result", ""}, {"actions", QJsonArray{QJsonObject{{"type", "read_file"}, {"path", "README.md"}}}}};
        QJsonObject parsed;
        QString error;
        QVERIFY(orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::Researcher, parsed, error));
        const auto action = parsed.value("actions").toArray().first().toObject();
        QCOMPARE(action.value("path").toString(), QStringLiteral("README.md"));
        QVERIFY(action.value("arguments").isArray());
        QVERIFY(action.value("program").toString().isEmpty());
        response.insert("status", "ready");
        QVERIFY(orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::Researcher, parsed, error));
        QCOMPARE(parsed.value("status").toString(), QStringLiteral("actions"));
        response.insert("result", "Claimed completion");
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::Researcher, parsed, error));
        response = specialistActions({QJsonObject{{"type", "write_file"}, {"path", "forbidden.txt"}, {"content", "bad"}}});
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::Researcher, parsed, error));
        response = specialistActions({QJsonObject{{"type", "run_command"}, {"path", ""}, {"program", "/bin/bash"}, {"arguments", QJsonArray{}}}});
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(response), AgentRole::Researcher, parsed, error));
        QVERIFY(error.contains(QStringLiteral("оболочку")));
        const auto fenced = QStringLiteral("```json\n%1\n```").arg(orchestrator::jsonText(specialistReady()));
        QVERIFY(orchestrator::parseAgentResponse(fenced, AgentRole::Researcher, parsed, error));
    }

    void reasoningSupportIsDiscoveredForAnArbitraryModelAlias()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.modelMetadata = {QJsonObject{{"key", "custom-model"},
                                            {"loaded_instances", QJsonArray{QJsonObject{{"id", "served-alias"}}}},
                                            {"capabilities", QJsonObject{{"reasoning", QJsonObject{{"allowed_options", QJsonArray{"off", "on"}}}}}}}};
        server.enqueue(specialistReady());
        orchestrator::LmStudioClient client;
        client.configure(config(server, workspace.path()));
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        client.complete("served-alias", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(server.metadataRequests.size(), 1);
        QCOMPARE(server.requests.last().value("payload").toObject().value("reasoning_effort").toString(), QStringLiteral("none"));
        QVERIFY(client.reasoningCanBeDisabled("served-alias"));
        server.enqueue(specialistReady());
        client.complete("served-alias", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 2);
        QCOMPARE(server.metadataRequests.size(), 1);
    }

    void missingReasoningMetadataRetainsTheKnownModelFallback()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.modelMetadata = {QJsonObject{{"key", "qwen3.6-35b-a3b"}, {"capabilities", QJsonObject{}}}};
        server.enqueue(specialistReady());
        orchestrator::LmStudioClient client;
        client.configure(config(server, workspace.path()));
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        client.complete("qwen3.6-35b-a3b", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(server.requests.last().value("payload").toObject().value("reasoning_effort").toString(), QStringLiteral("none"));
    }

    void unavailableNativeMetadataDoesNotBreakCompatibleServers()
    {
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.metadataAvailable = false;
        server.enqueue(specialistReady());
        orchestrator::LmStudioClient client;
        client.configure(config(server, workspace.path()));
        QSignalSpy completed(&client, &orchestrator::LmStudioClient::completionReady);
        QSignalSpy failed(&client, &orchestrator::LmStudioClient::failed);
        client.complete("ordinary-text-model", {}, orchestrator::agentSchema(AgentRole::Researcher), "researcher_result");
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(server.metadataRequests.size(), 1);
        QVERIFY(!server.requests.last().value("payload").toObject().contains("reasoning_effort"));
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

    void persistenceIncludesAllRoleContextsAndUtf8()
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
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("agents").toObject().size(), 7);
        QVERIFY(!orchestrator::SessionStore::save({{"id", "../escape"}}, "", directory.path(), error));
    }
};
QTEST_GUILESS_MAIN(OrchestrationTests)
#include "orchestration_tests.moc"