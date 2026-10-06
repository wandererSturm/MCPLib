#include "gsusbhealth.h"

#include <QStringList>

namespace {
// Linux SocketCAN error frame layout (gs_usb devices use it).
constexpr quint32 CAN_ERR_FLAG = 0x20000000u;
constexpr quint32 CAN_ERR_TX_TIMEOUT = 0x00000001u;
constexpr quint32 CAN_ERR_CRTL = 0x00000004u;
constexpr quint32 CAN_ERR_ACK = 0x00000020u;
constexpr quint32 CAN_ERR_BUSOFF = 0x00000040u;
constexpr quint32 CAN_ERR_RESTARTED = 0x00000100u;
// data[1] for CAN_ERR_CRTL
constexpr quint8 CAN_ERR_CRTL_RX_OVERFLOW = 0x01;
constexpr quint8 CAN_ERR_CRTL_TX_OVERFLOW = 0x02;
constexpr quint8 CAN_ERR_CRTL_RX_WARNING = 0x04;
constexpr quint8 CAN_ERR_CRTL_TX_WARNING = 0x08;
constexpr quint8 CAN_ERR_CRTL_RX_PASSIVE = 0x10;
constexpr quint8 CAN_ERR_CRTL_TX_PASSIVE = 0x20;
} // namespace

int GsUsbHealth::takeEchoId(qint64 nowMs) {
    for (int i = 0; i < m_sentAt.size(); ++i) {
        if (m_sentAt[i] < 0) {
            m_sentAt[i] = nowMs;
            return i;
        }
    }
    return -1;
}

void GsUsbHealth::giveBack(quint32 echoId) {
    if (echoId < quint32(m_sentAt.size()))
        m_sentAt[int(echoId)] = -1;
}

bool GsUsbHealth::echoed(quint32 echoId) {
    if (echoId >= quint32(m_sentAt.size()) || m_sentAt[int(echoId)] < 0)
        return false;
    m_sentAt[int(echoId)] = -1;
    return true;
}

int GsUsbHealth::inFlight() const {
    int n = 0;
    for (qint64 t : m_sentAt)
        if (t >= 0) ++n;
    return n;
}

bool GsUsbHealth::stalled(qint64 nowMs) const {
    for (qint64 t : m_sentAt)
        if (t >= 0 && nowMs - t > kConfirmTimeoutMs) return true;
    return false;
}

void GsUsbHealth::reset() {
    m_sentAt.fill(-1);
}

bool GsUsbHealth::isErrorFrame(quint32 canId) {
    return canId & CAN_ERR_FLAG;
}

GsUsbHealth::ErrorReport GsUsbHealth::describeErrorFrame(quint32 canId, const QByteArray &data) {
    ErrorReport r;
    QStringList what;
    if (canId & CAN_ERR_BUSOFF) {
        r.busOff = true;
        what << "the CAN controller went bus-off (too many transmit errors)";
    }
    if (canId & CAN_ERR_RESTARTED) {
        r.restarted = true;
        what << "the controller restarted after bus-off";
    }
    if (canId & CAN_ERR_ACK) {
        r.noAck = true;
        what << "nobody acknowledged a frame (wrong bitrate, the ECU asleep or ignition off, or wiring)";
    }
    if (canId & CAN_ERR_TX_TIMEOUT)
        what << "a frame timed out in the adapter";
    if ((canId & CAN_ERR_CRTL) && data.size() > 1) {
        const quint8 c = quint8(data[1]);
        if (c & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) {
            r.passive = true;
            what << "the controller is error-passive (frequent errors on the bus)";
        } else if (c & (CAN_ERR_CRTL_RX_WARNING | CAN_ERR_CRTL_TX_WARNING)) {
            r.passive = true;
            what << "error warning level reached on the bus";
        }
        if (c & (CAN_ERR_CRTL_RX_OVERFLOW | CAN_ERR_CRTL_TX_OVERFLOW))
            what << "the adapter's buffer overflowed";
    }
    r.text = what.join("; ");
    return r;
}
