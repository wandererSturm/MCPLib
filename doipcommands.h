#ifndef DOIPCOMMANDS_H
#define DOIPCOMMANDS_H
#include <QObject>
#include <QTcpSocket>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QTimer>
#include <QThread>
#include <functional>
#include "mcpcommand.h"
#include "mcpserverlib_global.h"

class MCPSERVERLIB_EXPORT DoipManager : public QObject {
    Q_OBJECT
public:
    explicit DoipManager(QObject *parent = nullptr);

    QJsonObject open(const QString &host, quint16 port, quint16 sourceAddress, quint8 activationType, int timeoutMs,
                     quint8 protocolVersion = 0x02);
    QJsonObject discoverVehicles(const QString &broadcastAddress, int timeoutMs);

    // Optional DoIP UDP status services (ISO 13400-2): query the entity's node
    // type / socket counts, and its diagnostic power-mode readiness.
    QJsonObject entityStatus(const QString &host, quint16 port, int timeoutMs);
    QJsonObject powerMode(const QString &host, quint16 port, int timeoutMs);

    // Parse helpers (static + pure, testable without a socket). Empty object if
    // the payload is too short.
    static QJsonObject parseVehicleAnnouncement(const QByteArray &payload); // 0x0004
    static QJsonObject parseEntityStatus(const QByteArray &payload);        // 0x4002
    static QJsonObject parsePowerMode(const QByteArray &payload);           // 0x4004
    QJsonObject close();
    QJsonObject getReceivedMessages(int limit);
    QJsonObject sendUdsRequest(quint16 targetAddress, const QByteArray &payload, int timeoutMs);
    QJsonObject testerPresentStart(quint16 targetAddress, int intervalMs, bool suppressPositiveResponse);
    QJsonObject testerPresentStop(const QString &handle);
    // A snapshot of the connection and its running tester presents, safe to
    // call from any thread: it never waits for this manager's own thread,
    // which may be busy in a request.
    QJsonObject status() const;

signals:
    void messageAppended();

private slots:
    void onReadyRead();

private:
    struct DoipMsg { quint16 type; QByteArray payload; };

    template<typename F>
    auto onOwnThread(F &&f) -> decltype(f()) {
        if (QThread::currentThread() == thread()) return f();
        decltype(f()) result{};
        QMetaObject::invokeMethod(this, [&]() { result = f(); }, Qt::BlockingQueuedConnection);
        return result;
    }

    void sendMessage(quint16 type, const QByteArray &payload);
    DoipMsg waitForMessage(int timeoutMs, const std::function<bool(const DoipMsg &)> &pred = nullptr);
    // One UDP request/response exchange with an entity (discovery, status, power mode).
    QByteArray udpExchange(const QString &host, quint16 port, quint16 reqType, const QByteArray &reqPayload,
                           quint16 respType, int timeoutMs, QString *error);

    QTcpSocket *m_socket = nullptr;
    QByteArray m_recvBuffer;
    QList<DoipMsg> m_buffer;
    quint16 m_sourceAddress = 0x0E00;
    quint8 m_protocolVersion = 0x02; // ISO 13400-2:2012; configurable via open()
    QMap<QString, QTimer *> m_testerPresentTimers;
    int m_handleCounter = 0;

    void publishStatus(); // on this manager's thread, after every change
    QString m_host;
    quint16 m_port = 0;
    QMap<QString, QJsonObject> m_testerPresentInfo; // handle -> what it keeps alive
    mutable QMutex m_statusMutex;
    QJsonObject m_status;
};

class MCPSERVERLIB_EXPORT DoipCommandBase : public McpCommand {
public:
    explicit DoipCommandBase(DoipManager *manager) : m_manager(manager) {}
protected:
    DoipManager *m_manager;
};

class MCPSERVERLIB_EXPORT DoipDiscoverVehiclesCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipOpenCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipCloseCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipGetReceivedMessagesCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipSendRequestCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipTesterPresentStartCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipTesterPresentStopCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipEntityStatusCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

class MCPSERVERLIB_EXPORT DoipPowerModeCommand : public DoipCommandBase {
public:
    using DoipCommandBase::DoipCommandBase;
    QJsonObject definition() const override;
    QJsonObject execute(const QJsonObject &args) override;
};

#endif
