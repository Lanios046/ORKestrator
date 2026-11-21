#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

namespace orchestrator
{
struct Configuration
{
    QUrl baseUrl{QStringLiteral("http://127.0.0.1:51325/v1")};
    QString apiKey;
    QString pmModel;
    QString criticModel;
    QString developerModel;
    QString developerCriticModel;
    QString censorModel;
    QString workspace;
    int timeoutMs{1800000};
    int maxTokens{65536};
    int maxPmTurns{24};
    int maxReviews{6};
    int maxAgentTurns{256};
    int commandTimeoutMs{1800000};
    bool structuredOutput{true};
};

class LmStudioClient final : public QObject
{
    Q_OBJECT

  public:
    explicit LmStudioClient(QObject *parent = nullptr);
    ~LmStudioClient() override;
    void configure(const Configuration &configuration);
    void listModels();
    void complete(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName);
    void cancel();
    static QUrl normalizedBaseUrl(QUrl url);

  signals:
    void modelsReady(const QStringList &models);
    void completionReady(const QString &content);
    void failed(const QString &error);

  private:
    void request(const QString &path, const QJsonObject &body, bool models);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    Configuration m_configuration;
    QByteArray m_received;
};
} // namespace orchestrator