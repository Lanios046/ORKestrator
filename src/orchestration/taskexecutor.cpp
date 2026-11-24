#include "taskexecutor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

namespace orchestrator
{
namespace
{
constexpr qsizetype fileLimit = 256 * 1024;
constexpr qsizetype outputLimit = 64 * 1024;

QJsonObject directorySnapshot(const QDir &directory)
{
    QJsonArray entries;
    const auto files = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);
    bool truncated = false;
    for (const auto &file : files)
    {
        if (file.fileName() == ".git")
        {
            continue;
        }
        if (entries.size() == 200)
        {
            truncated = true;
            break;
        }
        entries.append(QJsonObject{{"name", file.fileName()}, {"directory", file.isDir()}, {"bytes", file.size()}});
    }
    return {{"entries", entries}, {"truncated", truncated}};
}

bool inside(const QString &path, const QString &root)
{
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.compare(root, sensitivity) == 0 || path.startsWith(root + '/', sensitivity);
}

QByteArray hash(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
} // namespace

TaskExecutor::TaskExecutor(QObject *parent) : QObject(parent)
{
}
TaskExecutor::~TaskExecutor()
{
    cancel();
}

bool TaskExecutor::configure(const QString &workspace, QString &error)
{
    cancel();
    m_observed.clear();
    const QFileInfo info(workspace);
    if (workspace.trimmed().isEmpty() || !info.isDir() || info.canonicalFilePath().isEmpty() || QDir(info.canonicalFilePath()).isRoot())
    {
        error = QStringLiteral("Выберите существующую рабочую папку проекта.");
        return false;
    }
    m_workspace = QDir::fromNativeSeparators(info.canonicalFilePath());
    while (m_workspace.endsWith('/') && m_workspace.size() > 1)
    {
        m_workspace.chop(1);
    }
    return true;
}

QString TaskExecutor::workspace() const
{
    return m_workspace;
}

QString TaskExecutor::observationFingerprint(const QJsonObject &action) const
{
    QString error;
    const auto path = resolvePath(action.value("path").toString(), error);
    if (path.isEmpty())
    {
        return {};
    }
    const auto type = action.value("type").toString();
    if (type == "read_file" || type == "write_file")
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > fileLimit)
        {
            return {};
        }
        return QString::fromLatin1(hash(file.readAll()));
    }
    if (type == "list_files" && QFileInfo(path).isDir())
    {
        return QString::fromLatin1(hash(QJsonDocument(directorySnapshot(QDir(path))).toJson(QJsonDocument::Compact)));
    }
    return {};
}
QString TaskExecutor::resolvePath(const QString &relative, QString &error) const
{
    const auto normalized = QDir::fromNativeSeparators(relative);
    if (QDir::isAbsolutePath(normalized) || normalized.contains(':') || normalized.split('/').contains("..") ||
        normalized.split('/').contains(".git", Qt::CaseInsensitive))
    {
        error = QStringLiteral("Нужен относительный путь внутри проекта; выход за корень и доступ к .git запрещены.");
        return {};
    }
    const auto absolute = QDir::cleanPath(QDir(m_workspace).absoluteFilePath(normalized.isEmpty() ? "." : normalized));
    if (!inside(absolute, m_workspace))
    {
        error = QStringLiteral("Путь выходит за рабочую папку.");
        return {};
    }
    QFileInfo existing(absolute);
    while (!existing.exists() && !existing.isSymLink())
    {
        const auto parent = existing.dir().absolutePath();
        if (parent == existing.absoluteFilePath())
        {
            break;
        }
        existing.setFile(parent);
    }
    const auto canonical = QDir::fromNativeSeparators(existing.canonicalFilePath());
    if (canonical.isEmpty() || !inside(canonical, m_workspace))
    {
        error = QStringLiteral("Ссылка или родительский каталог выходит за рабочую папку.");
        return {};
    }
    return absolute;
}

void TaskExecutor::execute(const QJsonArray &actions, bool canWrite, int timeoutMs)
{
    cancel();
    m_actions = actions;
    m_results = {};
    m_index = 0;
    m_canWrite = canWrite;
    m_timeoutMs = timeoutMs;
    m_active = true;
    scheduleNext();
}

void TaskExecutor::cancel()
{
    ++m_generation;
    m_active = false;
    if (m_process)
    {
        auto *process = m_process.data();
        m_process = nullptr;
        disconnect(process, nullptr, this, nullptr);
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
        process->kill();
        if (process->state() == QProcess::NotRunning)
        {
            process->deleteLater();
        }
    }
}

void TaskExecutor::scheduleNext()
{
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation == m_generation && m_active)
        {
            next();
        }
    });
}

void TaskExecutor::next()
{
    if (m_index >= m_actions.size())
    {
        m_active = false;
        emit finished(m_results);
        return;
    }
    const auto action = m_actions.at(m_index++).toObject();
    emit activity(QStringLiteral("%1: %2").arg(action.value("type").toString(), action.value("type").toString() == "run_command"
                                                                                    ? action.value("program").toString()
                                                                                    : action.value("path").toString()));
    if (action.value("type").toString() == "run_command")
    {
        runCommand(action);
        return;
    }
    m_results.append(fileAction(action));
    scheduleNext();
}

