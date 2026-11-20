#include "orchestration/orchestrator.h"
#include "saving/sessionstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ORKestratorSmoke");
    const auto arguments = app.arguments();
    if (arguments.size() < 3)
    {
        QTextStream(stderr) << "Usage: lmstudio_smoke <model-id> <output-directory>\n";
        return 2;
    }
    orchestrator::Configuration config;
    config.pmModel = arguments.at(1);
    config.criticModel = arguments.at(1);
    config.maxReviews = 3;
    config.maxPmTurns = 6;
    config.apiKey = qEnvironmentVariable("LM_STUDIO_API_KEY");
    orchestrator::Orchestrator workflow;
    QObject::connect(&workflow, &orchestrator::Orchestrator::eventOccurred, &app, [](const QString &text) { QTextStream(stdout) << text << Qt::endl; });
    QObject::connect(&workflow, &orchestrator::Orchestrator::questionsReady, &app, [&workflow](const QJsonArray &questions, const QString &) {
        QJsonObject answers;
        for (const auto &value : questions)
        {
            answers.insert(
                value.toObject().value("id").toString(),
                QStringLiteral("Согласен с минимальным вариантом. Консоль Windows, UTF-8 TXT, одна строка на задачу, без хранения данных между запусками; "
                               "добавление и просмотр задач, без удаления и редактирования. Готовность: после добавления задача отображается в списке; "
                               "пустой ввод не принимается. Остальное не знаю — зафиксируй допущения."));
        }
        QTimer::singleShot(0, &workflow, [&workflow, answers]() {
            QString error;
            workflow.submitAnswers(answers, error);
        });
    });
    QObject::connect(&workflow, &orchestrator::Orchestrator::changed, &app, [&]() {
        const auto snapshot = workflow.snapshot();
        if (!snapshot.value("id").toString().isEmpty())
        {
            QString error;
            if (!orchestrator::SessionStore::save(snapshot, workflow.markdown(), arguments.at(2), error))
            {
                QTextStream(stderr) << error << Qt::endl;
                app.exit(3);
            }
        }
        if (!workflow.active())
        {
            app.exit(workflow.state() == orchestrator::Orchestrator::State::Completed ? 0 : 1);
        }
    });
    QTimer::singleShot(0, &workflow, [&]() {
        workflow.start(QStringLiteral("Нужен простой список задач. Помоги уточнить требования и составить декомпозицию. "
                                      "Это учебный MVP одного разработчика, без внешних библиотек и сервера."),
                       config);
    });
    QTimer::singleShot(900000, &app, [&]() {
        workflow.stop();
        app.exit(4);
    });
    return app.exec();
}