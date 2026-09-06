#ifndef MCPCOMMANDREGISTRY_H
#define MCPCOMMANDREGISTRY_H
#include <QObject>
#include <QMap>
#include <QMutex>
#include <QSharedPointer>
#include <QJsonArray>
#include "mcpcommand.h"
#include "mcpserverlib_global.h"

// Thread-safe: executeCommand() is called from worker threads (see McpServer),
// while addCommand()/removeCommand() are typically called from the app's main
// thread, so access to the command map is mutex-guarded.
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
