#include "mainwindow.h"
#include "orchestration/protocol.h"
#include "saving/sessionstore.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace client
{
namespace
{
QLabel *plainLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QTextBrowser *markdownView(QWidget *parent)
{
    auto *view = new QTextBrowser(parent);
    view->setOpenLinks(false);
    view->setOpenExternalLinks(false);
    view->setReadOnly(true);
    return view;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_orchestrator(this), m_modelsClient(this)
{
    setupUi();
    setupConnections();
    loadSettings();
    updateState(orchestrator::Orchestrator::State::Idle);
    QTimer::singleShot(0, this, &MainWindow::refreshModels);
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("ORKestrator — оркестр ИИ-агентов"));
    resize(1480, 1020);
    setMinimumSize(1100, 800);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("ORKestrator"), central);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 600;"));
    layout->addWidget(title);
    layout->addWidget(
        plainLabel(QStringLiteral("Исследователь → PM ⇄ PM-критик → Разработчик ⇄ Критик разработчика → Цензор. Диагност подключается при сбоях."), central));

    ui.connectionGroup = new QGroupBox(QStringLiteral("LM Studio"), central);
    auto *connection = new QFormLayout(ui.connectionGroup);
    auto *serverRow = new QHBoxLayout();
    ui.server = new QLineEdit(ui.connectionGroup);
    ui.server->setObjectName("serverUrl");
    ui.server->setPlaceholderText(QStringLiteral("http://127.0.0.1:51325"));
    ui.apiKey = new QLineEdit(ui.connectionGroup);
    ui.apiKey->setEchoMode(QLineEdit::Password);
    ui.apiKey->setPlaceholderText(QStringLiteral("API-ключ, если включена авторизация"));
    ui.apiKey->setToolTip(QStringLiteral("Не сохраняется на диск. Можно задать через LM_STUDIO_API_KEY."));
    serverRow->addWidget(ui.server, 3);
    serverRow->addWidget(ui.apiKey, 2);
    connection->addRow(QStringLiteral("Сервер"), serverRow);
    auto *modelsRow = new QHBoxLayout();
    ui.pmModel = new QComboBox(ui.connectionGroup);
    ui.pmModel->setEditable(true);
    ui.pmModel->setObjectName("pmModel");
    ui.criticModel = new QComboBox(ui.connectionGroup);
    ui.criticModel->setEditable(true);
    ui.criticModel->setObjectName("criticModel");
    ui.refreshModels = new QPushButton(QStringLiteral("Обновить модели"), ui.connectionGroup);
    modelsRow->addWidget(new QLabel(QStringLiteral("PM")));
    modelsRow->addWidget(ui.pmModel, 1);
    modelsRow->addWidget(new QLabel(QStringLiteral("Критик")));
    modelsRow->addWidget(ui.criticModel, 1);
    modelsRow->addWidget(ui.refreshModels);
    connection->addRow(QStringLiteral("Модели"), modelsRow);
    auto makeModel = [this](const char *name) {
        auto *combo = new QComboBox(ui.connectionGroup);
        combo->setEditable(true);
        combo->setObjectName(name);
        return combo;
    };
    ui.developerModel = makeModel("developerModel");
    ui.developerCriticModel = makeModel("developerCriticModel");
    ui.censorModel = makeModel("censorModel");
    auto *developmentModels = new QHBoxLayout();
    developmentModels->addWidget(new QLabel(QStringLiteral("Разработчик")));
    developmentModels->addWidget(ui.developerModel, 1);
    developmentModels->addWidget(new QLabel(QStringLiteral("Критик разработчика")));
    developmentModels->addWidget(ui.developerCriticModel, 1);
    connection->addRow(QStringLiteral("Реализация"), developmentModels);
    connection->addRow(QStringLiteral("Цензор"), ui.censorModel);
    ui.researcherModel = makeModel("researcherModel");
    ui.diagnosticianModel = makeModel("diagnosticianModel");
    ui.researcherModel->setToolTip(QStringLiteral("Изучает проект перед PM. Пустое значение использует модель PM."));
    ui.diagnosticianModel->setToolTip(QStringLiteral("Подключается при сбоях. Пустое значение использует модель разработчика."));
    auto *specialistModels = new QHBoxLayout();
    specialistModels->addWidget(new QLabel(QStringLiteral("Исследователь")));
    specialistModels->addWidget(ui.researcherModel, 1);
    specialistModels->addWidget(new QLabel(QStringLiteral("Диагност")));
    specialistModels->addWidget(ui.diagnosticianModel, 1);
    connection->addRow(QStringLiteral("Специалисты"), specialistModels);
    auto *workspaceRow = new QHBoxLayout();
    ui.workspace = new QLineEdit(ui.connectionGroup);
    ui.workspace->setObjectName("workspacePath");
    ui.workspace->setPlaceholderText(QStringLiteral("Папка проекта: здесь разработчик изменяет файлы и запускает проверки"));
    auto *browse = new QPushButton(QStringLiteral("Выбрать…"), ui.connectionGroup);
    workspaceRow->addWidget(ui.workspace, 1);
    workspaceRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("Рабочая папка проекта"), ui.workspace->text());
        if (!path.isEmpty())
        {
            ui.workspace->setText(path);
        }
    });
    connection->addRow(QStringLiteral("Рабочая папка"), workspaceRow);
    layout->addWidget(ui.connectionGroup);

    ui.limits = new QGroupBox(QStringLiteral("Параметры генерации"), central);
    auto *limitsLayout = new QVBoxLayout(ui.limits);
    auto *limits = new QHBoxLayout();
    limitsLayout->addLayout(limits);
    auto addSpin = [limits, this](const QString &caption, int minimum, int maximum, int value) {
        auto *spin = new QSpinBox(ui.limits);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        limits->addWidget(new QLabel(caption));
        limits->addWidget(spin);
        return spin;
    };
    const orchestrator::Configuration defaults;
    ui.timeout = addSpin(QStringLiteral("Таймаут, с"), 10, 604800, 1800);
    ui.tokens = addSpin(QStringLiteral("Токены ответа"), 512, 1048576, defaults.maxTokens);
    ui.reviews = addSpin(QStringLiteral("Проверки"), 1, 1000000, defaults.maxReviews);
    ui.reviews->setObjectName("reviewLimit");
    ui.pmTurns = addSpin(QStringLiteral("Ходы PM"), 1, 1000000, defaults.maxPmTurns);
    ui.pmTurns->setObjectName("pmTurnLimit");
    ui.structuredOutput = new QCheckBox(QStringLiteral("JSON schema"), ui.limits);
    ui.structuredOutput->setChecked(true);
    ui.structuredOutput->setToolTip(QStringLiteral("Отключите, если модель не поддерживает Structured Output. Проверка JSON останется включена."));
    limits->addWidget(ui.structuredOutput);
    limits->addStretch();
    auto *executionLimits = new QHBoxLayout();
    executionLimits->addWidget(new QLabel(QStringLiteral("Запросы каждой роли, кроме PM")));
    ui.agentTurns = new QSpinBox(ui.limits);
    ui.agentTurns->setObjectName("agentTurnLimit");
    ui.agentTurns->setRange(1, 1000000);
    ui.agentTurns->setValue(defaults.maxAgentTurns);
    executionLimits->addWidget(ui.agentTurns);
    executionLimits->addWidget(new QLabel(QStringLiteral("Таймаут команды, с")));
    ui.commandTimeout = new QSpinBox(ui.limits);
    ui.commandTimeout->setRange(10, 604800);
    ui.commandTimeout->setValue(1800);
    executionLimits->addWidget(ui.commandTimeout);
    executionLimits->addStretch();
    limitsLayout->addLayout(executionLimits);
    auto *specialistLimits = new QHBoxLayout();
    auto addSpecialistLimit = [this, specialistLimits](const QString &caption, const char *name, int maximum, int value) {
        auto *spin = new QSpinBox(ui.limits);
        spin->setObjectName(name);
        spin->setRange(1, maximum);
        spin->setValue(value);
        specialistLimits->addWidget(new QLabel(caption));
        specialistLimits->addWidget(spin);
        return spin;
    };
    ui.researchTurns = addSpecialistLimit(QStringLiteral("Запросы исследователя"), "researchTurns", 1000000, defaults.maxResearchTurns);
    ui.diagnosticTurns = addSpecialistLimit(QStringLiteral("Запросы на диагностику"), "diagnosticTurns", 1000000, defaults.maxDiagnosticTurns);
    ui.diagnosticRuns = addSpecialistLimit(QStringLiteral("Диагностик на этап"), "diagnosticRuns", 1000000, defaults.maxDiagnosticsPerStage);
    specialistLimits->addStretch();
    limitsLayout->addLayout(specialistLimits);
    auto *sampling = new QHBoxLayout();
    ui.samplingProfile = new QComboBox(ui.limits);
    ui.samplingProfile->setObjectName("samplingProfile");
    ui.samplingProfile->addItem(QStringLiteral("Авто по модели"), static_cast<int>(orchestrator::SamplingProfile::Auto));
    ui.samplingProfile->addItem(QStringLiteral("Параметры LM Studio"), static_cast<int>(orchestrator::SamplingProfile::ServerDefaults));
    ui.samplingProfile->addItem(QStringLiteral("Qwen: thinking"), static_cast<int>(orchestrator::SamplingProfile::QwenThinking));
    ui.samplingProfile->addItem(QStringLiteral("Qwen: instruct"), static_cast<int>(orchestrator::SamplingProfile::QwenInstruct));
    ui.samplingProfile->setToolTip(
        QStringLiteral("Авто: профиль Qwen3.6/3.8 с учетом выбранного режима мышления и роли, настройки сервера для остальных моделей. "
                       "Профиль задает параметры выборки и не переключает режим мышления в LM Studio."));
    sampling->addWidget(new QLabel(QStringLiteral("Профиль генерации")));
    sampling->addWidget(ui.samplingProfile);
    ui.reasoningMode = new QComboBox(ui.limits);
    ui.reasoningMode->setObjectName("reasoningMode");
    ui.reasoningMode->addItem(QStringLiteral("Авто по роли"), static_cast<int>(orchestrator::ReasoningMode::Auto));
    ui.reasoningMode->addItem(QStringLiteral("Параметры LM Studio"), static_cast<int>(orchestrator::ReasoningMode::ServerDefaults));
    ui.reasoningMode->addItem(QStringLiteral("Выключить"), static_cast<int>(orchestrator::ReasoningMode::Off));
    ui.reasoningMode->setToolTip(
        QStringLiteral("Авто по возможностям модели из LM Studio: служебные роли без мышления; разработчик и цензор используют режим сервера. "
                       "После исчерпания бюджета мышления — одна попытка без него."));
    sampling->addWidget(new QLabel(QStringLiteral("Мышление")));
    sampling->addWidget(ui.reasoningMode);
    sampling->addStretch();
    limitsLayout->addLayout(sampling);
    auto *responseLimits = new QHBoxLayout();
    ui.serviceTokens = new QSpinBox(ui.limits);
    ui.serviceTokens->setObjectName("serviceTokens");
    ui.serviceTokens->setRange(0, 1048576);
    ui.serviceTokens->setSpecialValueText(QStringLiteral("Общий лимит"));
    ui.serviceTokens->setValue(defaults.maxServiceTokens);
    ui.serviceTokens->setToolTip(QStringLiteral("Отдельный предел ответа всех ролей, кроме разработчика. Ноль использует общий лимит токенов."));
    ui.reasoningCharacters = new QSpinBox(ui.limits);
    ui.reasoningCharacters->setObjectName("reasoningCharacters");
    ui.reasoningCharacters->setRange(0, 1048576);
    ui.reasoningCharacters->setSpecialValueText(QStringLiteral("Без ограничения"));
    ui.reasoningCharacters->setValue(defaults.maxReasoningCharacters);
    ui.reasoningCharacters->setToolTip(
        QStringLiteral("Защита от долгого мышления без буквальных повторов. Измеряется в символах, не токенах. Ноль отключает."));
    responseLimits->addWidget(new QLabel(QStringLiteral("Служебный ответ, токены")));
    responseLimits->addWidget(ui.serviceTokens);
    responseLimits->addWidget(new QLabel(QStringLiteral("Мышление, символы")));
    responseLimits->addWidget(ui.reasoningCharacters);
    responseLimits->addStretch();
    limitsLayout->addLayout(responseLimits);
    ui.stage = plainLabel(QStringLiteral("Этап реализации ещё не выбран"));
    ui.stage->setObjectName("stageLabel");
    layout->addWidget(ui.limits);
    layout->addWidget(ui.stage);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    ui.inputTabs = new QTabWidget(splitter);
    auto *taskPage = new QWidget(ui.inputTabs);
    auto *taskLayout = new QVBoxLayout(taskPage);
    taskLayout->addWidget(plainLabel(QStringLiteral("Опишите задачу и ожидаемый результат. PM уточнит существенные неизвестные.")));
    ui.task = new QPlainTextEdit(taskPage);
    ui.task->setObjectName("taskInput");
    ui.task->setPlaceholderText(QStringLiteral("Что нужно сделать? Для кого? Какие ограничения уже известны?"));
    taskLayout->addWidget(ui.task, 1);
    ui.loadTask = new QPushButton(QStringLiteral("Загрузить задачу из файла…"), taskPage);
    taskLayout->addWidget(ui.loadTask);
    ui.inputTabs->addTab(taskPage, QStringLiteral("Задача"));

    auto *questionsPage = new QWidget(ui.inputTabs);
    auto *questionsLayout = new QVBoxLayout(questionsPage);
    ui.questions = new QScrollArea(questionsPage);
    ui.questions->setWidgetResizable(true);
    questionsLayout->addWidget(ui.questions, 1);
    ui.submit = new QPushButton(QStringLiteral("Отправить ответы"), questionsPage);
    ui.submit->setObjectName("submitAnswers");
    questionsLayout->addWidget(ui.submit);
    ui.inputTabs->addTab(questionsPage, QStringLiteral("Опросник"));
    clearQuestions();

    ui.outputTabs = new QTabWidget(splitter);
    ui.plan = markdownView(ui.outputTabs);
    ui.plan->setObjectName("decompositionView");
    ui.implementation = markdownView(ui.outputTabs);
    ui.implementation->setObjectName("implementationView");
    ui.terminal = new QPlainTextEdit(ui.outputTabs);
    ui.terminal->setReadOnly(true);
    ui.terminal->setObjectName("commandOutput");
    ui.terminal->setMaximumBlockCount(6000);
    ui.reviewsView = markdownView(ui.outputTabs);
    ui.reviewsView->setObjectName("reviewsView");
    ui.log = new QPlainTextEdit(ui.outputTabs);
    ui.log->setReadOnly(true);
    ui.log->setMaximumBlockCount(3000);
    ui.outputTabs->addTab(ui.plan, QStringLiteral("Декомпозиция"));
    ui.outputTabs->addTab(ui.implementation, QStringLiteral("Реализация"));
    ui.outputTabs->addTab(ui.reviewsView, QStringLiteral("Ревью"));
    ui.outputTabs->addTab(ui.terminal, QStringLiteral("Команды"));
    ui.outputTabs->addTab(ui.log, QStringLiteral("Журнал"));
    ui.specialists = markdownView(ui.outputTabs);
    ui.specialists->setObjectName("specialistsView");
    ui.outputTabs->addTab(ui.specialists, QStringLiteral("Исследование и диагностика"));
    splitter->addWidget(ui.inputTabs);
    splitter->addWidget(ui.outputTabs);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({520, 820});
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout();
    ui.start = new QPushButton(QStringLiteral("Запустить оркестр"), central);
    ui.start->setObjectName("startButton");
    ui.start->setDefault(true);
    ui.start->setMinimumHeight(36);
    ui.start->setStyleSheet(QStringLiteral("QPushButton { background: #305b91; color: white; padding: 6px 18px; }"
                                           "QPushButton:disabled { background: #343c48; color: #8893a2; }"));
    ui.stop = new QPushButton(QStringLiteral("Остановить"), central);
    ui.stop->setObjectName("stopButton");
    ui.exportMarkdown = new QPushButton(QStringLiteral("Сохранить Markdown…"), central);
    ui.exportJson = new QPushButton(QStringLiteral("Экспорт сессии JSON…"), central);
    buttons->addWidget(ui.start);
    buttons->addWidget(ui.stop);
    ui.retry = new QPushButton(QStringLiteral("Повторить запрос"), central);
    ui.retry->setObjectName("retryButton");
    buttons->addWidget(ui.retry);
    buttons->addStretch();
    buttons->addWidget(ui.exportMarkdown);
    buttons->addWidget(ui.exportJson);
    layout->addLayout(buttons);
    ui.state = plainLabel({});
    ui.state->setObjectName("stateLabel");
    ui.progress = new QProgressBar(central);
    ui.progress->setTextVisible(false);
    ui.progress->setMaximumWidth(160);
    statusBar()->addWidget(ui.state, 1);
    statusBar()->addPermanentWidget(ui.progress);
    ui.saveStatus = new QLabel(central);
    ui.saveStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(ui.saveStatus);
    setCentralWidget(central);

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("Файл"));
    auto *folder = fileMenu->addAction(QStringLiteral("Открыть папку сессий"));
    connect(folder, &QAction::triggered, this, []() {
        const auto directory = orchestrator::SessionStore::defaultDirectory();
        QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Выход"), this, &QWidget::close);
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Справка"));
    helpMenu->addAction(QStringLiteral("О программе"), this, [this]() {
        QMessageBox::about(
            this, QStringLiteral("ORKestrator 0.2.0"),
            QStringLiteral(
                "Семь ролей: исследователь, PM, PM-критик, разработчик, критик разработчика, диагност и цензор.\n\n"
                "Контексты разработки разделены по этапам и редакциям плана. Полные истории сохраняются в журнале. Диагност подключается при сбоях."));
    });
}

void MainWindow::setupConnections()
{
    connect(ui.refreshModels, &QPushButton::clicked, this, &MainWindow::refreshModels);
    connect(ui.start, &QPushButton::clicked, this, &MainWindow::startTask);
    connect(ui.retry, &QPushButton::clicked, this, &MainWindow::retryRequest);
    connect(ui.stop, &QPushButton::clicked, &m_orchestrator, &orchestrator::Orchestrator::stop);
    connect(ui.submit, &QPushButton::clicked, this, &MainWindow::submitAnswers);
    connect(ui.exportMarkdown, &QPushButton::clicked, this, [this]() { exportResult(false); });
    connect(ui.exportJson, &QPushButton::clicked, this, [this]() { exportResult(true); });
    connect(ui.task, &QPlainTextEdit::textChanged, this,
            [this]() { ui.start->setEnabled(!m_orchestrator.active() && !ui.task->toPlainText().trimmed().isEmpty()); });
    connect(ui.loadTask, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Задача"), {}, QStringLiteral("Текст (*.txt *.md);;Все файлы (*)"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        {
            QMessageBox::warning(this, QStringLiteral("Загрузка"), QStringLiteral("Не удалось прочитать файл либо его размер превышает 1 МиБ."));
            return;
        }
        ui.task->setPlainText(QString::fromUtf8(file.readAll()));
    });
    connect(&m_modelsClient, &orchestrator::LmStudioClient::modelsReady, this, [this](const QStringList &models) {
        ui.refreshModels->setEnabled(!m_orchestrator.active());
        for (auto *combo : {ui.pmModel, ui.criticModel, ui.developerModel, ui.developerCriticModel, ui.censorModel, ui.researcherModel, ui.diagnosticianModel})
        {
            const auto current = combo->currentText();
            combo->clear();
            combo->addItems(models);
            if (!current.trimmed().isEmpty())
            {
                combo->setCurrentText(current);
            }
        }
        ui.log->appendPlainText(QStringLiteral("Доступные текстовые модели: %1").arg(models.join(", ")));
        if (models.isEmpty())
        {
            ui.log->appendPlainText(QStringLiteral("Список пуст. Загрузите текстовую модель в LM Studio."));
        }
    });
    connect(&m_modelsClient, &orchestrator::LmStudioClient::failed, this, [this](const QString &error) {
        ui.refreshModels->setEnabled(!m_orchestrator.active());
        ui.log->appendPlainText(error);
        statusBar()->showMessage(QStringLiteral("Нет списка моделей. Проверьте сервер; идентификатор модели можно ввести вручную."), 15000);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::stateChanged, this, &MainWindow::updateState);
    connect(&m_orchestrator, &orchestrator::Orchestrator::generationProgress, this, [this](const QString &role, qint64 content, qint64 reasoning) {
        if (m_orchestrator.active())
        {
            ui.state->setText(QStringLiteral("%1: ответ %2, мышление %3 символов").arg(role).arg(content).arg(reasoning));
        }
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::questionsReady, this, &MainWindow::showQuestions);
    connect(&m_orchestrator, &orchestrator::Orchestrator::specialistReportReady, this, [this](const QString &role, const QString &report) {
        m_specialistsMarkdown += QStringLiteral("## %1\n\n%2\n\n---\n\n").arg(role, report);
        ui.specialists->setMarkdown(m_specialistsMarkdown);
        ui.outputTabs->setCurrentIndex(ui.outputTabs->indexOf(ui.specialists));
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::planChanged, this, [this](const QString &plan) {
        ui.plan->setMarkdown(plan);
        ui.outputTabs->setCurrentIndex(0);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::roleReviewReady, this, [this](const QString &role, const QJsonObject &review, int round) {
        m_reviewsMarkdown += QStringLiteral("# %1 — проверка %2\n\n%3\n\n---\n\n").arg(role).arg(round).arg(orchestrator::reviewMarkdown(review));
        ui.reviewsView->setMarkdown(m_reviewsMarkdown);
        ui.outputTabs->setCurrentIndex(2);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::implementationChanged, this, [this](const QString &result) {
        ui.implementation->setMarkdown(result);
        ui.outputTabs->setCurrentIndex(1);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::stageChanged, this,
            [this](int index, int count, const QString &title) { ui.stage->setText(QStringLiteral("Этап %1/%2: %3").arg(index).arg(count).arg(title)); });
    connect(&m_orchestrator, &orchestrator::Orchestrator::toolResultsReady, this, [this](const QJsonArray &results) {
        for (const auto &value : results)
        {
            const auto result = value.toObject();
            ui.terminal->appendPlainText(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
        }
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::eventOccurred, this,
            [this](const QString &text) { ui.log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + QStringLiteral("  ") + text); });
    connect(&m_orchestrator, &orchestrator::Orchestrator::changed, this, &MainWindow::autosave);
    connect(&m_orchestrator, &orchestrator::Orchestrator::failed, this,
            [this](const QString &error) { QMessageBox::warning(this, QStringLiteral("Не удалось завершить задачу"), error); });
}

orchestrator::Configuration MainWindow::configuration() const
{
    orchestrator::Configuration config;
    config.baseUrl = QUrl(ui.server->text().trimmed());
    config.apiKey = ui.apiKey->text();
    config.pmModel = ui.pmModel->currentText().trimmed();
    config.criticModel = ui.criticModel->currentText().trimmed();
    config.developerModel = ui.developerModel->currentText().trimmed();
    config.developerCriticModel = ui.developerCriticModel->currentText().trimmed();
    config.censorModel = ui.censorModel->currentText().trimmed();
    config.researcherModel = ui.researcherModel->currentText().trimmed();
    config.diagnosticianModel = ui.diagnosticianModel->currentText().trimmed();
    config.maxResearchTurns = ui.researchTurns->value();
    config.maxDiagnosticTurns = ui.diagnosticTurns->value();
    config.maxDiagnosticsPerStage = ui.diagnosticRuns->value();
    config.workspace = ui.workspace->text().trimmed().isEmpty() ? QString{} : QFileInfo(ui.workspace->text().trimmed()).canonicalFilePath();
    config.maxAgentTurns = ui.agentTurns->value();
    config.commandTimeoutMs = ui.commandTimeout->value() * 1000;
    config.timeoutMs = ui.timeout->value() * 1000;
    config.maxTokens = ui.tokens->value();
    config.maxReviews = ui.reviews->value();
    config.maxPmTurns = ui.pmTurns->value();
    config.structuredOutput = ui.structuredOutput->isChecked();
    config.samplingProfile = static_cast<orchestrator::SamplingProfile>(ui.samplingProfile->currentData().toInt());
    config.reasoningMode = static_cast<orchestrator::ReasoningMode>(ui.reasoningMode->currentData().toInt());
    config.maxServiceTokens = ui.serviceTokens->value();
    config.maxReasoningCharacters = ui.reasoningCharacters->value();
    return config;
}

void MainWindow::refreshModels()
{
    if (m_orchestrator.active())
    {
        return;
    }
    m_modelsClient.cancel();
    m_modelsClient.configure(configuration());
    ui.refreshModels->setEnabled(false);
    m_modelsClient.listModels();
}

void MainWindow::startTask()
{
    if (m_orchestrator.active())
    {
        return;
    }
    const auto config = configuration();
    if (config.pmModel.isEmpty() || config.criticModel.isEmpty() || config.developerModel.isEmpty() || config.developerCriticModel.isEmpty() ||
        config.censorModel.isEmpty() || config.workspace.isEmpty() || ui.task->toPlainText().trimmed().isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Задача"), QStringLiteral("Введите задачу, выберите рабочую папку и модели основных ролей."));
        return;
    }
    m_modelsClient.cancel();
    saveSettings();
    clearQuestions();
    ui.plan->clear();
    ui.implementation->clear();
    ui.terminal->clear();
    ui.stage->setText(QStringLiteral("Планирование задачи"));
    ui.reviewsView->clear();
    m_reviewsMarkdown.clear();
    m_specialistsMarkdown.clear();
    ui.specialists->clear();
    ui.log->clear();
    m_saveErrorShown = false;
    ui.outputTabs->setCurrentIndex(0);
    m_orchestrator.start(ui.task->toPlainText(), config);
}

void MainWindow::retryRequest()
{
    m_modelsClient.cancel();
    saveSettings();
    QString error;
    if (!m_orchestrator.retry(configuration(), error))
    {
        QMessageBox::information(this, QStringLiteral("Повтор запроса"), error);
    }
}
void MainWindow::clearQuestions()
{
    m_inputs.clear();
    delete ui.questions->takeWidget();
    auto *placeholder = new QWidget(ui.questions);
    auto *layout = new QVBoxLayout(placeholder);
    layout->addWidget(plainLabel(QStringLiteral("Здесь появятся уточняющие вопросы PM. Критик подключится, когда ответы будут собраны.")));
    layout->addStretch();
    ui.questions->setWidget(placeholder);
}

void MainWindow::showQuestions(const QJsonArray &questions, const QString &summary)
{
    m_inputs.clear();
    delete ui.questions->takeWidget();
    auto *page = new QWidget(ui.questions);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(plainLabel(summary, page));
    int number = 0;
    for (const auto &value : questions)
    {
        const auto question = value.toObject();
        auto *group = new QGroupBox(QStringLiteral("Вопрос %1").arg(++number), page);
        auto *questionLayout = new QVBoxLayout(group);
        questionLayout->addWidget(plainLabel(question.value("text").toString(), group));
        auto *reason = plainLabel(question.value("reason").toString(), group);
        reason->setStyleSheet(QStringLiteral("color: #aab5c4; font-size: 11px;"));
        questionLayout->addWidget(reason);
        auto *answer = new QPlainTextEdit(group);
        answer->setObjectName(QStringLiteral("answer_%1").arg(question.value("id").toString()));
        answer->setPlaceholderText(QStringLiteral("Ваш ответ. Если пока неизвестно, напишите «не знаю»."));
        answer->setFixedHeight(84);
        const auto options = question.value("options").toArray();
        if (!options.isEmpty())
        {
            auto *choices = new QComboBox(group);
            choices->addItem(QStringLiteral("Выбрать вариант или ввести свой ответ…"));
            for (const auto &option : options)
            {
                choices->addItem(option.toString());
            }
            questionLayout->addWidget(choices);
            connect(choices, &QComboBox::activated, answer, [choices, answer](int index) {
                if (index > 0)
                {
                    answer->setPlainText(choices->itemText(index));
                }
            });
        }
        questionLayout->addWidget(answer);
        m_inputs.insert(question.value("id").toString(), answer);
        layout->addWidget(group);
    }
    layout->addStretch();
    ui.questions->setWidget(page);
    ui.questions->setEnabled(true);
    ui.inputTabs->setCurrentIndex(1);
}

void MainWindow::submitAnswers()
{
    QJsonObject answers;
    for (auto iterator = m_inputs.cbegin(); iterator != m_inputs.cend(); ++iterator)
    {
        answers.insert(iterator.key(), iterator.value()->toPlainText());
    }
    QString error;
    if (!m_orchestrator.submitAnswers(answers, error))
    {
        QMessageBox::information(this, QStringLiteral("Опросник"), error);
    }
}

void MainWindow::updateState(orchestrator::Orchestrator::State state)
{
    using State = orchestrator::Orchestrator::State;
    const bool active = m_orchestrator.active();
    const bool generating = active && state != State::AwaitingAnswers;
    ui.state->setText(orchestrator::Orchestrator::stateName(state));
    ui.progress->setRange(0, generating ? 0 : 1);
    ui.progress->setValue(state == State::Completed ? 1 : 0);
    ui.connectionGroup->setEnabled(!active);
    ui.refreshModels->setEnabled(!active);
    ui.limits->setEnabled(!active);
    ui.task->setReadOnly(active);
    ui.loadTask->setEnabled(!active);
    ui.start->setEnabled(!active && !ui.task->toPlainText().trimmed().isEmpty());
    ui.stop->setEnabled(active);
    ui.retry->setEnabled(m_orchestrator.canRetry());
    ui.retry->setText(state == State::LimitReached ? QStringLiteral("Продолжить после лимита") : QStringLiteral("Повторить запрос"));
    ui.submit->setEnabled(state == State::AwaitingAnswers);
    ui.questions->setEnabled(state == State::AwaitingAnswers);
    ui.exportMarkdown->setEnabled(!m_orchestrator.snapshot().value("decomposition").toString().isEmpty());
    ui.exportJson->setEnabled(!m_orchestrator.snapshot().value("id").toString().isEmpty());
    ui.outputTabs->setTabText(0, m_orchestrator.snapshot().value("plan_approved").toBool() ? QStringLiteral("План — одобрен")
                                                                                           : QStringLiteral("Декомпозиция — черновик"));
    if (state == State::Completed)
    {
        ui.outputTabs->setCurrentIndex(1);
    }
}

void MainWindow::autosave()
{
    if (m_orchestrator.snapshot().value("id").toString().isEmpty())
    {
        return;
    }
    QString error;
    const auto directory = orchestrator::SessionStore::defaultDirectory();
    if (orchestrator::SessionStore::save(m_orchestrator.snapshot(), m_orchestrator.markdown(), directory, error))
    {
        ui.saveStatus->setText(QStringLiteral("Автосохранение: %1").arg(QDir::toNativeSeparators(directory)));
        ui.saveStatus->setToolTip(directory);
    }
    else
    {
        ui.saveStatus->setText(error);
        if (!m_saveErrorShown)
        {
            m_saveErrorShown = true;
            ui.log->appendPlainText(error);
            QMessageBox::warning(this, QStringLiteral("Автосохранение"), error);
        }
    }
}

void MainWindow::exportResult(bool json)
{
    const auto path = QFileDialog::getSaveFileName(this, json ? QStringLiteral("Экспорт сессии") : QStringLiteral("Сохранить отчёт о задаче"),
                                                   json ? QStringLiteral("session.json") : QStringLiteral("task-report.md"),
                                                   json ? QStringLiteral("JSON (*.json)") : QStringLiteral("Markdown (*.md)"));
    if (path.isEmpty())
    {
        return;
    }
    const auto bytes = json ? QJsonDocument(m_orchestrator.snapshot()).toJson(QJsonDocument::Indented) : m_orchestrator.markdown().toUtf8();
    QString error;
    if (!orchestrator::SessionStore::write(path, bytes, error))
    {
        QMessageBox::warning(this, QStringLiteral("Сохранение"), error);
    }
}

void MainWindow::loadSettings()
{
    QSettings settings;
    const orchestrator::Configuration defaults;
    const bool migrateLimits = !settings.value("iteration_defaults_v3", false).toBool();
    auto iterationLimit = [&settings, migrateLimits](const char *key, int legacy, int fallback) {
        const int value = settings.value(key, fallback).toInt();
        return migrateLimits && value == legacy ? fallback : value;
    };
    ui.server->setText(settings.value("server", QStringLiteral("http://127.0.0.1:51325")).toString());
    ui.pmModel->setCurrentText(settings.value("pm_model", QStringLiteral("qwen/qwen3.8-27b")).toString());
    ui.criticModel->setCurrentText(settings.value("critic_model", QStringLiteral("qwen/qwen3.8-27b")).toString());
    ui.developerModel->setCurrentText(settings.value("developer_model", ui.pmModel->currentText()).toString());
    ui.developerCriticModel->setCurrentText(settings.value("developer_critic_model", ui.criticModel->currentText()).toString());
    ui.censorModel->setCurrentText(settings.value("censor_model", ui.criticModel->currentText()).toString());
    ui.researcherModel->setCurrentText(settings.value("researcher_model", ui.pmModel->currentText()).toString());
    ui.diagnosticianModel->setCurrentText(settings.value("diagnostician_model", ui.developerModel->currentText()).toString());
    ui.researchTurns->setValue(iterationLimit("research_turns", 12, defaults.maxResearchTurns));
    ui.diagnosticTurns->setValue(iterationLimit("diagnostic_turns", 8, defaults.maxDiagnosticTurns));
    ui.diagnosticRuns->setValue(iterationLimit("diagnostic_runs", 2, defaults.maxDiagnosticsPerStage));
    ui.workspace->setText(settings.value("workspace").toString());
    ui.agentTurns->setValue(iterationLimit("agent_turns", 256, defaults.maxAgentTurns));
    ui.commandTimeout->setValue(settings.value("command_timeout", 1800).toInt());
    ui.apiKey->setText(qEnvironmentVariable("LM_STUDIO_API_KEY"));
    int timeout = settings.value("timeout", 1800).toInt();
    if (!settings.value("timeout_defaults_v2", false).toBool() && timeout == 300)
    {
        timeout = 1800;
    }
    ui.timeout->setValue(timeout);
    const int tokens = settings.value("tokens", 65536).toInt();
    ui.tokens->setValue(!settings.value("token_defaults_v2", false).toBool() && tokens == 12000 ? 65536 : tokens);
    ui.reviews->setValue(iterationLimit("reviews", 6, defaults.maxReviews));
    ui.pmTurns->setValue(iterationLimit("pm_turns", 24, defaults.maxPmTurns));
    ui.structuredOutput->setChecked(settings.value("structured_output", true).toBool());
    const int sampling = ui.samplingProfile->findData(settings.value("sampling_profile", static_cast<int>(defaults.samplingProfile)).toInt());
    ui.samplingProfile->setCurrentIndex(sampling < 0 ? 0 : sampling);
    const int reasoning = ui.reasoningMode->findData(settings.value("reasoning_mode", static_cast<int>(defaults.reasoningMode)).toInt());
    ui.reasoningMode->setCurrentIndex(reasoning < 0 ? 0 : reasoning);
    ui.serviceTokens->setValue(settings.value("service_tokens", defaults.maxServiceTokens).toInt());
    ui.reasoningCharacters->setValue(settings.value("reasoning_characters", defaults.maxReasoningCharacters).toInt());
    restoreGeometry(settings.value("geometry").toByteArray());
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue("server", ui.server->text());
    settings.setValue("pm_model", ui.pmModel->currentText());
    settings.setValue("critic_model", ui.criticModel->currentText());
    settings.setValue("developer_model", ui.developerModel->currentText());
    settings.setValue("developer_critic_model", ui.developerCriticModel->currentText());
    settings.setValue("censor_model", ui.censorModel->currentText());
    settings.setValue("researcher_model", ui.researcherModel->currentText());
    settings.setValue("diagnostician_model", ui.diagnosticianModel->currentText());
    settings.setValue("research_turns", ui.researchTurns->value());
    settings.setValue("diagnostic_turns", ui.diagnosticTurns->value());
    settings.setValue("diagnostic_runs", ui.diagnosticRuns->value());
    settings.setValue("workspace", ui.workspace->text());
    settings.setValue("agent_turns", ui.agentTurns->value());
    settings.setValue("command_timeout", ui.commandTimeout->value());
    settings.setValue("token_defaults_v2", true);
    settings.setValue("iteration_defaults_v3", true);
    settings.setValue("timeout", ui.timeout->value());
    settings.setValue("timeout_defaults_v2", true);
    settings.setValue("tokens", ui.tokens->value());
    settings.setValue("reviews", ui.reviews->value());
    settings.setValue("pm_turns", ui.pmTurns->value());
    settings.setValue("structured_output", ui.structuredOutput->isChecked());
    settings.setValue("sampling_profile", ui.samplingProfile->currentData().toInt());
    settings.setValue("reasoning_mode", ui.reasoningMode->currentData().toInt());
    settings.setValue("service_tokens", ui.serviceTokens->value());
    settings.setValue("reasoning_characters", ui.reasoningCharacters->value());
    settings.setValue("geometry", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_orchestrator.active() &&
        QMessageBox::question(this, QStringLiteral("Завершить работу"),
                              QStringLiteral("Остановить текущую сессию и закрыть приложение? Черновик будет сохранен.")) != QMessageBox::Yes)
    {
        event->ignore();
        return;
    }
    m_orchestrator.stop();
    saveSettings();
    event->accept();
}
} // namespace client