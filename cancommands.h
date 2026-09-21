#ifndef CANCOMMANDS_H
#define CANCOMMANDS_H
#include <QObject>
#include <QCanBusDevice>
#include <QCanBusFrame>
#include <QMap>
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
    QJsonObject sendUdsRequest(quint32 txId, quint32 rxId, const QByteArray &payload, int timeoutMs);
    QJsonObject testerPresentStart(quint32 id, int intervalMs, bool functional, bool suppressPositiveResponse);
    QJsonObject testerPresentStop(const QString &handle);

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
    bool sendIsoTp(quint32 txId, quint32 rxId, const QByteArray &payload);
    QByteArray receiveIsoTp(quint32 txId, quint32 rxId, int timeoutMs);
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
