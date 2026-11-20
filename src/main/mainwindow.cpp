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
    resize(1420, 940);
    setMinimumSize(1050, 720);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("ORKestrator"), central);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 600;"));
    layout->addWidget(title);
    layout->addWidget(plainLabel(QStringLiteral("Этап 01  /  Менеджер проекта → уточнения → независимый критик → декомпозиция"), central));

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
    layout->addWidget(ui.connectionGroup);

    ui.limits = new QGroupBox(QStringLiteral("Параметры генерации"), central);
    auto *limits = new QHBoxLayout(ui.limits);
    auto addSpin = [limits, this](const QString &caption, int minimum, int maximum, int value) {
        auto *spin = new QSpinBox(ui.limits);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        limits->addWidget(new QLabel(caption));
        limits->addWidget(spin);
        return spin;
    };
    ui.timeout = addSpin(QStringLiteral("Таймаут, с"), 10, 3600, 300);
    ui.tokens = addSpin(QStringLiteral("Токены ответа"), 512, 65536, 12000);
    ui.reviews = addSpin(QStringLiteral("Проверки"), 1, 30, 6);
    ui.pmTurns = addSpin(QStringLiteral("Ходы PM"), 1, 100, 24);
    ui.structuredOutput = new QCheckBox(QStringLiteral("JSON schema"), ui.limits);
    ui.structuredOutput->setChecked(true);
    ui.structuredOutput->setToolTip(QStringLiteral("Отключите, если модель не поддерживает Structured Output. Проверка JSON останется включена."));
    limits->addWidget(ui.structuredOutput);
    limits->addStretch();
    layout->addWidget(ui.limits);

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
    ui.submit = new QPushButton(QStringLiteral("Отправить ответы PM"), questionsPage);
    ui.submit->setObjectName("submitAnswers");
    questionsLayout->addWidget(ui.submit);
    ui.inputTabs->addTab(questionsPage, QStringLiteral("Опросник"));
    clearQuestions();

    ui.outputTabs = new QTabWidget(splitter);
    ui.plan = markdownView(ui.outputTabs);
    ui.plan->setObjectName("decompositionView");
    ui.reviewsView = markdownView(ui.outputTabs);
    ui.reviewsView->setObjectName("reviewsView");
    ui.log = new QPlainTextEdit(ui.outputTabs);
    ui.log->setReadOnly(true);
    ui.log->setMaximumBlockCount(3000);
    ui.outputTabs->addTab(ui.plan, QStringLiteral("Декомпозиция"));
    ui.outputTabs->addTab(ui.reviewsView, QStringLiteral("Критик"));
    ui.outputTabs->addTab(ui.log, QStringLiteral("Журнал"));
    splitter->addWidget(ui.inputTabs);
    splitter->addWidget(ui.outputTabs);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({520, 820});
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout();
    ui.start = new QPushButton(QStringLiteral("Начать декомпозицию"), central);
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
            this, QStringLiteral("ORKestrator 0.1.0"),
            QStringLiteral("Этап 1: PM уточняет задачу и создает план. Независимый критик одобряет его или возвращает на доработку.\n\n"
                           "Сессии агентов — отдельные истории сообщений API. Их можно вести на одной модели. Выполнение подзадач в этот этап не входит."));
    });
}

void MainWindow::setupConnections()
{
    connect(ui.refreshModels, &QPushButton::clicked, this, &MainWindow::refreshModels);
    connect(ui.start, &QPushButton::clicked, this, &MainWindow::startTask);
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
        for (auto *combo : {ui.pmModel, ui.criticModel})
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
    connect(&m_orchestrator, &orchestrator::Orchestrator::questionsReady, this, &MainWindow::showQuestions);
    connect(&m_orchestrator, &orchestrator::Orchestrator::planChanged, this, [this](const QString &plan) {
        ui.plan->setMarkdown(plan);
        ui.outputTabs->setCurrentIndex(0);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::reviewReady, this, [this](const QJsonObject &review, int round) {
        m_reviewsMarkdown += QStringLiteral("# Проверка %1\n\n%2\n\n---\n\n").arg(round).arg(orchestrator::reviewMarkdown(review));
        ui.reviewsView->setMarkdown(m_reviewsMarkdown);
        ui.outputTabs->setCurrentIndex(1);
    });
    connect(&m_orchestrator, &orchestrator::Orchestrator::eventOccurred, this,
            [this](const QString &text) { ui.log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + QStringLiteral("  ") + text); });
    connect(&m_orchestrator, &orchestrator::Orchestrator::changed, this, &MainWindow::autosave);
    connect(&m_orchestrator, &orchestrator::Orchestrator::failed, this,
            [this](const QString &error) { QMessageBox::warning(this, QStringLiteral("Не удалось завершить декомпозицию"), error); });
}

