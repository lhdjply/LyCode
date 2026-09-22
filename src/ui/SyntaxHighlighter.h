// LyCode — 代码语法高亮
//
// 把一段代码转成带 `<span class="tok-*">` 的 HTML，供 Markdown 代码块与
// 只读代码视图使用。
//
// ── 为什么自己写而不用现成的高亮库 ──────────────────────────────────────────
// 环境里没有可用的高亮库（Qt 本身不带，也不引入额外的第三方依赖）。
// 而 AI 对话里出现的代码块以片段为主，需要的是"扫一眼能分清字符串/
// 注释/关键字"，不是逐像素还原某个编辑器的主题。所以做的是一个**单遍扫描的
// 轻量分词器**：够快、无依赖、行为可预测。
//
// ── 设计取舍 ────────────────────────────────────────────────────────────────
//   * 单遍字符扫描，不用正则回溯。正则版本在长行上容易出现灾难性回溯，
//     而代码块长度不可控（用户可能贴一个几千行的文件）。
//   * 字符串与注释**优先于**关键字识别：否则 `"for"` 里的 for 会被着色成
//     关键字，`// for` 里的 for 同理。这是最容易做错的地方。
//   * 识别失败就退回纯文本片段，绝不因为"看不懂"而丢字符——
//     高亮是锦上添花，内容完整性是底线。
//   * 语言只用于挑选关键字表与注释风格；未知语言按 C 系风格兜底。
#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>

namespace lycode::ui {

/// 语法高亮分词器。
/// 着色用的颜色。由调用方（Markdown / 代码视图）从主题取，
/// 分词器本身不依赖 Theme。
///
/// ⚠ 为什么是内联样式而不是 CSS 类：
/// Qt 的富文本 CSS（QTextDocument::setDefaultStyleSheet）对选择器的支持很窄。
/// 实测 `span.tok-keyword { color: … }` 和 `.tok-keyword { … }` 都**不生效**，
/// 而同文件里 `pre.code`、`div.code-block` 这类元素+类选择器是生效的——
/// 与其继续试探选择器形状，不如把颜色写进 span 的内联 style，行为确定。
/// class 仍然保留，供测试与将来的样式化使用。
struct SyntaxColors {
    QColor keyword;
    QColor string;
    QColor comment;
    QColor number;
    QColor type;
    QColor function;
    QColor preprocessor;
};

/// token 类别。渲染端据此挑颜色与字形。
enum class TokenKind {
    Plain,
    Keyword,
    String,
    Comment,
    Number,
    Type,
    Function,
    Preprocessor,
};

/// 一段被识别出来的 token（区间为**字符偏移**，不是字节）。
struct Token {
    int start = 0;
    int length = 0;
    TokenKind kind = TokenKind::Plain;
};

class SyntaxHighlighter {
public:
    /// 把代码切成 token 区间。**两个渲染端共用它**：
    /// 富文本走 HTML，QPlainTextEdit 走 QTextCharFormat。
    /// 共用同一份规则是刻意的——各写一份分词早晚会漂移。
    /// 只返回非 Plain 的区间（Plain 就是区间之间的空隙）。
    static QList<Token> tokenize(const QString &code, const QString &language);

    /// 按文件扩展名猜语言（`main.cpp` → `cpp`）。
    /// 猜不出时返回空串，调用方按通用规则处理。
    static QString languageForFile(const QString &path);

    /// 把 `code` 渲染成 HTML 片段（不含 <pre> 包裹）。
    /// 所有文本都会被 HTML 转义；`language` 为空或未知时用通用规则。
    static QString highlight(const QString &code, const QString &language,
                             const SyntaxColors &colors);
    /// 只要 token 结构、不要颜色时用（返回的 span 不带内联样式）。
    static QString highlight(const QString &code, const QString &language);

    /// 语言是否被专门支持（供 UI 决定要不要显示语言标签）。
    static bool isKnownLanguage(const QString &language);

    /// 把语言标识归一化到内部表（`c++`/`cpp`/`cc` → `cpp` 等）。
    /// 未知语言原样返回（小写、去空白）。
    static QString normalizeLanguage(const QString &language);

    /// 已知语言的列表（用于测试与文档）。
    static QStringList knownLanguages();
};

}  // namespace lycode::ui
