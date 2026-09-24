// LyCode — Markdown 渲染单元测试
//
// 渲染器直接产出 HTML，任何一处转义遗漏都是安全问题（模型输出不可信），
// 任何一处规则顺序错误都会破坏代码块的显示。因此两个方向都要覆盖。
#include <QtTest>

#include "ui/Markdown.h"

using namespace lycode::ui;

namespace
{

MarkdownStyle testStyle()
{
  MarkdownStyle style;
  style.foreground = QColor(QStringLiteral("#262626"));
  style.foregroundSubtle = QColor(QStringLiteral("#737373"));
  style.foregroundSubtlest = QColor(QStringLiteral("#a3a3a3"));
  style.surface = QColor(QStringLiteral("#f5f5f5"));
  style.border = QColor(QStringLiteral("#e5e5e5"));
  style.codeBackground = QColor(QStringLiteral("#f5f5f5"));
  style.link = QColor(QStringLiteral("#001d3d"));
  style.quoteBar = QColor(QStringLiteral("#d4d4d4"));
  style.tableHeader = QColor(QStringLiteral("#f5f5f5"));
  style.baseFontPx = 14;
  style.codeFontPx = 14;
  style.sansFamily = QStringLiteral("Noto Sans CJK SC");
  style.monoFamily = QStringLiteral("Noto Sans Mono CJK SC");
  return style;
}

}  // namespace

class TestMarkdown : public QObject
{
    Q_OBJECT

  private slots:
    void escapesHtmlInText();
    void escapesHtmlInsideCodeBlock();
    void rejectsUnsafeLinkSchemes();
    void keepsSafeLinks();
    void rendersHeadings();
    void rendersFencedCodeBlockWithLanguage();
    void rendersIndentedFenceWithTildes();
    void inlineCodeSurvivesEmphasisRules();
    void detectsChoicesOnlyFromTheUnifiedFormat();
    void parsesRichQuestionsWithHeaderDescriptionAndRecommendation();
    void rendersEmphasis();
    void rendersLists();
    void rendersTaskList();
    void rendersBlockquote();
    void rendersHorizontalRule();
    void rendersPipeTable();
    void rendersNestedList();
    void styleSheetHasNoPlaceholders();
    void plainPreviewStripsMarkup();
};

void TestMarkdown::escapesHtmlInText()
{
  // 模型输出不可信：标签必须被转义而不是直通。
  const QString html = Markdown::toHtml(
                         QStringLiteral("hello <script>alert(1)</script> & \"quoted\""), testStyle());

  QVERIFY(!html.contains(QStringLiteral("<script>")));
  QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
  QVERIFY(html.contains(QStringLiteral("&amp;")));
  QVERIFY(html.contains(QStringLiteral("&quot;")));
}

void TestMarkdown::escapesHtmlInsideCodeBlock()
{
  const QString html =
    Markdown::toHtml(QStringLiteral("```html\n<b>bold</b>\n```"), testStyle());

  QVERIFY(html.contains(QStringLiteral("code-block")));
  QVERIFY(html.contains(QStringLiteral("&lt;b&gt;bold&lt;/b&gt;")));
  QVERIFY(!html.contains(QStringLiteral("<b>bold</b>")));
}

void TestMarkdown::rejectsUnsafeLinkSchemes()
{
  // javascript: 链接必须降级为纯文本。
  const QString html =
    Markdown::toHtml(QStringLiteral("[click](javascript:alert(1))"), testStyle());

  QVERIFY(!html.contains(QStringLiteral("javascript:")));
  QVERIFY(html.contains(QStringLiteral("click")));
}

void TestMarkdown::keepsSafeLinks()
{
  const QString html =
    Markdown::toHtml(QStringLiteral("[docs](https://example.com/a)"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<a href=\"https://example.com/a\">docs</a>")));
}

void TestMarkdown::rendersHeadings()
{
  const QString html = Markdown::toHtml(
                         QStringLiteral("# Title\n\n## Sub\n\n### Third\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<h1>Title</h1>")));
  QVERIFY(html.contains(QStringLiteral("<h2>Sub</h2>")));
  QVERIFY(html.contains(QStringLiteral("<h3>Third</h3>")));
}

