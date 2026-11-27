#include "pipeline/pipeline.h"
#include "pipeline/pipelineexecutor.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() < 3)
    {
        QTextStream(stderr) << "Usage: lmstudio_smoke pipeline.json task [workspace] [server]\n";
        return 2;
    }
    QJsonObject json;
    pipeline::Pipeline graph;
    QString error;
    if (!pipeline::readJson(arguments[1], json, error) || !pipeline::Pipeline::fromJson(json, graph, error))
    {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }
    orchestrator::Configuration connection;
    connection.apiKey = qEnvironmentVariable("LM_STUDIO_API_KEY");
    if (arguments.size() > 3)
        connection.workspace = arguments[3];
    if (arguments.size() > 4)
        connection.baseUrl = QUrl(arguments[4]);
    pipeline::PipelineExecutor runner;
    QObject::connect(&runner, &pipeline::PipelineExecutor::eventOccurred, &app, [](const QString &message) { QTextStream(stdout) << message << Qt::endl; });
    QObject::connect(&runner, &pipeline::PipelineExecutor::stateChanged, &app, [&](pipeline::PipelineExecutor::State state) {
        if (state == pipeline::PipelineExecutor::State::Finished)
            app.exit(0);
        if (state == pipeline::PipelineExecutor::State::Failed)
            app.exit(1);
    });
    if (!runner.start(graph, arguments[2], connection, error))
    {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }
    return app.exec();
}
