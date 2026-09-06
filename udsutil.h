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
MCPSERVERLIB_EXPORT QJsonObject udsTextResult(const QString &text, bool isError = false);

#endif
