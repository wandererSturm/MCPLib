#include "doipcommands.h"
#include "udsutil.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <utility>

namespace {
// ISO 13400-2 diagnostic message negative-acknowledge codes (payload type 0x8003).
QString doipDiagNackName(quint8 code) {
    switch (code) {
    case 0x02: return "invalid source address";
    case 0x03: return "unknown target address";
    case 0x04: return "diagnostic message too large";
    case 0x05: return "out of memory";
    case 0x06: return "target unreachable";
    case 0x07: return "unknown network";
    case 0x08: return "transport protocol error";
    default:   return "unknown";
    }
}
// Generic DoIP header negative-acknowledge codes (payload type 0x0000).
QString doipGenericNackName(quint8 code) {
    switch (code) {
    case 0x00: return "incorrect pattern format";
    case 0x01: return "unknown payload type";
    case 0x02: return "message too large";
    case 0x03: return "out of memory";
    case 0x04: return "invalid payload length";
    default:   return "unknown";
    }
}
} // namespace

DoipManager::DoipManager(QObject *parent) : QObject(parent) {
    publishStatus();
}

QJsonObject DoipManager::parseVehicleAnnouncement(const QByteArray &payload) {
    if (payload.size() < 32)
        return {};
    QJsonObject obj{
        {"vin", QString::fromLatin1(payload.mid(0, 17))},
        {"logicalAddress", QString("0x%1").arg((static_cast<quint8>(payload[17]) << 8)
                                               | static_cast<quint8>(payload[18]), 4, 16, QChar('0'))},
        {"eid", udsHexBytes(payload.mid(19, 6))},
        {"gid", udsHexBytes(payload.mid(25, 6))},
    };
    if (payload.size() > 31) {
        const quint8 fa = static_cast<quint8>(payload[31]);
        obj["furtherActionRequired"] = QString("0x%1").arg(fa, 2, 16, QChar('0'));
        obj["routingActivationRequired"] = (fa != 0x00);
    }
    return obj;
}

QJsonObject DoipManager::parseEntityStatus(const QByteArray &payload) {
    if (payload.size() < 3)
        return {};
    const quint8 nodeType = static_cast<quint8>(payload[0]);
    QJsonObject obj{
        {"nodeType", nodeType == 0x00 ? "gateway" : nodeType == 0x01 ? "node" : "reserved"},
        {"maxConcurrentSockets", static_cast<quint8>(payload[1])},
        {"currentlyOpenSockets", static_cast<quint8>(payload[2])},
    };
    if (payload.size() >= 7) {
        quint32 maxData = (static_cast<quint8>(payload[3]) << 24) | (static_cast<quint8>(payload[4]) << 16)
                        | (static_cast<quint8>(payload[5]) << 8) | static_cast<quint8>(payload[6]);
        obj["maxDataSize"] = int(maxData);
    }
    return obj;
}

QJsonObject DoipManager::parsePowerMode(const QByteArray &payload) {
    if (payload.size() < 1)
        return {};
    const quint8 mode = static_cast<quint8>(payload[0]);
    return QJsonObject{{"powerMode", mode == 0x00 ? "not ready" : mode == 0x01 ? "ready"
                                     : mode == 0x02 ? "not supported" : "reserved"},
                       {"powerModeCode", QString("0x%1").arg(mode, 2, 16, QChar('0'))}};
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
    header[0] = char(m_protocolVersion);
    header[1] = char(0xFF - m_protocolVersion);
    header[2] = char((type >> 8) & 0xFF);
    header[3] = char(type & 0xFF);
    quint32 len = static_cast<quint32>(payload.size());
    header[4] = char((len >> 24) & 0xFF);
    header[5] = char((len >> 16) & 0xFF);
    header[6] = char((len >> 8) & 0xFF);
    header[7] = char(len & 0xFF);
    m_socket->write(header + payload);
    emit wireMessage(true, type, payload);
}

