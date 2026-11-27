#include "fakelmstudioserver.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipelineexecutor.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QJsonObject reply(const QString &value, const QString &content = "result")
{
    return {{"status", "ready"}, {"summary", "done"}, {"content", content}, {"result", value}, {"actions", QJsonArray{}}};
}
pipeline::Agent agent(const QString &name)
{
    pipeline::Agent result;
    result.name = name;
    result.model = "test-model";
    result.timeout = 3;
    return result;
}
}
class PipelineTests final : public QObject
{
    Q_OBJECT
  private slots:
    void resultsChooseDifferentEdges()
    {
        const auto a = agent("A"), b = agent("B"), c = agent("C");
        pipeline::Pipeline graph{{a, b, c}, {{a.id, b.id, true}, {a.id, c.id, false}}, a.id};
        QVERIFY(graph.validate().isEmpty());
        QCOMPARE(graph.next(a.id, true), b.id);
        QCOMPARE(graph.next(a.id, false), c.id);
        pipeline::Pipeline restored;
        QString error;
        QVERIFY(pipeline::Pipeline::fromJson(graph.toJson(), restored, error));
        QCOMPARE(QJsonDocument(restored.toJson()).toJson(QJsonDocument::Compact), QJsonDocument(graph.toJson()).toJson(QJsonDocument::Compact));
        graph.edges.append({a.id, c.id, true});
        QVERIFY(!graph.validate().isEmpty());
    }
    void positiveAndNegativeRoutes_data()
    {
        QTest::addColumn<bool>("positive");
        QTest::newRow("positive") << true;
        QTest::newRow("negative") << false;
    }
    void positiveAndNegativeRoutes()
    {
        QFETCH(bool, positive);
        auto a = agent("A"), b = agent("B"), c = agent("C");
        b.automatic = false;
        c.automatic = false;
        pipeline::Pipeline graph{{a, b, c}, {{a.id, b.id, true}, {a.id, c.id, false}}, a.id};
        FakeLmStudioServer server;
        server.enqueue(reply(positive ? "positive" : "negative", "source answer"));
        server.enqueue(reply("positive", "target answer"));
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        QTRY_COMPARE(runner.current(), positive ? b.id : c.id);
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::AwaitingChoice);
        QCOMPARE(runner.context(a.id).size(), 3);
        QCOMPARE(runner.context(positive ? b.id : c.id).size(), 3);
        QVERIFY(runner.context(positive ? c.id : b.id).isEmpty());
        QVERIFY(runner.context(positive ? b.id : c.id)[1].toObject().value("content").toString().contains("source answer"));
    }
    void cyclesPauseAtTheConfiguredBudget()
    {
        auto a = agent("A"), b = agent("B");
        pipeline::Pipeline graph{{a, b}, {{a.id, b.id, true}, {b.id, a.id, true}}, a.id, 1};
        FakeLmStudioServer server;
        server.enqueue(reply("positive"));
        server.enqueue(reply("positive"));
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        QTRY_COMPARE(runner.current(), b.id);
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::AwaitingChoice);
        QCOMPARE(runner.context(a.id).size(), 3);
        QCOMPARE(runner.context(b.id).size(), 3);
    }
    void invalidResultDoesNotStartAnotherAgent()
    {
        const auto a = agent("A"), b = agent("B");
        pipeline::Pipeline graph{{a, b}, {{a.id, b.id, true}}, a.id};
        FakeLmStudioServer server;
        server.enqueue(reply("unknown"));
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Failed);
        QCOMPARE(runner.current(), a.id);
        QVERIFY(runner.context(b.id).isEmpty());
    }
    void newTaskStartsWithFreshWorkerContexts()
    {
        const auto a = agent("A"), b = agent("B");
        pipeline::Pipeline graph{{a, b}, {{a.id, b.id, true}}, a.id};
        FakeLmStudioServer server;
        server.enqueue(reply("positive", "first task handoff"));
        server.enqueue(reply("positive", "first task result"));
        server.enqueue(reply("positive", "second task handoff"));
        server.enqueue(reply("positive", "second task result"));
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "first task", connection, error));
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Finished);
        const auto firstId = runner.sessionId();
        QVERIFY(!runner.context(b.id).isEmpty());

        QVERIFY(runner.start(graph, "second task", connection, error));
        QVERIFY(runner.sessionId() != firstId);
        QVERIFY(runner.context(b.id).isEmpty());
        const auto snapshot = runner.snapshot();
        QVERIFY(snapshot.value("events").toArray().first().toObject().value("agent").toString().isEmpty());
        QVERIFY(snapshot.value("results").toObject().isEmpty());
        QCOMPARE(snapshot.value("visits").toArray().size(), 1);
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Finished);
        QCOMPARE(server.requests.size(), 4);
        for (const auto &request : server.requests.sliced(2))
        {
            const auto messages = request.value("payload").toObject().value("messages").toArray();
            QCOMPARE(messages.size(), 2);
            QCOMPARE(messages.first().toObject().value("role").toString(), QStringLiteral("system"));
            const auto user = messages.last().toObject();
            QCOMPARE(user.value("role").toString(), QStringLiteral("user"));
            const auto content = user.value("content").toString();
            QCOMPARE(QJsonDocument::fromJson(content.toUtf8()).object().value("task").toString(), QStringLiteral("second task"));
            QVERIFY(!content.contains("first task"));
        }
    }
    void loadedSessionKeepsContextsAndDoesNotRunAutomatically()
    {
        const auto a = agent("A");
        pipeline::Pipeline graph{{a}, {}, a.id};
        FakeLmStudioServer server;
        server.enqueue(reply("positive"));
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        connection.apiKey = "not-saved";
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Finished);
        const auto snapshot = runner.snapshot();
        QVERIFY(!QJsonDocument(snapshot).toJson().contains("not-saved"));
        pipeline::PipelineExecutor restored;
        QVERIFY(restored.restore(snapshot, connection, error));
        QCOMPARE(restored.state(), pipeline::PipelineExecutor::State::AwaitingChoice);
        QCOMPARE(QJsonDocument(restored.context(a.id)).toJson(QJsonDocument::Compact), QJsonDocument(runner.context(a.id)).toJson(QJsonDocument::Compact));
        QCOMPARE(restored.sessionId(), runner.sessionId());
    }
    void toolPauseDiagnostics_data()
    {
        QTest::addColumn<int>("limit");
        QTest::addColumn<QString>("reason");
        QTest::newRow("budget") << 1 << QStringLiteral("1 + 1 > 1");
        QTest::newRow("repeat") << 20 << QStringLiteral("в третий раз");
    }
    void toolPauseDiagnostics()
    {
        QFETCH(int, limit);
        QFETCH(QString, reason);
        auto a = agent("A");
        a.tools = {"find_files"};
        a.toolLimit = limit;
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        const QJsonObject action{{"type", "find_files"}, {"path", "."}, {"pattern", "*.cpp"}};
        for (int i = 0; i < 3; ++i)
            server.enqueue({{"status", "actions"}, {"summary", "find sources"}, {"actions", QJsonArray{action}}});
        orchestrator::Configuration connection;
        connection.workspace = workspace.path();
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start({{a}, {}, a.id}, "task", connection, error));
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::AwaitingChoice);
        QVERIFY(runner.resultText().contains(reason));
        QVERIFY(runner.resultText().contains("*.cpp"));
        const auto snapshot = runner.snapshot();
        const auto visit = snapshot.value("visits").toArray().last().toObject();
        QCOMPARE(visit.value("tools").toArray().size(), limit == 1 ? 1 : 2);
        QStringList events;
        for (const auto &entry : snapshot.value("events").toArray())
            events.append(entry.toObject().value("text").toString());
        const auto journal = events.join('\n');
        QVERIFY(journal.contains("find sources"));
        QVERIFY(journal.contains(QStringLiteral("совпадений: 0")));
        QVERIFY(journal.contains(reason));
    }
    void repeatedToolBatchUsesNegativeEdge_data()
    {
        QTest::addColumn<bool>("automatic");
        QTest::newRow("automatic") << true;
        QTest::newRow("manual") << false;
    }
    void repeatedToolBatchUsesNegativeEdge()
    {
        QFETCH(bool, automatic);
        auto worker = agent("Implementation");
        worker.tools = {"list_files"};
        worker.automatic = automatic;
        worker.resultField = "verdict";
        worker.negative = "blocked";
        const auto diagnostic = agent("Diagnostics");
        pipeline::Pipeline graph{{worker, diagnostic}, {{worker.id, diagnostic.id, false}}, worker.id};
        FakeLmStudioServer server;
        const QJsonArray actions{QJsonObject{{"type", "list_files"}, {"path", "."}}};
        for (int i = 0; i < 3; ++i)
            server.enqueue({{"status", "actions"}, {"summary", "inspecting workspace"}, {"actions", actions}});
        server.enqueue(reply("positive"));
        QTemporaryDir workspace;
        orchestrator::Configuration connection;
        connection.workspace = workspace.path();
        connection.baseUrl = server.baseUrl();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        if (!automatic)
        {
            QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::AwaitingChoice);
            QCOMPARE(runner.current(), worker.id);
            QVERIFY(runner.context(diagnostic.id).isEmpty());
            QVERIFY(runner.advance(false, {}, error));
        }
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Finished);
        const auto visits = runner.snapshot().value("visits").toArray();
        QCOMPARE(visits.size(), 2);
        const auto source = visits.first().toObject();
        QCOMPARE(source.value("tools").toArray().size(), 2);
        const auto result = source.value("result").toObject();
        QCOMPARE(result.value("verdict").toString(), QStringLiteral("blocked"));
        QVERIFY(result.value("content").toString().contains("list_files"));
        QVERIFY(result.value("content").toString().contains("inspecting workspace"));
        const auto target = visits.last().toObject();
        QCOMPARE(target.value("agent").toString(), diagnostic.id);
        QCOMPARE(target.value("input").toString(), pipeline::jsonText(result));
    }
    void forbiddenToolsDoNotExecute()
    {
        auto a = agent("A");
        a.tools = {"read_file"};
        pipeline::Pipeline graph{{a}, {}, a.id};
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        server.enqueue({{"status", "actions"},
                        {"summary", "write"},
                        {"actions", QJsonArray{QJsonObject{{"type", "write_file"}, {"path", "forbidden.txt"}, {"content", "bad"}}}}});
        orchestrator::Configuration connection;
        connection.baseUrl = server.baseUrl();
        connection.workspace = workspace.path();
        pipeline::PipelineExecutor runner;
        QString error;
        QVERIFY(runner.start(graph, "task", connection, error));
        QTRY_COMPARE(runner.state(), pipeline::PipelineExecutor::State::Failed);
        QVERIFY(!QFile::exists(workspace.filePath("forbidden.txt")));
    }
};
QTEST_GUILESS_MAIN(PipelineTests)
#include "orchestration_tests.moc"
