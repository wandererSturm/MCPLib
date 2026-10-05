#ifndef UDSUTIL_H
#define UDSUTIL_H
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include "mcpserverlib_global.h"

MCPSERVERLIB_EXPORT QString udsHexBytes(const QByteArray &data);
MCPSERVERLIB_EXPORT QString udsNrcName(quint8 nrc);
MCPSERVERLIB_EXPORT quint32 udsParseId(const QJsonValue &v);
// UDS request bytes: hex text ("22 F1 90", "22F190", "0x22,0xF1") or an
// array of numbers 0-255 (or hex strings). Sets *error when they can't be
// read exactly - a byte is never guessed.
MCPSERVERLIB_EXPORT QByteArray udsParseBytes(const QJsonValue &v, QString *error);
MCPSERVERLIB_EXPORT QJsonObject udsTextResult(const QString &text, bool isError = false);

#endif
