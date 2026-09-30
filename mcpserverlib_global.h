#ifndef MCPSERVERLIB_GLOBAL_H
#define MCPSERVERLIB_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(MCPSERVERLIB_STATIC) // mcpcommands: a static library, nothing to export
#define MCPSERVERLIB_EXPORT
#elif defined(MCPSERVERLIB_LIBRARY)
#define MCPSERVERLIB_EXPORT Q_DECL_EXPORT
#else
#define MCPSERVERLIB_EXPORT Q_DECL_IMPORT
#endif

#endif // MCPSERVERLIB_GLOBAL_H
