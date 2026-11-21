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
// Qt 6.4.3 leaves meType unset in the unsupported mouse-action branch.
// Keep this MSVC workaround scoped to the third-party Qt Test headers.
#if defined(_MSC_VER) && QT_VERSION == QT_VERSION_CHECK(6, 4, 3)
#pragma warning(push)
#pragma warning(disable : 4701)
#endif
#include <QtTest>
#if defined(_MSC_VER) && QT_VERSION == QT_VERSION_CHECK(6, 4, 3)
#pragma warning(pop)
#endif

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
        QTemporaryDir workspace;
        FakeLmStudioServer server;
        QSettings settings;
        settings.setValue("server", server.baseUrl().toString());
        settings.setValue("pm_model", "test-pm");
        settings.setValue("critic_model", "test-critic");
        settings.setValue("workspace", workspace.path());
        settings.sync();
        server.enqueue(questions());
        server.enqueue(ready("# Готовый план\n\nT1. Согласовать результат."));
        server.enqueue(approved());
        server.enqueue(developerReady("Готовая реализация"));
        server.enqueue(approved());
        server.enqueue(censorApproved());
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
        QVERIFY(window.findChild<QComboBox *>("developerModel"));
        QVERIFY(window.findChild<QComboBox *>("developerCriticModel"));
        QVERIFY(window.findChild<QComboBox *>("censorModel"));
        QVERIFY(window.findChild<QLineEdit *>("workspacePath"));
        QVERIFY(window.findChild<QPushButton *>("retryButton"));
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
        QTRY_COMPARE(state->text(), QStringLiteral("Задача завершена — цензор одобрил"));
        QVERIFY(!task->isReadOnly());
        QVERIFY(start->isEnabled());
        QVERIFY(!stop->isEnabled());
        QVERIFY(!submit->isEnabled());
        auto *plan = window.findChild<QTextBrowser *>("decompositionView");
        QVERIFY(plan->toPlainText().contains("Готовый план"));
        QVERIFY(window.findChild<QTextBrowser *>("implementationView")->toPlainText().contains("Готовая реализация"));
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