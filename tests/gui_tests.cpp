#include "fakelmstudioserver.h"
#include "main/applicationstyle.h"
#include "main/mainwindow.h"

#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QtTest>

class GuiTests final : public QObject
{
    Q_OBJECT

  private slots:
    void questionnaireToApprovedPlan()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("ORKestratorTests");
        QCoreApplication::setApplicationName("ORKestratorTests");
        QTemporaryDir settingsDirectory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        FakeLmStudioServer server;
        QSettings settings;
        settings.setValue("server", server.baseUrl().toString());
        settings.setValue("pm_model", "test-pm");
        settings.setValue("critic_model", "test-critic");
        settings.sync();
        server.enqueue(questions());
        server.enqueue(ready("# Готовый план\n\nT1. Согласовать результат."));
        server.enqueue(approved());
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + "/Fonts/segoeui.ttf");
        client::setupApplicationStyle(*qApp);
        client::MainWindow window;
        window.show();
        auto *task = window.findChild<QPlainTextEdit *>("taskInput");
        auto *start = window.findChild<QPushButton *>("startButton");
        auto *stop = window.findChild<QPushButton *>("stopButton");
        auto *submit = window.findChild<QPushButton *>("submitAnswers");
        auto *state = window.findChild<QLabel *>("stateLabel");
        QVERIFY(task && start && stop && submit && state);
        QTRY_COMPARE(server.requests.size(), 1);
        QVERIFY(!start->isEnabled());
        task->setPlainText("Разработать MVP");
        QVERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(window.findChild<QPlainTextEdit *>("answer_scope") != nullptr);
        QVERIFY(task->isReadOnly());
        QVERIFY(!start->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(submit->isEnabled());
        auto *answer = window.findChild<QPlainTextEdit *>("answer_scope");
        QCOMPARE(answer->toPlainText(), QString{});
        const auto artifacts = qEnvironmentVariable("ORKESTRATOR_TEST_ARTIFACTS");
        if (!artifacts.isEmpty())
        {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(window.grab().save(QDir(artifacts).filePath("ui-questionnaire.png")));
        }
        answer->setPlainText("MVP с локальным хранением");
        QTest::mouseClick(submit, Qt::LeftButton);
        QTRY_COMPARE(state->text(), QStringLiteral("Декомпозиция одобрена"));
        QVERIFY(!task->isReadOnly());
        QVERIFY(start->isEnabled());
        QVERIFY(!stop->isEnabled());
        QVERIFY(!submit->isEnabled());
        auto *plan = window.findChild<QTextBrowser *>("decompositionView");
        QVERIFY(plan->toPlainText().contains("Готовый план"));
        auto *reviews = window.findChild<QTextBrowser *>("reviewsView");
        QVERIFY(reviews->toPlainText().contains("Одобрено"));
        QVERIFY(window.grab().save(settingsDirectory.filePath("mainwindow.png")));
        if (!artifacts.isEmpty())
        {
            QVERIFY(window.grab().save(QDir(artifacts).filePath("ui-approved.png")));
        }
        window.close();
    }
};

QTEST_MAIN(GuiTests)
#include "gui_tests.moc"