void DoipManager::onReadyRead() {
    m_recvBuffer += m_socket->readAll();
    while (m_recvBuffer.size() >= 8) {
        const quint8 ver = static_cast<quint8>(m_recvBuffer[0]);
        const quint8 inv = static_cast<quint8>(m_recvBuffer[1]);
        quint32 len = (static_cast<quint8>(m_recvBuffer[4]) << 24) | (static_cast<quint8>(m_recvBuffer[5]) << 16)
                    | (static_cast<quint8>(m_recvBuffer[6]) << 8) | static_cast<quint8>(m_recvBuffer[7]);
        if (m_recvBuffer.size() < 8 + int(len)) break;
        quint16 type = (static_cast<quint8>(m_recvBuffer[2]) << 8) | static_cast<quint8>(m_recvBuffer[3]);
        QByteArray payload = m_recvBuffer.mid(8, int(len));
        m_recvBuffer.remove(0, 8 + int(len));

        // Validate the generic header: known protocol version with correct inverse
        // byte. A foreign/malformed frame isn't treated as a DoIP message (we trust
        // the length to resync, the common case being an accepted 0x03 peer).
        const bool versionOk = (inv == quint8(0xFF - ver)) && (ver == 0x01 || ver == 0x02 || ver == 0x03 || ver == 0xFF);
        if (!versionOk)
            continue;

        emit wireMessage(false, type, payload);

        // Alive check request: answer immediately with our source address so the
        // entity doesn't tear down the connection. Handled here, not buffered.
        if (type == 0x0007) {
            QByteArray p(2, char(0));
            p[0] = char((m_sourceAddress >> 8) & 0xFF);
            p[1] = char(m_sourceAddress & 0xFF);
            sendMessage(0x0008, p);
            continue;
        }

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
        QUdpSocket udp4;
        if (!udp4.bind(QHostAddress::AnyIPv4, 0))
            return udsTextResult("failed to bind UDP socket: " + udp4.errorString(), true);

        // Vehicle Identification Request (payload type 0x0001), empty payload.
        QByteArray vir(8, char(0));
        vir[0] = char(m_protocolVersion);
        vir[1] = char(0xFF - m_protocolVersion);
        vir[3] = char(0x01);
        udp4.writeDatagram(vir, QHostAddress(broadcastAddress), 13400);

        // IPv6: send to the link-local all-nodes multicast group on each capable
        // interface. Best effort — failures (no IPv6, no route) are ignored.
        QUdpSocket udp6;
        const bool have6 = udp6.bind(QHostAddress::AnyIPv6, 0);
        if (have6) {
            const QHostAddress group("ff02::1");
            for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
                if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::CanMulticast))
                    continue;
                udp6.setMulticastInterface(iface);
                udp6.writeDatagram(vir, group, 13400);
            }
        }

        auto drain = [](QUdpSocket &sock, QJsonArray &out) {
            while (sock.hasPendingDatagrams()) {
                QNetworkDatagram dg = sock.receiveDatagram();
                QByteArray data = dg.data();
                if (data.size() < 8)
                    continue;
                quint16 type = (static_cast<quint8>(data[2]) << 8) | static_cast<quint8>(data[3]);
                quint32 len = (static_cast<quint8>(data[4]) << 24) | (static_cast<quint8>(data[5]) << 16)
                            | (static_cast<quint8>(data[6]) << 8) | static_cast<quint8>(data[7]);
                if (type != 0x0004)
                    continue;
                QJsonObject v = parseVehicleAnnouncement(data.mid(8, int(len)));
                if (v.isEmpty())
                    continue;
                v["ip"] = dg.senderAddress().toString();
                out.append(v);
            }
        };

        QJsonArray found;
        QDeadlineTimer deadline(timeoutMs);
        while (!deadline.hasExpired()) {
            drain(udp4, found);
            if (have6)
                drain(udp6, found);
            qint64 remaining = deadline.remainingTime();
            if (remaining <= 0)
                break;
            if (udp4.hasPendingDatagrams() || (have6 && udp6.hasPendingDatagrams()))
                continue;
            QEventLoop loop;
            connect(&udp4, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
            if (have6)
                connect(&udp6, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
            QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
            loop.exec();
        }

        return udsTextResult(found.isEmpty() ? "no vehicles found"
            : QString::fromUtf8(QJsonDocument(found).toJson(QJsonDocument::Compact)));
    });
}

