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

CanManager::CanManager(QObject *parent) : QObject(parent) {}

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

bool CanManager::sendIsoTp(quint32 txId, quint32 rxId, const QByteArray &payload) {
    int ae = m_extendedAddressing ? 1 : 0;
    int classicMax = 7 - ae;

    if (payload.size() <= classicMax) {
        // Fits the classic 4-bit SF_DL, sent as an 8-byte frame either way.
        QByteArray d(8, char(m_padByte));
        int i = 0;
        if (ae) d[i++] = char(m_addressExtension);
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
        if (ae) d[i++] = char(m_addressExtension);
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
    if (ae) ff[i++] = char(m_addressExtension);
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
            if (ae) cf[j++] = char(m_addressExtension);
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

QByteArray CanManager::receiveIsoTp(quint32 txId, quint32 rxId, int timeoutMs) {
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
    if (ae) fc[0] = char(m_addressExtension);
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
        if (!m_device) return udsTextResult("already closed");
        m_device->disconnectDevice();
        delete m_device;
        m_device = nullptr;
        m_buffer.clear();
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

QJsonObject CanManager::sendUdsRequest(quint32 txId, quint32 rxId, const QByteArray &payload, int timeoutMs) {
    return onOwnThread([this, txId, rxId, payload, timeoutMs]() -> QJsonObject {
        if (!m_device) return udsTextResult("no CAN interface open", true);
        if (!sendIsoTp(txId, rxId, payload))
            return udsTextResult("failed to send request (no flow control from ECU)", true);

        for (int attempt = 0; attempt < 10; ++attempt) {
            QByteArray resp = receiveIsoTp(txId, rxId, timeoutMs);
            if (resp.isEmpty()) return udsTextResult("timeout waiting for response", true);
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

QJsonObject CanManager::testerPresentStart(quint32 id, int intervalMs, bool functional, bool suppressPositiveResponse) {
    return onOwnThread([this, id, intervalMs, functional, suppressPositiveResponse]() -> QJsonObject {
        if (!m_device) return udsTextResult("no CAN interface open", true);
        QString handle = QString("tp_%1").arg(++m_handleCounter);
        quint8 sub = (functional || suppressPositiveResponse) ? 0x80 : 0x00;
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, id, sub]() {
            int ae = m_extendedAddressing ? 1 : 0;
            QByteArray d(8, char(m_padByte));
            int i = 0;
            if (ae) d[i++] = char(m_addressExtension);
            d[i] = char(0x02);
            d[i + 1] = char(0x3E);
            d[i + 2] = char(sub);
            sendRaw(id, d);
        });
        timer->start(intervalMs);
        m_testerPresentTimers.insert(handle, timer);
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
        return udsTextResult("stopped");
    });
}

QJsonObject CanListInterfacesCommand::definition() const {
    return QJsonObject{
        {"name", "can_list_interfaces"},
        {"description", "Discover which CAN backend plugins and interfaces actually exist on this machine. Call this with no arguments before can_open - plugin/interface names are platform- and install-dependent, so treat this as the source of truth rather than guessing names."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"plugin", QJsonObject{{"type", "string"}, {"description", "e.g. socketcan, socketcanfd, peakcan, tinycan, passthrucan, virtualcan, gsusb. Omit to list all available plugins. On Linux, gs_usb/candleLight-class CAN-FD adapters bind to the kernel driver and enumerate under socketcan/socketcanfd. On Windows (no such kernel driver) they're reached directly over USB via the built-in 'gsusb' backend instead - its entries are serial numbers (or bus/address if no serial)"}}}
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
        {"description", "Open a CAN interface on a given backend plugin. Call can_list_interfaces first (with no arguments) to see which plugins and interfaces actually exist on this machine - plugin availability and driver support vary by OS and by what's installed, so don't guess a plugin name from memory."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"plugin", QJsonObject{{"type", "string"}, {"description", "backend plugin name as returned by can_list_interfaces, e.g. socketcan, socketcanfd, peakcan, tinycan, passthrucan, virtualcan, gsusb"}}},
                {"interface", QJsonObject{{"type", "string"}, {"description", "an interface name as returned by can_list_interfaces for the chosen plugin"}}},
                {"paddingByte", QJsonObject{{"type", "integer"}, {"description", "byte used to pad ISO-TP frames to 8 bytes, default 0x00, common alternatives 0xAA/0xCC"}}},
                {"extendedId", QJsonObject{{"type", "boolean"}, {"description", "force 29-bit extended CAN IDs; IDs above 0x7FF already get this automatically, so this is only needed to force it for a smaller ID"}}},
                {"extendedAddressing", QJsonObject{{"type", "boolean"}, {"description", "ISO-TP extended addressing: prepend addressExtension as an address-extension byte before the PCI byte on every frame"}}},
                {"addressExtension", QJsonObject{{"type", "integer"}, {"description", "address-extension byte value, only used when extendedAddressing is true"}}},
                {"fd", QJsonObject{{"type", "boolean"}, {"description", "enable CAN FD (up to 64-byte frames), e.g. for a gs_usb/candleLight-class adapter (socketcan/socketcanfd on Linux, gsusb on Windows). Default false (classic CAN, 8-byte frames)"}}},
                {"bitrate", QJsonObject{{"type", "integer"}, {"description", "nominal (arbitration phase) bitrate in bit/s, e.g. 500000. Omit to use the interface's configured default"}}},
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
        {"description", "Close the currently open CAN interface"},
        {"inputSchema", QJsonObject{{"type", "object"}}}
    };
}
QJsonObject CanCloseCommand::execute(const QJsonObject &) {
    return m_manager->close();
}

QJsonObject CanGetReceivedFramesCommand::definition() const {
    return QJsonObject{
        {"name", "can_get_received_frames"},
        {"description", "Drain buffered raw CAN frames received since the last call"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"limit", QJsonObject{{"type", "integer"}}}
            }}
        }}
    };
}
QJsonObject CanGetReceivedFramesCommand::execute(const QJsonObject &args) {
    return m_manager->getReceivedFrames(args["limit"].toInt(0));
}

QJsonObject UdsSendRequestCommand::definition() const {
    return QJsonObject{
        {"name", "uds_send_request"},
        {"description", "Send a UDS request over ISO-TP and return the decoded response"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"txId", QJsonObject{{"type", "string"}, {"description", "request CAN ID, e.g. 0x7E0"}}},
                {"rxId", QJsonObject{{"type", "string"}, {"description", "response CAN ID, e.g. 0x7E8"}}},
                {"data", QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{{"type", "integer"}}},
                    {"description", "SID plus payload bytes, e.g. [34, 241, 144] for ReadDataByIdentifier"}
                }},
                {"timeoutMs", QJsonObject{{"type", "integer"}}}
            }},
            {"required", QJsonArray{"txId", "rxId", "data"}}
        }}
    };
}
QJsonObject UdsSendRequestCommand::execute(const QJsonObject &args) {
    quint32 txId = udsParseId(args["txId"]);
    quint32 rxId = udsParseId(args["rxId"]);
    QByteArray payload;
    for (const auto &v : args["data"].toArray()) payload.append(char(v.toInt()));
    int timeoutMs = args.contains("timeoutMs") ? args["timeoutMs"].toInt() : 1000;
    return m_manager->sendUdsRequest(txId, rxId, payload, timeoutMs);
}

