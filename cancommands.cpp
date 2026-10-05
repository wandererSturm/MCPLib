#include "cancommands.h"
#include "udsutil.h"
#include <QCanBus>
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDeadlineTimer>
#include <utility>
#ifdef MCPSERVERLIB_HAVE_GSUSB
#include "gsusbcanbus.h"
#endif

CanManager::CanManager(QObject *parent) : QObject(parent) {
    publishStatus();
}

void CanManager::publishStatus() {
    QJsonObject s{{"open", m_device != nullptr}};
    if (m_device) {
        s["plugin"] = m_plugin;
        s["interface"] = m_interface;
        if (m_bitrate) s["bitrate"] = qint64(m_bitrate);
        s["fd"] = m_fdMode;
        if (m_fdMode && m_dataBitrate) s["dataBitrate"] = qint64(m_dataBitrate);
        s["extendedId"] = m_forceExtendedId;
        s["extendedAddressing"] = m_extendedAddressing;
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

QJsonObject CanManager::status() const {
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

void CanManager::onFramesReceived() {
    while (m_device && m_device->framesAvailable())
        m_buffer.append(m_device->readFrame());
    if (m_buffer.size() > 500)
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + (m_buffer.size() - 500));
    emit frameAppended();
}

QCanBusFrame CanManager::waitForFrame(quint32 id, int timeoutMs, const std::function<bool(const QCanBusFrame &)> &pred) {
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        for (int i = 0; i < m_buffer.size(); ++i) {
            if (m_buffer[i].frameId() == id && (!pred || pred(m_buffer[i])))
                return m_buffer.takeAt(i);
        }
        qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) break;
        QEventLoop loop;
        connect(this, &CanManager::frameAppended, &loop, &QEventLoop::quit);
        QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QCanBusFrame(QCanBusFrame::InvalidFrame);
}

void CanManager::sendRaw(quint32 id, const QByteArray &data) {
    if (!m_device) return;
    QCanBusFrame frame(id, data);
    if (m_forceExtendedId) frame.setExtendedFrameFormat(true);
    if (m_fdMode) {
        frame.setFlexibleDataRateFormat(true);
        if (m_fdBitrateSwitch) frame.setBitrateSwitch(true);
    }
    m_device->writeFrame(frame);
}

// Valid CAN FD payload lengths (ISO 11898-1); classic CAN only ever uses 8.
static int fdFrameSizeFor(int totalLen) {
    static const int sizes[] = {8, 12, 16, 20, 24, 32, 48, 64};
    for (int s : sizes) if (totalLen <= s) return s;
    return 64;
}

static int pciTypeAt(const QByteArray &d, int ae) {
    if (d.size() <= ae) return -1;
    return (static_cast<quint8>(d[ae]) >> 4) & 0xF;
}

bool CanManager::sendIsoTp(quint32 txId, quint32 rxId, const QByteArray &payload, quint8 ext) {
    int ae = m_extendedAddressing ? 1 : 0;
    int classicMax = 7 - ae;

    if (payload.size() <= classicMax) {
        // Fits the classic 4-bit SF_DL, sent as an 8-byte frame either way.
        QByteArray d(8, char(m_padByte));
        int i = 0;
        if (ae) d[i++] = char(ext);
        d[i] = char(payload.size());
        d.replace(i + 1, payload.size(), payload);
        sendRaw(txId, d);
        return true;
    }

    int escapeMax = m_fdMode ? (64 - 2 - ae) : 0;
    if (m_fdMode && payload.size() <= escapeMax) {
        // ISO 15765-2:2016 SF escape sequence: SF_DL==0 in the PCI nibble,
        // actual length in the following byte. Lets a whole payload up to
        // ~62 bytes go out as a single CAN FD frame instead of segmenting.
        int frameSize = fdFrameSizeFor(ae + 2 + payload.size());
        QByteArray d(frameSize, char(m_padByte));
        int i = 0;
        if (ae) d[i++] = char(ext);
        d[i] = char(0x00);
        d[i + 1] = char(payload.size());
        d.replace(i + 2, payload.size(), payload);
        sendRaw(txId, d);
        return true;
    }

    int len = payload.size();
    int frameSize = m_fdMode ? 64 : 8;
    int ffChunk = frameSize - 2 - ae;
    QByteArray ff(frameSize, char(m_padByte));
    int i = 0;
    if (ae) ff[i++] = char(ext);
    ff[i] = char(0x10 | ((len >> 8) & 0xF));
    ff[i + 1] = char(len & 0xFF);
    ff.replace(i + 2, ffChunk, payload.left(ffChunk));
    sendRaw(txId, ff);

    int sent = ffChunk;
    int seq = 1;
    while (sent < len) {
        QCanBusFrame fc = waitForFrame(rxId, 1000, [ae](const QCanBusFrame &f) {
            return pciTypeAt(f.payload(), ae) == 0x3;
        });
        if (!fc.isValid()) return false;
        QByteArray fp = fc.payload();
        quint8 fs = static_cast<quint8>(fp[ae]) & 0xF;
        quint8 bs = fp.size() > ae + 1 ? static_cast<quint8>(fp[ae + 1]) : 0;
        quint8 stmin = fp.size() > ae + 2 ? static_cast<quint8>(fp[ae + 2]) : 0;
        if (fs == 2) return false;
        if (fs == 1) continue;

        int stminMs = stmin <= 0x7F ? stmin : 1;
        int cfChunkMax = frameSize - 1 - ae;
        int count = 0;
        while (sent < len && (bs == 0 || count < bs)) {
            int chunk = qMin(cfChunkMax, len - sent);
            int cfSize = m_fdMode ? fdFrameSizeFor(ae + 1 + chunk) : 8;
            QByteArray cf(cfSize, char(m_padByte));
            int j = 0;
            if (ae) cf[j++] = char(ext);
            cf[j] = char(0x20 | (seq & 0xF));
            cf.replace(j + 1, chunk, payload.mid(sent, chunk));
            sendRaw(txId, cf);
            sent += chunk;
            seq = (seq + 1) & 0xF;
            count++;
            if (sent < len && (bs == 0 || count < bs)) QThread::msleep(stminMs);
        }
    }
    return true;
}

QByteArray CanManager::receiveIsoTp(quint32 txId, quint32 rxId, int timeoutMs, quint8 ext) {
    int ae = m_extendedAddressing ? 1 : 0;

    QCanBusFrame frame = waitForFrame(rxId, timeoutMs, [ae](const QCanBusFrame &f) {
        int type = pciTypeAt(f.payload(), ae);
        return type == 0x0 || type == 0x1;
    });
    if (!frame.isValid()) return QByteArray();

    QByteArray d = frame.payload();
    if (d.size() <= ae) return QByteArray();
    quint8 pci = static_cast<quint8>(d[ae]);
    int type = (pci >> 4) & 0xF;

    if (type == 0x0) {
        quint8 sfDl = pci & 0xF;
        if (sfDl == 0) {
            // ISO 15765-2:2016 SF escape sequence (CAN FD single frames > 7 bytes)
            if (d.size() <= ae + 1) return QByteArray();
            quint8 escLen = static_cast<quint8>(d[ae + 1]);
            return d.mid(ae + 2, escLen);
        }
        return d.mid(ae + 1, sfDl);
    }

    if (type != 0x1) return QByteArray();

    int expectedLen = ((pci & 0xF) << 8) | static_cast<quint8>(d[ae + 1]);
    QByteArray result = d.mid(ae + 2);

    int fcFrameSize = m_fdMode ? fdFrameSizeFor(ae + 3) : 8;
    QByteArray fc(fcFrameSize, char(m_padByte));
    if (ae) fc[0] = char(ext);
    fc[ae] = char(0x30);
    sendRaw(txId, fc);

    int nextSeq = 1;
    QDeadlineTimer deadline(timeoutMs);
    while (result.size() < expectedLen && !deadline.hasExpired()) {
        QCanBusFrame cf = waitForFrame(rxId, int(deadline.remainingTime()), [ae, nextSeq](const QCanBusFrame &f) {
            return pciTypeAt(f.payload(), ae) == 0x2
                && (static_cast<quint8>(f.payload()[ae]) & 0xF) == (nextSeq & 0xF);
        });
        if (!cf.isValid()) return QByteArray();
        result += cf.payload().mid(ae + 1);
        nextSeq++;
    }
    return result.left(expectedLen);
}

QJsonObject CanManager::listInterfaces(const QString &plugin) {
    return onOwnThread([this, plugin]() -> QJsonObject {
        auto bus = QCanBus::instance();
        QStringList plugins = plugin.isEmpty() ? bus->plugins() : QStringList{plugin};
        QStringList lines;
        for (const auto &p : plugins) {
            QString err;
            auto infos = bus->availableDevices(p, &err);
            QStringList names;
            for (const auto &info : infos) names << info.name();
            lines << QString("%1: %2").arg(p, names.isEmpty() ? (err.isEmpty() ? "none" : "error: " + err) : names.join(", "));
        }
#ifdef MCPSERVERLIB_HAVE_GSUSB
        if (plugin.isEmpty() || plugin == "gsusb") {
            QString err;
            QStringList ids = GsUsbCanBusDevice::availableDevices(&err);
            lines << QString("gsusb: %1").arg(ids.isEmpty() ? (err.isEmpty() ? "none" : "error: " + err) : ids.join(", "));
        }
#endif
        return udsTextResult(lines.join(" | "));
    });
}

QJsonObject CanManager::open(const QString &plugin, const QString &interfaceName, quint8 padByte, bool forceExtendedId,
                              bool extendedAddressing, quint8 addressExtension, bool fdMode, quint32 bitrate,
                              quint32 dataBitrate, bool bitrateSwitch) {
    return onOwnThread([this, plugin, interfaceName, padByte, forceExtendedId, extendedAddressing, addressExtension,
                         fdMode, bitrate, dataBitrate, bitrateSwitch]() -> QJsonObject {
        QString err;
        QCanBusDevice *newDevice = nullptr;
        if (plugin == "gsusb") {
#ifdef MCPSERVERLIB_HAVE_GSUSB
            newDevice = new GsUsbCanBusDevice(interfaceName);
#else
            return udsTextResult("gsusb support was not compiled into this build of mcpserverlib (libusb not found)", true);
#endif
        } else {
            newDevice = QCanBus::instance()->createDevice(plugin, interfaceName, &err);
        }
        if (!newDevice)
            return udsTextResult("failed to open " + plugin + ":" + interfaceName + ": " + err, true);

        if (fdMode) newDevice->setConfigurationParameter(QCanBusDevice::CanFdKey, true);
        if (bitrate) newDevice->setConfigurationParameter(QCanBusDevice::BitRateKey, bitrate);
        if (fdMode && dataBitrate) newDevice->setConfigurationParameter(QCanBusDevice::DataBitRateKey, dataBitrate);

        connect(newDevice, &QCanBusDevice::framesReceived, this, &CanManager::onFramesReceived);
        if (!newDevice->connectDevice()) {
            QString e = newDevice->errorString();
            delete newDevice;
            return udsTextResult("failed to connect " + plugin + ":" + interfaceName + ": " + e, true);
        }

        if (m_device) {
            m_device->disconnectDevice();
            delete m_device;
        }
        m_device = newDevice;
        m_padByte = padByte;
        m_forceExtendedId = forceExtendedId;
        m_extendedAddressing = extendedAddressing;
        m_addressExtension = addressExtension;
        m_fdMode = fdMode;
        m_fdBitrateSwitch = fdMode && bitrateSwitch;
        m_plugin = plugin;
        m_interface = interfaceName;
        m_bitrate = bitrate;
        m_dataBitrate = dataBitrate;
        publishStatus();
        return udsTextResult("opened " + interfaceName + (fdMode ? " (CAN FD)" : ""));
    });
}

QJsonObject CanManager::close() {
    return onOwnThread([this]() -> QJsonObject {
        for (auto *t : std::as_const(m_testerPresentTimers)) {
            t->stop();
            t->deleteLater();
        }
        m_testerPresentTimers.clear();
        m_testerPresentInfo.clear();
        if (!m_device) {
            publishStatus();
            return udsTextResult("already closed");
        }
        m_device->disconnectDevice();
        delete m_device;
        m_device = nullptr;
        m_buffer.clear();
        publishStatus();
        return udsTextResult("closed");
    });
}

QJsonObject CanManager::getReceivedFrames(int limit) {
    return onOwnThread([this, limit]() -> QJsonObject {
        int n = limit > 0 ? qMin(limit, m_buffer.size()) : m_buffer.size();
        QJsonArray arr;
        for (int i = 0; i < n; ++i) {
            const auto &f = m_buffer.at(i);
            arr.append(QJsonObject{
                {"id", QString("0x%1").arg(f.frameId(), 0, 16)},
                {"data", udsHexBytes(f.payload())},
                {"fd", f.hasFlexibleDataRateFormat()},
                {"brs", f.hasBitrateSwitch()}
            });
        }
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + n);
        return udsTextResult(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    });
}

// BMW D-CAN: the tester (0xF1) sends on 0x600 + its address; an ECU
// answers on 0x600 + its own, its frames starting with the tester's address.
static constexpr quint32 BmwTesterAddress = 0xF1;
static constexpr quint32 BmwIdBase = 0x600;

QJsonObject CanManager::sendUdsRequest(qint64 txIdIn, qint64 rxIdIn, const QByteArray &payload, int timeoutMs,
                                       int targetAddress) {
    return onOwnThread([this, txIdIn, rxIdIn, payload, timeoutMs, targetAddress]() -> QJsonObject {
        if (!m_device) return udsTextResult("no CAN interface open", true);
        if (targetAddress >= 0 && !m_extendedAddressing)
            return udsTextResult("targetAddress is for extended addressing (BMW D-CAN): can_open with "
                                 "extendedAddressing=true first, or address the ECU by txId/rxId alone", true);
        const quint8 ext = targetAddress >= 0 ? quint8(targetAddress) : m_addressExtension;
        if (m_extendedAddressing && ext == BmwTesterAddress)
            return udsTextResult("0xF1 is the tester's own address, not an ECU's: set targetAddress to the ECU "
                                 "(e.g. 0x12 for the engine/DME)", true);
        if ((txIdIn < 0 || rxIdIn < 0) && !m_extendedAddressing)
            return udsTextResult("txId and rxId are required (e.g. 0x7E0 / 0x7E8)", true);
        const quint32 txId = txIdIn >= 0 ? quint32(txIdIn) : BmwIdBase + BmwTesterAddress;
        const quint32 rxId = rxIdIn >= 0 ? quint32(rxIdIn) : BmwIdBase + ext;
        const QString route = m_extendedAddressing
            ? QString("sent on 0x%1 to ECU 0x%2, listening on 0x%3")
                  .arg(txId, 0, 16).arg(ext, 2, 16, QChar('0')).arg(rxId, 0, 16)
            : QString("sent on 0x%1, listening on 0x%2").arg(txId, 0, 16).arg(rxId, 0, 16);

        if (!sendIsoTp(txId, rxId, payload, ext))
            return udsTextResult(QString("failed to send request: no flow control from the ECU (%1)").arg(route), true);

        for (int attempt = 0; attempt < 10; ++attempt) {
            QByteArray resp = receiveIsoTp(txId, rxId, timeoutMs, ext);
            if (resp.isEmpty())
                return udsTextResult(QString("timeout waiting for response (%1)%2").arg(route,
                    m_extendedAddressing ? QString("; with extended addressing the ECU answers on 0x600 + its "
                                                   "address - check targetAddress")
                                         : QString("; check the bitrate (can_get_received_frames shows whether "
                                                   "the bus has traffic at all) and txId/rxId")), true);
            if (resp.size() >= 3 && static_cast<quint8>(resp[0]) == 0x7F) {
                quint8 nrc = static_cast<quint8>(resp[2]);
                if (nrc == 0x78) continue;
                return udsTextResult(QString("negative response: NRC 0x%1 (%2)")
                    .arg(nrc, 2, 16, QChar('0')).arg(udsNrcName(nrc)), true);
            }
            return udsTextResult(udsHexBytes(resp));
        }
        return udsTextResult("ECU kept responding pending (0x78), gave up", true);
    });
}

QJsonObject CanManager::testerPresentStart(qint64 idIn, int intervalMs, bool functional, bool suppressPositiveResponse,
                                           int targetAddress) {
    return onOwnThread([this, idIn, intervalMs, functional, suppressPositiveResponse, targetAddress]() -> QJsonObject {
        if (!m_device) return udsTextResult("no CAN interface open", true);
        if (targetAddress >= 0 && !m_extendedAddressing)
            return udsTextResult("targetAddress is for extended addressing (BMW D-CAN): can_open with "
                                 "extendedAddressing=true first", true);
        const quint8 ext = targetAddress >= 0 ? quint8(targetAddress) : m_addressExtension;
        if (idIn < 0 && !m_extendedAddressing)
            return udsTextResult("id is required (the ECU's request ID, e.g. 0x7E0)", true);
        const quint32 id = idIn >= 0 ? quint32(idIn) : BmwIdBase + BmwTesterAddress;
        QString handle = QString("tp_%1").arg(++m_handleCounter);
        quint8 sub = (functional || suppressPositiveResponse) ? 0x80 : 0x00;
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, id, sub, ext]() {
            int ae = m_extendedAddressing ? 1 : 0;
            QByteArray d(8, char(m_padByte));
            int i = 0;
            if (ae) d[i++] = char(ext);
            d[i] = char(0x02);
            d[i + 1] = char(0x3E);
            d[i + 2] = char(sub);
            sendRaw(id, d);
        });
        timer->start(intervalMs);
        m_testerPresentTimers.insert(handle, timer);
        QJsonObject info{{"id", QString("0x%1").arg(id, 0, 16)},
                         {"intervalMs", intervalMs},
                         {"functional", functional}};
        if (m_extendedAddressing)
            info["targetAddress"] = QString("0x%1").arg(ext, 2, 16, QChar('0'));
        m_testerPresentInfo.insert(handle, info);
        publishStatus();
        return udsTextResult(handle);
    });
}