QByteArray DoipManager::udpExchange(const QString &host, quint16 port, quint16 reqType, const QByteArray &reqPayload,
                                    quint16 respType, int timeoutMs, QString *error) {
    QUdpSocket udp;
    if (!udp.bind(QHostAddress::AnyIPv4, 0)) {
        *error = "failed to bind UDP socket: " + udp.errorString();
        return {};
    }
    QByteArray hdr(8, char(0));
    hdr[0] = char(m_protocolVersion);
    hdr[1] = char(0xFF - m_protocolVersion);
    hdr[2] = char((reqType >> 8) & 0xFF);
    hdr[3] = char(reqType & 0xFF);
    quint32 len = static_cast<quint32>(reqPayload.size());
    hdr[4] = char((len >> 24) & 0xFF);
    hdr[5] = char((len >> 16) & 0xFF);
    hdr[6] = char((len >> 8) & 0xFF);
    hdr[7] = char(len & 0xFF);
    udp.writeDatagram(hdr + reqPayload, QHostAddress(host), port);

    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        while (udp.hasPendingDatagrams()) {
            QNetworkDatagram dg = udp.receiveDatagram();
            QByteArray data = dg.data();
            if (data.size() < 8)
                continue;
            quint16 type = (static_cast<quint8>(data[2]) << 8) | static_cast<quint8>(data[3]);
            quint32 l = (static_cast<quint8>(data[4]) << 24) | (static_cast<quint8>(data[5]) << 16)
                      | (static_cast<quint8>(data[6]) << 8) | static_cast<quint8>(data[7]);
            if (type == respType)
                return data.mid(8, int(l));
        }
        qint64 remaining = deadline.remainingTime();
        if (remaining <= 0)
            break;
        QEventLoop loop;
        connect(&udp, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
        QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
        loop.exec();
    }
    *error = "timeout waiting for response";
    return {};
}

QJsonObject DoipManager::entityStatus(const QString &host, quint16 port, int timeoutMs) {
    return onOwnThread([this, host, port, timeoutMs]() -> QJsonObject {
        const QString h = host.isEmpty() ? m_host : host;
        if (h.isEmpty())
            return udsTextResult("no host: give a host, or open a connection first", true);
        QString err;
        QByteArray resp = udpExchange(h, port, 0x4001, {}, 0x4002, timeoutMs, &err);
        if (!err.isEmpty())
            return udsTextResult("DoIP entity status request: " + err, true);
        QJsonObject o = parseEntityStatus(resp);
        if (o.isEmpty())
            return udsTextResult("malformed DoIP entity status response", true);
        return udsTextResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    });
}

QJsonObject DoipManager::powerMode(const QString &host, quint16 port, int timeoutMs) {
    return onOwnThread([this, host, port, timeoutMs]() -> QJsonObject {
        const QString h = host.isEmpty() ? m_host : host;
        if (h.isEmpty())
            return udsTextResult("no host: give a host, or open a connection first", true);
        QString err;
        QByteArray resp = udpExchange(h, port, 0x4003, {}, 0x4004, timeoutMs, &err);
        if (!err.isEmpty())
            return udsTextResult("DoIP power mode request: " + err, true);
        QJsonObject o = parsePowerMode(resp);
        if (o.isEmpty())
            return udsTextResult("malformed DoIP power mode response", true);
        return udsTextResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    });
}