void TestMarkdown::rendersFencedCodeBlockWithLanguage()
{
  const QString html = Markdown::toHtml(
                         QStringLiteral("before\n\n```cpp\nint main() {}\n```\n\nafter"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<div class=\"code-lang\">cpp</div>")));
  QVERIFY(html.contains(QStringLiteral("<pre class=\"code\">")));
  // 代码块现在带语法着色，所以正文不再以原始文本出现——断言的是
  // "内容还在 + 着色生效"，而不是"一字不改地原样输出"。
  QVERIFY2(html.contains(QStringLiteral("int")), "关键字/类型必须仍然可见");
  QVERIFY(html.contains(QStringLiteral("main")));
  QVERIFY(html.contains(QStringLiteral("{}")));
  QVERIFY2(html.contains(QStringLiteral("class=\"tok-")),
           "代码块必须产生至少一个着色 span");
  // 代码块前后必须是独立段落，不能被并进代码块。
  QVERIFY(html.contains(QStringLiteral("<p>before</p>")));
  QVERIFY(html.contains(QStringLiteral("<p>after</p>")));
}

void TestMarkdown::rendersIndentedFenceWithTildes()
{
  const QString html =
    Markdown::toHtml(QStringLiteral("  ~~~python\nprint(1)\n  ~~~\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<div class=\"code-lang\">python</div>")));
  // print 是函数调用，会被包进 tok-function；参数保持可见。
  QVERIFY2(html.contains(QStringLiteral("print")), qPrintable(html));
  // 括号是普通文本、1 是数字 span，所以不连续——分别断言。
  QVERIFY(html.contains(QStringLiteral("(")));
  QVERIFY2(html.contains(QStringLiteral("<span class=\"tok-number\">1</span>")),
           qPrintable(html));
  QVERIFY2(html.contains(QStringLiteral("<span class=\"tok-function\">print</span>")),
           qPrintable(html));
}

void TestMarkdown::inlineCodeSurvivesEmphasisRules()
{
  // 行内代码里的 * 不能被当成强调标记——这是规则顺序的经典陷阱。
  const QString html = Markdown::toHtml(
                         QStringLiteral("use `a * b * c` here"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<code class=\"inline\">a * b * c</code>")));
  QVERIFY(!html.contains(QStringLiteral("<i>")));
}

