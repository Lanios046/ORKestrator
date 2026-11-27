#pragma once
#include "pipeline/agentgraph.h"
#include "pipeline/pipelineexecutor.h"

#include <QMainWindow>
#include <QMap>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QToolBar;

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
    void addAgent();
    void selectAgent(QtNodes::NodeId id);
    bool applyAgent();
    pipeline::Pipeline capture() const;
    void loadGraph(const pipeline::Pipeline &graph);
    bool syncGraph();
    void startTask();
    void changeState(pipeline::PipelineExecutor::State state);
    void refreshRoutes();
    void refreshContext();
    void resetResultHistory();
    void addVisitResult(const QString &name, const QString &time, const QJsonObject &result);
    void savePipeline();
    void openPipeline();
    void saveSession();
    void openSession();
    void autosave();
    void log(const QString &message);
    void error(const QString &message);
    QString dataDirectory() const;
    orchestrator::Configuration connection() const;
    pipeline::AgentDelegate *selected() const;
    pipeline::AgentGraphModel *m_graph{nullptr};
    QtNodes::DataFlowGraphicsScene *m_scene{nullptr};
    QtNodes::GraphicsView *m_view{nullptr};
    pipeline::PipelineExecutor m_engine;
    orchestrator::LmStudioClient m_models;
    QtNodes::NodeId m_selected{QtNodes::InvalidNodeId};
    QString m_start, m_file;
    bool m_loading{false};
    QWidget *m_inspector{nullptr};
    QToolBar *m_toolbar{nullptr};
    QLineEdit *m_server{nullptr}, *m_key{nullptr}, *m_workspace{nullptr}, *m_name{nullptr};
    QLineEdit *m_resultField{nullptr}, *m_positive{nullptr}, *m_negative{nullptr};
    QComboBox *m_model{nullptr}, *m_thinking{nullptr}, *m_route{nullptr}, *m_resultHistory{nullptr};
    QPlainTextEdit *m_task{nullptr}, *m_rules{nullptr}, *m_prompt{nullptr}, *m_result{nullptr};
    QPlainTextEdit *m_context{nullptr}, *m_log{nullptr}, *m_note{nullptr};
    QCheckBox *m_automatic{nullptr}, *m_fresh{nullptr};
    QSpinBox *m_tokens{nullptr}, *m_timeout{nullptr}, *m_toolLimit{nullptr}, *m_commandTimeout{nullptr}, *m_reasoningBudget{nullptr};
    QSpinBox *m_autoLimit{nullptr};
    QMap<QString, QCheckBox *> m_tools;
    QLabel *m_startLabel{nullptr}, *m_stateLabel{nullptr};
    QPushButton *m_startButton{nullptr}, *m_stopButton{nullptr}, *m_repeatButton{nullptr}, *m_routeButton{nullptr}, *m_modelsButton{nullptr};
};
}
