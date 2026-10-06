#include "gsusbcanbus.h"
#include "gsusbhealth.h"

#include <QElapsedTimer>
#include <libusb-1.0/libusb.h>
#include <QVariant>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QVector>
#include <QAtomicInt>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------
// gs_usb protocol constants (candleLight/canable-class CAN-FD adapters).
// Verified against real hardware (VID 0x1d50 PID 0x606f, "Quarkslab
// CAN-FD"): control-transfer byte counts, bit-timing acceptance, and
// classic + CAN-FD frame round-trip (loopback TX-echo) all confirmed to
// match this layout exactly.
// ---------------------------------------------------------------------
namespace {

constexpr quint16 GS_USB_VID = 0x1d50;
constexpr quint16 GS_USB_PID = 0x606f;
constexpr int GS_USB_EP_IN = 0x81;
constexpr int GS_USB_EP_OUT = 0x02;

enum GsUsbBreq {
    GS_USB_BREQ_HOST_FORMAT = 0,
    GS_USB_BREQ_BITTIMING = 1,
    GS_USB_BREQ_MODE = 2,
    GS_USB_BREQ_BT_CONST = 4,
    GS_USB_BREQ_DATA_BITTIMING = 10,
    GS_USB_BREQ_BT_CONST_EXT = 11,
};

constexpr quint32 GS_CAN_MODE_RESET = 0;
constexpr quint32 GS_CAN_MODE_START = 1;
constexpr quint32 GS_CAN_MODE_FD_FLAG = 0x100;
constexpr quint32 GS_CAN_MODE_BERR_REPORTING = 1u << 12; // bus errors as error frames (if the device can)
constexpr quint32 GS_CAN_FEATURE_BERR_REPORTING = 1u << 12;

constexpr quint8 GS_CAN_FLAG_FD = 0x02;
constexpr quint8 GS_CAN_FLAG_BRS = 0x04;
constexpr quint8 GS_CAN_FLAG_ESI = 0x08;

constexpr quint32 CAN_EFF_FLAG = 0x80000000u;
constexpr quint32 CAN_RTR_FLAG = 0x40000000u;
constexpr quint32 CAN_EFF_MASK = 0x1FFFFFFFu;
constexpr quint32 CAN_SFF_MASK = 0x000007FFu;

constexpr quint32 FEATURE_FD = 1u << 8;

constexpr int kHeaderSize = 12; // echo_id(4) can_id(4) can_dlc(1) channel(1) flags(1) reserved(1)

#pragma pack(push, 1)
struct GsDeviceMode { quint32 mode, flags; };
struct GsDeviceBittiming { quint32 prop_seg, phase_seg1, phase_seg2, sjw, brp; };
struct GsDeviceBtConst {
    quint32 feature, fclk_can, tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc;
};
struct GsDeviceBtConstExt {
    quint32 feature, fclk_can, tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc;
    quint32 dtseg1_min, dtseg1_max, dtseg2_min, dtseg2_max, dsjw_max, dbrp_min, dbrp_max, dbrp_inc;
};
#pragma pack(pop)

struct BitTiming { quint32 prop_seg = 0, phase_seg1 = 0, phase_seg2 = 0, sjw = 0, brp = 0; };

// Standard CAN bit-timing search: find a brp that evenly divides the clock
// for the target bitrate, pick the candidate closest to 16 time quanta
// (~87.5% sample point), then split into tseg1/tseg2 within the device's
// limits. Verified against this hardware's real constraints (40MHz clock):
// 500 kbit/s -> brp=5, tseg1=13, tseg2=2, sjw=2 (control transfer accepted,
// loopback frame round-tripped correctly).
bool calcBitTiming(quint32 fclk, quint32 tseg1Min, quint32 tseg1Max, quint32 tseg2Min, quint32 tseg2Max,
                    quint32 sjwMax, quint32 brpMin, quint32 brpMax, quint32 brpInc,
                    quint32 bitrate, BitTiming *out) {
    if (bitrate == 0 || fclk == 0) return false;
    const quint32 tqMin = 1 + tseg1Min + tseg2Min;
    const quint32 tqMax = 1 + tseg1Max + tseg2Max;

    quint32 bestBrp = 0, bestTq = 0;
    int bestScore = 0;
    bool found = false;
    for (quint32 brp = brpMin; brp <= brpMax; brp += (brpInc ? brpInc : 1)) {
        const quint64 denom = quint64(brp) * bitrate;
        if (denom == 0 || fclk % denom != 0) continue;
        const quint64 tq64 = fclk / denom;
        if (tq64 < tqMin || tq64 > tqMax) continue;
        const quint32 tq = quint32(tq64);
        const int score = -std::abs(int(tq) - 16);
        if (!found || score > bestScore) { found = true; bestScore = score; bestBrp = brp; bestTq = tq; }
    }
    if (!found) return false;

    quint32 tseg2 = quint32(std::lround(bestTq * 0.125));
    tseg2 = qBound(tseg2Min, tseg2, tseg2Max);
    quint32 tseg1 = bestTq - 1 - tseg2;
    if (tseg1 < tseg1Min) { tseg1 = tseg1Min; tseg2 = bestTq > tseg1 + 1 ? bestTq - 1 - tseg1 : tseg2Min; }
    if (tseg1 > tseg1Max) tseg1 = tseg1Max;
    tseg2 = qBound(tseg2Min, tseg2, tseg2Max);

    out->prop_seg = 0;
    out->phase_seg1 = tseg1;
    out->phase_seg2 = tseg2;
    out->sjw = qMax(1u, qMin(sjwMax, qMin(4u, tseg2)));
    out->brp = bestBrp;
    return true;
}

int ctrlOut(libusb_device_handle *h, int req, int value, const void *data, int len) {
    return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
                                    quint8(req), quint16(value), 0, const_cast<unsigned char *>(static_cast<const unsigned char *>(data)),
                                    quint16(len), 1000);
}

