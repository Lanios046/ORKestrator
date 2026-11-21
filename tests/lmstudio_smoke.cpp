#include "orchestration/orchestrator.h"
#include "saving/sessionstore.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ORKestratorSmoke");
    const auto arguments = app.arguments();
    if (arguments.size() != 4)
    {
        QTextStream(stderr) << "Usage: lmstudio_smoke <model-id> <session-output-directory> <workspace>\n";
        return 2;
    }
    orchestrator::Configuration config;
    config.pmModel = config.criticModel = config.developerModel = config.developerCriticModel = config.censorModel = arguments.at(1);
    config.workspace = arguments.at(3);
    config.apiKey = qEnvironmentVariable("LM_STUDIO_API_KEY");
    config.maxReviews = 3;
    orchestrator::Orchestrator workflow;
    QObject::connect(&workflow, &orchestrator::Orchestrator::eventOccurred, &app, [](const QString &text) { QTextStream(stdout) << text << Qt::endl; });
    QObject::connect(&workflow, &orchestrator::Orchestrator::questionsReady, &app, [&workflow](const QJsonArray &questions, const QString &) {
        QJsonObject answers;
        for (const auto &value : questions)
        {
            answers.insert(value.toObject().value("id").toString(),
                           QStringLiteral("Учебный минимум: Python 3, консольный список задач без сторонних зависимостей. "
                                          "Добавление и просмотр непустых строк, без сохранения между запусками. "
                                          "Нужны реальные файлы и запуск unittest. Остальное не знаю — зафиксируй допущения."));
        }
        QTimer::singleShot(0, &workflow, [&workflow, answers]() {
            QString error;
            workflow.submitAnswers(answers, error);
        });
    });
    QObject::connect(&workflow, &orchestrator::Orchestrator::changed, &app, [&]() {
        QString error;
        if (!orchestrator::SessionStore::save(workflow.snapshot(), workflow.markdown(), arguments.at(2), error))
        {
            QTextStream(stderr) << error << Qt::endl;
            app.exit(3);
            return;
        }
        if (!workflow.active())
        {
            app.exit(workflow.state() == orchestrator::Orchestrator::State::Completed ? 0 : 1);
        }
    });
    QTimer::singleShot(0, &workflow, [&]() {
        workflow.start(QStringLiteral("Реализовать учебный консольный список задач на Python: добавление и просмотр. "
                                      "Нужны код, тесты и проверка запуска в выбранной рабочей папке."),
                       config);
    });
    return app.exec();
}