#include "mcpserver.h"
#include <cstdio>
#include <iostream>
#include <thread>
#include <QHttpServerResponse>
#include <QDebug>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

McpServer::McpServer(McpCommandRegistry *registry, quint16 port, QObject *parent)
    : QObject(parent), m_registry(registry)
{
    m_toolThreadPool.setMaxThreadCount(qMax(1, QThread::idealThreadCount()));

    m_httpServer = new QHttpServer(this);
    m_httpServer->route("/mcp", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QFuture<QHttpServerResponse> {
            QJsonDocument doc = QJsonDocument::fromJson(request.body());
            if (!doc.isObject()) {
                return QtFuture::makeReadyValueFuture(
                    QHttpServerResponse(QHttpServerResponder::StatusCode::BadRequest));
            }
            QJsonObject req = doc.object();

            if (req["method"].toString() == "tools/call") {
                return callToolAsync(req).then([](const QJsonObject &res) {
                    return res.isEmpty()
                        ? QHttpServerResponse(QHttpServerResponder::StatusCode::Accepted)
                        : QHttpServerResponse(res);
                });
            }

            QJsonObject responseObj;
            bool hasResponse = false;
            handleRequest(req, [&](const QJsonObject &res) {
                responseObj = res;
                hasResponse = true;
            });
            QHttpServerResponse resp = hasResponse
                ? QHttpServerResponse(responseObj)
                : QHttpServerResponse(QHttpServerResponder::StatusCode::Accepted);
            return QtFuture::makeReadyValueFuture(std::move(resp));
        });

    m_tcpServer = new QTcpServer(this);
    if (!m_tcpServer->listen(QHostAddress::LocalHost, port) || !m_httpServer->bind(m_tcpServer)) {
        qWarning() << "McpServer: failed to listen on port" << port;
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }

    startStdinReader();

    connect(m_registry, &McpCommandRegistry::registryChanged, this, &McpServer::onRegistryChanged);
}

void McpServer::startStdinReader() {
    std::thread([this]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            QByteArray lineCopy = QByteArray::fromStdString(line);
            QMetaObject::invokeMethod(this, [this, lineCopy]() {
                handleLine(lineCopy);
            }, Qt::QueuedConnection);
        }
    }).detach();
}

void McpServer::handleLine(const QByteArray &line) {
    if (line.trimmed().isEmpty()) return;

    QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) return;

    QJsonObject req = doc.object();
    if (req["method"].toString() == "tools/call") {
        callToolAsync(req).then(this, [this](const QJsonObject &res) {
            if (!res.isEmpty()) writeStdioMessage(res);
        });
        return;
    }

    handleRequest(req, [this](const QJsonObject &res) {
        writeStdioMessage(res);
    });
}

void McpServer::onRegistryChanged() {
    writeStdioMessage(QJsonObject{
        {"jsonrpc", "2.0"},
        {"method", "notifications/tools/list_changed"}
    });
}

void McpServer::writeStdioMessage(const QJsonObject &msg) {
    QByteArray out = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

QFuture<QJsonObject> McpServer::callToolAsync(const QJsonObject &req) {
    const QJsonValue id = req["id"];
    const bool isNotification = !req.contains("id");
    const QJsonObject params = req["params"].toObject();
    const QString name = params["name"].toString();
    const QJsonObject args = params["arguments"].toObject();
    McpCommandRegistry *registry = m_registry;

    return QtConcurrent::run(&m_toolThreadPool,
        [registry, id, isNotification, name, args]() -> QJsonObject {
            bool ok = false;
            QJsonObject result = registry->executeCommand(name, args, &ok);
            if (isNotification) return QJsonObject{};
            return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        });
}

void McpServer::handleRequest(const QJsonObject &req, const SendFn &sendFn) {
    const QString method = req["method"].toString();
    const QJsonValue id = req["id"];
    const bool isNotification = !req.contains("id");

    if (method == "initialize") {
        QJsonObject result{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", true}}}}},
            {"serverInfo", QJsonObject{{"name", "QtCommandApp"}, {"version", "1.0"}}}
        };
        if (!isNotification) {
            sendFn(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
        return;
    }

    if (method == "tools/list") {
        QJsonObject result{{"tools", m_registry->getToolDefinitions()}};
        if (!isNotification) {
            sendFn(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
        return;
    }

    if (isNotification) return;

    sendFn(QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", QJsonObject{{"code", -32601}, {"message", "Method not found: " + method}}}
    });
}
