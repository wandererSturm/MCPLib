#include "doipcommands.h"
#include "udsutil.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <utility>

DoipManager::DoipManager(QObject *parent) : QObject(parent) {
    publishStatus();
}

void DoipManager::publishStatus() {
    const bool connected = m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
    QJsonObject s{{"connected", connected}};
    if (connected) {
        s["host"] = m_host;
        s["port"] = int(m_port);
        s["sourceAddress"] = QString("0x%1").arg(m_sourceAddress, 4, 16, QChar('0'));
    }
    QJsonArray tp;
    for (auto it = m_testerPresentInfo.cbegin(); it != m_testerPresentInfo.cend(); ++it) {
        QJsonObject info = it.value();
        info["handle"] = it.key();
        tp.append(info);
    }
    s["testerPresent"] = tp;
    QMutexLocker locker(&m_statusMutex);
    m_status = s;
}

QJsonObject DoipManager::status() const {
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

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

QJsonObject DoipManager::discoverVehicles(const QString &broadcastAddress, int timeoutMs) {
    return onOwnThread([this, broadcastAddress, timeoutMs]() -> QJsonObject {
        QUdpSocket udp;
        if (!udp.bind(QHostAddress::AnyIPv4, 0))
            return udsTextResult("failed to bind UDP socket: " + udp.errorString(), true);

        QByteArray header(8, char(0));
        header[0] = char(0x02);
        header[1] = char(0xFD);
        header[3] = char(0x01);
        udp.writeDatagram(header, QHostAddress(broadcastAddress), 13400);

        QJsonArray found;
        QDeadlineTimer deadline(timeoutMs);
        while (!deadline.hasExpired()) {
            if (!udp.hasPendingDatagrams()) {
                qint64 remaining = deadline.remainingTime();
                if (remaining <= 0) break;
                QEventLoop loop;
                connect(&udp, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
                QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
                loop.exec();
                if (!udp.hasPendingDatagrams()) continue;
            }

            QNetworkDatagram dg = udp.receiveDatagram();
            QByteArray data = dg.data();
            if (data.size() < 8) continue;
            quint16 type = (static_cast<quint8>(data[2]) << 8) | static_cast<quint8>(data[3]);
            quint32 len = (static_cast<quint8>(data[4]) << 24) | (static_cast<quint8>(data[5]) << 16)
                        | (static_cast<quint8>(data[6]) << 8) | static_cast<quint8>(data[7]);
            QByteArray payload = data.mid(8, int(len));
            if (type != 0x0004 || payload.size() < 32) continue;

            QString vin = QString::fromLatin1(payload.mid(0, 17));
            quint16 logicalAddr = (static_cast<quint8>(payload[17]) << 8) | static_cast<quint8>(payload[18]);
            found.append(QJsonObject{
                {"ip", dg.senderAddress().toString()},
                {"vin", vin},
                {"logicalAddress", QString("0x%1").arg(logicalAddr, 4, 16, QChar('0'))},
                {"eid", udsHexBytes(payload.mid(19, 6))},
                {"gid", udsHexBytes(payload.mid(25, 6))}
            });
        }

        return udsTextResult(found.isEmpty() ? "no vehicles found"
            : QString::fromUtf8(QJsonDocument(found).toJson(QJsonDocument::Compact)));
    });
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
        m_host = host;
        m_port = port;
        connect(m_socket, &QTcpSocket::readyRead, this, &DoipManager::onReadyRead);
        connect(m_socket, &QAbstractSocket::disconnected, this, [this] { publishStatus(); }); // the vehicle hung up
        publishStatus();

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
        m_testerPresentInfo.clear();
        if (!m_socket) {
            publishStatus();
            return udsTextResult("already closed");
        }
        m_socket->disconnectFromHost();
        delete m_socket;
        m_socket = nullptr;
        m_buffer.clear();
        m_recvBuffer.clear();
        publishStatus();
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
        m_testerPresentInfo.insert(handle, QJsonObject{{"target", QString("0x%1").arg(targetAddress, 4, 16, QChar('0'))},
                                                       {"intervalMs", intervalMs}});
        publishStatus();
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
        m_testerPresentInfo.remove(handle);
        publishStatus();
        return udsTextResult("stopped");
    });
}

