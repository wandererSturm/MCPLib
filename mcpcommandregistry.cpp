#include "mcpcommandregistry.h"



McpCommandRegistry::McpCommandRegistry(QObject *parent) : QObject(parent) {

}

bool McpCommandRegistry::addCommand(const QSharedPointer<McpCommand> &cmd) {
    if (!cmd) return false;
    QString name = cmd->definition()["name"].toString();
    if (name.isEmpty()) return false;

    {
        QMutexLocker locker(&m_mutex);
        if (m_commands.contains(name)) return false;
        m_commands.insert(name, cmd);
    }
    emit registryChanged();
    return true;
}

bool McpCommandRegistry::removeCommand(const QString &commandName) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_commands.remove(commandName) == 0) return false;
    }
    emit registryChanged();
    return true;
}

QJsonArray McpCommandRegistry::getToolDefinitions() const {
    QMutexLocker locker(&m_mutex);
    QJsonArray tools;
    for (const auto &cmd : m_commands) {
        tools.append(cmd->definition());
    }
    return tools;
}

QJsonObject McpCommandRegistry::executeCommand(const QString &name, const QJsonObject &args, bool *ok) {
    QSharedPointer<McpCommand> cmd;
    {
        QMutexLocker locker(&m_mutex);
        cmd = m_commands.value(name);
    }

    if (cmd) {
        if (ok) *ok = true;
        return cmd->execute(args);
    }
    if (ok) *ok = false;
    return QJsonObject{
        {"isError", true},
        {"content", QJsonArray{
                        QJsonObject{{"type", "text"}, {"text", "Unknown command: " + name}}
                    }}
    };
}
