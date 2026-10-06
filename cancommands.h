#ifndef CANCOMMANDS_H
#define CANCOMMANDS_H
#include <QObject>
#include <QCanBusDevice>
#include <QCanBusFrame>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QTimer>
#include <QThread>
#include <functional>
#include "mcpcommand.h"
#include "mcpserverlib_global.h"

class MCPSERVERLIB_EXPORT CanManager : public QObject {
    Q_OBJECT
public:
    explicit CanManager(QObject *parent = nullptr);

    QJsonObject listInterfaces(const QString &plugin);
    QJsonObject open(const QString &plugin, const QString &interfaceName, quint8 padByte, bool forceExtendedId,
                      bool extendedAddressing, quint8 addressExtension, bool fdMode, quint32 bitrate,
                      quint32 dataBitrate, bool bitrateSwitch);
    QJsonObject close();
    QJsonObject getReceivedFrames(int limit);
    // With extended addressing, targetAddress is the ECU's address byte for
    // this request (-1: the one can_open set), and a txId/rxId of -1 is
    // derived BMW D-CAN style: sent on 0x600 + tester address (0xF1, so
    // 0x6F1), answered on 0x600 + targetAddress.
    QJsonObject sendUdsRequest(qint64 txId, qint64 rxId, const QByteArray &payload, int timeoutMs,
                               int targetAddress = -1);
    QJsonObject testerPresentStart(qint64 id, int intervalMs, bool functional, bool suppressPositiveResponse,
                                   int targetAddress = -1);
    QJsonObject testerPresentStop(const QString &handle);
    // A snapshot of the connection and its running tester presents, safe to
    // call from any thread: it never waits for this manager's own thread,
    // which may be busy in a request.
    QJsonObject status() const;

signals:
    void frameAppended();

private slots:
    void onFramesReceived();

private:
    template<typename F>
    auto onOwnThread(F &&f) -> decltype(f()) {
        if (QThread::currentThread() == thread()) return f();
        decltype(f()) result{};
        QMetaObject::invokeMethod(this, [&]() { result = f(); }, Qt::BlockingQueuedConnection);
        return result;
    }

    QCanBusFrame waitForFrame(quint32 id, int timeoutMs, const std::function<bool(const QCanBusFrame &)> &pred = nullptr);
    bool sendIsoTp(quint32 txId, quint32 rxId, const QByteArray &payload, quint8 ext);
    QByteArray receiveIsoTp(quint32 txId, quint32 rxId, int timeoutMs, quint8 ext);
    void sendRaw(quint32 id, const QByteArray &data);

    QCanBusDevice *m_device = nullptr;
    quint8 m_padByte = 0x00;
    bool m_forceExtendedId = false;
    bool m_extendedAddressing = false;
    quint8 m_addressExtension = 0x00;
    bool m_fdMode = false;
    bool m_fdBitrateSwitch = false;
    QList<QCanBusFrame> m_buffer;
    QMap<QString, QTimer *> m_testerPresentTimers;
    int m_handleCounter = 0;

    void publishStatus(); // on this manager's thread, after every change
    // What the device last reported going wrong (errorOccurred), for the
    // status and for errors; fatal (the adapter stopped working): nothing
    // more is sent until it's opened again.
    QString m_fault;
    bool m_faultFatal = false;
    QString withFault(const QString &error) const;
    QString m_plugin, m_interface;
    quint32 m_bitrate = 0, m_dataBitrate = 0;
    QMap<QString, QJsonObject> m_testerPresentInfo; // handle -> what it keeps alive
    mutable QMutex m_statusMutex;
    QJsonObject m_status;
};

class MCPSERVERLIB_EXPORT CanCommandBase : public McpCommand {
public:
    explicit CanCommandBase(CanManager *manager) : m_manager(manager) {}
protected:
    CanManager *m_manager;
};

class MCPSERVERLIB_EXPORT CanListInterfacesCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT CanOpenCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT CanCloseCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT CanGetReceivedFramesCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT UdsSendRequestCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT UdsTesterPresentStartCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT UdsTesterPresentStopCommand : public CanCommandBase {
public:
    using CanCommandBase::CanCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

#endif