void TestMarkdown::rendersEmphasis()
{
  const QString html = Markdown::toHtml(
                         QStringLiteral("**bold** and *italic* and ~~gone~~ and __also bold__"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<b>bold</b>")));
  QVERIFY(html.contains(QStringLiteral("<i>italic</i>")));
  QVERIFY(html.contains(QStringLiteral("<s>gone</s>")));
  QVERIFY(html.contains(QStringLiteral("<b>also bold</b>")));
}

void TestMarkdown::rendersLists()
{
  const QString html = Markdown::toHtml(
                         QStringLiteral("- one\n- two\n- three\n\n1. first\n2. second\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<ul>")));
  QVERIFY(html.contains(QStringLiteral("<li>one</li>")));
  QVERIFY(html.contains(QStringLiteral("<ol>")));
  QVERIFY(html.contains(QStringLiteral("<li>first</li>")));
}

void TestMarkdown::rendersTaskList()
{
  const QString html =
    Markdown::toHtml(QStringLiteral("- [x] done\n- [ ] todo\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("\u2611 done")));
  QVERIFY(html.contains(QStringLiteral("\u2610 todo")));
}

void TestMarkdown::rendersBlockquote()
{
  const QString html = Markdown::toHtml(QStringLiteral("> quoted line\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<blockquote>")));
  QVERIFY(html.contains(QStringLiteral("quoted line")));
  QVERIFY(html.contains(QStringLiteral("</blockquote>")));
}

void TestMarkdown::rendersHorizontalRule()
{
  const QString html = Markdown::toHtml(QStringLiteral("a\n\n---\n\nb"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<hr/>")));
}

void TestMarkdown::rendersPipeTable()
{
  const QString markdown = QStringLiteral(
                             "| name | value |\n"
                             "| --- | --- |\n"
                             "| alpha | 1 |\n"
                             "| beta | 2 |\n"
                             "\n"
                             "after");

  const QString html = Markdown::toHtml(markdown, testStyle());

  QVERIFY(html.contains(QStringLiteral("<table class=\"md-table\">")));
  QVERIFY(html.contains(QStringLiteral("<th>name</th>")));
  QVERIFY(html.contains(QStringLiteral("<td>alpha</td>")));
  QVERIFY(html.contains(QStringLiteral("<td>2</td>")));
  // 表格之后的内容不能被吞进表格。
  QVERIFY(html.contains(QStringLiteral("<p>after</p>")));
}

void TestMarkdown::rendersNestedList()
{
  const QString html =
    Markdown::toHtml(QStringLiteral("- outer\n  - inner\n"), testStyle());

  QVERIFY(html.contains(QStringLiteral("<li>outer</li>")));
  QVERIFY(html.contains(QStringLiteral("<li class=\"nested\">inner</li>")));
}

void TestMarkdown::styleSheetHasNoPlaceholders()
{
  const QString css = Markdown::styleSheet(testStyle());

  QVERIFY(!css.isEmpty());
  // .arg() 链漏掉一个就会留下 %1 之类的痕迹，必须显式检查。
  QVERIFY(!css.contains(QStringLiteral("%1")));
  QVERIFY(!css.contains(QStringLiteral("%2")));
  QVERIFY(!css.contains(QStringLiteral("var(--")));
  // 关键选择器必须在场，否则 class 规则不生效。
  QVERIFY(css.contains(QStringLiteral("pre.code")));
  QVERIFY(css.contains(QStringLiteral("code.inline")));
  QVERIFY(css.contains(QStringLiteral("blockquote")));
  QVERIFY(css.contains(QStringLiteral("table.md-table")));
  // 字号阶梯：h1 应比正文大 4px，h2 大 2px（与设计规范一致）。
  QVERIFY(css.contains(QStringLiteral("font-size: 18px")));
  QVERIFY(css.contains(QStringLiteral("font-size: 16px")));
}

void TestMarkdown::plainPreviewStripsMarkup()
{
  const QString markdown = QStringLiteral(
                             "# Heading\n\nSome **bold** and `code` and [link](https://x.y).\n\n"
                             "```cpp\nint x;\n```\n");

  const QString preview = Markdown::toPlainPreview(markdown, 120);

  QVERIFY(!preview.contains(QLatin1Char('#')));
  QVERIFY(!preview.contains(QStringLiteral("**")));
  QVERIFY(!preview.contains(QLatin1Char('`')));
  QVERIFY(!preview.contains(QStringLiteral("int x;")));
  QVERIFY(preview.contains(QStringLiteral("Heading")));
  QVERIFY(preview.contains(QStringLiteral("bold")));
  QVERIFY(preview.contains(QStringLiteral("link")));

  // 超长时必须截断并带省略标记。
  const QString longPreview =
    Markdown::toPlainPreview(QString(500, QLatin1Char('a')), 40);
  QVERIFY(longPreview.size() <= 41);
  QVERIFY(longPreview.endsWith(QStringLiteral("…")));
}

void TestMarkdown::detectsChoicesOnlyFromTheUnifiedFormat()
{
  // 只认统一的 `choices` 围栏。这是刻意的：去猜正文里的编号列表必然误判，
  // 把"步骤如下：1. 2. 3."或目录变成一张卡片，正常的回答看起来像在逼用户选。
  const QList<Markdown::ChoiceQuestion> fromFence = Markdown::detectQuestions(
                                                      QStringLiteral("我看了两种改法：\n\n"
                                                                     "```choices\n"
                                                                     "- 只改 Read 工具，最小改动\n"
                                                                     "- 同时改 Read 与 Grep\n"
                                                                     "```\n"));
  QCOMPARE(fromFence.size(), 1);
  QCOMPARE(fromFence.at(0).options.size(), 2);
  QCOMPARE(fromFence.at(0).options.at(0).label, QStringLiteral("只改 Read 工具，最小改动"));
  // 提交后填进输入框的就是选项标题。
  QCOMPARE(fromFence.at(0).options.at(1).value, QStringLiteral("同时改 Read 与 Grep"));

  // 有序列表写法的选项也要能认。
  const QList<Markdown::ChoiceQuestion> ordered = Markdown::detectQuestions(
                                                    QStringLiteral("```choices\n1. 方案甲\n2. 方案乙\n3. 方案丙\n```"));
  QCOMPARE(ordered.size(), 1);
  QCOMPARE(ordered.at(0).options.size(), 3);
  QCOMPARE(ordered.at(0).options.at(2).label, QStringLiteral("方案丙"));

  // ★ 正文里的普通编号列表**不该**被当成选项——这正是放弃启发式识别的理由。
  QVERIFY2(Markdown::detectQuestions(QStringLiteral("步骤如下：\n\n"
                                                     "1. 打开文件\n"
                                                     "2. 修改配置\n"
                                                     "3. 重启服务\n"))
           .isEmpty(),
           "普通的编号步骤不该被当成选项");

  // 列表就该是列表，不能因为"看起来像选项"就变成卡片。
  QVERIFY(Markdown::detectQuestions(QStringLiteral("你可以选择：\n- 甲\n- 乙\n")).isEmpty());

  // 只有一个选项、或超过 6 项，都不算选择。
  QVERIFY(Markdown::detectQuestions(QStringLiteral("```choices\n- 只有一个\n```")).isEmpty());
  QVERIFY(Markdown::detectQuestions(QStringLiteral(
                                      "```choices\n- 1\n- 2\n- 3\n- 4\n- 5\n- 6\n- 7\n```"))
          .isEmpty());

  // 只认**最后**一个 choices 块：前一轮的选项不该再摆出来。
  const QList<Markdown::ChoiceQuestion> last = Markdown::detectQuestions(
                                                 QStringLiteral("```choices\n- 旧的甲\n- 旧的乙\n```\n\n中间说明\n\n"
                                                                "```choices\n- 新的甲\n- 新的乙\n```\n"));
  QCOMPARE(last.size(), 1);
  QCOMPARE(last.at(0).options.at(0).label, QStringLiteral("新的甲"));

  // 渲染成列表，而不是代码块（否则选项会在正文与卡片里各出现一次）。
  const QString html = Markdown::toHtml(
                         QStringLiteral("```choices\n- 甲方案\n- 乙方案\n```"), testStyle());
  QVERIFY2(html.contains(QStringLiteral("<ul class=\"choices\">")), qPrintable(html));
  QVERIFY2(!html.contains(QStringLiteral("code-block")),
           "choices 块不该渲染成代码块");
  QVERIFY(html.contains(QStringLiteral("甲方案")));
}

void TestMarkdown::parsesRichQuestionsWithHeaderDescriptionAndRecommendation()
{
  // 问询卡片需要比"一行一个标签"更多的信息：眉标、问题、每个选项的灰字说明、
  // 以及推荐徽标。这些都在同一个 choices 围栏里用行首标记表达。
  const QList<Markdown::ChoiceQuestion> questions = Markdown::detectQuestions(
                                                     QStringLiteral(
                                                       "我看到两条路：\n\n"
                                                       "```choices\n"
                                                       "# 重定方向\n"
                                                       "? 按哪个方向做完？\n"
                                                       "- 中文源 + 补 302 条英译 (推荐) :: 源文案保持中文，只补英译\n"
                                                       "- 仍按英文源重做 :: 把 302 处中文改成英文源码\n"
                                                       "```\n"));
  QCOMPARE(questions.size(), 1);
  QCOMPARE(questions.at(0).header, QStringLiteral("重定方向"));
  QCOMPARE(questions.at(0).question, QStringLiteral("按哪个方向做完？"));
  QCOMPARE(questions.at(0).options.size(), 2);

  const Markdown::ChoiceOption & first = questions.at(0).options.at(0);
  QCOMPARE(first.label, QStringLiteral("中文源 + 补 302 条英译"));
  QVERIFY2(first.recommended, "末尾的 (推荐) 应当变成推荐标记，并从标题里去掉");
  QCOMPARE(first.description, QStringLiteral("源文案保持中文，只补英译"));
  QCOMPARE(first.value, first.label);

  const Markdown::ChoiceOption & second = questions.at(0).options.at(1);
  QVERIFY(!second.recommended);
  QCOMPARE(second.label, QStringLiteral("仍按英文源重做"));
  QCOMPARE(second.description, QStringLiteral("把 302 处中文改成英文源码"));

  // `?` 开新的一题：卡片靠它分页（1 / N）。
  const QList<Markdown::ChoiceQuestion> twoPages = Markdown::detectQuestions(
                                                     QStringLiteral(
                                                       "```choices\n"
                                                       "? 第一题\n"
                                                       "- 甲\n- 乙\n"
                                                       "? 第二题\n"
                                                       "- 丙\n- 丁\n"
                                                       "```\n"));
  QCOMPARE(twoPages.size(), 2);
  QCOMPARE(twoPages.at(1).question, QStringLiteral("第二题"));
  QCOMPARE(twoPages.at(1).options.at(0).label, QStringLiteral("丙"));

  // 说明文字的分隔符要求两侧有空格：C++ 限定名不能被切成两半。
  const QList<Markdown::ChoiceQuestion> scoped = Markdown::detectQuestions(
                                                   QStringLiteral("```choices\n"
                                                                  "- 改 ProviderRegistry::resolve()\n"
                                                                  "- 不改\n"
                                                                  "```\n"));
  QCOMPARE(scoped.at(0).options.at(0).label,
           QStringLiteral("改 ProviderRegistry::resolve()"));
  QVERIFY(scoped.at(0).options.at(0).description.isEmpty());

  // 全角括号的 (推荐) 也认。
  const QList<Markdown::ChoiceQuestion> fullWidth = Markdown::detectQuestions(
                                                      QStringLiteral("```choices\n"
                                                                     "- 甲（推荐）\n"
                                                                     "- 乙\n"
                                                                     "```\n"));
  QCOMPARE(fullWidth.at(0).options.at(0).label, QStringLiteral("甲"));
  QVERIFY(fullWidth.at(0).options.at(0).recommended);

  // 旧格式（只有选项、没有眉标与问题）仍然要能解析，且拿得到空的头部字段。
  const QList<Markdown::ChoiceQuestion> legacy = Markdown::detectQuestions(
                                                   QStringLiteral("```choices\n- 甲\n- 乙\n```\n"));
  QCOMPARE(legacy.size(), 1);
  QVERIFY(legacy.at(0).header.isEmpty());
  QVERIFY(legacy.at(0).question.isEmpty());
  QCOMPARE(legacy.at(0).options.size(), 2);

  // 合规性仍然按"每道问题"判定：出问题的那一题丢掉，不牵连别题。
  const QList<Markdown::ChoiceQuestion> partlyBad = Markdown::detectQuestions(
                                                      QStringLiteral("```choices\n"
                                                                     "? 七个选项的那题\n"
                                                                     "- 1\n- 2\n- 3\n- 4\n- 5\n- 6\n- 7\n"
                                                                     "? 正常的一题\n"
                                                                     "- 甲\n- 乙\n"
                                                                     "```\n"));
  QCOMPARE(partlyBad.size(), 1);
  QCOMPARE(partlyBad.at(0).question, QStringLiteral("正常的一题"));

  // 正文里的普通列表依旧不能变成卡片。
  QVERIFY(Markdown::detectQuestions(QStringLiteral("步骤如下：\n\n1. 打开文件\n2. 修改配置\n")).isEmpty());
}

QTEST_MAIN(TestMarkdown)
#include "test_markdown.moc"
