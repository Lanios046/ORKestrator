#include "pipeline/pipeline.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

namespace pipeline
{
namespace
{
QJsonObject object(const QJsonObject &properties)
{
    QJsonArray required;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        required.append(it.key());
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject text()
{
    return {{"type", "string"}};
}
QJsonObject choice(const QString &value)
{
    return {{"type", "string"}, {"enum", QJsonArray{value}}};
}
QJsonObject array(const QJsonObject &items, int minimum, int maximum)
{
    return {{"type", "array"}, {"items", items}, {"minItems", minimum}, {"maxItems", maximum}};
}
}
QStringList toolNames()
{
    return {"list_files", "read_file", "find_files", "search_text", "write_file", "run_command"};
}
QString jsonText(const QJsonObject &value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
QJsonObject Agent::toJson() const
{
    return {{"id", id},
            {"name", name},
            {"model", model},
            {"rules", rules},
            {"prompt", prompt},
            {"result_field", resultField},
            {"positive", positive},
            {"negative", negative},
            {"tools", QJsonArray::fromStringList(tools)},
            {"thinking", static_cast<int>(thinking)},
            {"tokens", tokens},
            {"timeout", timeout},
            {"command_timeout", commandTimeout},
            {"tool_limit", toolLimit},
            {"reasoning_budget", reasoningBudget},
            {"automatic", automatic},
            {"fresh_context", freshContext},
            {"x", position.x()},
            {"y", position.y()}};
}
Agent Agent::fromJson(const QJsonObject &json)
{
    Agent result;
    result.id = json.value("id").toString();
    result.name = json.value("name").toString();
    result.model = json.value("model").toString();
    result.rules = json.value("rules").toString(result.rules);
    result.prompt = json.value("prompt").toString(result.prompt);
    result.resultField = json.value("result_field").toString("result");
    result.positive = json.value("positive").toString("positive");
    result.negative = json.value("negative").toString("negative");
    for (const auto &entry : json.value("tools").toArray())
        result.tools.append(entry.toString());
    result.thinking = static_cast<orchestrator::ReasoningMode>(json.value("thinking").toInt(0));
    result.tokens = json.value("tokens").toInt(result.tokens);
    result.timeout = json.value("timeout").toInt(result.timeout);
    result.commandTimeout = json.value("command_timeout").toInt(result.commandTimeout);
    result.toolLimit = json.value("tool_limit").toInt(result.toolLimit);
    result.reasoningBudget = json.value("reasoning_budget").toInt(result.reasoningBudget);
    result.automatic = json.value("automatic").toBool(true);
    result.freshContext = json.value("fresh_context").toBool(false);
    result.position = {json.value("x").toDouble(), json.value("y").toDouble()};
    return result;
}
QString Agent::validate(bool requireModel) const
{
    if (id.isEmpty() || name.trimmed().isEmpty())
        return QStringLiteral("У агента должны быть ID и имя.");
    if (requireModel && model.trimmed().isEmpty())
        return QStringLiteral("У агента «%1» не выбрана модель.").arg(name);
    if (resultField.isEmpty() || QStringList{"status", "summary", "content", "actions"}.contains(resultField) || positive.isEmpty() || negative.isEmpty() ||
        positive == negative)
        return QStringLiteral("Укажите отдельное поле результата и два различных непустых значения.");
    for (const auto &tool : tools)
        if (!toolNames().contains(tool))
            return QStringLiteral("Неизвестный инструмент: %1.").arg(tool);
    if (tokens < 1 || tokens > 1048576 || timeout < 1 || timeout > 604800 || commandTimeout < 1 || commandTimeout > 604800 || toolLimit < 1 ||
        toolLimit > 1000000 || reasoningBudget < 0 || reasoningBudget > 1048576 || static_cast<int>(thinking) < 0 || static_cast<int>(thinking) > 2)
        return QStringLiteral("Некорректные лимиты или режим мышления агента.");
    return {};
}
QJsonObject Agent::responseSchema() const
{
    const auto emptyActions = array(object({}), 0, 0);
    QJsonObject properties{{"status", choice("ready")},
                           {"summary", text()},
                           {"content", text()},
                           {resultField, QJsonObject{{"type", "string"}, {"enum", QJsonArray{positive, negative}}}},
                           {"actions", emptyActions}};
    const auto ready = object(properties);
    if (tools.isEmpty())
        return ready;
    QJsonArray operations;
    for (const auto &tool : tools)
    {
        QJsonObject fields{{"type", choice(tool)}, {"path", text()}};
        if (tool == "find_files")
            fields.insert("pattern", text());
        if (tool == "search_text")
            fields.insert("query", text());
        if (tool == "write_file")
            fields.insert("content", text());
        if (tool == "run_command")
        {
            fields.insert("program", text());
            fields.insert("arguments", QJsonObject{{"type", "array"}, {"items", text()}});
        }
        operations.append(object(fields));
    }
    const auto actions =
        object({{"status", choice("actions")}, {"summary", text()}, {"actions", array(QJsonObject{{"type", "object"}, {"anyOf", operations}}, 1, 4)}});
    return {{"type", "object"}, {"anyOf", QJsonArray{actions, ready}}};
}
QString Agent::systemPrompt() const
{
    QString result = rules + QStringLiteral("\nВерни один JSON-объект. Для итогового ответа: status=ready, summary — краткий итог, "
                                            "content — полный результат работы, actions=[]. Поле «%1»: «%2» для положительного результата "
                                            "или «%3» для отрицательного. Критерии результата определяются твоими правилами и задачей.\n")
                                 .arg(resultField, positive, negative);
    if (!tools.isEmpty())
        result += QStringLiteral("Для инструментов: status=actions, summary, actions из 1–4 операций. Дождись tool_results. "
                                 "Корень проекта обозначай точкой, файлы — путями вида src/file.cpp. Для каталога используй list_files. "
                                 "Неизвестное имя уточни через find_files, в search_text передавай буквальный текст. "
                                 "Используй уже полученное содержимое, пока файл не изменился.\n");
    if (tools.contains("write_file"))
        result += QStringLiteral("Перед записью существующего файла прочитай его. Передай в write_file полный новый текст и проверь tool_results.\n");
    if (tools.contains("run_command"))
        result += QStringLiteral("Команду задавай через program и arguments без интерактивного ввода. Для cmd используй /C, для ninja — -C.\n");
    return result + QStringLiteral("JSON Schema:\n") + jsonText(responseSchema());
}
QJsonObject Edge::toJson() const
{
    return {{"from", from}, {"to", to}, {"output", positive ? "positive" : "negative"}};
}
const Agent *Pipeline::find(const QString &id) const
{
    for (const auto &agent : agents)
        if (agent.id == id)
            return &agent;
    return nullptr;
}
QString Pipeline::next(const QString &from, bool positive) const
{
    for (const auto &edge : edges)
        if (edge.from == from && edge.positive == positive)
            return edge.to;
    return {};
}
QString Pipeline::validate(bool requireModels) const
{
    if (agents.isEmpty())
        return QStringLiteral("Добавьте хотя бы одного агента.");
    QSet<QString> ids, outputs;
    for (const auto &agent : agents)
    {
        const auto error = agent.validate(requireModels);
        if (!error.isEmpty())
            return error;
        if (ids.contains(agent.id))
            return QStringLiteral("У агентов повторяются ID.");
        ids.insert(agent.id);
    }
    if (!ids.contains(start))
        return QStringLiteral("Назначьте стартового агента.");
    if (automaticLimit < 1 || automaticLimit > 1000000)
        return QStringLiteral("Некорректный лимит автопереходов.");
    for (const auto &edge : edges)
    {
        if (!ids.contains(edge.from) || !ids.contains(edge.to) || edge.from == edge.to)
            return QStringLiteral("Связь должна соединять двух существующих агентов.");
        const auto key = edge.from + (edge.positive ? ":positive" : ":negative");
        if (outputs.contains(key))
            return QStringLiteral("К одному выходу можно подключить одного следующего агента.");
        outputs.insert(key);
    }
    return {};
}
QJsonObject Pipeline::toJson() const
{
    QJsonArray nodes, links;
    for (const auto &agent : agents)
        nodes.append(agent.toJson());
    for (const auto &edge : edges)
        links.append(edge.toJson());
    return {{"format", "orkestrator.pipeline"}, {"version", 1}, {"start", start}, {"automatic_limit", automaticLimit}, {"agents", nodes}, {"edges", links}};
}
bool Pipeline::fromJson(const QJsonObject &json, Pipeline &result, QString &error)
{
    if (json.value("format").toString() != "orkestrator.pipeline" || json.value("version").toInt() != 1 || !json.value("agents").isArray() ||
        !json.value("edges").isArray())
    {
        error = QStringLiteral("Неподдерживаемый формат пайплайна.");
        return false;
    }
    Pipeline parsed;
    parsed.start = json.value("start").toString();
    parsed.automaticLimit = json.value("automatic_limit").toInt(100);
    for (const auto &entry : json.value("agents").toArray())
        parsed.agents.append(Agent::fromJson(entry.toObject()));
    for (const auto &entry : json.value("edges").toArray())
    {
        const auto edge = entry.toObject();
        const auto output = edge.value("output").toString();
        if (output != "positive" && output != "negative")
        {
            error = QStringLiteral("Неизвестный выход агента.");
            return false;
        }
        parsed.edges.append({edge.value("from").toString(), edge.value("to").toString(), output == "positive"});
    }
    error = parsed.validate(false);
    if (!error.isEmpty())
        return false;
    result = parsed;
    return true;
}
bool writeJson(const QString &path, const QJsonObject &json, QString &error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        error = QStringLiteral("Не удалось создать папку.");
        return false;
    }
    QSaveFile file(path);
    const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = file.errorString();
        return false;
    }
    return true;
}
bool readJson(const QString &path, QJsonObject &json, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = file.errorString();
        return false;
    }

    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("Некорректный JSON: %1").arg(parse.errorString());
        return false;
    }
    json = document.object();
    return true;
}
}
