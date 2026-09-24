// LyCode — 界面翻译
//
// 只用 Qt 的标准做法：源码里的**英文**是源文案，tr() 把它标记为可翻译；
// lupdate 抽出 translations/lycode_zh_CN.ts，lrelease 编译成 .qm，
// 构建时通过 qt_add_translations 嵌进资源（:/i18n/lycode_zh_CN.qm），
// 运行时由 QTranslator 装载。
//
// ── 为什么源文案是英文 ──────────────────────────────────────────────────
// 英文是软件界面的通用源语言：第三方贡献者不必先读中文才能改界面，与 Qt 生态的
// 惯例也一致。代价是**中文界面依赖 zh_CN.qm**——它必须译全，缺一条就会回退成
// 英文。所以有一条测试专门断言"中文界面下界面文案仍是中文"（见 test_translator），
// 避免哪天翻译文件构建漏了却没人发现。
//
// ── 与"给模型看的文本"的边界（**必须**守住）──────────────────────────────
// 本模块只服务于**用户可见**的界面。系统提示词（SystemPromptBuilder）、工具声明与
// 参数描述（ToolSpec::description / inputSchema）不是界面文案：它们是要发给模型的
// 指令，翻译会改变模型行为，也拿不到任何本地化收益。所以那些字符串**不要**用
// tr()——它们保持源码里的字面量（英文），并且有脚本钉住这一点
//（scripts/check-translations.py 会拒绝把已知的模型文案放进 .ts）。
#pragma once

#include <QString>

class QTranslator;

namespace lycode::ui
{

/// 界面语言。取值与 AppSettings::language 一致。
enum class UiLanguage {
  Chinese,   ///< zh-CN（需要 zh_CN.qm）
  English,   ///< en-US（源文案，无需翻译文件）
};

/// 语言取值串（`zh-CN` / `en-US`）。
QString toToken(UiLanguage language);
/// 解析语言取值；未知值回退到英文（源文案），保证界面至少是可读的英文，
/// 而不是一堆未翻译的键。
UiLanguage languageFromToken(const QString & token);

/// 本机语言偏好对应的界面语言。仅用于"用户还没选过语言"时的初值。
UiLanguage systemLanguage();

/// 安装 .qm 到 QApplication，并在语言变化时 qApp->installTranslator()。
///
/// 全局单例（与 Theme 同样式）：语言是进程级状态，一个窗口换语言要同时影响
/// 所有窗口。构造后语言是英文（源文案，不装 translator），需要显式 switchTo()。
class Translator
{
  public:
    static Translator & instance();

    Translator(const Translator &) = delete;
    Translator & operator=(const Translator &) = delete;

    /// 切换界面语言。返回是否发生了变化（用于决定要不要重译界面）。
    /// 目标语言的 .qm 缺失时返回 false 且保持原语言——不静默变成另一种语言。
    bool switchTo(UiLanguage language);

    /// 当前语言。
    UiLanguage language() const
    {
      return language_;
    }

    /// 该语言的 .qm 是否可用（资源里有没有）。源语言（英文）恒为 true。
    /// 缺失时设置页应把这一项标出来，而不是让用户选完发现界面没变。
    static bool isTranslationAvailable(UiLanguage language);

  private:
    Translator();
    ~Translator();

    QTranslator * translator_ = nullptr;
    UiLanguage language_ = UiLanguage::English;
};

}  // namespace lycode::ui
