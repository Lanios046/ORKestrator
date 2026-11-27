#include "main/mainwindow.h"

#include <QComboBox>
#include <QGraphicsView>
#include <QLineEdit>
#include <QPlainTextEdit>
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
