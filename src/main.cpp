// LyCode — 应用入口
//
// 只做进程级初始化（应用身份、样式、日志、高 DPI），随后交给 MainWindow。
// 业务组装在 MainWindow 里，入口保持无逻辑，便于将来加命令行参数或
// 无头模式时不必改动业务代码。
#include <QApplication>
#include <QIcon>
#include <QFont>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QStyleFactory>

#include "core/Logging.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

int main(int argc, char * argv[])
{
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

  // 应用图标：任务栏 / dock / alt-tab 用的就是它。
  // 八档尺寸都加进去——只给一张 512 的位图，任务栏的 16px 那一档就得靠
  // 系统去缩，小尺寸会发虚。
  {
    QIcon appIcon;
#ifdef Q_OS_WIN
    // Windows 用 .ico：它本身就是多尺寸容器（16–256，实测 Qt 的 ICO 插件
    // 会把七档都报给 QIcon），系统也认这个格式。而且它正是编进 exe 资源的
    // 那一份——运行中的窗口与文件图标同源，不会出现两套图像不一致。
    // 512 那一档 Windows 用不到（任务栏/alt-tab/资源管理器都到 256 为止）。
    appIcon.addFile(QStringLiteral(":/icons/windows/lycode.ico"));
#else
    // 其他平台用 PNG：资源路径与安装树一致，所以按尺寸直接拼，不需要别名。
    for(const int size : {
          16, 24, 32, 48, 64, 128, 256, 512
        }) {
      appIcon.addFile(QStringLiteral(":/icons/linux/hicolor/%1x%1/apps/lycode.png")
                      .arg(size));
    }
#endif
    QApplication::setWindowIcon(appIcon);
  }
  // 让窗口与桌面条目关联起来：Linux 上任务栏据此把运行中的窗口
  // 对上 .desktop 文件（装完之后图标才不会显示成默认的空白方块）。
  QApplication::setDesktopFileName(QStringLiteral("lycode"));

  const QStringList arguments = app.arguments();
  const bool verbose = arguments.contains(QStringLiteral("--verbose"));
  lycode::logging::init(verbose);

  // 用 Fusion 作为样式基底：原生样式（如 Linux 上的 gtk/dde）会忽略
  // 我们生成的部分 QSS 规则，导致主题令牌在个别控件上失效。
  // Fusion 对 QSS 的支持最完整，跨平台表现也最一致。
  if(QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  }

  // 主题需要在 QApplication 建立之后才能解析系统配色。
  lycode::ui::Theme & theme = lycode::ui::Theme::instance();
  QApplication::setFont(theme.font(lycode::ui::FontRole::UiBase));

  lycode::ui::MainWindow window;
  window.show();

  return app.exec();
}
