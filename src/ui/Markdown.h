// ZCode Qt — Markdown 渲染
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
#include <QString>

namespace zcode::ui {

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
};

}  // namespace zcode::ui
