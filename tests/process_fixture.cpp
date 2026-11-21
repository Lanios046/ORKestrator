#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("sleep"))
    {
        QTimer::singleShot(10000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    QTextStream(stdout) << "fixture-ok\n";
    return 0;
}