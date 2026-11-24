#pragma once

#include "orchestration/orchestrator.h"

#include <QMainWindow>
#include <QMap>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTextBrowser;

namespace client
{
class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void setupUi();
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void refreshModels();
    void startTask();
    void retryRequest();
    void showQuestions(const QJsonArray &questions, const QString &summary);
    void clearQuestions();
    void submitAnswers();
    void updateState(orchestrator::Orchestrator::State state);
    void autosave();
    void exportResult(bool json);
    orchestrator::Configuration configuration() const;

    struct
    {
        QGroupBox *connectionGroup{nullptr};
        QLineEdit *server{nullptr};
        QLineEdit *apiKey{nullptr};
        QComboBox *pmModel{nullptr};
        QComboBox *criticModel{nullptr};
        QComboBox *developerModel{nullptr};
        QComboBox *developerCriticModel{nullptr};
        QComboBox *censorModel{nullptr};
        QComboBox *researcherModel{nullptr};
        QComboBox *diagnosticianModel{nullptr};
        QLineEdit *workspace{nullptr};
        QPushButton *refreshModels{nullptr};
        QGroupBox *limits{nullptr};
        QSpinBox *timeout{nullptr};
        QSpinBox *tokens{nullptr};
        QSpinBox *reviews{nullptr};
        QSpinBox *pmTurns{nullptr};
        QSpinBox *agentTurns{nullptr};
        QSpinBox *commandTimeout{nullptr};
        QSpinBox *researchTurns{nullptr};
        QSpinBox *diagnosticTurns{nullptr};
        QSpinBox *diagnosticRuns{nullptr};
        QCheckBox *structuredOutput{nullptr};
        QComboBox *samplingProfile{nullptr};
        QComboBox *reasoningMode{nullptr};
        QSpinBox *serviceTokens{nullptr};
        QSpinBox *reasoningCharacters{nullptr};
        QPlainTextEdit *task{nullptr};
        QPushButton *loadTask{nullptr};
        QPushButton *start{nullptr};
        QPushButton *stop{nullptr};
        QPushButton *retry{nullptr};
        QLabel *stage{nullptr};
        QLabel *state{nullptr};
        QProgressBar *progress{nullptr};
        QTabWidget *inputTabs{nullptr};
        QScrollArea *questions{nullptr};
        QPushButton *submit{nullptr};
        QTabWidget *outputTabs{nullptr};
        QTextBrowser *plan{nullptr};
        QTextBrowser *implementation{nullptr};
        QPlainTextEdit *terminal{nullptr};
        QTextBrowser *reviewsView{nullptr};
        QTextBrowser *specialists{nullptr};
        QPlainTextEdit *log{nullptr};
        QPushButton *exportMarkdown{nullptr};
        QPushButton *exportJson{nullptr};
        QLabel *saveStatus{nullptr};
    } ui;

    orchestrator::Orchestrator m_orchestrator;
    orchestrator::LmStudioClient m_modelsClient;
    QMap<QString, QPlainTextEdit *> m_inputs;
    QString m_reviewsMarkdown, m_specialistsMarkdown;
    bool m_saveErrorShown{false};
};
} // namespace client