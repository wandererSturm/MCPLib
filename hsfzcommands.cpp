#include "hsfzcommands.h"
#include "udsutil.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <utility>

static const quint8 HSFZ_TYPE_DIAG = 0x01;

HsfzManager::HsfzManager(QObject *parent) : QObject(parent) {}

void HsfzManager::sendMessage(quint8 target, quint8 type, const QByteArray &payload) {
    if (!m_socket) return;
    quint32 len = 3 + static_cast<quint32>(payload.size());
    QByteArray frame(4, char(0));
    frame[0] = char((len >> 24) & 0xFF);
    frame[1] = char((len >> 16) & 0xFF);
    frame[2] = char((len >> 8) & 0xFF);
    frame[3] = char(len & 0xFF);
    frame += char(m_sourceAddress);
    frame += char(target);
    frame += char(type);
    frame += payload;
    m_socket->write(frame);
}

void HsfzManager::onReadyRead() {
    m_recvBuffer += m_socket->readAll();
    while (m_recvBuffer.size() >= 4) {
        quint32 len = (static_cast<quint8>(m_recvBuffer[0]) << 24) | (static_cast<quint8>(m_recvBuffer[1]) << 16)
                    | (static_cast<quint8>(m_recvBuffer[2]) << 8) | static_cast<quint8>(m_recvBuffer[3]);
        if (m_recvBuffer.size() < 4 + int(len) || len < 3) break;
        quint8 source = static_cast<quint8>(m_recvBuffer[4]);
        quint8 target = static_cast<quint8>(m_recvBuffer[5]);
        quint8 type = static_cast<quint8>(m_recvBuffer[6]);
        QByteArray payload = m_recvBuffer.mid(7, int(len) - 3);
        m_recvBuffer.remove(0, 4 + int(len));
        m_buffer.append({source, target, type, payload});
        if (m_buffer.size() > 500) m_buffer.removeFirst();
    }
    emit messageAppended();
}

HsfzManager::HsfzMsg HsfzManager::waitForMessage(int timeoutMs, const std::function<bool(const HsfzMsg &)> &pred) {
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        for (int i = 0; i < m_buffer.size(); ++i) {
            if (!pred || pred(m_buffer[i])) {
                HsfzMsg m = m_buffer[i];
                m_buffer.removeAt(i);
                return m;
            }
        }
        qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) break;
        QEventLoop loop;
        connect(this, &HsfzManager::messageAppended, &loop, &QEventLoop::quit);
        QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
        loop.exec();
    }
    return HsfzMsg{0, 0, 0, QByteArray()};
}

QJsonObject HsfzManager::open(const QString &host, quint16 port, quint8 sourceAddress, int timeoutMs) {
    return onOwnThread([this, host, port, sourceAddress, timeoutMs]() -> QJsonObject {
        auto *newSocket = new QTcpSocket(this);
        QEventLoop connectLoop;
        QTimer::singleShot(timeoutMs, &connectLoop, &QEventLoop::quit);
        connect(newSocket, &QTcpSocket::connected, &connectLoop, &QEventLoop::quit);
        connect(newSocket, &QAbstractSocket::errorOccurred, &connectLoop, &QEventLoop::quit);
        newSocket->connectToHost(host, port);
        connectLoop.exec();
        if (newSocket->state() != QAbstractSocket::ConnectedState) {
            QString e = newSocket->errorString();
            delete newSocket;
            return udsTextResult("failed to connect to " + host + ":" + QString::number(port) + ": " + e, true);
        }

        if (m_socket) {
            m_socket->disconnectFromHost();
            delete m_socket;
        }
        m_socket = newSocket;
        m_recvBuffer.clear();
        m_buffer.clear();
        m_sourceAddress = sourceAddress;
        connect(m_socket, &QTcpSocket::readyRead, this, &HsfzManager::onReadyRead);

        return udsTextResult(QString("connected to %1:%2").arg(host).arg(port));
    });
}

QJsonObject HsfzManager::close() {
    return onOwnThread([this]() -> QJsonObject {
        for (auto *t : std::as_const(m_testerPresentTimers)) {
            t->stop();
            t->deleteLater();
        }
        m_testerPresentTimers.clear();
        if (!m_socket) return udsTextResult("already closed");
        m_socket->disconnectFromHost();
        delete m_socket;
        m_socket = nullptr;
        m_buffer.clear();
        m_recvBuffer.clear();
        return udsTextResult("closed");
    });
}

QJsonObject HsfzManager::getReceivedMessages(int limit) {
    return onOwnThread([this, limit]() -> QJsonObject {
        int n = limit > 0 ? qMin(limit, m_buffer.size()) : m_buffer.size();
        QJsonArray arr;
        for (int i = 0; i < n; ++i) {
            const auto &m = m_buffer.at(i);
            arr.append(QJsonObject{
                {"source", QString("0x%1").arg(m.source, 2, 16, QChar('0'))},
                {"target", QString("0x%1").arg(m.target, 2, 16, QChar('0'))},
                {"type", QString("0x%1").arg(m.type, 2, 16, QChar('0'))},
                {"data", udsHexBytes(m.payload)}
            });
        }
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + n);
        return udsTextResult(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    });
}

