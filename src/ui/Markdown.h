// LyCode — Markdown 渲染
//
// 为什么不用 QTextDocument::setMarkdown()：它能解析 Markdown，但产出的结构
// 无法按设计规范定制（代码块需要带语言标签的卡片外壳、引用块需要左侧色条、
// 表格需要独立的边框与表头底色）。这里自己把 Markdown 转成带 class 的 HTML，
// 再用 QTextDocument 的 defaultStyleSheet 统一着色。
//
// 支持范围（覆盖模型输出的绝大多数情况）：
//   ATX 标题 h1-h6、段落、无序/有序列表（含单层嵌套）、围栏代码块（``` 与 ~~~）、
//   行内代码、粗体、斜体、删除线、链接、图片语法（降级为链接）、引用块、
//   水平线、管道表格、GFM 任务列表复选框。
//
// **不支持**：HTML 直通（出于安全考虑一律转义）、脚注、数学公式、
// 深层嵌套列表、引用内嵌代码块。这些都是有意取舍，不是遗漏。
#pragma once

#include <QColor>
#include <QList>
#include <QString>

namespace lycode::ui {

/// 渲染所需的样式令牌。全部来自 Theme，不在这里硬编码颜色。
struct MarkdownStyle {
    QColor foreground;
    QColor foregroundSubtle;
    QColor foregroundSubtlest;
    QColor surface;
    QColor border;
    QColor codeBackground;
    QColor link;
    QColor quoteBar;
    QColor tableHeader;

    // 语法高亮。放在 MarkdownStyle 而不是 Theme 的控件样式表里：
    // 富文本走的是 QTextDocument::setDefaultStyleSheet，控件样式表
    // （QWidget::setStyleSheet）**不会**作用到文档内容上——加错地方
    // 的结果是 token span 都生成了、却一个都没上色（实测踩到）。
    QColor syntaxKeyword;
    QColor syntaxString;
    QColor syntaxComment;
    QColor syntaxNumber;
    QColor syntaxType;
    QColor syntaxFunction;
    QColor syntaxPreproc;

    int baseFontPx = 14;
    int codeFontPx = 14;
    QString sansFamily;
    QString monoFamily;
};

class Markdown {
public:
    /// Markdown → HTML 片段。输出已做 HTML 转义，可安全放进 QTextDocument。
    static QString toHtml(const QString &markdown, const MarkdownStyle &style);

    /// QTextDocument 的默认样式表。设置后 `<pre class="code">` 等 class 才会生效。
    static QString styleSheet(const MarkdownStyle &style);

    /// 纯文本预览：去掉 Markdown 标记，用于会话列表的一行摘要。
    static QString toPlainPreview(const QString &markdown, int maxChars = 120);

    /// 一个可点选的选项。
    struct Choice {
        /// 按钮上显示的短标签。
        QString label;
        /// 点选后填进输入框的文本（通常就是选项原文）。
        QString text;
    };

    /// 从 markdown **尾部**识别"让用户在若干选项里选一个"的列表。
    ///
    /// 判定刻意保守：宁可不识别，也不要把正文里的普通编号列表变成一排按钮——
    /// 那会让正常的回答看起来像在要求用户做选择。全部条件都要满足：
    ///   * 尾部是一个 2–6 项的列表（有序 `1.` / `1)` / `1、`，或无序 `-` / `*`）
    ///   * 每项只有一行、且不超过 80 字符
    ///   * 列表**前面紧邻**一行是提问（以 ? / ？ 结尾，或含"选择/哪种/哪个/which/choose"）
    /// 不满足时返回空列表，调用方据此不显示任何按钮。
    static QList<Choice> detectChoices(const QString &markdown);
};

}  // namespace lycode::ui