orchestrator::Configuration MainWindow::configuration() const
{
    orchestrator::Configuration config;
    config.baseUrl = QUrl(ui.server->text().trimmed());
    config.apiKey = ui.apiKey->text();
    config.pmModel = ui.pmModel->currentText().trimmed();
    config.criticModel = ui.criticModel->currentText().trimmed();
    config.timeoutMs = ui.timeout->value() * 1000;
    config.maxTokens = ui.tokens->value();
    config.maxReviews = ui.reviews->value();
    config.maxPmTurns = ui.pmTurns->value();
    config.structuredOutput = ui.structuredOutput->isChecked();
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
    if (config.pmModel.isEmpty() || config.criticModel.isEmpty() || ui.task->toPlainText().trimmed().isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Задача"), QStringLiteral("Введите задачу и выберите модели PM и критика."));
        return;
    }
    m_modelsClient.cancel();
    saveSettings();
    clearQuestions();
    ui.plan->clear();
    ui.reviewsView->clear();
    m_reviewsMarkdown.clear();
    ui.log->clear();
    m_saveErrorShown = false;
    ui.outputTabs->setCurrentIndex(0);
    m_orchestrator.start(ui.task->toPlainText(), config);
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
    const bool generating = state == State::PmWorking || state == State::CriticWorking;
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
    ui.submit->setEnabled(state == State::AwaitingAnswers);
    ui.questions->setEnabled(state == State::AwaitingAnswers);
    ui.exportMarkdown->setEnabled(!m_orchestrator.snapshot().value("decomposition").toString().isEmpty());
    ui.exportJson->setEnabled(!m_orchestrator.snapshot().value("id").toString().isEmpty());
    ui.outputTabs->setTabText(0, state == State::Completed ? QStringLiteral("Декомпозиция — одобрено") : QStringLiteral("Декомпозиция — черновик"));
    if (state == State::Completed)
    {
        ui.outputTabs->setCurrentIndex(0);
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
    const auto path = QFileDialog::getSaveFileName(this, json ? QStringLiteral("Экспорт сессии") : QStringLiteral("Сохранить декомпозицию"),
                                                   json ? QStringLiteral("session.json") : QStringLiteral("decomposition.md"),
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
    ui.server->setText(settings.value("server", QStringLiteral("http://127.0.0.1:51325")).toString());
    ui.pmModel->setCurrentText(settings.value("pm_model", QStringLiteral("qwen/qwen3.8-27b")).toString());
    ui.criticModel->setCurrentText(settings.value("critic_model", QStringLiteral("qwen/qwen3.8-27b")).toString());
    ui.apiKey->setText(qEnvironmentVariable("LM_STUDIO_API_KEY"));
    ui.timeout->setValue(settings.value("timeout", 300).toInt());
    ui.tokens->setValue(settings.value("tokens", 12000).toInt());
    ui.reviews->setValue(settings.value("reviews", 6).toInt());
    ui.pmTurns->setValue(settings.value("pm_turns", 24).toInt());
    ui.structuredOutput->setChecked(settings.value("structured_output", true).toBool());
    restoreGeometry(settings.value("geometry").toByteArray());
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue("server", ui.server->text());
    settings.setValue("pm_model", ui.pmModel->currentText());
    settings.setValue("critic_model", ui.criticModel->currentText());
    settings.setValue("timeout", ui.timeout->value());
    settings.setValue("tokens", ui.tokens->value());
    settings.setValue("reviews", ui.reviews->value());
    settings.setValue("pm_turns", ui.pmTurns->value());
    settings.setValue("structured_output", ui.structuredOutput->isChecked());
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