#ifndef GSUSBHEALTH_H
#define GSUSBHEALTH_H
#include <QByteArray>
#include <QString>
#include <QVector>
#include "mcpserverlib_global.h"

// What a gs_usb adapter's TX echoes and CAN error frames say about whether
// frames really reach the bus. A gs_usb device sends every frame it put on
// the bus back to the host under the echo_id it was given; one it can't
// send (nobody acknowledges it, the controller is bus-off or stuck) never
// comes back. Pure bookkeeping, no USB: GsUsbCanBusDevice's I/O thread
// drives it (and tests do).
class MCPSERVERLIB_EXPORT GsUsbHealth {
public:
    static constexpr int kMaxInFlight = 10;         // echo slots, as Linux's gs_usb driver keeps
    static constexpr int kConfirmTimeoutMs = 1000;  // unconfirmed this long: it isn't getting out

    // The echo_id to send the next frame under, or -1: every slot is taken,
    // hold the frame back until an echo frees one.
    int takeEchoId(qint64 nowMs);
    void giveBack(quint32 echoId); // the frame never went to the device (its USB write failed)
    // A TX confirmation came back. False: not one of ours.
    bool echoed(quint32 echoId);
    int inFlight() const;
    // The oldest unconfirmed frame has waited longer than kConfirmTimeoutMs.
    bool stalled(qint64 nowMs) const;
    void reset(); // the controller was restarted: unconfirmed frames are gone

    // An error frame (can_id carries CAN_ERR_FLAG): what it says.
    struct ErrorReport {
        bool busOff = false;
        bool noAck = false;       // nobody acknowledged a frame
        bool restarted = false;   // the controller came back from bus-off
        bool passive = false;     // error-passive (or warning): trouble on the bus
        QString text;             // for people (and the model), "" when nothing worth saying
    };
    static bool isErrorFrame(quint32 canId);
    static ErrorReport describeErrorFrame(quint32 canId, const QByteArray &data);

private:
    QVector<qint64> m_sentAt = QVector<qint64>(kMaxInFlight, -1); // per slot; -1 = free
};

#endif
