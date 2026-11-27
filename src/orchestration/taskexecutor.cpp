#include "taskexecutor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
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

QJsonObject TaskExecutor::projectContext(const QString &task) const
{
    auto inventoryAction = QJsonObject{{"type", "find_files"}, {"path", ""}, {"pattern", "*"}};
    const auto inventory = searchAction(inventoryAction, m_workspace);
    QStringList candidates{"AGENTS.md", ".clang-format"};
    static const QRegularExpression references(
        QStringLiteral("[\\p{L}\\p{N}_./\\\\:-]+\\.(?:hpp|cpp|cxx|cmake|json|yaml|proto|txt|qml|ps1|yml|md|py|ui|h|c)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    auto matches = references.globalMatch(task);
    while (matches.hasNext() && candidates.size() < 12)
    {
        candidates.append(matches.next().captured());
    }
    candidates.append({"CMakeLists.txt", "README.md"});
    QSet<QString> seen;
    QJsonArray files, unavailable;
    int remaining = 32768;
    for (auto candidate : candidates)
    {
        if (files.size() >= 8 || remaining <= 0)
        {
            break;
        }
        candidate = QDir::fromNativeSeparators(candidate);
        if (QDir::isAbsolutePath(candidate))
        {
            candidate = QDir(m_workspace).relativeFilePath(candidate);
        }
        QString error;
        auto path = resolvePath(candidate, error);
        if ((path.isEmpty() || !QFileInfo(path).isFile()) && !candidate.contains('/') && !candidate.contains(':'))
        {
            QString match;
            for (const auto &entry : inventory.value("matches").toArray())
            {
                const auto relative = entry.toObject().value("path").toString();
                if (QFileInfo(relative).fileName().compare(candidate, Qt::CaseInsensitive) == 0)
                {
                    if (!match.isEmpty())
                    {
                        match.clear();
                        break;
                    }
                    match = relative;
                }
            }
            if (!match.isEmpty())
            {
                path = resolvePath(match, error);
            }
        }
        if (path.isEmpty() || !QFileInfo(path).isFile())
        {
            continue;
        }
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (seen.contains(canonical))
        {
            continue;
        }
        seen.insert(canonical);
        const auto relative = QDir::fromNativeSeparators(QDir(m_workspace).relativeFilePath(path));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            unavailable.append(QJsonObject{{"path", relative}, {"error", file.errorString()}});
            continue;
        }
        QTextStream stream(&file);
        const auto content = stream.read(qMin(remaining, 8192));
        if (content.contains(QChar::Null))
        {
            unavailable.append(QJsonObject{{"path", relative}, {"error", QStringLiteral("Файл не является текстом.")}});
            continue;
        }
        remaining -= content.size();
        files.append(QJsonObject{{"path", relative}, {"content", content}, {"content_truncated", !stream.atEnd()}, {"file_bytes", file.size()}});
    }
    return {{"workspace", m_workspace},
            {"root", directorySnapshot(QDir(m_workspace))},
            {"inventory", inventory},
            {"files", files},
            {"unavailable", unavailable},
            {"limits", QJsonObject{{"included_files", 8}, {"characters_per_file", 8192}, {"total_characters", 32768}}},
            {"note", QStringLiteral("Контекст собран приложением один раз. Это ограниченный обзор, а не полное исследование проекта. "
                                    "Разработчик читает нужный код самостоятельно перед изменением.")}};
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
    if (type == "find_files" || type == "search_text")
    {
        auto result = searchAction(action, path);
        if (!result.value("success").toBool())
        {
            return {};
        }
        result.remove("path");
        return QString::fromLatin1(hash(QJsonDocument(result).toJson(QJsonDocument::Compact)));
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

QString TaskExecutor::describeAction(const QJsonObject &action)
{
    const auto type = action.value("type").toString();
    const auto path = action.value("path").toString();
    QString label = type + " " + (path.isEmpty() ? QStringLiteral(".") : path);
    if (type == "search_text" || type == "find_files")
        label += QStringLiteral(" «%1»").arg(action.value(type == "search_text" ? "query" : "pattern").toString());
    else if (type == "run_command")
        label += " " + action.value("program").toString();
    return label;
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
    if (action.value("type").toString() == "run_command")
    {
        runCommand(action);
        return;
    }
    const auto result = fileAction(action);
    m_results.append(result);
    QString detail;
    if (!result.value("success").toBool())
        detail = QStringLiteral("ошибка: %1").arg(result.value("error").toString());
    else if (result.contains("matches"))
        detail = QStringLiteral("совпадений: %1").arg(result.value("matches").toArray().size());
    else if (result.contains("entries"))
        detail = QStringLiteral("элементов: %1").arg(result.value("entries").toArray().size());
    else if (result.contains("changed"))
        detail = QStringLiteral("%1, %2 байт")
                     .arg(result.value("changed").toBool() ? QStringLiteral("изменён") : QStringLiteral("без изменений"))
                     .arg(result.value("bytes").toInteger());
    else
        detail = QStringLiteral("прочитано символов: %1").arg(result.value("content").toString().size());
    if (result.value("truncated").toBool())
        detail += QStringLiteral(" (выдача ограничена)");
    emit activity(describeAction(action) + QStringLiteral(" — ") + detail);
    scheduleNext();
}

QJsonObject TaskExecutor::searchAction(const QJsonObject &action, const QString &root) const
{
    const auto type = action.value("type").toString();
    const bool filenames = type == "find_files";
    QJsonObject result{{"type", type}, {"path", action.value("path")}, {"success", false}};
    const auto needle = action.value(filenames ? "pattern" : "query").toString();
    if (needle.trimmed().isEmpty() || needle.size() > 512 || !QFileInfo::exists(root))
    {
        result.insert("error", QStringLiteral("Нужны существующий путь и непустой pattern/query длиной до 512 символов."));
        return result;
    }
    const QRegularExpression pattern(QRegularExpression::wildcardToRegularExpression(needle), QRegularExpression::CaseInsensitiveOption);
    if (filenames && !pattern.isValid())
    {
        result.insert("error", QStringLiteral("Некорректная маска имени файла."));
        return result;
    }
    static const QSet<QString> excluded{".git", ".svn", "node_modules", ".venv", "venv", "__pycache__", "libs", "build", "out", "dist"};
    QQueue<QFileInfo> pending;
    pending.enqueue(QFileInfo(root));
    QJsonArray matches;
    int scanned = 0, skipped = 0;
    qsizetype bytesRead = 0;
    bool truncated = false;
    while (!pending.isEmpty())
    {
        if (scanned >= 10000 || matches.size() >= 100)
        {
            truncated = true;
            break;
        }
        const auto entry = pending.dequeue();
        ++scanned;
        if (entry.isSymLink() || !inside(QDir::fromNativeSeparators(entry.canonicalFilePath()), m_workspace))
        {
            continue;
        }
        if (entry.isDir())
        {
            const auto children = QDir(entry.absoluteFilePath())
                                      .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks, QDir::DirsFirst | QDir::Name);
            for (const auto &child : children)
            {
                if (child.isDir() && excluded.contains(child.fileName().toCaseFolded()))
                {
                    continue;
                }
                if (pending.size() + scanned >= 10000)
                {
                    truncated = true;
                    break;
                }
                pending.enqueue(child);
            }
            continue;
        }
        if (!entry.isFile())
        {
            continue;
        }
        const auto relative = QDir::fromNativeSeparators(QDir(m_workspace).relativeFilePath(entry.absoluteFilePath()));
        if (filenames)
        {
            if (pattern.match(entry.fileName()).hasMatch())
            {
                matches.append(QJsonObject{{"path", relative}, {"bytes", entry.size()}});
            }
            continue;
        }
        if (entry.size() > fileLimit)
        {
            ++skipped;
            continue;
        }
        if (bytesRead + entry.size() > 16 * 1024 * 1024)
        {
            truncated = true;
            break;
        }
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            ++skipped;
            continue;
        }
        const auto bytes = file.read(fileLimit + 1);
        if (bytes.size() > fileLimit)
        {
            ++skipped;
            continue;
        }
        bytesRead += bytes.size();
        if (bytes.contains('\0'))
        {
            continue;
        }
        const auto lines = QString::fromUtf8(bytes).split('\n');
        for (qsizetype line = 0; line < lines.size(); ++line)
        {
            const auto position = lines[line].indexOf(needle, 0, Qt::CaseInsensitive);
            if (position < 0)
            {
                continue;
            }
            matches.append(QJsonObject{{"path", relative}, {"line", line + 1}, {"text", lines[line].mid(qMax(qsizetype{0}, position - 80), 240)}});
            if (matches.size() >= 100)
            {
                truncated = true;
                break;
            }
        }
    }
    result.insert("success", true);
    result.insert("matches", matches);
    result.insert("scanned_entries", scanned);
    result.insert("skipped_files", skipped);
    result.insert("truncated", truncated);
    return result;
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
    if (type == "find_files" || type == "search_text")
    {
        return searchAction(action, path);
    }
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
    emit activity(QStringLiteral("run_command: %1; arguments=%2; cwd=%3; таймаут=%4 с")
                      .arg(action.value("program").toString(),
                           QString::fromUtf8(QJsonDocument(action.value("arguments").toArray()).toJson(QJsonDocument::Compact)), directory)
                      .arg(m_timeoutMs / 1000));
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || action.value("program").toString().trimmed().isEmpty())
    {
        initial.insert("error", error.isEmpty() ? QStringLiteral("Нужны программа и существующий рабочий каталог.") : error);
        m_results.append(initial);
        scheduleNext();
        return;
    }
    const auto program = action.value("program").toString();
    auto executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
    {
        const QFileInfo candidate(QDir(directory).absoluteFilePath(program));
        if (candidate.isFile())
        {
            executable = candidate.absoluteFilePath();
        }
    }
    if (executable.isEmpty())
    {
        initial.insert("error", QStringLiteral("Исполняемая программа не найдена. program должен содержать только имя или путь программы; "
                                               "команда, ключи и shell-синтаксис передаются отдельно в arguments."));
        initial.insert("exit_code", -1);
        m_results.append(initial);
        scheduleNext();
        return;
    }
    QStringList arguments;
    for (const auto &value : action.value("arguments").toArray())
    {
        arguments.append(value.toString());
    }
    if (QFileInfo(executable).baseName().compare("cmd", Qt::CaseInsensitive) == 0)
    {
        bool command = false, keepOpen = false;
        for (qsizetype i = 0; i < arguments.size(); ++i)
        {
            keepOpen |= arguments[i].compare("/k", Qt::CaseInsensitive) == 0;
            command |= arguments[i].compare("/c", Qt::CaseInsensitive) == 0 && i + 1 < arguments.size() && !arguments[i + 1].trimmed().isEmpty();
        }
        if (!command || keepOpen)
        {
            initial.insert("error", QStringLiteral("cmd требует /C и команду; -C и интерактивный /K здесь не подходят. "
                                                   "Для Ninja используйте program=ninja, arguments=[\"-C\",\"каталог_сборки\"]."));
            initial.insert("exit_code", -1);
            m_results.append(initial);
            scheduleNext();
            return;
        }
    }
    auto *process = new QProcess(this);
    m_process = process;
    process->setWorkingDirectory(directory);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardInputFile(QProcess::nullDevice());
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process]() {
        process->setProperty("timed_out", true);
        process->kill();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        const auto chunk = process->readAllStandardOutput();
        auto output = process->property("output").toByteArray() + chunk;
        const auto message = QString::fromUtf8(chunk.right(2048)).trimmed();
        if (!message.isEmpty())
        {
            emit activity(QStringLiteral("Вывод команды: %1").arg(message));
        }
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
    process->start(executable, arguments);
    timer->start(m_timeoutMs);
}
} // namespace orchestrator