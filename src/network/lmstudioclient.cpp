#include "lmstudioclient.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace orchestrator
{
LmStudioClient::LmStudioClient(QObject *parent) : QObject(parent)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
}

LmStudioClient::~LmStudioClient()
{
    cancel();
}

void LmStudioClient::configure(const Configuration &configuration)
{
    m_configuration = configuration;
    m_configuration.baseUrl = normalizedBaseUrl(configuration.baseUrl);
}

QUrl LmStudioClient::normalizedBaseUrl(QUrl url)
{
    auto path = url.path();
    while (path.endsWith('/'))
    {
        path.chop(1);
    }
    url.setPath(path.isEmpty() ? QStringLiteral("/v1") : path);
    return url;
}

void LmStudioClient::listModels()
{
    request(QStringLiteral("/models"), {}, true);
}

void LmStudioClient::complete(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName)
{
    QJsonObject body{{"model", model}, {"messages", messages}, {"temperature", 0.2}, {"max_tokens", m_configuration.maxTokens}, {"stream", false}};
    if (m_configuration.structuredOutput)
    {
        body.insert("response_format",
                    QJsonObject{{"type", "json_schema"}, {"json_schema", QJsonObject{{"name", schemaName}, {"strict", true}, {"schema", schema}}}});
    }
    request(QStringLiteral("/chat/completions"), body, false);
}

void LmStudioClient::cancel()
{
    if (m_reply)
    {
        const auto reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_received.clear();
}

void LmStudioClient::request(const QString &path, const QJsonObject &body, bool models)
{
    if (m_reply)
    {
        emit failed(QStringLiteral("Предыдущий запрос еще выполняется."));
        return;
    }
    const auto base = m_configuration.baseUrl;
    if (!base.isValid() || (base.scheme() != "http" && base.scheme() != "https") || base.host().isEmpty() || !base.userInfo().isEmpty() || base.hasQuery() ||
        base.hasFragment() || base.path() != "/v1")
    {
        emit failed(QStringLiteral("Укажите URL вида http://127.0.0.1:51325 или http://127.0.0.1:51325/v1."));
        return;
    }
    QUrl url = base;
    url.setPath(base.path() + path);
    QNetworkRequest networkRequest(url);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!m_configuration.apiKey.isEmpty())
    {
        networkRequest.setRawHeader("Authorization", "Bearer " + m_configuration.apiKey.toUtf8());
    }
    m_received.clear();
    auto *reply = models ? m_network.get(networkRequest) : m_network.post(networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("ork_error", QStringLiteral("Истекло время ожидания LM Studio. Увеличьте таймаут или проверьте модель."));
        reply->abort();
    });
    deadline->start(models ? qMin(m_configuration.timeoutMs, 15000) : m_configuration.timeoutMs);
    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        m_received += reply->readAll();
        if (m_received.size() > 8 * 1024 * 1024)
        {
            reply->setProperty("ork_error", QStringLiteral("Ответ сервера превышает 8 МиБ."));
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, deadline, models]() {
        deadline->stop();
        m_received += reply->readAll();
        const auto bytes = m_received;
        m_received.clear();
        m_reply = nullptr;
        reply->deleteLater();
        const auto customError = reply->property("ork_error").toString();
        if (!customError.isEmpty())
        {
            emit failed(customError);
            return;
        }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        {
            QString detail = reply->errorString();
            const auto serverError = QJsonDocument::fromJson(bytes).object().value("error");
            if (serverError.isObject())
            {
                detail = serverError.toObject().value("message").toString(detail);
            }
            else if (serverError.isString())
            {
                detail = serverError.toString();
            }
            emit failed(QStringLiteral("LM Studio: HTTP %1 — %2. Адрес: %3").arg(status).arg(detail.left(1200), reply->url().toString()));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            emit failed(QStringLiteral("Сервер вернул некорректный JSON."));
            return;
        }
        if (models)
        {
            if (!document.object().value("data").isArray())
            {
                emit failed(QStringLiteral("В ответе /v1/models отсутствует массив data."));
                return;
            }
            QStringList ids;
            for (const auto &value : document.object().value("data").toArray())
            {
                const auto id = value.toObject().value("id").toString();
                if (!id.isEmpty() && !id.contains("embedding", Qt::CaseInsensitive))
                {
                    ids.append(id);
                }
            }
            ids.removeDuplicates();
            emit modelsReady(ids);
            return;
        }
        const auto choices = document.object().value("choices").toArray();
        if (choices.isEmpty())
        {
            emit failed(QStringLiteral("В ответе LM Studio отсутствуют choices."));
            return;
        }
        const auto choice = choices.first().toObject();
        if (choice.value("finish_reason").toString() == "length")
        {
            emit failed(
                QStringLiteral("Ответ обрезан по лимиту токенов. Увеличьте «Токены ответа» и нажмите «Повторить запрос»: контексты и ответы сохранятся."));
            return;
        }
        const auto content = choice.value("message").toObject().value("content");
        if (!content.isString() || content.toString().trimmed().isEmpty())
        {
            emit failed(QStringLiteral("Модель вернула пустой ответ или отказ вместо текста."));
            return;
        }
        emit completionReady(content.toString());
    });
}
} // namespace orchestrator