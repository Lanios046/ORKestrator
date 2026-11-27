#include "network/lmstudioclient.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

namespace orchestrator
{
namespace
{
bool validBase(const QUrl &base)
{
    return base.isValid() && (base.scheme() == "http" || base.scheme() == "https") && !base.host().isEmpty() && base.userInfo().isEmpty() && !base.hasQuery() &&
           !base.hasFragment() && base.path() == "/v1";
}
bool repetitive(const QString &text)
{
    static const QRegularExpression words(QStringLiteral("[\\p{L}][\\p{L}\\p{N}_]*"), QRegularExpression::UseUnicodePropertiesOption);
    QStringList tokens;
    auto matches = words.globalMatch(text.right(8192).toCaseFolded());
    while (matches.hasNext())
    {
        tokens.append(matches.next().captured());
    }
    tokens = tokens.mid(qMax(qsizetype{0}, tokens.size() - 128));
    for (qsizetype end = 24; end <= tokens.size(); ++end)
    {
        for (qsizetype period = 1; period <= 8 && period * 8 <= end; ++period)
        {
            qsizetype matched = period;
            while (matched < end && tokens[end - matched - 1] == tokens[end - matched - 1 + period])
            {
                ++matched;
            }
            if (matched >= 24 && matched / period >= 8)
            {
                for (qsizetype i = end - period; i < end; ++i)
                {
                    if (tokens[i].size() >= 3)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
bool repetitiveNarrative(const QJsonValue &value)
{
    if (value.isString())
    {
        return repetitive(value.toString());
    }
    if (value.isArray())
    {
        for (const auto &entry : value.toArray())
        {
            if (repetitiveNarrative(entry))
            {
                return true;
            }
        }
    }
    else if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (it.key() != "actions" && repetitiveNarrative(it.value()))
            {
                return true;
            }
        }
    }
    return false;
}
bool repetitiveResponse(const QString &content, bool containsCode)
{
    const auto document = QJsonDocument::fromJson(content.toUtf8());
    return document.isObject() ? repetitiveNarrative(document.object()) : (!containsCode && repetitive(content));
}
QString loopError()
{
    return QStringLiteral("Обнаружено зацикливание генерации модели. Ответ с повторами отклонен и не добавлен в рабочий контекст.");
}
} // namespace

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
    m_capabilitiesQueried = false;
    m_reasoningCapabilities.clear();
    m_enabledReasoningEfforts.clear();
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

bool LmStudioClient::reasoningCanBeDisabled(const QString &model) const
{
    if (m_reasoningCapabilities.contains(model))
    {
        return m_reasoningCapabilities.value(model);
    }
    return model.contains("qwen3.5", Qt::CaseInsensitive) || model.contains("qwen3.6", Qt::CaseInsensitive) || model.contains("qwen3.8", Qt::CaseInsensitive) ||
           model.contains("glm-4.7", Qt::CaseInsensitive) || model.contains("glm4.7", Qt::CaseInsensitive);
}
void LmStudioClient::fetchReasoningCapabilities(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName,
                                                bool disableReasoning)
{
    m_capabilitiesQueried = true;
    auto url = m_configuration.baseUrl;
    url.setPath("/api/v1/models");
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!m_configuration.apiKey.isEmpty())
    {
        request.setRawHeader("Authorization", "Bearer " + m_configuration.apiKey.toUtf8());
    }
    auto *reply = m_network.get(request);
    m_metadataReply = reply;
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(qMin(m_configuration.timeoutMs, 5000));
    connect(reply, &QNetworkReply::finished, this, [this, reply, deadline, model, messages, schema, schemaName, disableReasoning]() {
        if (m_metadataReply != reply)
        {
            return;
        }
        deadline->stop();
        const auto bytes = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && bytes.size() <= 8 * 1024 * 1024)
        {
            const auto models = QJsonDocument::fromJson(bytes).object().value("models").toArray();
            for (const auto &value : models)
            {
                const auto entry = value.toObject();
                const auto reasoning = entry.value("capabilities").toObject().value("reasoning");
                if (!reasoning.isObject() || !reasoning.toObject().value("allowed_options").isArray())
                {
                    continue;
                }
                const auto options = reasoning.toObject().value("allowed_options").toArray();
                const bool supported = options.contains(QStringLiteral("off")) || options.contains(QStringLiteral("none"));
                QString effort;
                for (const auto &option : {"medium", "on", "low", "high", "minimal", "xhigh"})
                {
                    if (options.contains(QString::fromLatin1(option)))
                    {
                        effort = QString::fromLatin1(option);
                        if (effort == "on")
                        {
                            effort = "medium";
                        }
                        break;
                    }
                }
                const auto key = entry.value("key").toString();
                if (!key.isEmpty())
                {
                    m_reasoningCapabilities.insert(key, supported);
                    m_enabledReasoningEfforts.insert(key, effort);
                }
                for (const auto &instance : entry.value("loaded_instances").toArray())
                {
                    const auto id = instance.toObject().value("id").toString();
                    if (!id.isEmpty())
                    {
                        m_reasoningCapabilities.insert(id, supported);
                        m_enabledReasoningEfforts.insert(id, effort);
                    }
                }
            }
        }
        m_metadataReply = nullptr;
        reply->deleteLater();
        complete(model, messages, schema, schemaName, disableReasoning);
    });
}
void LmStudioClient::complete(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName, bool disableReasoning)
{
    if (m_reply || m_metadataReply)
    {
        emit failed(QStringLiteral("Предыдущий запрос еще выполняется."));
        return;
    }
    if (!m_capabilitiesQueried && m_configuration.reasoningMode == ReasoningMode::Auto && validBase(m_configuration.baseUrl))
    {
        fetchReasoningCapabilities(model, messages, schema, schemaName, disableReasoning);
        return;
    }
    const bool developer = schemaName == "agent_result";
    const bool qwen36 = model.contains("qwen3.6", Qt::CaseInsensitive);
    const bool hybridQwen = qwen36 || model.contains("qwen3.8", Qt::CaseInsensitive);
    const bool reasoningOff = m_configuration.reasoningMode == ReasoningMode::Off ||
                              (m_configuration.reasoningMode == ReasoningMode::Auto && reasoningCanBeDisabled(model) && disableReasoning);
    const int outputLimit =
        !developer && m_configuration.maxServiceTokens > 0 ? qMin(m_configuration.maxTokens, m_configuration.maxServiceTokens) : m_configuration.maxTokens;
    m_outputTokenLimit = outputLimit;
    m_serviceResponse = !developer && m_configuration.maxServiceTokens > 0 && m_configuration.maxServiceTokens < m_configuration.maxTokens;
    QJsonObject body{{"model", model}, {"messages", messages}, {"max_tokens", outputLimit}, {"stream", true}};
    QString reasoningEffort;
    if (reasoningOff)
    {
        reasoningEffort = "none";
    }
    else if (m_configuration.reasoningMode == ReasoningMode::Auto)
    {
        reasoningEffort = m_enabledReasoningEfforts.value(model);
        if (reasoningEffort.isEmpty() && !m_reasoningCapabilities.contains(model) && reasoningCanBeDisabled(model))
        {
            reasoningEffort = "medium";
        }
    }
    if (!reasoningEffort.isEmpty())
    {
        body.insert("reasoning_effort", reasoningEffort);
    }
    auto profile = m_configuration.samplingProfile;
    if (profile == SamplingProfile::Auto)
    {
        profile = hybridQwen ? (reasoningOff ? SamplingProfile::QwenInstruct : SamplingProfile::QwenThinking) : SamplingProfile::ServerDefaults;
    }
    if (profile == SamplingProfile::QwenThinking || profile == SamplingProfile::QwenInstruct)
    {
        const bool thinking = profile == SamplingProfile::QwenThinking;
        const bool preciseCoding = thinking && qwen36 && developer;
        body.insert("temperature", thinking ? (preciseCoding ? 0.6 : 1.0) : 0.7);
        body.insert("top_p", thinking ? 0.95 : 0.8);
        body.insert("top_k", 20);
        body.insert("presence_penalty", thinking ? (qwen36 && !preciseCoding ? 1.5 : 0.0) : 1.5);
        body.insert("repeat_penalty", 1.0);
    }
    m_guardContent = schemaName != "agent_result";
    if (m_configuration.structuredOutput)
    {
        body.insert("response_format",
                    QJsonObject{{"type", "json_schema"}, {"json_schema", QJsonObject{{"name", schemaName}, {"strict", true}, {"schema", schema}}}});
    }
    const auto reasoningLabel = reasoningOff                ? QStringLiteral("выключено (none)")
                                : reasoningEffort.isEmpty() ? QStringLiteral("настройки сервера")
                                                            : QStringLiteral("включено (%1)").arg(reasoningEffort);
    emit generationConfigured(outputLimit, reasoningLabel);
    request(QStringLiteral("/chat/completions"), body, false);
}

void LmStudioClient::resetResponse()
{
    m_received.clear();
    m_streamBuffer.clear();
    m_streamContent.clear();
    m_reasoningTail.clear();
    m_finishReason.clear();
    m_streamBytes = 0;
    m_guardCharacters = 0;
    m_reasoningCharacters = 0;
    m_reportedCharacters = 0;
    m_streaming = false;
    m_streamDone = false;
}

void LmStudioClient::cancel()
{
    if (m_metadataReply)
    {
        m_capabilitiesQueried = false;
        const auto reply = m_metadataReply;
        m_metadataReply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (m_reply)
    {
        const auto reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    resetResponse();
}

void LmStudioClient::consumeReply(QNetworkReply *reply, bool models)
{
    if (m_reply != reply || !reply->property("ork_error").toString().isEmpty())
    {
        return;
    }
    const auto bytes = reply->readAll();
    m_streaming = !models && reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/event-stream", Qt::CaseInsensitive);
    auto reject = [reply](const QString &error, bool loop = false) {
        reply->setProperty("ork_error", error);
        reply->setProperty("ork_loop", loop);
        reply->abort();
    };
    if (!m_streaming)
    {
        m_received += bytes;
        if (m_received.size() > 8 * 1024 * 1024)
        {
            reject(QStringLiteral("Ответ сервера превышает 8 МиБ."));
        }
        return;
    }
    m_streamBuffer += bytes;
    if (m_streamBuffer.size() > 8 * 1024 * 1024)
    {
        reject(QStringLiteral("Буфер потока LM Studio превышает 8 МиБ."));
        return;
    }
    while (true)
    {
        const auto newline = m_streamBuffer.indexOf('\n');
        if (newline < 0)
        {
            break;
        }
        const auto line = m_streamBuffer.left(newline).trimmed();
        m_streamBuffer.remove(0, newline + 1);
        if (!line.startsWith("data:"))
        {
            continue;
        }
        const auto data = line.mid(5).trimmed();
        if (data == "[DONE]")
        {
            m_streamDone = true;
            continue;
        }
        if (m_streamDone || data.isEmpty())
        {
            continue;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            reject(QStringLiteral("Некорректный JSON в потоке LM Studio."));
            return;
        }
        if (document.object().contains("error"))
        {
            reject(QStringLiteral("LM Studio сообщил ошибку в потоке генерации."));
            return;
        }
        const auto choices = document.object().value("choices").toArray();
        if (choices.isEmpty())
        {
            continue;
        }
        const auto choice = choices.first().toObject();
        const auto delta = choice.value("delta").toObject();
        const auto content = delta.value("content").toString();
        const auto reasoning = delta.value("reasoning_content").toString() + delta.value("reasoning").toString();
        m_streamContent += content;
        m_reasoningTail = (m_reasoningTail + reasoning).right(8192);
        m_reasoningCharacters += reasoning.size();
        m_streamBytes += content.toUtf8().size() + reasoning.toUtf8().size();
        m_guardCharacters += content.size() + reasoning.size();
        const auto finish = choice.value("finish_reason");
        if (finish.isString() && !finish.toString().isEmpty())
        {
            m_finishReason = finish.toString();
        }
        if (m_streamBytes > 8 * 1024 * 1024)
        {
            reject(QStringLiteral("Ответ сервера превышает 8 МиБ."));
            return;
        }
        const auto generatedCharacters = m_streamContent.size() + m_reasoningCharacters;
        if (generatedCharacters - m_reportedCharacters >= 1024)
        {
            m_reportedCharacters = generatedCharacters;
            emit generationProgress(m_streamContent.size(), m_reasoningCharacters);
        }
        if (m_configuration.maxReasoningCharacters > 0 && m_reasoningCharacters > m_configuration.maxReasoningCharacters)
        {
            reply->setProperty("ork_reasoning_budget", true);
            reject(QStringLiteral("Превышен бюджет мышления: %1 символов без завершения ответа. Генерация остановлена.")
                       .arg(m_configuration.maxReasoningCharacters));
            return;
        }
        if (m_guardCharacters >= 128)
        {
            m_guardCharacters = 0;
            if (repetitive(m_reasoningTail) || (m_guardContent && repetitive(m_streamContent)))
            {
                reject(loopError(), true);
                return;
            }
        }
    }
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
    resetResponse();
    auto *reply = models ? m_network.get(networkRequest) : m_network.post(networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("ork_error", QStringLiteral("Истекло время ожидания LM Studio. Увеличьте таймаут или проверьте модель."));
        reply->abort();
    });
    deadline->start(models ? qMin(m_configuration.timeoutMs, 15000) : m_configuration.timeoutMs);
    connect(reply, &QIODevice::readyRead, this, [this, reply, models]() { consumeReply(reply, models); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, deadline, models]() {
        if (m_reply != reply)
        {
            return;
        }
        deadline->stop();
        consumeReply(reply, models);
        if (m_reply != reply)
        {
            return;
        }
        const auto bytes = m_received;
        const auto streamedContent = m_streamContent;
        const auto reasoning = m_reasoningTail;
        const auto finishReason = m_finishReason;
        const bool streaming = m_streaming;
        const bool done = m_streamDone;
        const bool incompleteFrame = !m_streamBuffer.trimmed().isEmpty();
        resetResponse();
        m_reply = nullptr;
        reply->deleteLater();
        const auto customError = reply->property("ork_error").toString();
        if (!customError.isEmpty())
        {
            if (reply->property("ork_reasoning_budget").toBool())
            {
                emit reasoningBudgetExceeded(customError);
            }
            else if (reply->property("ork_loop").toBool())
            {
                emit generationLoopDetected(customError);
            }
            else
            {
                emit failed(customError);
            }
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
        if (streaming && ((!done && finishReason.isEmpty()) || incompleteFrame))
        {
            emit failed(QStringLiteral("Поток LM Studio завершился до окончания ответа."));
            return;
        }
        QJsonParseError parseError{};
        const auto document =
            streaming ? QJsonDocument(QJsonObject{
                            {"choices", QJsonArray{QJsonObject{{"finish_reason", finishReason},
                                                               {"message", QJsonObject{{"content", streamedContent}, {"reasoning_content", reasoning}}}}}}})
                      : QJsonDocument::fromJson(bytes, &parseError);
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
        const auto result = choice.value("message").toObject();
        const auto content = result.value("content");
        if (m_configuration.maxReasoningCharacters > 0 &&
            result.value("reasoning_content").toString().size() + result.value("reasoning").toString().size() > m_configuration.maxReasoningCharacters)
        {
            emit reasoningBudgetExceeded(QStringLiteral("Модель превысила бюджет мышления. Ответ отклонен."));
            return;
        }
        if (repetitive(result.value("reasoning_content").toString()) || repetitive(result.value("reasoning").toString()) ||
            (content.isString() && repetitiveResponse(content.toString(), !m_guardContent)))
        {
            emit generationLoopDetected(loopError());
            return;
        }
        if (choice.value("finish_reason").toString() == "length")
        {
            if (m_serviceResponse)
            {
                emit failed(QStringLiteral("Исчерпан лимит служебного ответа (%1 токенов). Проверьте режим мышления или увеличьте «Служебный ответ». "
                                           "Затем повторите запрос; контекст сохранен.")
                                .arg(m_outputTokenLimit));
                return;
            }
            emit failed(
                QStringLiteral("Ответ обрезан по лимиту токенов. Увеличьте «Токены ответа» и нажмите «Повторить запрос»: контексты и ответы сохранятся."));
            return;
        }
        if (!content.isString() || content.toString().trimmed().isEmpty())
        {
            emit failed(QStringLiteral("Модель вернула пустой ответ или отказ вместо текста."));
            return;
        }
        emit completionReady(content.toString());
    });
}
} // namespace orchestrator
