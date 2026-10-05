#include "udsutil.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

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

static bool hexByte(QString s, quint8 *out) {
    s = s.trimmed();
    if (s.startsWith("0x", Qt::CaseInsensitive)) s = s.mid(2);
    bool ok = false;
    const uint v = s.toUInt(&ok, 16);
    if (!ok || s.isEmpty() || v > 0xFF) return false;
    *out = quint8(v);
    return true;
}

QByteArray udsParseBytes(const QJsonValue &v, QString *error) {
    QByteArray out;
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) {
            const QJsonValue b = a.at(i);
            quint8 byte = 0;
            if (b.isDouble() && b.toDouble() >= 0 && b.toDouble() <= 255 && b.toDouble() == double(int(b.toDouble())))
                byte = quint8(b.toInt());
            else if (!(b.isString() && hexByte(b.toString(), &byte))) {
                *error = QString("data[%1] isn't a byte (0-255, or hex like \"0x22\")").arg(i);
                return {};
            }
            out.append(char(byte));
        }
    } else if (v.isString()) {
        QStringList tokens = v.toString().trimmed().split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts);
        if (tokens.size() == 1) {
            QString packed = tokens.first();
            if (packed.startsWith("0x", Qt::CaseInsensitive)) packed = packed.mid(2);
            if (packed.size() > 2) {
                if (packed.size() % 2) {
                    *error = "data has an odd number of hex digits";
                    return {};
                }
                tokens.clear();
                for (int i = 0; i < packed.size(); i += 2) tokens << packed.mid(i, 2);
            }
        }
        for (const QString &t : tokens) {
            quint8 byte = 0;
            if (!hexByte(t, &byte)) {
                *error = QString("data: \"%1\" isn't a hex byte").arg(t);
                return {};
            }
            out.append(char(byte));
        }
    }
    if (out.isEmpty() && error->isEmpty())
        *error = "data is empty: give the request bytes, e.g. \"22 F1 90\"";
    return out;
}

QJsonObject udsTextResult(const QString &text, bool isError) {
    QJsonObject obj{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    if (isError) obj["isError"] = true;
    return obj;
}
