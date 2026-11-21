#pragma once

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>

class FakeLmStudioServer final : public QTcpServer
{
  public:
    struct Reply
    {
        QString content;
        int status{200};
        QString finishReason{QStringLiteral("stop")};
    };

    explicit FakeLmStudioServer(QObject *parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections())
            {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { consume(socket); });
            }
        });
        listen(QHostAddress::LocalHost, 0);
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(serverPort()));
    }

    void enqueue(const QJsonObject &object)
    {
        replies.enqueue({QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))});
    }

    QList<QJsonObject> requests;
    QQueue<Reply> replies;
    bool hold{false};

  private:
    void consume(QTcpSocket *socket)
    {
        if (socket->property("handled").toBool())
        {
            return;
        }
        auto bytes = socket->property("buffer").toByteArray() + socket->readAll();
        socket->setProperty("buffer", bytes);
        const auto separator = bytes.indexOf("\r\n\r\n");
        if (separator < 0)
        {
            return;
        }
        const auto headers = bytes.left(separator).split('\n');
        int length = 0;
        for (const auto &header : headers)
        {
            if (header.toLower().startsWith("content-length:"))
            {
                length = header.mid(header.indexOf(':') + 1).trimmed().toInt();
            }
        }
        if (bytes.size() < separator + 4 + length)
        {
            return;
        }
        socket->setProperty("handled", true);
        const auto path = QString::fromUtf8(headers.first().split(' ').value(1));
        const auto payload = QJsonDocument::fromJson(bytes.mid(separator + 4, length)).object();
        requests.append({{"path", path}, {"payload", payload}});
        if (hold)
        {
            return;
        }
        QByteArray response;
        int status = 200;
        if (path == "/v1/models")
        {
            response = QJsonDocument(QJsonObject{{"data", QJsonArray{QJsonObject{{"id", "test-pm"}}, QJsonObject{{"id", "test-critic"}},
                                                                     QJsonObject{{"id", "text-embedding-test"}}}}})
                           .toJson(QJsonDocument::Compact);
        }
        else if (replies.isEmpty())
        {
            status = 500;
            response = R"({"error":{"message":"No queued response in test server"}})";
        }
        else
        {
            const auto reply = replies.dequeue();
            status = reply.status;
            const QJsonObject envelope =
                status == 200 ? QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", reply.finishReason},
                                                                               {"message", QJsonObject{{"role", "assistant"}, {"content", reply.content}}}}}}}
                              : QJsonObject{{"error", QJsonObject{{"message", reply.content}}}};
            response = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        }
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
        socket->disconnectFromHost();
    }
};

inline QJsonArray mockStages(int count = 1)
{
    QJsonArray stages;
    for (int i = 1; i <= count; ++i)
    {
        QJsonArray dependencies;
        if (i > 1)
        {
            dependencies.append(QStringLiteral("S%1").arg(i - 1));
        }
        stages.append(QJsonObject{{"id", QStringLiteral("S%1").arg(i)},
                                  {"title", QStringLiteral("Stage %1").arg(i)},
                                  {"goal", "Implement stage"},
                                  {"acceptance_criteria", QJsonArray{"Result is verified"}},
                                  {"depends_on", dependencies}});
    }
    return stages;
}
inline QJsonObject questions(const QString &id = QStringLiteral("scope"))
{
    return {{"status", "questions"},
            {"summary", "Уточните объем"},
            {"decomposition", "# Черновик"},
            {"stages", QJsonArray{}},
            {"questions", QJsonArray{QJsonObject{
                              {"id", id}, {"text", "Какой объем?"}, {"reason", "Определяет границы"}, {"options", QJsonArray{"MVP", "Полный продукт"}}}}}};
}
inline QJsonObject ready(const QString &plan = QStringLiteral("# Готовый план"), const QJsonArray &stages = mockStages())
{
    return {{"status", "ready"}, {"summary", "План готов"}, {"decomposition", plan}, {"questions", QJsonArray{}}, {"stages", stages}};
}
inline QJsonObject approved()
{
    return {{"status", "ready"}, {"verdict", "approved"}, {"summary", "Требования покрыты"}, {"issues", QJsonArray{}}, {"actions", QJsonArray{}}};
}
inline QJsonObject revise()
{
    return {{"status", "ready"},
            {"verdict", "revise"},
            {"summary", "Нужна проверка"},
            {"actions", QJsonArray{}},
            {"issues",
             QJsonArray{QJsonObject{
                 {"severity", "blocking"}, {"problem", "Нет проверки"}, {"reason", "Результат не подтвержден"}, {"requested_change", "Добавить проверку"}}}}};
}
inline QJsonObject censorApproved()
{
    auto result = approved();
    result.insert("return_to", "none");
    result.insert("stage_id", "");
    return result;
}
inline QJsonObject censorRevise(const QString &target, const QString &stage)
{
    auto result = revise();
    result.insert("return_to", target);
    result.insert("stage_id", stage);
    return result;
}
inline QJsonObject developerReady(const QString &report = QStringLiteral("Этап реализован"))
{
    return {{"status", "ready"}, {"summary", "Готово"}, {"result", report}, {"actions", QJsonArray{}}, {"questions", QJsonArray{}}};
}
inline QJsonObject tool(const QString &type, const QString &path = {}, const QString &content = {}, const QString &program = {},
                        const QJsonArray &arguments = {})
{
    return {{"type", type}, {"path", path}, {"content", content}, {"program", program}, {"arguments", arguments}};
}
inline QJsonObject developerActions(const QJsonArray &actions)
{
    return {{"status", "actions"}, {"summary", "Операции"}, {"result", ""}, {"actions", actions}, {"questions", QJsonArray{}}};
}