QJsonObject DoipManager::open(const QString &host, quint16 port, quint16 sourceAddress, quint8 activationType, int timeoutMs, quint8 protocolVersion) {
    return onOwnThread([this, host, port, sourceAddress, activationType, timeoutMs, protocolVersion]() -> QJsonObject {
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
        m_protocolVersion = protocolVersion;
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
        if (code == 0x10)
            return udsTextResult(QString("connected to %1:%2, routing activated").arg(host).arg(port));
        if (code == 0x11) // activated, but the entity requires a confirmation step
            return udsTextResult(QString("connected to %1:%2, routing activated (code 0x11 - confirmation required; "
                                         "some gateways need a follow-up activation)").arg(host).arg(port));
        return udsTextResult(QString("routing activation refused, code 0x%1").arg(code, 2, 16, QChar('0')), true);
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

        // The entity acknowledges the diagnostic message first: 0x8002 positive, or
        // 0x8003 negative (don't wait out a timeout — report why), and a generic
        // header NACK (0x0000) can arrive instead of either.
        DoipMsg ack = waitForMessage(300, [](const DoipMsg &m) {
            return m.type == 0x8002 || m.type == 0x8003 || m.type == 0x0000;
        });
        if (ack.type == 0x8003) {
            quint8 nack = ack.payload.size() >= 5 ? static_cast<quint8>(ack.payload[4]) : 0xFF;
            return udsTextResult(QString("DoIP diagnostic message rejected (negative ack 0x%1: %2)")
                .arg(nack, 2, 16, QChar('0')).arg(doipDiagNackName(nack)), true);
        }
        if (ack.type == 0x0000) {
            quint8 nack = ack.payload.size() >= 1 ? static_cast<quint8>(ack.payload[0]) : 0xFF;
            return udsTextResult(QString("DoIP generic header negative ack (0x%1: %2)")
                .arg(nack, 2, 16, QChar('0')).arg(doipGenericNackName(nack)), true);
        }

        for (int attempt = 0; attempt < 10; ++attempt) {
            DoipMsg resp = waitForMessage(timeoutMs, [](const DoipMsg &m) { return m.type == 0x8001 || m.type == 0x0000; });
            if (resp.type == 0x0000) {
                quint8 nack = resp.payload.size() >= 1 ? static_cast<quint8>(resp.payload[0]) : 0xFF;
                return udsTextResult(QString("DoIP generic header negative ack (0x%1: %2)")
                    .arg(nack, 2, 16, QChar('0')).arg(doipGenericNackName(nack)), true);
            }
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
        {"description", "Connects to a vehicle's DoIP gateway (IP from doip_discover_vehicles) and activates routing; then send requests with doip_send_request. One connection at a time."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "the DoIP gateway's IP address or hostname (not an individual ECU's address - the gateway routes to ECUs by logical address once connected). Get this from doip_discover_vehicles if unknown."}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 13400 (standard DoIP port)"}}},
                {"sourceAddress", QJsonObject{{"type", "string"}, {"description", "this tester's own logical address, hex, like \"0x0E00\". Default 0x0E00. Rarely needs changing unless the vehicle/tool requires a specific registered tester address."}}},
                {"activationType", QJsonObject{{"type", "integer"}, {"description", "routing activation type as a number, default 0 (normal activation). Some vehicles need another value (manufacturer-specific, e.g. 1, 2 or 224) - consult the vehicle's DoIP spec if 0 is refused."}}},
                {"protocolVersion", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{2, 3}}, {"description", "DoIP protocol version, default 2 (ISO 13400-2:2012). Use 3 only for 2019 entities that reject 2."}}},
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
    quint8 protocolVersion = args.contains("protocolVersion") ? static_cast<quint8>(args["protocolVersion"].toInt()) : 0x02;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->open(args["host"].toString(), port, srcAddr, activationType, timeoutMs, protocolVersion);
}

QJsonObject DoipCloseCommand::definition() const {
    return QJsonObject{
        {"name", "doip_close"},
        {"description", "Closes the DoIP connection and stops its tester presents."},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject DoipCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject DoipGetReceivedMessagesCommand::definition() const {
    return QJsonObject{
        {"name", "doip_get_received_messages"},
        {"description", "Raw DoIP messages seen since the last call - only for investigating connection problems. Not needed for normal requests."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}, {"description", "at most this many (default all, up to 500)"}}}
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
        {"description", "Sends one UDS request to an ECU (targetAddress) over DoIP and returns its answer as hex, e.g. '62 F1 90 ...'. A negative answer comes back as an error naming the NRC. Needs doip_open."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "the target ECU's DoIP logical address (16-bit), hex, e.g. \"0x0010\". This is per-ECU, unlike the connection's gateway host - the same open connection can address different ECUs by changing this on each call."}}},
                {"data", QJsonObject{{"type", "string"}, {"description", "The UDS request as hex bytes, e.g. \"22 F1 90\" (read the VIN); the first byte is the service ID. An array of numbers 0-255 works too. No framing or length bytes."}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "how long to wait for the complete response, default 2000ms"}}}
            }},
            {"required", QJsonArray{"targetAddress", "data"}}
        }}
    };
}
QJsonObject DoipSendRequestCommand::execute(const QJsonObject &args) {
    quint16 targetAddress = static_cast<quint16>(udsParseId(args["targetAddress"]));
    QByteArray payload;
    QString bytesError;
    payload = udsParseBytes(args["data"], &bytesError);
    if (!bytesError.isEmpty()) return udsTextResult(bytesError, true);
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->sendUdsRequest(targetAddress, payload, timeoutMs);
}

