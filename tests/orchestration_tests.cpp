#include "fakelmstudioserver.h"
#include "orchestration/orchestrator.h"
#include "orchestration/protocol.h"
#include "saving/sessionstore.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using orchestrator::Orchestrator;

class OrchestrationTests final : public QObject
{
    Q_OBJECT

  private:
    static orchestrator::Configuration config(const FakeLmStudioServer &server)
    {
        orchestrator::Configuration configuration;
        configuration.baseUrl = server.baseUrl();
        configuration.pmModel = QStringLiteral("test-pm");
        configuration.criticModel = QStringLiteral("test-critic");
        configuration.timeoutMs = 3000;
        return configuration;
    }

  private slots:
    void questionnaireRevisionAndCriticIsolation()
    {
        FakeLmStudioServer server;
        QVERIFY(server.isListening());
        server.enqueue(questions());
        server.enqueue(ready("# Версия 1"));
        server.enqueue(revise());
        server.enqueue(questions("acceptance"));
        server.enqueue(ready("# Версия 2\n\nT1. Реализация.\nT2. Приемка, зависит от T1."));
        server.enqueue(approved());
        Orchestrator workflow;
        QSignalSpy failures(&workflow, &Orchestrator::failed);
        auto configuration = config(server);
        configuration.apiKey = "private-api-key";
        workflow.start("TASK_ORIGINAL", configuration);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QCOMPARE(server.requests.size(), 1);
        QString error;
        QVERIFY(!workflow.submitAnswers({}, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(workflow.submitAnswers({{"scope", "MVP"}}, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QCOMPARE(server.requests.size(), 4);
        QVERIFY(workflow.submitAnswers({{"acceptance", "Все сценарии проходят"}}, error));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(server.requests.size(), 6);
        QCOMPARE(failures.count(), 0);

        for (const int index : {2, 5})
        {
            const auto payload = server.requests[index].value("payload").toObject();
            QCOMPARE(payload.value("model").toString(), "test-critic");
            const auto messages = payload.value("messages").toArray();
            QCOMPARE(messages.size(), 2);
            QCOMPARE(messages.first().toObject().value("role").toString(), "system");
            const auto data = QJsonDocument::fromJson(messages.last().toObject().value("content").toString().toUtf8()).object();
            QCOMPARE(data.value("task").toString(), "TASK_ORIGINAL");
            QCOMPARE(data.keys().size(), 3);
            QVERIFY(!data.contains("critic_feedback"));
            QCOMPARE(data.value("answers").toArray().size(), index == 2 ? 1 : 2);
            QVERIFY(!messages.last().toObject().value("content").toString().contains("CRITIC_PRIVATE_MARKER"));
        }
        const auto pmAfterCritic = server.requests[3].value("payload").toObject().value("messages").toArray();
        QVERIFY(QJsonDocument(pmAfterCritic).toJson().contains("CRITIC_PRIVATE_MARKER"));
        const auto snapshot = workflow.snapshot();
        QVERIFY(snapshot.value("approved").toBool());
        QCOMPARE(snapshot.value("reviews").toArray().size(), 2);
        QCOMPARE(snapshot.value("answers").toArray().size(), 2);
        QCOMPARE(snapshot.value("exchanges").toArray().size(), 6);
        QVERIFY(!QJsonDocument(snapshot).toJson().contains("private-api-key"));
        QVERIFY(workflow.markdown().contains("MVP"));
    }

    void malformedResponsesAreRepairedWithinTheSameAgent()
    {
        FakeLmStudioServer server;
        server.replies.enqueue({"not JSON"});
        server.enqueue(ready());
        auto contradictory = revise();
        contradictory.insert("verdict", "approved");
        server.enqueue(contradictory);
        server.enqueue(approved());
        Orchestrator workflow;
        workflow.start("Тест формата", config(server));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(server.requests.size(), 4);
        QCOMPARE(server.requests[1].value("payload").toObject().value("messages").toArray().size(), 4);
        QCOMPARE(server.requests[2].value("payload").toObject().value("messages").toArray().size(), 2);
        QCOMPARE(server.requests[3].value("payload").toObject().value("messages").toArray().size(), 4);
        QCOMPARE(workflow.snapshot().value("exchanges").toArray().size(), 4);
    }

    void repeatedInvalidJsonFailsWithoutInventingApproval()
    {
        FakeLmStudioServer server;
        for (int i = 0; i < 3; ++i)
        {
            server.replies.enqueue({"not JSON"});
        }
        Orchestrator workflow;
        QSignalSpy failures(&workflow, &Orchestrator::failed);
        workflow.start("Тест", config(server));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QCOMPARE(server.requests.size(), 3);
        QCOMPARE(failures.count(), 1);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        QVERIFY(workflow.snapshot().value("reviews").toArray().isEmpty());
    }

    void cancellationAllowsCleanNewSession()
    {
        FakeLmStudioServer server;
        server.hold = true;
        Orchestrator workflow;
        QSignalSpy failures(&workflow, &Orchestrator::failed);
        workflow.start("OLD_TASK", config(server));
        QTRY_COMPARE(server.requests.size(), 1);
        workflow.stop();
        QCOMPARE(workflow.state(), Orchestrator::State::Stopped);
        server.hold = false;
        server.enqueue(ready());
        server.enqueue(approved());
        workflow.start("NEW_TASK", config(server));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Completed);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(server.requests.size(), 3);
        QVERIFY(!QJsonDocument(server.requests[1]).toJson().contains("OLD_TASK"));
        QCOMPARE(workflow.snapshot().value("task").toString(), "NEW_TASK");
    }

    void timeoutAndHttpErrors()
    {
        FakeLmStudioServer server;
        server.hold = true;
        auto configuration = config(server);
        configuration.timeoutMs = 80;
        Orchestrator workflow;
        workflow.start("Тест таймаута", configuration);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains("время ожидания"));
        server.hold = false;
        server.replies.enqueue({"Model unavailable", 503});
        workflow.start("Тест HTTP", config(server));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains("503"));
        QVERIFY(!workflow.snapshot().value("approved").toBool());
    }

    void truncatedGenerationIsRejected()
    {
        FakeLmStudioServer server;
        server.replies.enqueue({orchestrator::jsonText(ready()), 200, "length"});
        Orchestrator workflow;
        workflow.start("Тест обрезанного ответа", config(server));
        QTRY_COMPARE(workflow.state(), Orchestrator::State::Failed);
        QVERIFY(workflow.snapshot().value("error").toString().contains("обрезан"));
        QVERIFY(workflow.snapshot().value("decomposition").toString().isEmpty());
    }

    void reviewLimitKeepsDraft()
    {
        FakeLmStudioServer server;
        server.enqueue(ready());
        server.enqueue(revise());
        server.enqueue(ready("# Исправленный черновик"));
        auto configuration = config(server);
        configuration.maxReviews = 1;
        Orchestrator workflow;
        workflow.start("Тест лимита", configuration);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QCOMPARE(server.requests.size(), 3);
        QVERIFY(!workflow.snapshot().value("approved").toBool());
        QVERIFY(workflow.markdown().contains("черновик"));
    }

    void questionLimitKeepsAnswers()
    {
        FakeLmStudioServer server;
        server.enqueue(questions());
        auto configuration = config(server);
        configuration.maxPmTurns = 1;
        Orchestrator workflow;
        workflow.start("Тест лимита PM", configuration);
        QTRY_COMPARE(workflow.state(), Orchestrator::State::AwaitingAnswers);
        QString error;
        QVERIFY(workflow.submitAnswers({{"scope", "не знаю"}}, error));
        QCOMPARE(workflow.state(), Orchestrator::State::LimitReached);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(workflow.snapshot().value("answers").toArray().size(), 1);
    }

    void modelDiscoveryAndBaseUrl()
    {
        FakeLmStudioServer server;
        orchestrator::LmStudioClient client;
        auto configuration = config(server);
        configuration.baseUrl.setPath("/");
        client.configure(configuration);
        QSignalSpy models(&client, &orchestrator::LmStudioClient::modelsReady);
        client.listModels();
        QTRY_COMPARE(models.count(), 1);
        QCOMPARE(models.first().first().toStringList(), QStringList({"test-pm", "test-critic"}));
        QCOMPARE(server.requests.first().value("path").toString(), "/v1/models");
    }

    void persistenceIncludesStatusAndPreservesUtf8()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString error;
        const QJsonObject snapshot{{"id", "test-session"}, {"approved", false}, {"task", "Задача: кириллица"}};
        QVERIFY(orchestrator::SessionStore::save(snapshot, "# Черновик\n\nКириллица", directory.path(), error));
        QFile json(directory.filePath("test-session.json"));
        QVERIFY(json.open(QIODevice::ReadOnly));
        const auto read = QJsonDocument::fromJson(json.readAll()).object();
        QCOMPARE(read.value("task").toString(), "Задача: кириллица");
        QVERIFY(!read.value("approved").toBool());
        QFile markdown(directory.filePath("test-session.md"));
        QVERIFY(markdown.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(markdown.readAll()), "# Черновик\n\nКириллица");
        QVERIFY(!orchestrator::SessionStore::save({{"id", "../escape"}}, "", directory.path(), error));
    }

    void duplicateQuestionIdsAndUnreasonedCriticismAreInvalid()
    {
        auto response = questions();
        auto list = response.value("questions").toArray();
        list.append(list.first());
        response.insert("questions", list);
        QJsonObject result;
        QString error;
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(response), false, result, error));
        auto review = revise();
        review.insert("issues", QJsonArray{});
        QVERIFY(!orchestrator::parseAgentResponse(orchestrator::jsonText(review), true, result, error));
        QVERIFY(orchestrator::parseAgentResponse(orchestrator::jsonText(approved()), true, result, error));
    }
};

QTEST_GUILESS_MAIN(OrchestrationTests)
#include "orchestration_tests.moc"