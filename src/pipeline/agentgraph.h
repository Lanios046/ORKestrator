#pragma once
#include "pipeline/pipeline.h"

#include <QLabel>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/NodeDelegateModel>

namespace pipeline
{
class AgentGraphModel final : public QtNodes::DataFlowGraphModel
{
  public:
    using QtNodes::DataFlowGraphModel::DataFlowGraphModel;
    bool loopsEnabled() const override
    {
        return true;
    }
};
class AgentDelegate final : public QtNodes::NodeDelegateModel
{
  public:
    Agent agent;
    QString name() const override
    {
        return "Agent";
    }
    QString caption() const override
    {
        return agent.name;
    }
    bool portCaptionVisible(QtNodes::PortType, QtNodes::PortIndex) const override
    {
        return true;
    }
    QString portCaption(QtNodes::PortType type, QtNodes::PortIndex index) const override
    {
        if (type == QtNodes::PortType::In)
            return QStringLiteral("Вход");
        return index == 0 ? QStringLiteral("Положительный результат") : QStringLiteral("Отрицательный результат");
    }
    unsigned int nPorts(QtNodes::PortType type) const override
    {
        return type == QtNodes::PortType::In ? 1U : 2U;
    }
    QtNodes::NodeDataType dataType(QtNodes::PortType, QtNodes::PortIndex) const override
    {
        return {"agent-result", QStringLiteral("Результат")};
    }
    QtNodes::ConnectionPolicy portConnectionPolicy(QtNodes::PortType type, QtNodes::PortIndex) const override
    {
        return type == QtNodes::PortType::In ? QtNodes::ConnectionPolicy::Many : QtNodes::ConnectionPolicy::One;
    }
    void setInData(std::shared_ptr<QtNodes::NodeData>, QtNodes::PortIndex) override
    {
    }
    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex) override
    {
        return {};
    }
    QWidget *embeddedWidget() override
    {
        if (!m_label)
        {
            m_label = new QLabel;
            m_label->setMinimumWidth(190);
            m_label->setWordWrap(true);
        }
        refresh();
        return m_label;
    }
    void refresh()
    {
        if (m_label)
            m_label->setText(agent.model.isEmpty() ? QStringLiteral("Выберите модель в свойствах") : agent.model);
    }
    QJsonObject save() const override
    {
        auto result = QtNodes::NodeDelegateModel::save();
        result.insert("agent", agent.toJson());
        return result;
    }
    void load(const QJsonObject &json) override
    {
        QtNodes::NodeDelegateModel::load(json);
        if (json.value("agent").isObject())
            agent = Agent::fromJson(json.value("agent").toObject());
        refresh();
    }

  private:
    QLabel *m_label{nullptr};
};
}
