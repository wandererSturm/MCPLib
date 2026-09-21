#ifndef GSUSBCANBUS_H
#define GSUSBCANBUS_H
#include <QCanBusDevice>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QVector>
#include "mcpserverlib_global.h"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

// Userspace QCanBusDevice backend for gs_usb-protocol CAN/CAN-FD adapters
// (candleLight, canable, CANtact, and similar boards built around the
// open-source gs_usb firmware/protocol - USB VID 0x1d50, PID 0x606f).
//
// These devices have no OS-level CAN driver on Windows (unlike Linux, where
// the gs_usb kernel module exposes them as ordinary SocketCAN interfaces),
// so this talks the gs_usb USB control/bulk protocol directly via libusb.
class GsUsbIoWorker;

class MCPSERVERLIB_EXPORT GsUsbCanBusDevice : public QCanBusDevice {
    Q_OBJECT
public:
    explicit GsUsbCanBusDevice(const QString &busIdentifier, QObject *parent = nullptr);
    ~GsUsbCanBusDevice() override;

    bool writeFrame(const QCanBusFrame &frame) override;
    QString interpretErrorFrame(const QCanBusFrame &errorFrame) override;

    // Each entry is a stable identifier (serial number, or bus:address
    // fallback) suitable for passing back into the "interface" argument.
    static QStringList availableDevices(QString *errorReason = nullptr);

protected:
    bool open() override;
    void close() override;

private slots:
    void onFrameBytesReceived(const QList<QByteArray> &batch);

private:
    QString m_busIdentifier;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    QThread m_ioThread;
    GsUsbIoWorker *m_worker = nullptr;
    bool m_fdMode = false;
};

#endif
