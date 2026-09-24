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

namespace lycode::ui
{

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

class Markdown
{
  public:
    /// Markdown → HTML 片段。输出已做 HTML 转义，可安全放进 QTextDocument。
    static QString toHtml(const QString & markdown, const MarkdownStyle & style);

    /// QTextDocument 的默认样式表。设置后 `<pre class="code">` 等 class 才会生效。
    static QString styleSheet(const MarkdownStyle & style);

    /// 纯文本预览：去掉 Markdown 标记，用于会话列表的一行摘要。
    static QString toPlainPreview(const QString & markdown, int maxChars = 120);

    /// 卡片里的一个选项。
    struct ChoiceOption {
      /// 选项标题，显示成卡片里的主行。
      QString label;
      /// 灰字说明，可空。
      QString description;
      /// 是否显示推荐徽标——由标签末尾的 `(推荐)` / `(Recommended)` 标记。
      bool recommended = false;
      /// 选中后作为回答发出的文本。
      QString value;
    };

    /// 一道要用户回答的问题，对应卡片的一页。
    struct ChoiceQuestion {
      /// 眉标（卡片头部的浅色小字），可空。
      QString header;
      /// 问题标题，可空。
      QString question;
      QList<ChoiceOption> options;
    };

    /// 从 markdown **尾部**识别要用户做选择的内容，供问询卡片渲染。
    ///
    /// 判定刻意保守：宁可不识别，也不要把正文里的普通编号列表变成一张卡片——
    /// 那会让正常的回答看起来像在要求用户做选择。只认 info 为 `choices` 的围栏块，
    /// 块内语法在系统提示词里写明（改这里就要同步改 SystemPrompt）：
    ///
    ///   `# 眉标`          当前问题的眉标（可省）
    ///   `? 问题`          开一道新问题（可省；省了就是"只有选项没有问题"）
    ///   `- 标题 :: 说明`  一个选项，` :: ` 之后是灰字说明（可省）
    ///   `- 标题 (推荐)`   末尾的 `(推荐)` / `(Recommended)` 变成推荐徽标
    ///
    /// 每道问题 2–6 个选项、标签不超过 80 字符；任何一项不合规就丢掉**那道问题**，
    /// 不做"尽力而为"的截取。全都拿不到时返回空列表，调用方据此不显示卡片。
    static QList<ChoiceQuestion> detectQuestions(const QString & markdown);
};

}  // namespace lycode::ui