QJsonObject DoipTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "doip_tester_present_start"},
        {"description", "Keeps an ECU's diagnostic session open by sending TesterPresent every intervalMs - only needed in a non-default session (after 10 03). Returns a handle for doip_tester_present_stop."},
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
        {"description", "Stops a tester present started with doip_tester_present_start."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}, {"description", "the handle the matching _start call returned, e.g. \"tp_1\""}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject DoipTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}

QJsonObject DoipEntityStatusCommand::definition() const {
    return QJsonObject{
        {"name", "doip_entity_status"},
        {"description", "Optional, rarely needed: asks the DoIP gateway over UDP how many diagnostic connections it allows and has open (and its node type). Use only to investigate connection-limit problems, not for normal requests. Uses the open connection's host if none is given."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "the DoIP entity's IP (default: the currently open connection's host)"}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 13400"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "default 2000ms"}}}
            }}
        }}
    };
}
QJsonObject DoipEntityStatusCommand::execute(const QJsonObject &args) {
    QString host = args.contains("host") ? args["host"].toString() : QString();
    quint16 port = args.contains("port") ? static_cast<quint16>(args["port"].toInt()) : 13400;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->entityStatus(host, port, timeoutMs);
}

QJsonObject DoipPowerModeCommand::definition() const {
    return QJsonObject{
        {"name", "doip_power_mode"},
        {"description", "Optional, rarely needed: asks over UDP whether the vehicle is powered up enough for diagnostics (ready / not ready / not supported). Use only if requests fail because the vehicle may be asleep. Uses the open connection's host if none is given."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "the DoIP entity's IP (default: the currently open connection's host)"}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 13400"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "default 2000ms"}}}
            }}
        }}
    };
}
QJsonObject DoipPowerModeCommand::execute(const QJsonObject &args) {
    QString host = args.contains("host") ? args["host"].toString() : QString();
    quint16 port = args.contains("port") ? static_cast<quint16>(args["port"].toInt()) : 13400;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->powerMode(host, port, timeoutMs);
}

QJsonObject DoipDiscoverVehiclesCommand::definition() const {
    return QJsonObject{
        {"name", "doip_discover_vehicles"},
        {"description", "Finds DoIP vehicles on the network: VIN, logical address and IP. Use the ip with doip_open. Skip it if you know the IP."},
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
