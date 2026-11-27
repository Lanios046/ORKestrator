#include "fakelmstudioserver.h"
#include "main/mainwindow.h"

#include <QComboBox>
#include <QGraphicsView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QtTest>

class GraphGuiTests final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("ORKestratorTests");
        QCoreApplication::setApplicationName("GraphGuiTests");
        QSettings().clear();
    }
    void newTaskClearsPreviousSessionFromEditor()
    {
        client::MainWindow window;
        FakeLmStudioServer server;
        const QJsonObject response{
            {"status", "ready"}, {"summary", "first run finished"}, {"content", "old output"}, {"result", "positive"}, {"actions", QJsonArray{}}};
        server.enqueue(response);
        auto *journal = window.findChild<QPlainTextEdit *>("sessionLog");
        auto *note = window.findChild<QPlainTextEdit *>("transitionNote");
        auto *result = window.findChild<QPlainTextEdit *>("agentResult");
        auto *task = window.findChild<QPlainTextEdit *>("taskInput");
        auto *start = window.findChild<QPushButton *>("startTask");
        auto *url = window.findChild<QLineEdit *>("serverUrl");
        auto *model = window.findChild<QComboBox *>("agentModel");
        QVERIFY(journal && note && result && task && start && url && model);
        url->setText(server.baseUrl().toString());
        model->setCurrentText("test-model");
        task->setPlainText("first task");
        start->click();
        QTRY_VERIFY(journal->toPlainText().contains("first run finished"));
        QCOMPARE(result->toPlainText(), QStringLiteral("old output"));
        note->setPlainText("old note");

        task->setPlainText("second task");
        server.hold = true;
        start->click();
        QTRY_COMPARE(server.requests.size(), 2);
        QVERIFY(!journal->toPlainText().contains("first run finished"));
        QVERIFY(journal->toPlainText().contains(QStringLiteral("Новая задача")));
        QVERIFY(note->toPlainText().isEmpty());
        QVERIFY(result->toPlainText().isEmpty());
    }
    void graphHasAnAbstractAgentAndEditableInstructions()
    {
        client::MainWindow window;
        QVERIFY(window.findChild<QGraphicsView *>("pipelineGraph"));
        QVERIFY(window.findChild<QLineEdit *>("agentName"));
        QVERIFY(window.findChild<QComboBox *>("agentModel")->isEditable());
        QVERIFY(window.findChild<QPlainTextEdit *>("taskInput"));
        QVERIFY(!window.findChild<QComboBox *>("criticModel"));
    }
};
QTEST_MAIN(GraphGuiTests)
#include "gui_tests.moc"