QJsonObject TaskExecutor::fileAction(const QJsonObject &action)
{
    QJsonObject result{{"type", action.value("type")}, {"path", action.value("path")}, {"success", false}};
    QString error;
    const auto path = resolvePath(action.value("path").toString(), error);
    if (path.isEmpty())
    {
        result.insert("error", error);
        return result;
    }
    const auto type = action.value("type").toString();
    if (type == "list_files")
    {
        QDir directory(path);
        if (!directory.exists())
        {
            result.insert("error", "Directory does not exist");
            return result;
        }
        const auto snapshot = directorySnapshot(directory);
        result.insert("entries", snapshot.value("entries"));
        result.insert("truncated", snapshot.value("truncated"));
    }
    else if (type == "read_file")
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            result.insert("error", file.errorString());
            return result;
        }
        if (file.size() > fileLimit)
        {
            result.insert("error", "File exceeds 256 KiB; inspect a smaller part with a diagnostic command.");
            return result;
        }
        const auto bytes = file.readAll();
        if (bytes.contains('\0'))
        {
            result.insert("error", "Binary files cannot be read as UTF-8 text.");
            return result;
        }
        m_observed.insert(path, hash(bytes));
        result.insert("content", QString::fromUtf8(bytes));
        result.insert("sha256", QString::fromLatin1(hash(bytes)));
    }
    else if (type == "write_file")
    {
        if (!m_canWrite)
        {
            result.insert("error", "Only the developer can write files.");
            return result;
        }
        const auto content = action.value("content").toString().toUtf8();
        if (content.size() > fileLimit)
        {
            result.insert("error", "File exceeds the 256 KiB write limit.");
            return result;
        }
        bool changed = true;
        if (QFileInfo::exists(path))
        {
            QFile current(path);
            if (!current.open(QIODevice::ReadOnly))
            {
                result.insert("error", current.errorString());
                return result;
            }
            const auto previous = hash(current.readAll());
            changed = previous != hash(content);
            if (!m_observed.contains(path) || previous != m_observed.value(path))
            {
                result.insert("error", "Read the current file before overwriting it; it is unread or changed since the last read.");
                return result;
            }
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        {
            result.insert("error", "Cannot create parent directory.");
            return result;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
        {
            result.insert("error", file.errorString());
            return result;
        }
        m_observed.insert(path, hash(content));
        result.insert("sha256", QString::fromLatin1(hash(content)));
        result.insert("bytes", content.size());
        result.insert("changed", changed);
    }
    else
    {
        result.insert("error", "Unknown file operation.");
        return result;
    }
    result.insert("success", true);
    return result;
}

void TaskExecutor::runCommand(const QJsonObject &action)
{
    QString error;
    const auto directory = resolvePath(action.value("path").toString(), error);
    QJsonObject initial{{"type", "run_command"},
                        {"path", action.value("path")},
                        {"program", action.value("program")},
                        {"arguments", action.value("arguments")},
                        {"success", false}};
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || action.value("program").toString().trimmed().isEmpty())
    {
        initial.insert("error", error.isEmpty() ? QStringLiteral("Нужны программа и существующий рабочий каталог.") : error);
        m_results.append(initial);
        scheduleNext();
        return;
    }
    auto *process = new QProcess(this);
    m_process = process;
    process->setWorkingDirectory(directory);
    process->setProcessChannelMode(QProcess::MergedChannels);
    QStringList arguments;
    for (const auto &value : action.value("arguments").toArray())
    {
        arguments.append(value.toString());
    }
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process]() {
        process->setProperty("timed_out", true);
        process->kill();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [process]() {
        auto output = process->property("output").toByteArray() + process->readAllStandardOutput();
        if (output.size() > outputLimit)
        {
            output = output.right(outputLimit);
            process->setProperty("truncated", true);
        }
        process->setProperty("output", output);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, initial](QProcess::ProcessError processError) mutable {
        if (processError != QProcess::FailedToStart)
        {
            return;
        }
        initial.insert("error", process->errorString());
        initial.insert("exit_code", -1);
        m_results.append(initial);
        m_process = nullptr;
        process->deleteLater();
        scheduleNext();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, timer, initial](int code, QProcess::ExitStatus status) mutable {
                timer->stop();
                auto output = process->property("output").toByteArray() + process->readAllStandardOutput();
                const bool truncated = process->property("truncated").toBool() || output.size() > outputLimit;
                const bool timeout = process->property("timed_out").toBool();
                initial.insert("success", code == 0 && status == QProcess::NormalExit && !timeout);
                initial.insert("exit_code", code);
                initial.insert("timed_out", timeout);
                initial.insert("output_truncated", truncated);
                initial.insert("output", QString::fromUtf8(output.right(outputLimit)));
                m_results.append(initial);
                emit activity(QStringLiteral("Команда завершена: %1, код %2%3")
                                  .arg(initial.value("program").toString())
                                  .arg(code)
                                  .arg(timeout ? QStringLiteral(" (таймаут)") : QString{}));
                m_process = nullptr;
                process->deleteLater();
                scheduleNext();
            });
    process->start(action.value("program").toString(), arguments);
    timer->start(m_timeoutMs);
}
} // namespace orchestrator