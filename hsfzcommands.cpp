#include "hsfzcommands.h"
#include "udsutil.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <QNetworkInterface>
#include <QHostAddress>
#include <utility>

static const quint8 HSFZ_TYPE_DIAG = 0x01;

HsfzManager::HsfzManager(QObject *parent) : QObject(parent) {
    publishStatus();
}

void HsfzManager::publishStatus() {
    const bool connected = m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
    QJsonObject s{{"connected", connected}};
    if (connected) {
        s["host"] = m_host;
        s["port"] = int(m_port);
        s["sourceAddress"] = QString("0x%1").arg(m_sourceAddress, 2, 16, QChar('0'));
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

QJsonObject HsfzManager::status() const {
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

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

static QStringList localLinkLocalSubnetHosts() {
    QStringList hosts;
    for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
        QString s = addr.toString();
        if (!s.startsWith("169.254.")) continue;
        QStringList parts = s.split('.');
        QString prefix = parts[0] + "." + parts[1] + "." + parts[2] + ".";
        for (int i = 1; i <= 254; ++i) {
            QString candidate = prefix + QString::number(i);
            if (candidate != s) hosts << candidate;
        }
    }
    return hosts;
}

QJsonObject HsfzManager::discoverVehicles(const QStringList &hosts, quint16 port, int timeoutMs) {
    return onOwnThread([this, hosts, port, timeoutMs]() -> QJsonObject {
        QStringList candidates = hosts.isEmpty() ? localLinkLocalSubnetHosts() : hosts;
        if (candidates.isEmpty())
            return udsTextResult("no link-local (169.254.x.x) network interface found to scan; pass hosts explicitly", true);

        QJsonArray found;
        QEventLoop loop;
        int pending = candidates.size();
        QList<QTcpSocket *> sockets;

        for (const QString &host : candidates) {
            auto *sock = new QTcpSocket();
            sockets << sock;
            QObject::connect(sock, &QTcpSocket::connected, &loop, [sock, host, &found, &pending, &loop]() {
                found.append(host);
                sock->disconnectFromHost();
                if (--pending == 0) loop.quit();
            });
            QObject::connect(sock, &QAbstractSocket::errorOccurred, &loop, [&pending, &loop](QAbstractSocket::SocketError) {
                if (--pending == 0) loop.quit();
            });
            sock->connectToHost(host, port);
        }

        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();

        for (auto *sock : sockets) {
            sock->abort();
            sock->deleteLater();
        }

        return udsTextResult(found.isEmpty() ? "no HSFZ-capable hosts responded (best-effort connect-scan - HSFZ has no standardized discovery broadcast like DoIP)"
            : QString::fromUtf8(QJsonDocument(found).toJson(QJsonDocument::Compact)));
    });
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
        m_host = host;
        m_port = port;
        connect(m_socket, &QTcpSocket::readyRead, this, &HsfzManager::onReadyRead);
        connect(m_socket, &QAbstractSocket::disconnected, this, [this] { publishStatus(); }); // the vehicle hung up
        publishStatus();

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
        m_testerPresentInfo.insert(handle, QJsonObject{{"target", QString("0x%1").arg(targetAddress, 2, 16, QChar('0'))},
                                                       {"intervalMs", intervalMs}});
        publishStatus();
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
        m_testerPresentInfo.remove(handle);
        publishStatus();
        return udsTextResult("stopped");
    });
}

QJsonObject HsfzOpenCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_open"},
        {"description", "Connects to a BMW over HSFZ (ENET cable; IP from hsfz_discover_vehicles); then send requests with hsfz_send_request to an ECU's one-byte address."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"host", QJsonObject{{"type", "string"}, {"description", "the vehicle interface's IP address or hostname, typically a 169.254.x.x link-local address when connected via an ENET cable directly (no DHCP router in between). Get this from hsfz_discover_vehicles if unknown."}}},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 6801 (standard BMW ENET/HSFZ port)"}}},
                {"sourceAddress", QJsonObject{{"type", "string"}, {"description", "this tester's own address, one byte, hex, like \"0xF4\". Default 0xF4 (typical BMW tester address). Rarely needs changing."}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "connection timeout, default 2000ms"}}}
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
        {"description", "Closes the HSFZ connection and stops its tester presents."},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject HsfzCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject HsfzGetReceivedMessagesCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_get_received_messages"},
        {"description", "Raw HSFZ messages seen since the last call - only for investigating connection problems. Not needed for normal requests."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}, {"description", "at most this many (default all, up to 500)"}}}
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
        {"description", "Sends one UDS request to a BMW ECU (targetAddress, one byte) over HSFZ and returns its answer as hex, e.g. '62 F1 90 ...'. A negative answer comes back as an error naming the NRC. Needs hsfz_open."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "the target ECU's HSFZ address, one byte, hex, like \"0x10\" (unlike DoIP's 16-bit logical address, HSFZ addresses are one byte). The same open connection can address different ECUs by changing this on each call."}}},
                {"data", QJsonObject{{"type", "string"}, {"description", "The UDS request as hex bytes, e.g. \"22 F1 90\" (read the VIN); the first byte is the service ID. An array of numbers 0-255 works too. No framing or length bytes."}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "how long to wait for the complete response, default 2000ms"}}}
            }},
            {"required", QJsonArray{"targetAddress", "data"}}
        }}
    };
}
QJsonObject HsfzSendRequestCommand::execute(const QJsonObject &args) {
    quint8 targetAddress = static_cast<quint8>(udsParseId(args["targetAddress"]));
    QByteArray payload;
    QString bytesError;
    payload = udsParseBytes(args["data"], &bytesError);
    if (!bytesError.isEmpty()) return udsTextResult(bytesError, true);
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 2000;
    return m_manager->sendUdsRequest(targetAddress, payload, timeoutMs);
}

QJsonObject HsfzTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_tester_present_start"},
        {"description", "Keeps an ECU's diagnostic session open by sending TesterPresent every intervalMs - only needed in a non-default session (after 10 03). Returns a handle for hsfz_tester_present_stop."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "ECU's address, e.g. 0x10 (same address you use in hsfz_send_request)"}}},
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
        {"description", "Stops a tester present started with hsfz_tester_present_start."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}, {"description", "the handle the matching _start call returned, e.g. \"tp_1\""}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject HsfzTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}

QJsonObject HsfzDiscoverVehiclesCommand::definition() const {
    return QJsonObject{
        {"name", "hsfz_discover_vehicles"},
        {"description", "Finds BMW HSFZ interfaces (ENET cable, ICOM) on the local network and returns their IP addresses. Use one with hsfz_open. Skip it if you know the IP."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"hosts", QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{{"type", "string"}}},
                    {"description", "explicit list of IPs to probe; omit to auto-scan the local link-local subnet"}
                }},
                {"port", QJsonObject{{"type", "integer"}, {"description", "default 6801"}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "total scan time budget, default 1500"}}}
            }}
        }}
    };
}
QJsonObject HsfzDiscoverVehiclesCommand::execute(const QJsonObject &args) {
    QStringList hosts;
    for (const auto &v : args["hosts"].toArray()) hosts << v.toString();
    quint16 port = args.contains("port") ? static_cast<quint16>(args["port"].toInt()) : 6801;
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 1500;
    return m_manager->discoverVehicles(hosts, port, timeoutMs);
}