int ctrlIn(libusb_device_handle *h, int req, int value, void *data, int len) {
    return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN,
                                    quint8(req), quint16(value), 0, static_cast<unsigned char *>(data), quint16(len), 1000);
}

QByteArray encodeFrame(const QCanBusFrame &frame, bool fdMode) {
    const QByteArray payload = frame.payload();
    const int dataCap = fdMode ? 64 : 8;
    const int size = kHeaderSize + dataCap;
    QByteArray out(size, char(0));
    auto *p = reinterpret_cast<unsigned char *>(out.data());

    quint32 echoId = 0; // set by the I/O thread when it sends: the slot its confirmation comes back under
    quint32 canId = quint32(frame.frameId());
    if (frame.hasExtendedFrameFormat()) canId = (canId & CAN_EFF_MASK) | CAN_EFF_FLAG;
    else canId &= CAN_SFF_MASK;
    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame) canId |= CAN_RTR_FLAG;

    std::memcpy(p + 0, &echoId, 4);
    std::memcpy(p + 4, &canId, 4);
    p[8] = quint8(payload.size());
    p[9] = 0; // channel
    quint8 flags = 0;
    if (fdMode) {
        flags |= GS_CAN_FLAG_FD;
        if (frame.hasBitrateSwitch()) flags |= GS_CAN_FLAG_BRS;
        if (frame.hasErrorStateIndicator()) flags |= GS_CAN_FLAG_ESI;
    }
    p[10] = flags;
    p[11] = 0; // reserved
    std::memcpy(p + kHeaderSize, payload.constData(), qMin(payload.size(), dataCap));
    return out;
}