QJsonObject CanManager::testerPresentStop(const QString &handle) {
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

QJsonObject CanListInterfacesCommand::definition() const {
    return QJsonObject{
        {"name", "can_list_interfaces"},
        {"description", "Lists the CAN plugins and interfaces on this machine. Call it first and use its names in can_open - don't guess them."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"plugin", QJsonObject{{"type", "string"}, {"description", "plugin name from can_list_interfaces, e.g. peakcan, gsusb, socketcan, virtualcan"}}}
            }}
        }}
    };
}
QJsonObject CanListInterfacesCommand::execute(const QJsonObject &args) {
    return m_manager->listInterfaces(args["plugin"].toString());
}

QJsonObject CanOpenCommand::definition() const {
    return QJsonObject{
        {"name", "can_open"},
        {"description", "Opens a CAN interface (plugin and interface from can_list_interfaces); then send requests with can_send_request. Usual bitrate 500000. BMW over CAN (D-CAN): extendedAddressing true."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"plugin", QJsonObject{{"type", "string"}, {"description", "backend plugin name as returned by can_list_interfaces, e.g. socketcan, socketcanfd, peakcan, tinycan, passthrucan, virtualcan, gsusb"}}},
                {"interface", QJsonObject{{"type", "string"}, {"description", "interface name from can_list_interfaces for that plugin"}}},
                {"paddingByte", QJsonObject{{"type", "integer"}, {"description", "byte to pad frames to 8 bytes, default 0x00 (some ECUs want 0xAA or 0xCC)"}}},
                {"extendedId", QJsonObject{{"type", "boolean"}, {"description", "29-bit CAN ids. Default false; ids above 0x7FF use them anyway."}}},
                {"extendedAddressing", QJsonObject{{"type", "boolean"}, {"description", "ISO-TP extended addressing: the first byte of every frame you send is the target ECU's address. Turn it on for BMW D-CAN (all requests go out on 0x6F1, each ECU answers on 0x600 + its address); then pass targetAddress (the ECU) in each can_send_request. Not the same as 29-bit IDs (extendedId). Leave false for normal UDS/OBD with a txId/rxId pair per ECU (e.g. 0x7E0/0x7E8)."}}},
                {"addressExtension", QJsonObject{{"type", "integer"}, {"description", "With extendedAddressing: a default target ECU address, used when a request gives no targetAddress. Optional - prefer targetAddress per request. Never 0xF1: that is the tester's own address."}}},
                {"fd", QJsonObject{{"type", "boolean"}, {"description", "CAN FD (64-byte frames). Default false: classic CAN."}}},
                {"bitrate", QJsonObject{{"type", "integer"}, {"description", "bit/s: 500000 for most diagnostic CAN (OBD port), 125000 or 100000 for some body buses. A wrong bitrate shows as timeouts with no traffic."}}},
                {"dataBitrate", QJsonObject{{"type", "integer"}, {"description", "CAN FD data-phase bitrate in bit/s, e.g. 2000000. Only used when fd is true"}}},
                {"bitrateSwitch", QJsonObject{{"type", "boolean"}, {"description", "use bit-rate switching (BRS) for the data phase of FD frames. Default true when fd is true and dataBitrate is set"}}}
            }},
            {"required", QJsonArray{"plugin", "interface"}}
        }}
    };
}
QJsonObject CanOpenCommand::execute(const QJsonObject &args) {
    quint8 padByte = args.contains("paddingByte") ? static_cast<quint8>(args["paddingByte"].toInt()) : 0x00;
    bool extendedId = args["extendedId"].toBool();
    bool extendedAddressing = args["extendedAddressing"].toBool();
    quint8 addressExtension = static_cast<quint8>(args["addressExtension"].toInt());
    bool fdMode = args["fd"].toBool();
    quint32 bitrate = static_cast<quint32>(args["bitrate"].toInt(0));
    quint32 dataBitrate = static_cast<quint32>(args["dataBitrate"].toInt(0));
    bool bitrateSwitch = args.contains("bitrateSwitch") ? args["bitrateSwitch"].toBool() : (dataBitrate != 0);
    return m_manager->open(args["plugin"].toString(), args["interface"].toString(), padByte, extendedId, extendedAddressing,
                            addressExtension, fdMode, bitrate, dataBitrate, bitrateSwitch);
}

