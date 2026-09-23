#include "core/DataPaths.h"

#include <QDir>

namespace lycode {

QString dataDir() {
    const QString base = qEnvironmentVariable("LYCODE_DATA_BASE_DIR").trimmed();
    if (!base.isEmpty()) {
        return base;
    }
#ifdef Q_OS_WIN
    // Windows 上不用隐藏目录。
    return QDir::homePath() + QStringLiteral("/lycode");
#else
    return QDir::homePath() + QStringLiteral("/.cache/lycode");
#endif
}

}  // namespace lycode
