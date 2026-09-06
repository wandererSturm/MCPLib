#ifndef MCPCOMMANDREGISTRY_H
#define MCPCOMMANDREGISTRY_H
#include <QObject>
#include <QMap>
#include <QMutex>
#include <QSharedPointer>
#include <QJsonArray>
#include "mcpcommand.h"
#include "mcpserverlib_global.h"

class MCPSERVERLIB_EXPORT McpCommandRegistry : public QObject {
    Q_OBJECT

public:
    explicit McpCommandRegistry(QObject *parent = nullptr);

    // Add a command
    bool addCommand(const QSharedPointer<McpCommand> &cmd);

    // Remove a command by name
    bool removeCommand(const QString &commandName);

    // List all registered command definitions formatted for tools/list
    QJsonArray getToolDefinitions() const;

    // Execute a command by name
    QJsonObject executeCommand(const QString &name, const QJsonObject &args, bool *ok = nullptr);

signals:
    void registryChanged();

private:
    mutable QMutex m_mutex;
    QMap<QString, QSharedPointer<McpCommand>> m_commands;
};
#endif // MCPCOMMANDREGISTRY_H
