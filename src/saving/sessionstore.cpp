#include "sessionstore.h"

#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace orchestrator
{
QString SessionStore::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/sessions");
}

bool SessionStore::write(const QString &path, const QByteArray &data, QString &error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
    {
        error = QStringLiteral("Не удалось сохранить %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool SessionStore::save(const QJsonObject &session, const QString &markdown, const QString &directory, QString &error)
{
    const auto id = session.value("id").toString();
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id.contains(".."))
    {
        error = QStringLiteral("Некорректный идентификатор сессии.");
        return false;
    }
    if (!QDir().mkpath(directory))
    {
        error = QStringLiteral("Не удалось создать папку сессий: %1").arg(directory);
        return false;
    }
    const auto prefix = QDir(directory).filePath(id);
    return write(prefix + ".json", QJsonDocument(session).toJson(QJsonDocument::Indented), error) && write(prefix + ".md", markdown.toUtf8(), error);
}
} // namespace orchestrator