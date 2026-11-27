#include "main/mainwindow.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtNodes/NodeDelegateModelRegistry>
#include <algorithm>
#include <limits>

namespace client
{
namespace
{
QSpinBox *spin(int value, int minimum, int maximum, QWidget *parent)
{
    auto *result = new QSpinBox(parent);
    result->setRange(minimum, maximum);
    result->setValue(value);
    return result;
}
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ORKestrator — редактор пайплайнов"));
    resize(1580, 1020);
    auto registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    registry->registerModel<pipeline::AgentDelegate>(QStringLiteral("Агенты"));
    m_graph = new pipeline::AgentGraphModel(registry);
    m_scene = new QtNodes::DataFlowGraphicsScene(*m_graph, this);
    m_graph->setParent(m_scene);
    setupUi();
    connect(m_scene, &QtNodes::BasicGraphicsScene::nodeClicked, this, &MainWindow::selectAgent);
    connect(m_scene, &QtNodes::BasicGraphicsScene::nodeSelected, this, &MainWindow::selectAgent);
    connect(m_graph, &QtNodes::AbstractGraphModel::nodeCreated, this, [this](QtNodes::NodeId id) {
        if (m_loading)
            return;
        auto *agent = m_graph->delegateModel<pipeline::AgentDelegate>(id);
        if (!agent)
            return;
        for (auto other : m_graph->allNodeIds())
            if (other != id && m_graph->delegateModel<pipeline::AgentDelegate>(other)->agent.id == agent->agent.id)
                agent->agent.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (m_start.isEmpty())
            m_start = agent->agent.id;
        selectAgent(id);
        refreshRoutes();
    });
    connect(m_graph, &QtNodes::AbstractGraphModel::nodeDeleted, this, [this](QtNodes::NodeId id) {
        if (m_selected == id)
            m_selected = QtNodes::InvalidNodeId;
        if (!m_loading)
            refreshRoutes();
    });
    connect(m_graph, &QtNodes::AbstractGraphModel::connectionCreated, this, [this](QtNodes::ConnectionId) { refreshRoutes(); });
    connect(m_graph, &QtNodes::AbstractGraphModel::connectionDeleted, this, [this](QtNodes::ConnectionId) { refreshRoutes(); });
    connect(&m_engine, &pipeline::PipelineExecutor::stateChanged, this, &MainWindow::changeState);
    connect(&m_engine, &pipeline::PipelineExecutor::eventOccurred, this, &MainWindow::log);
    connect(&m_engine, &pipeline::PipelineExecutor::failed, this, &MainWindow::error);
    connect(&m_engine, &pipeline::PipelineExecutor::outputChanged, this, [this](const QString &text) {
        if (m_resultHistory->currentIndex() == 0)
            m_result->setPlainText(text);
    });
    connect(&m_engine, &pipeline::PipelineExecutor::changed, this, &MainWindow::refreshContext);
    connect(&m_engine, &pipeline::PipelineExecutor::nodeFinished, this, [this](const QString &id, const QJsonObject &result) {
        const auto graph = m_engine.graph();
        if (const auto *agent = graph.find(id))
            addVisitResult(agent->name, QDateTime::currentDateTime().toString("HH:mm:ss"), result);
        m_note->clear();
        refreshRoutes();
        autosave();
    });
    connect(&m_engine, &pipeline::PipelineExecutor::progress, this, [this](qint64 content, qint64 thinking) {
        statusBar()->showMessage(QStringLiteral("Ответ: %1, мышление: %2 символов").arg(content).arg(thinking));
    });
    connect(&m_models, &orchestrator::LmStudioClient::modelsReady, this, [this](const QStringList &models) {
        const auto value = m_model->currentText();
        m_model->clear();
        m_model->addItems(models);
        if (!value.isEmpty())
            m_model->setCurrentText(value);
        statusBar()->showMessage(QStringLiteral("Доступно моделей: %1").arg(models.size()));
    });
    connect(&m_models, &orchestrator::LmStudioClient::failed, this, &MainWindow::error);
    QSettings settings;
    m_server->setText(settings.value("graph/server", "http://127.0.0.1:51325").toString());
    m_workspace->setText(settings.value("graph/workspace").toString());
    m_key->setText(qEnvironmentVariable("LM_STUDIO_API_KEY"));
    restoreGeometry(settings.value("graph/geometry").toByteArray());
    pipeline::Pipeline graph;
    QJsonObject saved;
    QString failure;
    if (pipeline::readJson(dataDirectory() + "/editor.pipeline.json", saved, failure) && pipeline::Pipeline::fromJson(saved, graph, failure))
        loadGraph(graph);
    else
        addAgent();
    changeState(pipeline::PipelineExecutor::State::Idle);
}
void MainWindow::setupUi()
{
    m_toolbar = addToolBar(QStringLiteral("Пайплайн"));
    m_toolbar->setMovable(false);
    m_toolbar->addAction(QStringLiteral("Новый"), this, [this] {
        autosave();
        m_engine.clear();
        const auto ids = m_graph->allNodeIds();
        for (auto id : ids)
            m_graph->deleteNode(id);
        m_start.clear();
        m_file.clear();
        m_selected = QtNodes::InvalidNodeId;
        addAgent();
    });
    m_toolbar->addAction(QStringLiteral("Добавить агента"), this, &MainWindow::addAgent);
    m_toolbar->addAction(QStringLiteral("Удалить"), this, [this] {
        const auto ids = m_scene->selectedNodes();
        for (auto id : ids)
            m_graph->deleteNode(id);
        refreshRoutes();
    });
    m_toolbar->addAction(QStringLiteral("Стартовый агент"), this, [this] {
        if (auto *agent = selected())
        {
            m_start = agent->agent.id;
            refreshRoutes();
        }
    });
    m_toolbar->addAction(QStringLiteral("Открыть"), this, &MainWindow::openPipeline);
    m_toolbar->addAction(QStringLiteral("Сохранить"), this, &MainWindow::savePipeline);
    m_toolbar->addAction(QStringLiteral("Вписать граф"), m_view = new QtNodes::GraphicsView(m_scene), &QtNodes::GraphicsView::zoomFitAll);
    m_toolbar->addSeparator();
    m_toolbar->addAction(QStringLiteral("Загрузить сессию"), this, &MainWindow::openSession);
    m_toolbar->addAction(QStringLiteral("Сохранить сессию"), this, &MainWindow::saveSession);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    auto *hint = new QLabel(QStringLiteral("Один тип узла — агент. Соедините положительный или отрицательный выход со входом другого агента."), central);
    hint->setWordWrap(true);
    centralLayout->addWidget(hint);
    auto *splitter = new QSplitter(Qt::Vertical, central);
    m_view->setObjectName("pipelineGraph");
    splitter->addWidget(m_view);
    auto *bottom = new QWidget(splitter);
    auto *bottomLayout = new QVBoxLayout(bottom);
    auto *tabs = new QTabWidget(bottom);
    auto *resultPage = new QWidget(tabs);
    auto *resultLayout = new QVBoxLayout(resultPage);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    m_resultHistory = new QComboBox(resultPage);
    m_resultHistory->setAccessibleName(QStringLiteral("Результаты посещений"));
    m_result = new QPlainTextEdit(resultPage);
    resultLayout->addWidget(m_resultHistory);
    resultLayout->addWidget(m_result);
    connect(m_resultHistory, &QComboBox::currentIndexChanged, this,
            [this](int index) { m_result->setPlainText(index > 0 ? m_resultHistory->currentData().toString() : m_engine.resultText()); });
    m_result->setReadOnly(true);
    m_result->setObjectName("agentResult");
    m_log = new QPlainTextEdit(tabs);
    m_log->setObjectName("sessionLog");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    tabs->addTab(resultPage, QStringLiteral("Результат агента"));
    tabs->addTab(m_log, QStringLiteral("Журнал"));
    bottomLayout->addWidget(tabs);
    m_stateLabel = new QLabel(bottom);
    m_stateLabel->setWordWrap(true);
    bottomLayout->addWidget(m_stateLabel);
    m_note = new QPlainTextEdit(bottom);
    m_note->setObjectName("transitionNote");
    m_note->setPlaceholderText(QStringLiteral("Ваши ответы, уточнения или поручение следующему агенту"));
    m_note->setMaximumHeight(70);
    bottomLayout->addWidget(m_note);
    auto *routeLayout = new QHBoxLayout();
    m_route = new QComboBox(bottom);
    m_routeButton = new QPushButton(QStringLiteral("Перейти вручную"), bottom);
    m_repeatButton = new QPushButton(QStringLiteral("Повторить агента"), bottom);
    auto *finish = new QPushButton(QStringLiteral("Завершить"), bottom);
    routeLayout->addWidget(m_route, 1);
    routeLayout->addWidget(m_routeButton);
    routeLayout->addWidget(m_repeatButton);
    routeLayout->addWidget(finish);
    bottomLayout->addLayout(routeLayout);
    splitter->addWidget(bottom);
    splitter->setSizes({520, 370});
    centralLayout->addWidget(splitter);
    setCentralWidget(central);
    connect(m_routeButton, &QPushButton::clicked, this, [this] {
        if (!syncGraph())
            return;
        QString failure;
        if (!m_engine.advance(m_route->currentData().toBool(), m_note->toPlainText(), failure))
            error(failure);
    });
    connect(m_repeatButton, &QPushButton::clicked, this, [this] {
        if (!syncGraph())
            return;
        QString failure;
        if (!m_engine.repeat(m_note->toPlainText(), failure))
            error(failure);
    });
    connect(finish, &QPushButton::clicked, &m_engine, &pipeline::PipelineExecutor::finish);

    auto *taskPanel = new QWidget(this);
    auto *taskLayout = new QVBoxLayout(taskPanel);
    auto *serverForm = new QFormLayout();
    m_server = new QLineEdit(taskPanel);
    m_server->setObjectName("serverUrl");
    m_key = new QLineEdit(taskPanel);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("Не сохраняется на диск"));
    m_workspace = new QLineEdit(taskPanel);
    m_workspace->setObjectName("workspacePath");
    serverForm->addRow(QStringLiteral("LM Studio"), m_server);
    serverForm->addRow(QStringLiteral("API-ключ"), m_key);
    serverForm->addRow(QStringLiteral("Проект"), m_workspace);
    taskLayout->addLayout(serverForm);
    auto *browse = new QPushButton(QStringLiteral("Выбрать папку проекта"), taskPanel);
    taskLayout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("Папка проекта"), m_workspace->text());
        if (!path.isEmpty())
            m_workspace->setText(path);
    });
    m_modelsButton = new QPushButton(QStringLiteral("Обновить модели"), taskPanel);
    taskLayout->addWidget(m_modelsButton);
    connect(m_modelsButton, &QPushButton::clicked, this, [this] {
        m_models.configure(connection());
        m_models.listModels();
    });
    auto *assign = new QPushButton(QStringLiteral("Назначить выбранную модель пустым агентам"), taskPanel);
    taskLayout->addWidget(assign);
    connect(assign, &QPushButton::clicked, this, [this] {
        if (!applyAgent() || m_model->currentText().trimmed().isEmpty())
            return;
        const auto model = m_model->currentText().trimmed();
        for (auto id : m_graph->allNodeIds())
        {
            auto *delegate = m_graph->delegateModel<pipeline::AgentDelegate>(id);
            if (delegate->agent.model.isEmpty())
            {
                delegate->agent.model = model;
                delegate->refresh();
                emit m_graph->nodeUpdated(id);
            }
        }
    });
    m_task = new QPlainTextEdit(taskPanel);
    m_task->setObjectName("taskInput");
    m_task->setPlaceholderText(QStringLiteral("Задача и ваши явные ограничения. Контексты принадлежат отдельным узлам."));
    taskLayout->addWidget(m_task, 1);
    m_startLabel = new QLabel(taskPanel);
    m_startLabel->setWordWrap(true);
    taskLayout->addWidget(m_startLabel);
    auto *autoForm = new QFormLayout();
    m_autoLimit = spin(100, 1, 1000000, taskPanel);
    autoForm->addRow(QStringLiteral("Лимит автопереходов"), m_autoLimit);
    taskLayout->addLayout(autoForm);
    m_startButton = new QPushButton(QStringLiteral("Новая задача"), taskPanel);
    m_startButton->setObjectName("startTask");
    m_stopButton = new QPushButton(QStringLiteral("Остановить"), taskPanel);
    taskLayout->addWidget(m_startButton);
    taskLayout->addWidget(m_stopButton);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startTask);
    connect(m_stopButton, &QPushButton::clicked, &m_engine, &pipeline::PipelineExecutor::stop);
    auto *taskDock = new QDockWidget(QStringLiteral("Задача"), this);
    taskDock->setWidget(taskPanel);
    addDockWidget(Qt::LeftDockWidgetArea, taskDock);

    m_inspector = new QWidget(this);
    auto *inspectorLayout = new QVBoxLayout(m_inspector);
    auto *properties = new QTabWidget(m_inspector);
    auto *basic = new QWidget(properties);
    auto *form = new QFormLayout(basic);
    m_name = new QLineEdit(basic);
    m_name->setObjectName("agentName");
    m_model = new QComboBox(basic);
    m_model->setEditable(true);
    m_model->setObjectName("agentModel");
    m_thinking = new QComboBox(basic);
    m_thinking->addItem(QStringLiteral("Включено"), 0);
    m_thinking->addItem(QStringLiteral("Настройки сервера"), 1);
    m_thinking->addItem(QStringLiteral("Выключено"), 2);
    m_automatic = new QCheckBox(QStringLiteral("Автопереход по результату"), basic);
    m_fresh = new QCheckBox(QStringLiteral("Сбрасывать контекст при входе"), basic);
    m_resultField = new QLineEdit(basic);
    m_positive = new QLineEdit(basic);
    m_negative = new QLineEdit(basic);
    form->addRow(QStringLiteral("Имя"), m_name);
    form->addRow(QStringLiteral("Модель"), m_model);
    form->addRow(QStringLiteral("Мышление"), m_thinking);
    form->addRow(m_automatic);
    form->addRow(m_fresh);
    form->addRow(QStringLiteral("Поле результата"), m_resultField);
    form->addRow(QStringLiteral("Положительное значение"), m_positive);
    form->addRow(QStringLiteral("Отрицательное значение"), m_negative);
    for (const auto &name : pipeline::toolNames())
    {
        auto *check = new QCheckBox(name, basic);
        m_tools.insert(name, check);
        form->addRow(check);
    }
    auto *toolsHint = new QLabel(QStringLiteral("Разрешенные инструменты выполняются внутри агента автоматически."), basic);
    toolsHint->setWordWrap(true);
    form->addRow(toolsHint);
    auto *rulesPage = new QWidget(properties);
    auto *rulesLayout = new QVBoxLayout(rulesPage);
    rulesLayout->addWidget(new QLabel(QStringLiteral("Системные правила агента"), rulesPage));
    m_rules = new QPlainTextEdit(rulesPage);
    rulesLayout->addWidget(m_rules, 2);
    rulesLayout->addWidget(new QLabel(QStringLiteral("Поручение при каждом входе"), rulesPage));
    m_prompt = new QPlainTextEdit(rulesPage);
    rulesLayout->addWidget(m_prompt, 1);
    auto *limitsPage = new QWidget(properties);
    auto *limits = new QFormLayout(limitsPage);
    m_tokens = spin(65536, 1, 1048576, limitsPage);
    m_timeout = spin(1800, 1, 604800, limitsPage);
    m_commandTimeout = spin(300, 1, 604800, limitsPage);
    m_toolLimit = spin(12, 1, 1000000, limitsPage);
    m_reasoningBudget = spin(24000, 0, 1048576, limitsPage);
    m_reasoningBudget->setSpecialValueText(QStringLiteral("Без ограничения"));
    limits->addRow(QStringLiteral("Токены ответа"), m_tokens);
    limits->addRow(QStringLiteral("Таймаут ответа, с"), m_timeout);
    limits->addRow(QStringLiteral("Таймаут команды, с"), m_commandTimeout);
    limits->addRow(QStringLiteral("Операции за посещение"), m_toolLimit);
    limits->addRow(QStringLiteral("Мышление, символы"), m_reasoningBudget);
    auto *contextPage = new QWidget(properties);
    auto *contextLayout = new QVBoxLayout(contextPage);
    m_context = new QPlainTextEdit(contextPage);
    m_context->setReadOnly(true);
    contextLayout->addWidget(m_context);
    auto *reset = new QPushButton(QStringLiteral("Сбросить контекст"), contextPage);
    contextLayout->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this] {
        if (auto *agent = selected())
            m_engine.resetContext(agent->agent.id);
    });
    properties->addTab(basic, QStringLiteral("Агент"));
    properties->addTab(rulesPage, QStringLiteral("Правила"));
    properties->addTab(limitsPage, QStringLiteral("Лимиты"));
    properties->addTab(contextPage, QStringLiteral("Контекст"));
    auto *scroll = new QScrollArea(m_inspector);
    scroll->setWidgetResizable(true);
    scroll->setWidget(properties);
    inspectorLayout->addWidget(scroll);
    auto *apply = new QPushButton(QStringLiteral("Применить настройки"), m_inspector);
    inspectorLayout->addWidget(apply);
    connect(apply, &QPushButton::clicked, this, [this] { applyAgent(); });
    auto *inspectorDock = new QDockWidget(QStringLiteral("Свойства агента"), this);
    inspectorDock->setWidget(m_inspector);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
}

