#ifndef MCPCOMMAND_H
#define MCPCOMMAND_H
#include <QString>
#include <QJsonObject>
#include <QVariantMap>
#include "mcpserverlib_global.h"

// execute() runs on a worker thread (McpServer dispatches tool calls off the
// caller's thread), so implementations must be safe to call concurrently
// with themselves and with definition().
class MCPSERVERLIB_EXPORT McpCommand {
public:
    virtual ~McpCommand() = default;

    // Returns tool metadata matching MCP Schema (name, description, inputSchema)
    virtual QJsonObject definition() const = 0;

    // Executes the command with arguments passed from the AI client
    virtual QJsonObject execute(const QJsonObject &args) = 0;
};
#endif // MCPCOMMAND_H