QJsonObject HsfzManager::sendUdsRequest(quint8 targetAddress, const QByteArray &payload, int timeoutMs) {
    return onOwnThread([this, targetAddress, payload, timeoutMs]() -> QJsonObject {
        if (!m_socket) return udsTextResult("not connected", true);
        sendMessage(targetAddress, HSFZ_TYPE_DIAG, payload);

        for (int attempt = 0; attempt < 10; ++attempt) {
            HsfzMsg resp = waitForMessage(timeoutMs, [](const HsfzMsg &m) { return m.type == HSFZ_TYPE_DIAG; });
            if (resp.payload.isEmpty()) return udsTextResult("timeout waiting for response", true);
            const QByteArray &uds = resp.payload;
            if (uds.size() >= 3 && static_cast<quint8>(uds[0]) == 0x7F) {
                quint8 nrc = static_cast<quint8>(uds[2]);
                if (nrc == 0x78) continue;
                return udsTextResult(QString("negative response: NRC 0x%1 (%2)")
                    .arg(nrc, 2, 16, QChar('0')).arg(udsNrcName(nrc)), true);
            }
            return udsTextResult(udsHexBytes(uds));
        }
        return udsTextResult("ECU kept responding pending (0x78), gave up", true);
    });
}

QJsonObject HsfzManager::testerPresentStart(quint8 targetAddress, int intervalMs, bool suppressPositiveResponse) {
    return onOwnThread([this, targetAddress, intervalMs, suppressPositiveResponse]() -> QJsonObject {
        if (!m_socket) return udsTextResult("not connected", true);
        QString handle = QString("tp_%1").arg(++m_handleCounter);
        quint8 sub = suppressPositiveResponse ? 0x80 : 0x00;
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, targetAddress, sub]() {
            QByteArray payload;
            payload += char(0x3E);
            payload += char(sub);
            sendMessage(targetAddress, HSFZ_TYPE_DIAG, payload);
        });
        timer->start(intervalMs);
        m_testerPresentTimers.insert(handle, timer);
        return udsTextResult(handle);
    });
}

QJsonObject HsfzManager::testerPresentStop(const QString &handle) {
    return onOwnThread([this, handle]() -> QJsonObject {
        auto it = m_testerPresentTimers.find(handle);
        if (it == m_testerPresentTimers.end()) return udsTextResult("no such handle", true);
        it.value()->stop();
        it.value()->deleteLater();
        m_testerPresentTimers.erase(it);
        return udsTextResult("stopped");
    });
}

QJsonObject HsfzOpenCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_open"},
        {"description", "Connect to a vehicle over BMW HSFZ (Ethernet/TCP diagnostics, e.g. via ICOM/ENET)"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "interface IP address or hostname"}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 6801"}}},
                {"sourceAddress", QJsonObject{{"type", "string"}, {"description", "tester's own address, default 0xF4"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "default 2000"}}}
            }},
            {"required", QJsonArray{"host"}}
        }}
    };
}
QJsonObject HsfzOpenCommand::execute(const QJsonObject &args) {
    quint16 port = args.contains("port") ? static_cast<quint16>(args["port"].toInt()) : 6801;
    quint8 srcAddr = args.contains("sourceAddress") ? static_cast<quint8>(udsParseId(args["sourceAddress"])) : 0xF4;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->open(args["host"].toString(), port, srcAddr, timeoutMs);
}

QJsonObject HsfzCloseCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_close"},
        {"description", "Close the current HSFZ connection"},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject HsfzCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject HsfzGetReceivedMessagesCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_get_received_messages"},
        {"description", "Drain buffered raw HSFZ messages received since the last call"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}}}
            }}
        }}
    };
}
QJsonObject HsfzGetReceivedMessagesCommand::execute(const QJsonObject &args) {
    return m_manager->getReceivedMessages(args["limit"].toInt(0));
}

QJsonObject HsfzSendRequestCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_send_request"},
        {"description", "Send a UDS request over an open HSFZ connection and return the decoded response"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's address, e.g. 0x10"}}},
                {"data", QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{{"type", "integer"}}},
                    {"description", "SID plus payload bytes, e.g. [34, 241, 144] for ReadDataByIdentifier"}
                }},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "default 2000"}}}
            }},
            {"required", QJsonArray{"targetAddress", "data"}}
        }}
    };
}
QJsonObject HsfzSendRequestCommand::execute(const QJsonObject &args) {
    quint8 targetAddress = static_cast<quint8>(udsParseId(args["targetAddress"]));
    QByteArray payload;
    for (const auto &v : args["data"].toArray()) payload.append(char(v.toInt()));
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->sendUdsRequest(targetAddress, payload, timeoutMs);
}

QJsonObject HsfzTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_tester_present_start"},
        {"description", "Start sending periodic UDS TesterPresent (0x3E) over HSFZ to keep a diagnostic session alive"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's address, e.g. 0x10"}}},
                {"intervalMs", QJsonObject{{"type", "integer"}, {"description", "default 2000, keep below the ECU's S3 timeout (usually 5000ms)"}}},
                {"suppressPositiveResponse", QJsonObject{{"type", "boolean"}, {"description", "default true"}}}
            }},
            {"required", QJsonArray{"targetAddress"}}
        }}
    };
}
QJsonObject HsfzTesterPresentStartCommand::execute(const QJsonObject &args) {
    quint8 targetAddress = static_cast<quint8>(udsParseId(args["targetAddress"]));
    int intervalMs = args.contains("intervalMs") ? args["intervalMs"].toInt() : 2000;
    bool suppress = args.contains("suppressPositiveResponse") ? args["suppressPositiveResponse"].toBool() : true;
    return m_manager->testerPresentStart(targetAddress, intervalMs, suppress);
}

QJsonObject HsfzTesterPresentStopCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_tester_present_stop"},
        {"description", "Stop a running HSFZ TesterPresent timer"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject HsfzTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}