QJsonObject CanCloseCommand::definition() const {
    return QJsonObject{
        {"name", "can_close"},
        {"description", "Closes the CAN interface and stops its tester presents."},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject CanCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject CanGetReceivedFramesCommand::definition() const {
    return QJsonObject{
        {"name", "can_get_received_frames"},
        {"description", "Raw CAN frames seen since the last call - only for checking whether the bus has traffic when requests time out. Not needed for normal requests."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}, {"description", "at most this many (default all, up to 500)"}}}
            }}
        }}
    };
}
QJsonObject CanGetReceivedFramesCommand::execute(const QJsonObject &args) {
    return m_manager->getReceivedFrames(args["limit"].toInt(0));
}

QJsonObject UdsSendRequestCommand::definition() const {
    return QJsonObject{
        {"name", "can_send_request"},
        {"description", "Sends one UDS request to an ECU over CAN and returns its answer as hex, e.g. '62 F1 90 ...' (framing and 'response pending' are handled). A negative answer comes back as an error naming the NRC. Needs can_open."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "Extended addressing (BMW D-CAN) only: the ECU's address byte, e.g. \"0x12\" (engine/DME), \"0x40\". With it, txId and rxId can be left out: 0x6F1 and 0x600 + targetAddress. Never 0xF1 (that is the tester)."}}},
                {"txId", QJsonObject{{"type", "string"}, {"description", "CAN ID the request is sent on, e.g. \"0x7E0\". Required unless extended addressing derives it."}}},
                {"rxId", QJsonObject{{"type", "string"}, {"description", "CAN ID the ECU answers on, e.g. \"0x7E8\". Required unless extended addressing derives it."}}},
                {"data", QJsonObject{{"type", "string"}, {"description", "The UDS request as hex bytes, e.g. \"22 F1 90\" (read the VIN); the first byte is the service ID. An array of numbers 0-255 works too. No framing or length bytes."}}},
                {"timeoutMs", QJsonObject{{"type", "integer"}, {"description", "how long to wait for the complete response, default 1000ms. Increase for slow services (e.g. routine control, flashing-related requests, or ECUs known to respond slowly)."}}}
            }},
            {"required", QJsonArray{"data"}}
        }}
    };
}
QJsonObject UdsSendRequestCommand::execute(const QJsonObject &args) {
    qint64 txId = args.contains("txId") ? qint64(udsParseId(args["txId"])) : -1;
    qint64 rxId = args.contains("rxId") ? qint64(udsParseId(args["rxId"])) : -1;
    int target = args.contains("targetAddress") ? int(udsParseId(args["targetAddress"]) & 0xFF) : -1;
    QByteArray payload;
    QString bytesError;
    payload = udsParseBytes(args["data"], &bytesError);
    if (!bytesError.isEmpty()) return udsTextResult(bytesError, true);
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 1000;
    return m_manager->sendUdsRequest(txId, rxId, payload, timeoutMs, target);
}

