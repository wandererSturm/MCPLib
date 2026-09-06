#ifndef HSFZCOMMANDS_H
#define HSFZCOMMANDS_H
#include <QObject>
#include <QTcpSocket>
#include <QMap>
#include <QTimer>
#include <QThread>
#include <QStringList>
#include <functional>
#include "mcpcommand.h"
#include "mcpserverlib_global.h"

class MCPSERVERLIB_EXPORT HsfzManager : public QObject {
    Q_OBJECT
public:
    explicit HsfzManager(QObject *parent = nullptr);

    QJsonObject open(const QString &host, quint16 port, quint8 sourceAddress, int timeoutMs);
    QJsonObject discoverVehicles(const QStringList &hosts, quint16 port, int timeoutMs);
    QJsonObject close();
    QJsonObject getReceivedMessages(int limit);
    QJsonObject sendUdsRequest(quint8 targetAddress, const QByteArray &payload, int timeoutMs);
    QJsonObject testerPresentStart(quint8 targetAddress, int intervalMs, bool suppressPositiveResponse);
    QJsonObject testerPresentStop(const QString &handle);

signals:
    void messageAppended();

private slots:
    void onReadyRead();

private:
    struct HsfzMsg { quint8 source; quint8 target; quint8 type; QByteArray payload; };

    template<typename F>
    auto onOwnThread(F &&f) -> decltype(f()) {
        if (QThread::currentThread() == thread()) return f();
        decltype(f()) result{};
        QMetaObject::invokeMethod(this, [&]() { result = f(); }, Qt::BlockingQueuedConnection);
        return result;
    }

    void sendMessage(quint8 target, quint8 type, const QByteArray &payload);
    HsfzMsg waitForMessage(int timeoutMs, const std::function<bool(const HsfzMsg &)> &pred = nullptr);

    QTcpSocket *m_socket = nullptr;
    QByteArray m_recvBuffer;
    QList<HsfzMsg> m_buffer;
    quint8 m_sourceAddress = 0xF4;
    QMap<QString, QTimer *> m_testerPresentTimers;
    int m_handleCounter = 0;
};

class MCPSERVERLIB_EXPORT HsfzCommandBase : public McpCommand {
public:
    explicit HsfzCommandBase(HsfzManager *manager) : m_manager(manager) {}
protected:
    HsfzManager *m_manager;
};

class MCPSERVERLIB_EXPORT HsfzDiscoverVehiclesCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzOpenCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzCloseCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzGetReceivedMessagesCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzSendRequestCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzTesterPresentStartCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT HsfzTesterPresentStopCommand : public HsfzCommandBase {
public:
    using HsfzCommandBase::HsfzCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

#endif
