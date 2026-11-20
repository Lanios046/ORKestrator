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

inline QJsonObject questions(const QString &id = QStringLiteral("scope"))
{
    return {{"status", "questions"},
            {"summary", "Уточните объем"},
            {"decomposition", "# Предварительный план"},
            {"questions", QJsonArray{QJsonObject{
                              {"id", id}, {"text", "Какой объем?"}, {"reason", "Определяет границы"}, {"options", QJsonArray{"MVP", "Полный продукт"}}}}}};
}

inline QJsonObject ready(const QString &plan = QStringLiteral("# План\n\nT1. Сделать. Готово: проверено."))
{
    return {{"status", "ready"}, {"summary", "План готов"}, {"decomposition", plan}, {"questions", QJsonArray{}}};
}

inline QJsonObject approved()
{
    return {{"verdict", "approved"}, {"summary", "Требования покрыты, замечаний нет"}, {"issues", QJsonArray{}}};
}

inline QJsonObject revise()
{
    return {{"verdict", "revise"},
            {"summary", "Не хватает проверки результата"},
            {"issues", QJsonArray{QJsonObject{{"severity", "blocking"},
                                              {"problem", "Нет проверки"},
                                              {"reason", "CRITIC_PRIVATE_MARKER: нельзя оценить результат"},
                                              {"requested_change", "Добавить приемку"}}}}};
}