bool decodeFrame(const QByteArray &bytes, QCanBusFrame *frame) {
    if (bytes.size() < kHeaderSize) return false;
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    quint32 echoId, canId;
    std::memcpy(&echoId, p + 0, 4);
    std::memcpy(&canId, p + 4, 4);
    const quint8 dlc = p[8];
    const quint8 flags = p[10];

    // Only genuine bus/loopback receptions (echo_id == 0xFFFFFFFF) are
    // surfaced as received frames; TX-completion echoes are discarded.
    if (echoId != 0xFFFFFFFFu) return false;

    const bool isFd = flags & GS_CAN_FLAG_FD;
    const int dataCap = isFd ? 64 : 8;
    if (bytes.size() < kHeaderSize + qMin<int>(dlc, dataCap)) return false;

    *frame = QCanBusFrame(canId & (canId & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK),
                           bytes.mid(kHeaderSize, qMin<int>(dlc, dataCap)));
    frame->setExtendedFrameFormat(canId & CAN_EFF_FLAG);
    if (canId & CAN_RTR_FLAG) frame->setFrameType(QCanBusFrame::RemoteRequestFrame);
    if (isFd) {
        frame->setFlexibleDataRateFormat(true);
        frame->setBitrateSwitch(flags & GS_CAN_FLAG_BRS);
        frame->setErrorStateIndicator(flags & GS_CAN_FLAG_ESI);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------
// Background USB I/O: a dedicated thread polls the device with plain
// synchronous libusb_bulk_transfer calls (writes drained from a
// mutex-protected queue, reads coalesced into batches) so the caller's
// thread is never blocked on USB transfers.
// ---------------------------------------------------------------------
class GsUsbIoWorker : public QObject {
    Q_OBJECT
public:
    explicit GsUsbIoWorker(libusb_device_handle *handle) : m_handle(handle) {}

    void requestStop() { m_stop.storeRelease(1); }
    void requestHealthReset() { m_resetHealth.storeRelease(1); } // the controller was restarted
    void enqueueSend(const QByteArray &bytes) {
        QMutexLocker lock(&m_txMutex);
        m_txQueue.enqueue(bytes);
    }
    int queued() {
        QMutexLocker lock(&m_txMutex);
        return int(m_txQueue.size());
    }

public slots:
    // Plain synchronous libusb_bulk_transfer calls, polled in a loop - not
    // async submit/callback. This exact pattern was validated stable and
    // hang-free against real hardware; an earlier async-transfer version of
    // this loop caused a busy-spin / CPU-pegging hang, most likely because
    // this device's legacy libusb-win32 backend doesn't handle libusb's
    // async transfer completion cleanly on Windows.
    //
    // Nothing that goes wrong is swallowed: a USB transfer that fails, a
    // frame the adapter never confirms having put on the bus (its TX echo),
    // and the CAN error frames it reports all come out as signals.
    void run() {
        static constexpr int kBufSize = 128; // FD frame is 76B; leaves margin
        static constexpr int kMaxBatch = 32; // caps memory/signal cost under a flood
        static constexpr int kMaxReads = 256; // reads per round, whatever they were (error frames, echoes)
        static constexpr int kMaxTxTimeouts = 5; // ~0.5 s of the adapter not taking frames
        unsigned char buf[kBufSize];
        QElapsedTimer clock;
        clock.start();

        while (!m_stop.loadAcquire()) {
            if (m_resetHealth.fetchAndStoreAcquire(0))
                m_health.reset();

            // Send what the echo slots allow; the rest waits for confirmations.
            {
                QMutexLocker lock(&m_txMutex);
                while (!m_txQueue.isEmpty()) {
                    const int echo = m_health.takeEchoId(clock.elapsed());
                    if (echo < 0)
                        break; // every slot waits for the adapter to confirm a frame
                    QByteArray bytes = m_txQueue.dequeue();
                    const quint32 echoId = quint32(echo);
                    std::memcpy(bytes.data(), &echoId, 4);
                    lock.unlock();
                    int transferred = 0;
                    auto *data = reinterpret_cast<unsigned char *>(bytes.data());
                    int r = libusb_bulk_transfer(m_handle, GS_USB_EP_OUT, data, bytes.size(), &transferred, 100);
                    if (r == LIBUSB_ERROR_PIPE) { // a stalled endpoint: clear it and try once more
                        libusb_clear_halt(m_handle, GS_USB_EP_OUT);
                        r = libusb_bulk_transfer(m_handle, GS_USB_EP_OUT, data, bytes.size(), &transferred, 100);
                    }
                    lock.relock();
                    if (r == 0 && transferred == bytes.size()) {
                        m_txTimeouts = 0;
                        continue;
                    }
                    m_health.giveBack(echoId);
                    if (r == LIBUSB_ERROR_TIMEOUT || r == 0) {
                        m_txQueue.prepend(bytes); // tried again next round
                        if (++m_txTimeouts >= kMaxTxTimeouts) {
                            lock.unlock();
                            fail("the adapter stopped taking frames over USB (writes time out) - it seems frozen: "
                                 "unplug it and plug it back in, then can_open again");
                            return;
                        }
                        break;
                    }
                    lock.unlock();
                    fail(QString("USB write failed (%1)").arg(libusb_error_name(r)) + hintFor(r));
                    return;
                }
            }

            // Drain everything currently available in one pass and emit it as
            // a single batch. An unterminated/floating CAN bus can make this
            // device report a continuous stream of noise as "received"
            // frames (confirmed against real hardware); emitting one Qt
            // signal per frame in that situation floods the receiver's event
            // queue and stalls the whole process, so frames are coalesced
            // into a bounded batch instead.
            QList<QByteArray> batch;
            // Every read counts, not just the frames kept: a controller nobody
            // acknowledges reports an error frame (and a TX echo can't come)
            // on every retry - thousands a second - and a loop that only
            // counted data frames never ended, so Stop/close never got in.
            int reads = 0;
            while (batch.size() < kMaxBatch && reads < kMaxReads && !m_stop.loadAcquire()) {
                ++reads;
                int rxlen = 0;
                // timeout=0 means "wait forever" to libusb, so the drain
                // reads after the first use a small nonzero timeout instead.
                int r = libusb_bulk_transfer(m_handle, GS_USB_EP_IN, buf, kBufSize, &rxlen, reads == 1 ? 50 : 1);
                if (r == LIBUSB_ERROR_TIMEOUT)
                    break; // caught up, nothing more waiting
                if (r == LIBUSB_ERROR_PIPE) {
                    if (++m_rxStalls > 3) {
                        fail("USB read endpoint keeps stalling" + hintFor(r));
                        return;
                    }
                    libusb_clear_halt(m_handle, GS_USB_EP_IN);
                    break;
                }
                if (r == LIBUSB_ERROR_OVERFLOW)
                    break; // one oversized transfer: skip it
                if (r != 0) {
                    fail(QString("USB read failed (%1)").arg(libusb_error_name(r)) + hintFor(r));
                    return;
                }
                m_rxStalls = 0;
                if (rxlen < kHeaderSize)
                    continue;
                quint32 echoId, canId;
                std::memcpy(&echoId, buf + 0, 4);
                std::memcpy(&canId, buf + 4, 4);
                if (echoId != 0xFFFFFFFFu) { // a TX confirmation: that frame is on the bus
                    m_health.echoed(echoId);
                    continue;
                }
                if (GsUsbHealth::isErrorFrame(canId)) {
                    const QByteArray data(reinterpret_cast<const char *>(buf + kHeaderSize), qMin(8, rxlen - kHeaderSize));
                    const GsUsbHealth::ErrorReport report = GsUsbHealth::describeErrorFrame(canId, data);
                    // The same report again within a second isn't news (one per
                    // retry would flood the receiving thread); bus-off always is.
                    if (!report.text.isEmpty()
                        && (report.busOff || report.text != m_lastBusEvent || !m_lastBusEventAt.isValid()
                            || m_lastBusEventAt.elapsed() >= 1000)) {
                        m_lastBusEvent = report.text;
                        m_lastBusEventAt.start();
                        emit busEvent(report.busOff, report.text);
                    }
                    continue;
                }
                batch.append(QByteArray(reinterpret_cast<const char *>(buf), rxlen));
            }

            // Frames sent but never confirmed: they aren't getting onto the bus.
            const bool stalled = m_health.stalled(clock.elapsed());
            if (stalled != m_reportedStall) {
                m_reportedStall = stalled;
                emit confirmations(stalled, m_health.inFlight() + queued());
            }

            if (reads >= kMaxReads)
                QThread::msleep(5); // a flood of error frames/echoes: let the rest of the round (and Stop) in
            if (batch.isEmpty()) continue;
            emit frameReceived(batch);
            // Hitting the cap means the device is sustaining a continuous
            // flood (observed in practice from an unterminated/floating CAN
            // bus reading ambient noise as traffic) - without a pause here,
            // this thread would keep posting full batches back-to-back
            // faster than the receiving thread's event queue can drain,
            // growing unbounded and stalling the whole process.
            if (batch.size() == kMaxBatch)
                QThread::msleep(100);
        }
    }

signals:
    void frameReceived(const QList<QByteArray> &batch);
    void failed(const QString &reason);              // the adapter stopped working; this thread has ended
    void busEvent(bool busOff, const QString &text);  // a CAN error frame worth telling
    void confirmations(bool stalled, int waiting);    // frames stopped (or started again) being confirmed

private:
    static QString hintFor(int libusbError) {
        if (libusbError == LIBUSB_ERROR_NO_DEVICE)
            return " - the adapter was unplugged or reset: plug it in, then can_open again";
        return " - the adapter stopped responding: unplug it and plug it back in, then can_open again";
    }
    void fail(const QString &reason) {
        m_stop.storeRelease(1);
        emit failed(reason);
    }

    libusb_device_handle *m_handle;
    QMutex m_txMutex;
    QQueue<QByteArray> m_txQueue;
    QAtomicInt m_stop{0};
    QAtomicInt m_resetHealth{0};
    GsUsbHealth m_health;
    bool m_reportedStall = false;
    int m_txTimeouts = 0;
    int m_rxStalls = 0;
    QString m_lastBusEvent;
    QElapsedTimer m_lastBusEventAt;
};

// ---------------------------------------------------------------------
// GsUsbCanBusDevice
// ---------------------------------------------------------------------
GsUsbCanBusDevice::GsUsbCanBusDevice(const QString &busIdentifier, QObject *parent)
    : QCanBusDevice(parent), m_busIdentifier(busIdentifier) {}

GsUsbCanBusDevice::~GsUsbCanBusDevice() {
    close();
}

QStringList GsUsbCanBusDevice::availableDevices(QString *errorReason) {
    QStringList result;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        if (errorReason) *errorReason = "libusb_init failed";
        return result;
    }
    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != GS_USB_VID || desc.idProduct != GS_USB_PID) continue;

        QString id;
        libusb_device_handle *h = nullptr;
        if (desc.iSerialNumber && libusb_open(list[i], &h) == 0) {
            unsigned char buf[256];
            int n = libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, buf, sizeof(buf));
            if (n > 0) id = QString::fromLatin1(reinterpret_cast<char *>(buf), n);
            libusb_close(h);
        }
        if (id.isEmpty())
            id = QString("bus%1addr%2").arg(libusb_get_bus_number(list[i])).arg(libusb_get_device_address(list[i]));
        result << id;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return result;
}

