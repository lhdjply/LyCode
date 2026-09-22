// LyCode — 应用入口
//
// 只做进程级初始化（应用身份、样式、日志、高 DPI），随后交给 MainWindow。
// 业务组装在 MainWindow 里，入口保持无逻辑，便于将来加命令行参数或
// 无头模式时不必改动业务代码。
#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QStyleFactory>

#include "core/Logging.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

int main(int argc, char *argv[]) {
    // 高分屏缩放策略必须在 QApplication 构造之前设置，否则 Qt 会告警并沿用默认。
    // PassThrough 让非整数缩放因子（如 125%）如实生效，避免界面被取整后忽大忽小。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // 应用身份必须在任何 QSettings / QStandardPaths 调用之前设置，
    // 否则配置会落到错误的组织目录下。
    QCoreApplication::setOrganizationName(QStringLiteral("LyCode"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("lycode.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("LyCode"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LYCODE_QT_VERSION));

    const QStringList arguments = app.arguments();
    const bool verbose = arguments.contains(QStringLiteral("--verbose"));
    lycode::logging::init(verbose);

    // 用 Fusion 作为样式基底：原生样式（如 Linux 上的 gtk/dde）会忽略
    // 我们生成的部分 QSS 规则，导致主题令牌在个别控件上失效。
    // Fusion 对 QSS 的支持最完整，跨平台表现也最一致。
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    // 主题需要在 QApplication 建立之后才能解析系统配色。
    lycode::ui::Theme &theme = lycode::ui::Theme::instance();
    QApplication::setFont(theme.font(lycode::ui::FontRole::UiBase));

    lycode::ui::MainWindow window;
    window.show();

    return app.exec();
}