QJsonObject DoipOpenCommand::definition() const {
    return QJsonObject{
        {"name", "doip_open"},
        {"description", "Step 1 (or 2, after doip_discover_vehicles) of the DoIP workflow: opens a TCP connection to a vehicle's DoIP gateway (ISO 13400, diagnostics over Ethernet) and performs routing activation, which is required before any diagnostic traffic will be accepted. Only one connection can be open at a time; calling this again replaces it. After this succeeds, use doip_send_request to talk to an ECU by its logical address - you do not need to build or parse raw DoIP messages yourself. Full workflow: doip_discover_vehicles (optional, to find the gateway IP) -> doip_open (once) -> any number of doip_send_request / doip_tester_present_start+stop calls -> doip_close when done."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "the DoIP gateway's IP address or hostname (not an individual ECU's address - the gateway routes to ECUs by logical address once connected). Get this from doip_discover_vehicles if unknown."}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 13400 (standard DoIP port)"}}},
                {"sourceAddress", QJsonObject{{"type", "string"}, {"description", "this tester's own logical address, as a hex string like \"0x0E00\" or a decimal number. Default 0x0E00. Rarely needs changing unless the vehicle/tool requires a specific registered tester address."}}},
                {"activationType", QJsonObject{{"type", "integer"}, {"description", "DoIP routing activation type, default 0x00 (default/normal activation). Some vehicles require a different value (e.g. 0x01/0x02/0xE0 for manufacturer-specific central-security or WWH-OBD activation) - consult the vehicle's DoIP spec if 0x00 is refused."}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "connection + routing-activation timeout, default 2000ms"}}}
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
        {"description", "Close the current DoIP TCP connection and stop any running doip_tester_present_start timers. Safe to call even if nothing is open."},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject DoipCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject DoipGetReceivedMessagesCommand::definition() const {
    return QJsonObject{
        {"name", "doip_get_received_messages"},
        {"description", "Debugging/inspection tool only - drains up to 500 buffered raw DoIP messages seen since the last call. You do NOT need this for normal request/response diagnostics: doip_send_request already returns the decoded UDS payload directly. Use this to investigate unexpected traffic or connection issues."},
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
        {"description", "Requires doip_open to have succeeded first. Sends one UDS (ISO 14229) diagnostic request to a specific ECU over the open DoIP connection and blocks until the full response arrives or timeoutMs elapses, returning the decoded UDS payload as a space-separated hex string (e.g. '62 f1 90 ...'), NOT a raw DoIP message. ECU 'response pending' (NRC 0x78) is automatically waited out and retried internally. A negative response (0x7F ...) is returned as an error result naming the NRC, not as raw bytes. One call = one request/response pair; call it again for each subsequent request (e.g. session control, then security access, then the actual read/write service)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "the target ECU's DoIP logical address (16-bit), e.g. \"0x0010\" (hex string with 0x prefix) or a plain decimal number. This is per-ECU, unlike the connection's gateway host - the same open connection can address different ECUs by changing this on each call."}}},
                {"data", QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{{"type", "integer"}}},
                    {"description", "The raw UDS request bytes as plain decimal integers 0-255 (NOT hex strings) - the first byte is the Service ID (SID). E.g. [16, 3] = DiagnosticSessionControl(0x10) to extendedDiagnosticSession(0x03); [34, 241, 144] = ReadDataByIdentifier(0x22) of DID 0xF190 (VIN)."}
                }},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "how long to wait for the complete response, default 2000ms"}}}
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
        {"description", "Requires doip_open to have succeeded first. Starts a background timer that periodically sends UDS TesterPresent (0x3E) fire-and-forget (it does NOT wait for or return the ECU's response). Use this to keep a non-default diagnostic session (e.g. extendedDiagnosticSession) from timing out while you do other work; not required for simple one-off requests in the default session. Returns a handle immediately - call doip_tester_present_stop with that handle when done, and before doip_close if you want to stop it explicitly (it's also auto-stopped on doip_close)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's logical address, e.g. 0x0010 (same address you use in doip_send_request)"}}},
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

QJsonObject DoipDiscoverVehiclesCommand::definition() const {
    return QJsonObject{
        {"name", "doip_discover_vehicles"},
        {"description", "Optional first step of the DoIP workflow, only needed when you don't already know the gateway's IP address. Broadcasts an ISO 13400 Vehicle Identification Request over UDP and collects Vehicle Announcement responses (VIN, logical address, IP). Standardized DoIP discovery - does not require doip_open first. Use the returned 'ip' field as the host argument to doip_open."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"broadcastAddress", QJsonObject{{"type", "string"}, {"description", "default 255.255.255.255"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "how long to listen for responses, default 2000"}}}
            }}
        }}
    };
}
QJsonObject DoipDiscoverVehiclesCommand::execute(const QJsonObject &args) {
    QString broadcastAddress = args.contains("broadcastAddress") ? args["broadcastAddress"].toString() : "255.255.255.255";
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->discoverVehicles(broadcastAddress, timeoutMs);
}