QJsonObject UdsTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "can_tester_present_start"},
        {"description", "Keeps an ECU's diagnostic session open by sending TesterPresent every intervalMs - only needed in a non-default session (after 10 03). Returns a handle for can_tester_present_stop."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"txId", QJsonObject{{"type", "string"}, {"description", "CAN ID to send on: the same txId as in can_send_request (e.g. 0x7E0), or the broadcast ID (0x7DF) for addressing=functional. With extended addressing it can be left out (0x6F1)."}}},
                {"targetAddress", QJsonObject{{"type", "string"}, {"description", "Extended addressing (BMW D-CAN) only: the ECU to keep in its session, e.g. \"0x12\" - the same targetAddress as in can_send_request."}}},
                {"addressing", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"physical", "functional"}}, {"description", "default physical. functional always suppresses the response regardless of suppressPositiveResponse"}}},
                {"intervalMs", QJsonObject{{"type", "integer"}, {"description", "default 2000, keep below the ECU's S3 timeout (usually 5000ms)"}}},
                {"suppressPositiveResponse", QJsonObject{{"type", "boolean"}, {"description", "default true, ignored (always true) when addressing=functional"}}}
            }},
        }}
    };
}
QJsonObject UdsTesterPresentStartCommand::execute(const QJsonObject &args) {
    // "id" was this argument's name before it matched can_send_request's.
    const QJsonValue idArg = args.contains("txId") ? args["txId"] : args["id"];
    qint64 id = idArg.isUndefined() ? -1 : qint64(udsParseId(idArg));
    int target = args.contains("targetAddress") ? int(udsParseId(args["targetAddress"]) & 0xFF) : -1;
    int intervalMs = args.contains("intervalMs") ? args["intervalMs"].toInt() : 2000;
    bool functional = args["addressing"].toString() == "functional";
    bool suppress = args.contains("suppressPositiveResponse") ? args["suppressPositiveResponse"].toBool() : true;
    return m_manager->testerPresentStart(id, intervalMs, functional, suppress, target);
}

QJsonObject UdsTesterPresentStopCommand::definition() const {
    return QJsonObject{
        {"name", "can_tester_present_stop"},
        {"description", "Stops a tester present started with can_tester_present_start."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}, {"description", "the handle the matching _start call returned, e.g. \"tp_1\""}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject UdsTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}
