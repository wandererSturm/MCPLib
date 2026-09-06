#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QObject>
#include <QHttpServer>
#include <QTcpServer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QFuture>
#include <functional>
#include "mcpcommandregistry.h"
#include "mcpserverlib_global.h"

class MCPSERVERLIB_EXPORT McpServer : public QObject {
    Q_OBJECT

public:
    explicit McpServer(McpCommandRegistry *registry, quint16 port = 3768, QObject *parent = nullptr);

private slots:
    void onRegistryChanged();

private:
    using SendFn = std::function<void(const QJsonObject &)>;

    QHttpServer *m_httpServer;
    QTcpServer *m_tcpServer;
    McpCommandRegistry *m_registry;
    QThreadPool m_toolThreadPool;

    void startStdinReader();
    void handleLine(const QByteArray &line);

    void handleRequest(const QJsonObject &req, const SendFn &sendFn);

    QFuture<QJsonObject> callToolAsync(const QJsonObject &req);

    void writeStdioMessage(const QJsonObject &msg);
};
#endif // MCPSERVER_H