QJsonObject UdsTesterPresentStartCommand::definition() const {
    return QJsonObject{
        {"name", "uds_tester_present_start"},
        {"description", "Start sending periodic UDS TesterPresent (0x3E) to keep a diagnostic session alive"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"id", QJsonObject{{"type", "string"}, {"description", "CAN ID to send on: the ECU's physical request ID for addressing=physical, or the vehicle's broadcast ID (e.g. 0x7DF) for addressing=functional"}}},
                {"addressing", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"physical", "functional"}}, {"description", "default physical. functional always suppresses the response regardless of suppressPositiveResponse"}}},
                {"intervalMs", QJsonObject{{"type", "integer"}, {"description", "default 2000, keep below the ECU's S3 timeout (usually 5000ms)"}}},
                {"suppressPositiveResponse", QJsonObject{{"type", "boolean"}, {"description", "default true, ignored (always true) when addressing=functional"}}}
            }},
            {"required", QJsonArray{"id"}}
        }}
    };
}
QJsonObject UdsTesterPresentStartCommand::execute(const QJsonObject &args) {
    quint32 id = udsParseId(args["id"]);
    int intervalMs = args.contains("intervalMs") ? args["intervalMs"].toInt() : 2000;
    bool functional = args["addressing"].toString() == "functional";
    bool suppress = args.contains("suppressPositiveResponse") ? args["suppressPositiveResponse"].toBool() : true;
    return m_manager->testerPresentStart(id, intervalMs, functional, suppress);
}

QJsonObject UdsTesterPresentStopCommand::definition() const {
    return QJsonObject{
        {"name", "uds_tester_present_stop"},
        {"description", "Stop a running TesterPresent timer"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"handle", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"handle"}}
        }}
    };
}
QJsonObject UdsTesterPresentStopCommand::execute(const QJsonObject &args) {
    return m_manager->testerPresentStop(args["handle"].toString());
}
