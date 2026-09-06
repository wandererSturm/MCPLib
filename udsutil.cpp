#include "udsutil.h"
#include <QJsonArray>

QString udsHexBytes(const QByteArray &data) {
    QStringList parts;
    for (unsigned char b : data) parts << QString("%1").arg(b, 2, 16, QChar('0'));
    return parts.join(' ');
}

QString udsNrcName(quint8 nrc) {
    switch (nrc) {
        case 0x10: return "generalReject";
        case 0x11: return "serviceNotSupported";
        case 0x12: return "subFunctionNotSupported";
        case 0x13: return "incorrectMessageLengthOrInvalidFormat";
        case 0x22: return "conditionsNotCorrect";
        case 0x24: return "requestSequenceError";
        case 0x31: return "requestOutOfRange";
        case 0x33: return "securityAccessDenied";
        case 0x35: return "invalidKey";
        case 0x36: return "exceedNumberOfAttempts";
        case 0x37: return "requiredTimeDelayNotExpired";
        case 0x78: return "responsePending";
        default: return "unknown";
    }
}

quint32 udsParseId(const QJsonValue &v) {
    if (v.isString()) {
        QString s = v.toString();
        bool hex = s.startsWith("0x") || s.startsWith("0X");
        return s.toUInt(nullptr, hex ? 16 : 10);
    }
    return static_cast<quint32>(v.toInt());
}

QJsonObject udsTextResult(const QString &text, bool isError) {
    QJsonObject obj{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    if (isError) obj["isError"] = true;
    return obj;
}
