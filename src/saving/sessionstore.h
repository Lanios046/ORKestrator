#pragma once

#include <QJsonObject>
#include <QString>

namespace orchestrator
{
class SessionStore
{
  public:
    static QString defaultDirectory();
    static bool write(const QString &path, const QByteArray &data, QString &error);
    static bool save(const QJsonObject &session, const QString &markdown, const QString &directory, QString &error);
};
} // namespace orchestrator