pipeline::AgentDelegate *MainWindow::selected() const
{
    return m_graph->delegateModel<pipeline::AgentDelegate>(m_selected);
}
void MainWindow::addAgent()
{
    if (!applyAgent())
        return;
    const auto previousModel = m_model->currentText().trimmed();
    m_loading = true;
    const auto id = m_graph->addNode("Agent");
    auto *delegate = m_graph->delegateModel<pipeline::AgentDelegate>(id);
    delegate->agent.name = QStringLiteral("Агент %1").arg(m_graph->allNodeIds().size());
    delegate->agent.model = previousModel;
    delegate->refresh();
    m_graph->setNodeData(id, QtNodes::NodeRole::Position, QPointF((m_graph->allNodeIds().size() - 1) * 330, 0));
    emit m_graph->nodeUpdated(id);
    if (m_start.isEmpty())
        m_start = delegate->agent.id;
    m_loading = false;
    m_selected = QtNodes::InvalidNodeId;
    selectAgent(id);
    refreshRoutes();
}
void MainWindow::selectAgent(QtNodes::NodeId id)
{
    if (m_loading || id == m_selected)
        return;
    if (!applyAgent())
        return;
    auto *delegate = m_graph->delegateModel<pipeline::AgentDelegate>(id);
    if (!delegate)
        return;
    m_selected = id;
    const auto &agent = delegate->agent;
    m_name->setText(agent.name);
    m_model->setCurrentText(agent.model);
    m_rules->setPlainText(agent.rules);
    m_prompt->setPlainText(agent.prompt);
    m_resultField->setText(agent.resultField);
    m_positive->setText(agent.positive);
    m_negative->setText(agent.negative);
    m_thinking->setCurrentIndex(m_thinking->findData(static_cast<int>(agent.thinking)));
    m_automatic->setChecked(agent.automatic);
    m_fresh->setChecked(agent.freshContext);
    m_tokens->setValue(agent.tokens);
    m_timeout->setValue(agent.timeout);
    m_commandTimeout->setValue(agent.commandTimeout);
    m_toolLimit->setValue(agent.toolLimit);
    m_reasoningBudget->setValue(agent.reasoningBudget);
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it)
        it.value()->setChecked(agent.tools.contains(it.key()));
    refreshContext();
}
bool MainWindow::applyAgent()
{
    if (m_loading || m_engine.busy())
        return true;
    auto *delegate = selected();
    if (!delegate)
        return true;
    auto agent = delegate->agent;
    agent.name = m_name->text().trimmed();
    agent.model = m_model->currentText().trimmed();
    agent.rules = m_rules->toPlainText();
    agent.prompt = m_prompt->toPlainText();
    agent.resultField = m_resultField->text().trimmed();
    agent.positive = m_positive->text();
    agent.negative = m_negative->text();
    agent.thinking = static_cast<orchestrator::ReasoningMode>(m_thinking->currentData().toInt());
    agent.automatic = m_automatic->isChecked();
    agent.freshContext = m_fresh->isChecked();
    agent.tokens = m_tokens->value();
    agent.timeout = m_timeout->value();
    agent.commandTimeout = m_commandTimeout->value();
    agent.toolLimit = m_toolLimit->value();
    agent.reasoningBudget = m_reasoningBudget->value();
    agent.tools.clear();
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it)
        if (it.value()->isChecked())
            agent.tools.append(it.key());
    const auto failure = agent.validate(false);
    if (!failure.isEmpty())
    {
        error(failure);
        return false;
    }
    delegate->agent = agent;
    delegate->refresh();
    emit m_graph->nodeUpdated(m_selected);
    refreshRoutes();
    return true;
}
pipeline::Pipeline MainWindow::capture() const
{
    pipeline::Pipeline result;
    result.start = m_start;
    result.automaticLimit = m_autoLimit->value();
    QList<QtNodes::NodeId> ids;
    for (auto id : m_graph->allNodeIds())
        ids.append(id);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids)
    {
        auto agent = m_graph->delegateModel<pipeline::AgentDelegate>(id)->agent;
        agent.position = m_graph->nodeData(id, QtNodes::NodeRole::Position).toPointF();
        result.agents.append(agent);
        for (const auto &connection : m_graph->allConnectionIds(id))
        {
            if (connection.outNodeId != id)
                continue;
            const auto *target = m_graph->delegateModel<pipeline::AgentDelegate>(connection.inNodeId);
            if (target)
                result.edges.append({agent.id, target->agent.id, connection.outPortIndex == 0});
        }
    }
    return result;
}
void MainWindow::loadGraph(const pipeline::Pipeline &graph)
{
    m_loading = true;
    m_selected = QtNodes::InvalidNodeId;
    const auto oldIds = m_graph->allNodeIds();
    for (auto id : oldIds)
        m_graph->deleteNode(id);
    QMap<QString, QtNodes::NodeId> ids;
    for (const auto &agent : graph.agents)
    {
        const auto id = m_graph->addNode("Agent");
        auto *delegate = m_graph->delegateModel<pipeline::AgentDelegate>(id);
        delegate->agent = agent;
        delegate->refresh();
        m_graph->setNodeData(id, QtNodes::NodeRole::Position, agent.position);
        emit m_graph->nodeUpdated(id);
        ids.insert(agent.id, id);
    }
    for (const auto &edge : graph.edges)
        m_graph->addConnection({ids.value(edge.from), edge.positive ? 0U : 1U, ids.value(edge.to), 0U});
    m_start = graph.start;
    m_autoLimit->setValue(graph.automaticLimit);
    m_loading = false;
    if (!graph.agents.isEmpty())
        selectAgent(ids.value(graph.agents.first().id));
    refreshRoutes();
    QTimer::singleShot(0, m_view, &QtNodes::GraphicsView::zoomFitAll);
}
orchestrator::Configuration MainWindow::connection() const
{
    orchestrator::Configuration result;
    result.baseUrl = QUrl(m_server->text().trimmed());
    result.apiKey = m_key->text().trimmed();
    result.workspace = m_workspace->text().trimmed();
    return result;
}
bool MainWindow::syncGraph()
{
    if (!applyAgent())
        return false;
    QString failure;
    if (!m_engine.updateConnection(connection(), failure) || !m_engine.updateGraph(capture(), failure))
    {
        error(failure);
        return false;
    }
    return true;
}
void MainWindow::startTask()
{
    if (!applyAgent())
        return;
    autosave();
    QString failure;
    if (!m_engine.start(capture(), m_task->toPlainText(), connection(), failure))
        error(failure);
}
void MainWindow::changeState(pipeline::PipelineExecutor::State state)
{
    const bool busy = m_engine.busy();
    m_toolbar->setEnabled(!busy);
    m_view->setEnabled(!busy);
    m_inspector->setEnabled(!busy);
    m_startButton->setEnabled(!busy);
    m_stopButton->setEnabled(busy);
    m_modelsButton->setEnabled(!busy);
    m_server->setEnabled(!busy);
    m_key->setEnabled(!busy);
    m_workspace->setEnabled(!busy);
    m_task->setEnabled(!busy);
    m_autoLimit->setEnabled(!busy);
    m_repeatButton->setEnabled(!busy && !m_engine.current().isEmpty());
    QString name;
    switch (state)
    {
    case pipeline::PipelineExecutor::State::Idle:
        m_log->clear();
        m_note->clear();
        resetResultHistory();
        name = QStringLiteral("Готово");
        break;
    case pipeline::PipelineExecutor::State::Running:
        name = QStringLiteral("Ответ модели");
        break;
    case pipeline::PipelineExecutor::State::Tools:
        name = QStringLiteral("Выполняется инструмент");
        break;
    case pipeline::PipelineExecutor::State::AwaitingChoice:
        name = QStringLiteral("Ожидание перехода");
        break;
    case pipeline::PipelineExecutor::State::Stopped:
        name = QStringLiteral("Остановлено");
        break;
    case pipeline::PipelineExecutor::State::Failed:
        name = QStringLiteral("Ошибка — контексты сохранены");
        break;
    case pipeline::PipelineExecutor::State::Finished:
        name = QStringLiteral("Ветка завершена");
        break;
    }
    const auto graph = m_engine.graph();
    const auto *agent = graph.find(m_engine.current());
    m_stateLabel->setText((agent ? agent->name + ": " : QString{}) + name);
    statusBar()->showMessage(name);
    refreshRoutes();
    if (!busy && !m_engine.sessionId().isEmpty())
        autosave();
}
void MainWindow::refreshRoutes()
{
    if (m_loading)
        return;
    const auto previous = m_route->currentData();
    m_route->clear();
    const auto graph = capture();
    if (const auto *start = graph.find(m_start))
        m_startLabel->setText(QStringLiteral("Старт: %1").arg(start->name));
    else
        m_startLabel->setText(QStringLiteral("Назначьте стартового агента"));
    for (const bool positive : {true, false})
    {
        const auto *target = graph.find(graph.next(m_engine.current(), positive));
        if (target)
            m_route->addItem((positive ? QStringLiteral("Положительный → ") : QStringLiteral("Отрицательный → ")) + target->name, positive);
    }
    const auto index = m_route->findData(previous);
    if (index >= 0)
        m_route->setCurrentIndex(index);
    m_routeButton->setEnabled(!m_engine.busy() && m_route->count() > 0);
}
void MainWindow::resetResultHistory()
{
    m_resultHistory->clear();
    m_resultHistory->addItem(QStringLiteral("Текущий этап"));
}
void MainWindow::addVisitResult(const QString &name, const QString &time, const QJsonObject &result)
{
    m_resultHistory->addItem(time + "  " + name, result.value("content").toString());
}
void MainWindow::refreshContext()
{
    if (auto *agent = selected())
        m_context->setPlainText(QString::fromUtf8(QJsonDocument(m_engine.context(agent->agent.id)).toJson(QJsonDocument::Indented)));
    else
        m_context->clear();
}
QString MainWindow::dataDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}
void MainWindow::log(const QString &message)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + message);
}
void MainWindow::error(const QString &message)
{
    statusBar()->showMessage(message, 30000);
    log(QStringLiteral("Ошибка: ") + message);
}
void MainWindow::savePipeline()
{
    if (!applyAgent())
        return;
    const auto graph = capture();
    const auto validation = graph.validate(false);
    if (!validation.isEmpty())
    {
        error(validation);
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить пайплайн"), m_file.isEmpty() ? "pipeline.json" : m_file, "JSON (*.json)");
    if (path.isEmpty())
        return;
    QString failure;
    if (!pipeline::writeJson(path, graph.toJson(), failure))
        error(failure);
    else
    {
        m_file = path;
        statusBar()->showMessage(QStringLiteral("Пайплайн сохранен"));
    }
}
void MainWindow::openPipeline()
{
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Открыть пайплайн"), {}, "JSON (*.json)");
    if (path.isEmpty())
        return;
    QJsonObject json;
    pipeline::Pipeline graph;
    QString failure;
    if (!pipeline::readJson(path, json, failure) || !pipeline::Pipeline::fromJson(json, graph, failure))
    {
        error(failure);
        return;
    }
    autosave();
    m_engine.clear();
    loadGraph(graph);
    m_file = path;
}
void MainWindow::saveSession()
{
    if (m_engine.sessionId().isEmpty())
    {
        error(QStringLiteral("Задача еще не запущена."));
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить сессию"), "session.json", "JSON (*.json)");
    if (path.isEmpty())
        return;
    auto session = m_engine.snapshot();
    if (!m_engine.busy() && applyAgent())
    {
        const auto graph = capture();
        if (graph.validate(false).isEmpty() && graph.find(m_engine.current()))
            session.insert("pipeline", graph.toJson());
    }
    QString failure;
    if (!pipeline::writeJson(path, session, failure))
        error(failure);
}
void MainWindow::openSession()
{
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Загрузить сессию"), dataDirectory() + "/graph-sessions", "JSON (*.json)");
    if (path.isEmpty())
        return;
    QJsonObject json;
    QString failure;
    if (!pipeline::readJson(path, json, failure))
    {
        error(failure);
        return;
    }
    autosave();
    if (!m_engine.restore(json, connection(), failure))
    {
        error(failure);
        return;
    }
    loadGraph(m_engine.graph());
    m_task->setPlainText(m_engine.task());
    m_workspace->setText(json.value("workspace").toString());
    resetResultHistory();
    for (const auto &entry : json.value("visits").toArray())
    {
        const auto visit = entry.toObject();
        if (visit.value("result").isObject())
        {
            const auto time = QDateTime::fromString(visit.value("finished").toString(), Qt::ISODateWithMs).toLocalTime();
            addVisitResult(visit.value("name").toString(), time.toString("HH:mm:ss"), visit.value("result").toObject());
        }
    }
    m_log->clear();
    for (const auto &entry : json.value("events").toArray())
    {
        const auto event = entry.toObject();
        m_log->appendPlainText(event.value("time").toString() + "  " + event.value("text").toString());
    }
    changeState(m_engine.state());
}
void MainWindow::autosave()
{
    if (m_engine.sessionId().isEmpty())
        return;
    QString failure;
    if (!pipeline::writeJson(dataDirectory() + "/graph-sessions/" + m_engine.sessionId() + ".json", m_engine.snapshot(), failure))
        log(QStringLiteral("Не удалось сохранить сессию: ") + failure);
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    m_engine.stop();
    autosave();
    QSettings settings;
    settings.setValue("graph/server", m_server->text());
    settings.setValue("graph/workspace", m_workspace->text());
    settings.setValue("graph/geometry", saveGeometry());
    if (applyAgent())
    {
        const auto graph = capture();
        QString failure;
        if (graph.validate(false).isEmpty() && !pipeline::writeJson(dataDirectory() + "/editor.pipeline.json", graph.toJson(), failure))
            log(failure);
    }
    event->accept();
}
}
