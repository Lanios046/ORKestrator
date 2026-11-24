#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

namespace orchestrator
{
enum class SamplingProfile
{
    Auto,
    ServerDefaults,
    QwenThinking,
    QwenInstruct
};

enum class ReasoningMode
{
    Auto,
    ServerDefaults,
    Off
};

struct Configuration
{
    QUrl baseUrl{QStringLiteral("http://127.0.0.1:51325/v1")};
    QString apiKey;
    QString pmModel;
    QString criticModel;
    QString developerModel;
    QString developerCriticModel;
    QString censorModel;
    QString researcherModel;
    QString diagnosticianModel;
    QString workspace;
    int timeoutMs{1800000};
    int maxTokens{65536};
    int maxServiceTokens{8192};
    int maxReasoningCharacters{24000};
    int maxPmTurns{10000};
    int maxReviews{10000};
    int maxAgentTurns{100000};
    int commandTimeoutMs{1800000};
    int maxResearchTurns{1000};
    int maxDiagnosticTurns{1000};
    int maxDiagnosticsPerStage{1000};
    bool structuredOutput{true};
    SamplingProfile samplingProfile{SamplingProfile::Auto};
    ReasoningMode reasoningMode{ReasoningMode::Auto};
};

class LmStudioClient final : public QObject
{
    Q_OBJECT

  public:
    explicit LmStudioClient(QObject *parent = nullptr);
    ~LmStudioClient() override;
    void configure(const Configuration &configuration);
    void listModels();
    void complete(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName, bool disableReasoning = false);
    void cancel();
    static QUrl normalizedBaseUrl(QUrl url);
    bool reasoningCanBeDisabled(const QString &model) const;

  signals:
    void modelsReady(const QStringList &models);
    void completionReady(const QString &content);
    void failed(const QString &error);
    void generationLoopDetected(const QString &error);
    void reasoningBudgetExceeded(const QString &error);
    void generationConfigured(int maxTokens, const QString &reasoning);
    void generationProgress(qint64 contentCharacters, qint64 reasoningCharacters);

  private:
    void request(const QString &path, const QJsonObject &body, bool models);
    void consumeReply(QNetworkReply *reply, bool models);
    void fetchReasoningCapabilities(const QString &model, const QJsonArray &messages, const QJsonObject &schema, const QString &schemaName,
                                    bool disableReasoning);
    void resetResponse();

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply, m_metadataReply;
    QHash<QString, bool> m_reasoningCapabilities;
    bool m_capabilitiesQueried{false};
    Configuration m_configuration;
    QByteArray m_received, m_streamBuffer;
    QString m_streamContent, m_reasoningTail, m_finishReason;
    qsizetype m_streamBytes{0}, m_guardCharacters{0}, m_reasoningCharacters{0}, m_reportedCharacters{0};
    bool m_streaming{false}, m_streamDone{false}, m_guardContent{true}, m_serviceResponse{false};
    int m_outputTokenLimit{0};
};
} // namespace orchestrator