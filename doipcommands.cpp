#include "doipcommands.h"
#include "udsutil.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <utility>

DoipManager::DoipManager(QObject *parent) : QObject(parent) {}

void DoipManager::sendMessage(quint16 type, const QByteArray &payload) {
    if (!m_socket) return;
    QByteArray header(8, char(0));
    header[0] = char(0x02);
    header[1] = char(0xFD);
    header[2] = char((type >> 8) & 0xFF);
    header[3] = char(type & 0xFF);
    quint32 len = static_cast<quint32>(payload.size());
    header[4] = char((len >> 24) & 0xFF);
    header[5] = char((len >> 16) & 0xFF);
    header[6] = char((len >> 8) & 0xFF);
    header[7] = char(len & 0xFF);
    m_socket->write(header + payload);
}

void DoipManager::onReadyRead() {
    m_recvBuffer += m_socket->readAll();
    while (m_recvBuffer.size() >= 8) {
        quint32 len = (static_cast<quint8>(m_recvBuffer[4]) << 24) | (static_cast<quint8>(m_recvBuffer[5]) << 16)
                    | (static_cast<quint8>(m_recvBuffer[6]) << 8) | static_cast<quint8>(m_recvBuffer[7]);
        if (m_recvBuffer.size() < 8 + int(len)) break;
        quint16 type = (static_cast<quint8>(m_recvBuffer[2]) << 8) | static_cast<quint8>(m_recvBuffer[3]);
        QByteArray payload = m_recvBuffer.mid(8, int(len));
        m_recvBuffer.remove(0, 8 + int(len));
        m_buffer.append({type, payload});
        if (m_buffer.size() > 500) m_buffer.removeFirst();
    }
    emit messageAppended();
}

DoipManager::DoipMsg DoipManager::waitForMessage(int timeoutMs, const std::function<bool(const DoipMsg &)> &pred) {
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        for (int i = 0; i < m_buffer.size(); ++i) {
            if (!pred || pred(m_buffer[i])) {
                DoipMsg m = m_buffer[i];
                m_buffer.removeAt(i);
                return m;
            }
        }
        qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) break;
        QEventLoop loop;
        connect(this, &DoipManager::messageAppended, &loop, &QEventLoop::quit);
        QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
        loop.exec();
    }
    return DoipMsg{0, QByteArray()};
}

QJsonObject DoipManager::open(const QString &host, quint16 port, quint16 sourceAddress, quint8 activationType, int timeoutMs) {
    return onOwnThread([this, host, port, sourceAddress, activationType, timeoutMs]() -> QJsonObject {
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
        connect(m_socket, &QTcpSocket::readyRead, this, &DoipManager::onReadyRead);

        QByteArray actPayload(7, char(0));
        actPayload[0] = char((sourceAddress >> 8) & 0xFF);
        actPayload[1] = char(sourceAddress & 0xFF);
        actPayload[2] = char(activationType);
        sendMessage(0x0005, actPayload);

        DoipMsg resp = waitForMessage(timeoutMs, [](const DoipMsg &m) { return m.type == 0x0006; });
        if (resp.payload.size() < 5) return udsTextResult("no routing activation response", true);
        quint8 code = static_cast<quint8>(resp.payload[4]);
        if (code != 0x10 && code != 0x11)
            return udsTextResult(QString("routing activation refused, code 0x%1").arg(code, 2, 16, QChar('0')), true);

        return udsTextResult(QString("connected to %1:%2, routing activated").arg(host).arg(port));
    });
}

