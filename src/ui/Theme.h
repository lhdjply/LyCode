// LyCode — 设计令牌与主题
//
// 令牌以 CSS 变量名与十六进制值固化在 Theme.cpp，字段与变量一一对应。
// 设计原则：
//   * 只用语义令牌，不用一次性硬编码颜色
//   * 层次优先靠背景对比与边框，而不是重阴影
//   * 界面字号走 text-ui-* 阶梯，默认 --ui-font-size = 14px
//   * 代码 / Diff / 终端使用各自独立的等宽字号，不随界面字号缩放
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QString>

namespace lycode::ui
{

/// 主题模式。`System` 跟随系统配色。
enum class ThemeMode {
  System,
  Light,
  Dark,
};

QString toToken(ThemeMode mode);
ThemeMode themeModeFromToken(const QString & value);

/// 界面字号阶梯（对应 text-ui-*）。
enum class FontRole {
  UiXl,       ///< --ui-font-size + 4px，Markdown h1
  UiLg,       ///< +2px，Markdown h2
  UiBase,     ///< 基准，正文与常用控件
  UiCaption,  ///< -1px
  UiSm,       ///< -2px，次要说明、行内代码
  UiXs,       ///< -4px，徽章、极弱元数据
  Mono,       ///< 代码 / Diff / 终端等宽内容
  MonoSm,     ///< 紧凑等宽（工具输出内联）
};

/// 语义色令牌集合。字段名与 CSS 变量一一对应。
struct Palette {
  // 结构表面
  QColor background;
  QColor backgroundAlt;
  QColor header;
  QColor panel;
  QColor sidebar;
  QColor surface;
  QColor surfaceHover;
  QColor card;
  QColor cardSelected;
  QColor popover;
  QColor popoverBorder;
  QColor menu;
  QColor menuHover;

  // 输入
  QColor input;
  QColor inputFocused;
  QColor inputBorder;
  QColor inputBorderHover;
  QColor inputBorderFocused;

  // 边框
  QColor border;
  QColor borderHover;
  QColor cardBorder;

  // 文本
  QColor foreground;
  QColor foregroundSubtle;
  QColor foregroundSubtlest;
  QColor foregroundInverse;

  // 品牌与交互
  QColor brand;
  QColor accent;
  QColor iconBlue;
  QColor primary;
  QColor primaryForeground;
  QColor secondary;
  QColor hover;
  QColor selected;

  // 语义反馈
  QColor success;
  QColor successForeground;
  QColor warning;
  QColor warningForeground;
  QColor destructive;
  QColor destructiveForeground;
  QColor idleTask;
  QColor idleTaskSurface;

  // Diff
  QColor diffAdded;
  QColor diffAddedForeground;
  QColor diffRemoved;
  QColor diffRemovedForeground;

  // 语法高亮
  //
  // ⚠ 这组**不是** 本实现的设计令牌：本实现的 UI 变量里没有语法配色
  // （Diff 那组是唯一的例外，所以上面单独列了）。这里是编辑器风格的
  // 常用取值（One Light / One Dark 一族），挑选标准是与本主题的
  // 代码背景对比度足够、且彼此色相可区分。
  QColor syntaxKeyword;
  QColor syntaxString;
  QColor syntaxComment;
  QColor syntaxNumber;
  QColor syntaxType;
  QColor syntaxFunction;
  QColor syntaxPreproc;

  // 浮层
  QColor toast;
  QColor tooltip;
  QColor tooltipForeground;
  QColor tag;

  // 等待交互（权限 / 提问 / 计划确认统一用同一套绿色）
  QColor interactionConfirmationSurface;
  QColor interactionConfirmationForeground;

  /// 是否为深色主题；供需要走系统图标/阴影的地方判断。
  bool isDark = false;
};

/// 主题单例。进程内唯一，UI 构造时从这里取色与字号。
class Theme : public QObject
{
    Q_OBJECT

  public:
    static Theme & instance();

    ThemeMode mode() const
    {
      return mode_;
    }
    /// 设置主题模式；解析后的实际明暗写入 palette()。
    void setMode(ThemeMode mode);
    /// System 模式下系统配色变化时调用。
    void refreshFromSystem();

    /// 当前生效的调色板（System 已解析为具体明暗）。
    const Palette & palette() const
    {
      return palette_;
    }
    /// 解析 System 之后实际使用的明暗。
    bool isDark() const
    {
      return palette_.isDark;
    }

    /// 界面基准字号（对应 --ui-font-size），默认 14。
    int uiFontSize() const
    {
      return uiFontSize_;
    }
    void setUiFontSize(int pixels);

    /// 代码 / Diff / 终端字号，独立于界面字号。
    int codeFontSize() const
    {
      return codeFontSize_;
    }
    void setCodeFontSize(int pixels);

    /// 按角色取字体。等宽角色使用等宽族。
    QFont font(FontRole role) const;
    /// 按角色取像素字号。
    int fontPixelSize(FontRole role) const;

    /// 全局样式表。由应用在主题变化时重新应用到 QApplication。
    QString styleSheet() const;

    /// 供 QSS 使用的十六进制颜色（含 # 前缀）。
    static QString css(const QColor & color);
    /// 带 alpha 的颜色，alpha 取值 0.0-1.0。
    static QString css(const QColor & color, double alpha);

  signals:
    /// 主题（明暗或字号）发生实际变化后发出。
    void changed();

  private:
    Theme();
    Q_DISABLE_COPY_MOVE(Theme)

    /// 根据 mode_ 与系统配色解析出 palette_。
    void resolvePalette();

    ThemeMode mode_ = ThemeMode::System;
    Palette palette_;
    int uiFontSize_ = 14;
    int codeFontSize_ = 14;

    /// 系统当前是否为深色。仅在 GUI 线程读 QStyleHints，故缓存一份。
    bool systemDark_ = false;
};

/// 等宽字体族名（首选可用者）。用于代码块与终端。
QString monospaceFamily();
/// 界面字体族名。
QString sansFamily();

}  // namespace lycode::ui