bool GsUsbCanBusDevice::open() {
    if (libusb_init(&m_ctx) != 0) {
        setError("libusb_init failed", QCanBusDevice::ConnectionError);
        return false;
    }

    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(m_ctx, &list);
    libusb_device *match = nullptr;
    for (ssize_t i = 0; i < cnt && !match; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != GS_USB_VID || desc.idProduct != GS_USB_PID) continue;

        QString id;
        libusb_device_handle *h = nullptr;
        if (desc.iSerialNumber && libusb_open(list[i], &h) == 0) {
            unsigned char buf[256];
            int n = libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, buf, sizeof(buf));
            if (n > 0) id = QString::fromLatin1(reinterpret_cast<char *>(buf), n);
            libusb_close(h);
        }
        if (id.isEmpty())
            id = QString("bus%1addr%2").arg(libusb_get_bus_number(list[i])).arg(libusb_get_device_address(list[i]));
        if (id == m_busIdentifier) match = list[i];
    }

    if (!match) {
        libusb_free_device_list(list, 1);
        setError("gs_usb device not found: " + m_busIdentifier, QCanBusDevice::ConnectionError);
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    int r = libusb_open(match, &m_handle);
    libusb_free_device_list(list, 1);
    if (r != 0) {
        setError(QString("libusb_open failed: %1").arg(libusb_error_name(r)), QCanBusDevice::ConnectionError);
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    r = libusb_claim_interface(m_handle, 0);
    if (r != 0) {
        setError(QString("failed to claim interface: %1").arg(libusb_error_name(r)), QCanBusDevice::ConnectionError);
        libusb_close(m_handle);
        m_handle = nullptr;
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    quint32 hostFormat = 0x0000beef;
    ctrlOut(m_handle, GS_USB_BREQ_HOST_FORMAT, 0, &hostFormat, sizeof(hostFormat));

    GsDeviceMode reset{GS_CAN_MODE_RESET, 0};
    ctrlOut(m_handle, GS_USB_BREQ_MODE, 0, &reset, sizeof(reset));

    m_fdMode = configurationParameter(QCanBusDevice::CanFdKey).toBool();
    quint32 bitrate = configurationParameter(QCanBusDevice::BitRateKey).toUInt();
    if (bitrate == 0) bitrate = 500000;
    quint32 dataBitrate = configurationParameter(QCanBusDevice::DataBitRateKey).toUInt();
    if (m_fdMode && dataBitrate == 0) dataBitrate = 2000000;

    GsDeviceBtConst btc{};
    if (ctrlIn(m_handle, GS_USB_BREQ_BT_CONST, 0, &btc, sizeof(btc)) != int(sizeof(btc))) {
        setError("failed to read device bit-timing constants", QCanBusDevice::ConfigurationError);
        close();
        return false;
    }
    if (m_fdMode && !(btc.feature & FEATURE_FD)) {
        setError("this gs_usb device does not support CAN FD", QCanBusDevice::ConfigurationError);
        close();
        return false;
    }

    BitTiming nominal;
    if (!calcBitTiming(btc.fclk_can, btc.tseg1_min, btc.tseg1_max, btc.tseg2_min, btc.tseg2_max,
                        btc.sjw_max, btc.brp_min, btc.brp_max, btc.brp_inc, bitrate, &nominal)) {
        setError(QString("no valid bit-timing solution for %1 bit/s").arg(bitrate), QCanBusDevice::ConfigurationError);
        close();
        return false;
    }
    GsDeviceBittiming bt{nominal.prop_seg, nominal.phase_seg1, nominal.phase_seg2, nominal.sjw, nominal.brp};
    if (ctrlOut(m_handle, GS_USB_BREQ_BITTIMING, 0, &bt, sizeof(bt)) != int(sizeof(bt))) {
        setError("failed to set nominal bit-timing", QCanBusDevice::ConfigurationError);
        close();
        return false;
    }

    if (m_fdMode) {
        GsDeviceBtConstExt btcExt{};
        if (ctrlIn(m_handle, GS_USB_BREQ_BT_CONST_EXT, 0, &btcExt, sizeof(btcExt)) != int(sizeof(btcExt))) {
            setError("failed to read device FD bit-timing constants", QCanBusDevice::ConfigurationError);
            close();
            return false;
        }
        BitTiming data;
        if (!calcBitTiming(btcExt.fclk_can, btcExt.dtseg1_min, btcExt.dtseg1_max, btcExt.dtseg2_min, btcExt.dtseg2_max,
                            btcExt.dsjw_max, btcExt.dbrp_min, btcExt.dbrp_max, btcExt.dbrp_inc, dataBitrate, &data)) {
            setError(QString("no valid FD data-phase bit-timing solution for %1 bit/s").arg(dataBitrate),
                      QCanBusDevice::ConfigurationError);
            close();
            return false;
        }
        GsDeviceBittiming dbt{data.prop_seg, data.phase_seg1, data.phase_seg2, data.sjw, data.brp};
        if (ctrlOut(m_handle, GS_USB_BREQ_DATA_BITTIMING, 0, &dbt, sizeof(dbt)) != int(sizeof(dbt))) {
            setError("failed to set FD data-phase bit-timing", QCanBusDevice::ConfigurationError);
            close();
            return false;
        }
    }

    m_startFlags = m_fdMode ? GS_CAN_MODE_FD_FLAG : 0;
    if (btc.feature & GS_CAN_FEATURE_BERR_REPORTING)
        m_startFlags |= GS_CAN_MODE_BERR_REPORTING; // a missing ACK, bus-off etc. come back as error frames
    m_failure.clear();
    m_busOff = false;
    m_restartedOnce = false;
    m_lastEvent.clear();
    GsDeviceMode start{GS_CAN_MODE_START, m_startFlags};
    if (ctrlOut(m_handle, GS_USB_BREQ_MODE, 0, &start, sizeof(start)) != int(sizeof(start))) {
        setError("failed to start CAN controller", QCanBusDevice::ConnectionError);
        close();
        return false;
    }

    m_worker = new GsUsbIoWorker(m_handle);
    m_worker->moveToThread(&m_ioThread);
    connect(&m_ioThread, &QThread::started, m_worker, &GsUsbIoWorker::run);
    connect(m_worker, &GsUsbIoWorker::frameReceived, this, &GsUsbCanBusDevice::onFrameBytesReceived);
    connect(m_worker, &GsUsbIoWorker::failed, this, &GsUsbCanBusDevice::onFailed);
    connect(m_worker, &GsUsbIoWorker::busEvent, this, &GsUsbCanBusDevice::onBusEvent);
    connect(m_worker, &GsUsbIoWorker::confirmations, this, &GsUsbCanBusDevice::onConfirmations);
    m_ioThread.start();

    // The base class does not transition state() to ConnectedState on its
    // own; QCanBusDevice::readFrame() silently no-ops (with a qt.canbus
    // warning) while state() isn't ConnectedState, which left
    // CanManager::onFramesReceived()'s "while (framesAvailable()) readFrame()"
    // loop spinning forever once frames started arriving.
    setState(QCanBusDevice::ConnectedState);
    return true;
}

void GsUsbCanBusDevice::close() {
    if (m_worker) {
        m_worker->requestStop();
        m_ioThread.quit();
        m_ioThread.wait();
        delete m_worker;
        m_worker = nullptr;
    }
    if (m_handle) {
        GsDeviceMode stop{GS_CAN_MODE_RESET, 0};
        ctrlOut(m_handle, GS_USB_BREQ_MODE, 0, &stop, sizeof(stop));
        libusb_release_interface(m_handle, 0);
        libusb_close(m_handle);
        m_handle = nullptr;
    }
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
    setState(QCanBusDevice::UnconnectedState);
}

bool GsUsbCanBusDevice::writeFrame(const QCanBusFrame &frame) {
    if (state() != QCanBusDevice::ConnectedState || !m_worker) {
        setError("device not connected", QCanBusDevice::WriteError);
        return false;
    }
    if (!frame.isValid()) {
        setError("invalid frame", QCanBusDevice::WriteError);
        return false;
    }
    if ((frame.hasFlexibleDataRateFormat() || frame.payload().size() > 8) && !m_fdMode) {
        setError("CAN FD frame on a classic (non-FD) connection", QCanBusDevice::WriteError);
        return false;
    }
    if (!m_failure.isEmpty()) { // the adapter stopped working: nothing goes out
        setError(m_failure, QCanBusDevice::ConnectionError);
        return false;
    }
    if (m_busOff) {
        setError("the CAN controller is bus-off - can_close and can_open again (check the bitrate and wiring)",
                 QCanBusDevice::WriteError);
        return false;
    }
    if (m_worker->queued() >= kMaxQueued) {
        setError(QString("the adapter isn't sending: %1 frames are waiting to go out").arg(kMaxQueued),
                 QCanBusDevice::WriteError);
        return false;
    }
    m_worker->enqueueSend(encodeFrame(frame, m_fdMode));
    return true;
}

QString GsUsbCanBusDevice::interpretErrorFrame(const QCanBusFrame &errorFrame) {
    return QString::fromLatin1("gs_usb error frame: id=0x%1").arg(errorFrame.frameId(), 0, 16);
}

void GsUsbCanBusDevice::onFailed(const QString &reason) {
    m_failure = reason;
    setError(reason, QCanBusDevice::ConnectionError);
}

void GsUsbCanBusDevice::onBusEvent(bool busOff, const QString &text) {
    if (busOff && !m_busOff) {
        if (!m_restartedOnce) {
            // The usual recovery: restart the controller - once. Twice means
            // something on the bus is wrong, and a restart loop would hide it.
            m_restartedOnce = true;
            restartController();
            report(text + " - restarted the CAN controller once; if it happens again, check the bitrate "
                          "(500000 for diagnostics) and the wiring", QCanBusDevice::WriteError);
        } else {
            m_busOff = true;
            report(text + " - again, after a restart: the bus doesn't accept this adapter's frames. Check the "
                          "bitrate, the ignition and the wiring, then can_close and can_open",
                   QCanBusDevice::WriteError);
        }
        return;
    }
    report(text, QCanBusDevice::WriteError);
}

void GsUsbCanBusDevice::onConfirmations(bool stalled, int waiting) {
    if (stalled)
        report(QString("frames aren't reaching the bus: %1 waiting over a second for the adapter's confirmation - "
                       "nobody acknowledges them (wrong bitrate, the ECU asleep or ignition off, wiring), or the "
                       "adapter is stuck").arg(waiting),
               QCanBusDevice::TimeoutError);
}

void GsUsbCanBusDevice::restartController() {
    if (!m_handle)
        return;
    GsDeviceMode reset{GS_CAN_MODE_RESET, 0};
    ctrlOut(m_handle, GS_USB_BREQ_MODE, 0, &reset, sizeof(reset));
    GsDeviceMode start{GS_CAN_MODE_START, m_startFlags};
    ctrlOut(m_handle, GS_USB_BREQ_MODE, 0, &start, sizeof(start));
    if (m_worker)
        m_worker->requestHealthReset();
}

// The same text again within a second (a bus error on every frame) isn't news.
void GsUsbCanBusDevice::report(const QString &text, QCanBusDevice::CanBusError kind) {
    if (text == m_lastEvent && m_lastEventAt.isValid() && m_lastEventAt.elapsed() < 1000)
        return;
    m_lastEvent = text;
    m_lastEventAt.start();
    setError(text, kind);
}

void GsUsbCanBusDevice::onFrameBytesReceived(const QList<QByteArray> &batch) {
    QList<QCanBusFrame> frames;
    frames.reserve(batch.size());
    for (const QByteArray &bytes : batch) {
        QCanBusFrame frame;
        if (decodeFrame(bytes, &frame))
            frames.append(frame);
    }
    if (!frames.isEmpty())
        enqueueReceivedFrames(frames);
}

#include "gsusbcanbus.moc"