QJsonObject DoipManager::close() {
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

QJsonObject DoipManager::getReceivedMessages(int limit) {
    return onOwnThread([this, limit]() -> QJsonObject {
        int n = limit > 0 ? qMin(limit, m_buffer.size()) : m_buffer.size();
        QJsonArray arr;
        for (int i = 0; i < n; ++i) {
            arr.append(QJsonObject{
                {"type", QString("0x%1").arg(m_buffer.at(i).type, 4, 16, QChar('0'))},
                {"data", udsHexBytes(m_buffer.at(i).payload)}
            });
        }
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + n);
        return udsTextResult(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    });
}

QJsonObject DoipManager::sendUdsRequest(quint16 targetAddress, const QByteArray &payload, int timeoutMs) {
    return onOwnThread([this, targetAddress, payload, timeoutMs]() -> QJsonObject {
        if (!m_socket) return udsTextResult("not connected", true);

        QByteArray msg(4, char(0));
        msg[0] = char((m_sourceAddress >> 8) & 0xFF);
        msg[1] = char(m_sourceAddress & 0xFF);
        msg[2] = char((targetAddress >> 8) & 0xFF);
        msg[3] = char(targetAddress & 0xFF);
        msg += payload;
        sendMessage(0x8001, msg);

        waitForMessage(300, [](const DoipMsg &m) { return m.type == 0x8002 || m.type == 0x8003; });

        for (int attempt = 0; attempt < 10; ++attempt) {
            DoipMsg resp = waitForMessage(timeoutMs, [](const DoipMsg &m) { return m.type == 0x8001; });
            if (resp.payload.size() < 4) return udsTextResult("timeout waiting for response", true);
            QByteArray uds = resp.payload.mid(4);
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

QJsonObject DoipManager::testerPresentStart(quint16 targetAddress, int intervalMs, bool suppressPositiveResponse) {
    return onOwnThread([this, targetAddress, intervalMs, suppressPositiveResponse]() -> QJsonObject {
        if (!m_socket) return udsTextResult("not connected", true);
        QString handle = QString("tp_%1").arg(++m_handleCounter);
        quint8 sub = suppressPositiveResponse ? 0x80 : 0x00;
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, targetAddress, sub]() {
            QByteArray msg(4, char(0));
            msg[0] = char((m_sourceAddress >> 8) & 0xFF);
            msg[1] = char(m_sourceAddress & 0xFF);
            msg[2] = char((targetAddress >> 8) & 0xFF);
            msg[3] = char(targetAddress & 0xFF);
            msg += char(0x3E);
            msg += char(sub);
            sendMessage(0x8001, msg);
        });
        timer->start(intervalMs);
        m_testerPresentTimers.insert(handle, timer);
        return udsTextResult(handle);
    });
}

QJsonObject DoipManager::testerPresentStop(const QString &handle) {
    return onOwnThread([this, handle]() -> QJsonObject {
        auto it = m_testerPresentTimers.find(handle);
        if (it == m_testerPresentTimers.end()) return udsTextResult("no such handle", true);
        it.value()->stop();
        it.value()->deleteLater();
        m_testerPresentTimers.erase(it);
        return udsTextResult("stopped");
    });
}

QJsonObject DoipOpenCommand::definition() const {
    return QJsonObject{
        {"name", "doip_open"},
        {"description", "Connect to a vehicle over DoIP (ISO 13400, diagnostics over Ethernet/TCP) and perform routing activation"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "gateway IP address or hostname"}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 13400"}}},
                {"sourceAddress", QJsonObject{{"type", "string"}, {"description", "tester's own logical address, default 0x0E00"}}},
                {"activationType", QJsonObject{{"type", "integer"}, {"description", "default 0x00 (default activation)"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "default 2000"}}}
            }},
            {"required", QJsonArray{"host"}}
        }}
    };
}
QJsonObject DoipOpenCommand::execute(const QJsonObject &args) {
    quint16 port = args.contains("port") ? static_cast<quint16>(args["port"].toInt()) : 13400;
    quint16 srcAddr = args.contains("sourceAddress") ? static_cast<quint16>(udsParseId(args["sourceAddress"])) : 0x0E00;
    quint8 activationType = args.contains("activationType") ? static_cast<quint8>(args["activationType"].toInt()) : 0x00;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->open(args["host"].toString(), port, srcAddr, activationType, timeoutMs);
}

QJsonObject DoipCloseCommand::definition() const {
    return QJsonObject{
        {"name", "doip_close"},
        {"description", "Close the current DoIP connection"},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject DoipCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject DoipGetReceivedMessagesCommand::definition() const {
    return QJsonObject{
        {"name", "doip_get_received_messages"},
        {"description", "Drain buffered raw DoIP messages received since the last call"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}}}
            }}
        }}
    };
}
QJsonObject DoipGetReceivedMessagesCommand::execute(const QJsonObject &args) {
    return m_manager->getReceivedMessages(args["limit"].toInt(0));
}

QJsonObject DoipSendRequestCommand::definition() const {
    return QJsonObject{
        {"name", "doip_send_request"},
        {"description", "Send a UDS request over an open DoIP connection and return the decoded response"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's logical address, e.g. 0x0010"}}},
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
QJsonObject DoipSendRequestCommand::execute(const QJsonObject &args) {
    quint16 targetAddress = static_cast<quint16>(udsParseId(args["targetAddress"]));
    QByteArray payload;
    for (const auto &v : args["data"].toArray()) payload.append(char(v.toInt()));
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->sendUdsRequest(targetAddress, payload, timeoutMs);
}

QJsonObject DoipTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "doip_tester_present_start"},
        {"description", "Start sending periodic UDS TesterPresent (0x3E) over DoIP to keep a diagnostic session alive"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's logical address, e.g. 0x0010"}}},
                {"intervalMs", QJsonObject{{"type", "integer"}, {"description", "default 2000, keep below the ECU's S3 timeout (usually 5000ms)"}}},
                {"suppressPositiveResponse", QJsonObject{{"type", "boolean"}, {"description", "default true"}}}
            }},
            {"required", QJsonArray{"targetAddress"}}
        }}
    };
}
QJsonObject DoipTesterPresentStartCommand::execute(const QJsonObject &args) {
    quint16 targetAddress = static_cast<quint16>(udsParseId(args["targetAddress"]));
    int intervalMs = args.contains("intervalMs") ? args["intervalMs"].toInt() : 2000;
    bool suppress = args.contains("suppressPositiveResponse") ? args["suppressPositiveResponse"].toBool() : true;
    return m_manager->testerPresentStart(targetAddress, intervalMs, suppress);
}

QJsonObject DoipTesterPresentStopCommand::definition() const {
    return QJsonObject{
        {"name", "doip_tester_present_stop"},
        {"description", "Stop a running DoIP TesterPresent timer"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject DoipTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}
