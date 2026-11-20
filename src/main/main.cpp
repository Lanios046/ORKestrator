#include "applicationstyle.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>

#include <cstdlib>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
void logOutput(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const auto text = message.toUtf8();
    switch (type)
    {
    case QtDebugMsg:
        spdlog::debug("{}", text.constData());
        break;
    case QtInfoMsg:
        spdlog::info("{}", text.constData());
        break;
    case QtWarningMsg:
        spdlog::warn("{}", text.constData());
        break;
    case QtCriticalMsg:
        spdlog::error("{}", text.constData());
        break;
    case QtFatalMsg:
        spdlog::critical("{}", text.constData());
        std::abort();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ORKestrator"));
    QCoreApplication::setApplicationName(QStringLiteral("ORKestrator"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/logs");
    try
    {
        if (QDir().mkpath(directory))
        {
            const auto logger = spdlog::rotating_logger_mt("orkestrator", (directory + "/orkestrator.log").toStdString(), 4 * 1024 * 1024, 3);
            spdlog::set_default_logger(logger);
            spdlog::flush_on(spdlog::level::info);
            qInstallMessageHandler(logOutput);
        }
    }
    catch (const spdlog::spdlog_ex &error)
    {
        qWarning() << error.what();
    }
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/orkestrator.png")));
    client::setupApplicationStyle(app);
    client::MainWindow window;
    window.show();
    return app.exec();
}